// three::VoxelSync and TransformInterpolator, on top of the NetObject layer
// in thistle.cpp.
//
// VoxelSync is a NetObject class ("thistle.voxels:<name>") the server
// spawns. Every client that gets it says "hello"; the server answers that
// client alone (target RPCs) with the block types, then streams it every
// chunk, a byte budget per update. Separately, whenever the server's world
// changes, the changed chunks (found through the per-chunk revisions the
// physics sync also uses) go to every client that said hello. Chunk data is
// VoxelWorld::serialize_chunk()'s run-length bytes, base64'd into the JSON.
#include "thistle_core.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace thistle::detail {

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

std::string b64_encode(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (i + 1 < in.size() ? static_cast<uint32_t>(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? static_cast<uint32_t>(in[i + 2]) : 0);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[n & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> b64_decode(const std::string& in) {
    static const auto table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(kB64[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        const int v = table[static_cast<uint8_t>(ch)];
        if (v < 0) continue; // '=' padding (or junk)
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

} // namespace thistle::detail

namespace thistle::three {

using detail::b64_decode;
using detail::b64_encode;

namespace {


uint64_t pack(ivec3 c) {
    constexpr int64_t bias = 1 << 20;
    return (static_cast<uint64_t>(c.x + bias) << 42) | (static_cast<uint64_t>(c.y + bias) << 21) | static_cast<uint64_t>(c.z + bias);
}
ivec3 unpack(uint64_t k) {
    constexpr int64_t bias = 1 << 20, mask = (1 << 21) - 1;
    return {static_cast<int>(((k >> 42) & mask) - bias), static_cast<int>(((k >> 21) & mask) - bias), static_cast<int>((k & mask) - bias)};
}

nlohmann::json ivec_json(ivec3 v) { return {v.x, v.y, v.z}; }
ivec3 json_ivec(const nlohmann::json& j) {
    if (!j.is_array() || j.size() != 3) return {};
    return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

class VoxelSyncObject;

} // namespace

struct VoxelSyncImpl {
    VoxelWorld* world = nullptr;
    std::string name;
    VoxelSync* self = nullptr;
    VoxelSyncObject* object = nullptr; // server: the spawned one; client: our replica of it
    bool hosting = false;

    // server
    struct Stream {
        int conn;
        std::vector<ivec3> queue;
        size_t next = 0;
    };
    std::vector<Stream> streams;
    std::vector<int> joined;                      // clients that said hello: they get every change
    std::unordered_map<uint64_t, uint64_t> sent;  // chunk -> revision last sent out
    uint64_t world_revision = 0;

    // client
    bool said_hello = false;
    bool got_header = false;
    int expected = 0, received = 0;

    void reset_client() {
        said_hello = got_header = false;
        expected = received = 0;
    }
};

namespace {

// Which VoxelSync a newly made object belongs to. The server's host() sets
// `hosting` around its net_spawn(); anything else is a client's replica,
// and goes to the newest client-side VoxelSync of that name. (A test that
// runs a server and a client in one process has both.)
struct Registry {
    VoxelSyncImpl* hosting = nullptr;
    std::unordered_map<std::string, std::vector<VoxelSyncImpl*>> clients;
};
Registry& registry() {
    static Registry r;
    return r;
}

std::string class_for(const std::string& name) { return "thistle.voxels:" + name; }

class VoxelSyncObject final : public NetObject {
public:
    VoxelSyncImpl* owner = nullptr;

    VoxelSyncObject() {
        on_command("hello", [this](const NetArgs&) {
            if (owner && owner->hosting) welcome(net::command_sender());
        });
        on_command("set", [this](const NetArgs& a) {
            if (!owner || !owner->hosting) return;
            const int conn = net::command_sender();
            const ivec3 b = json_ivec(a.value("b", nlohmann::json()));
            const BlockId id = static_cast<BlockId>(a.value("id", 0));
            const auto& allow = owner->self->allow_edit;
            if (allow && !allow(conn, b, id)) {
                // Refused: put that client's optimistic edit back.
                const ivec3 c{b.x >> 5, b.y >> 5, b.z >> 5};
                call_target_rpc(conn, "chunk", {{"c", ivec_json(c)}, {"d", b64_encode(owner->world->serialize_chunk(c))}});
                return;
            }
            owner->world->set(b, id);
        });
        on_client_rpc("world", [this](const NetArgs& a) {
            if (!owner || owner->hosting) return;
            const std::vector<uint8_t> bytes = b64_decode(a.value("types", std::string()));
            VoxelWorld header;
            if (!header.deserialize(bytes.data(), bytes.size())) {
                log_warn("VoxelSync: the server's world header didn't decode");
                return;
            }
            VoxelWorld& w = *owner->world;
            w.clear();
            w.copy_block_types(header);
            w.voxel_size = header.voxel_size;
            w.origin = header.origin;
            w.rotation = header.rotation;
            owner->expected = a.value("chunks", 0);
            owner->received = 0;
            owner->got_header = true;
        });
        on_client_rpc("chunk", [this](const NetArgs& a) {
            if (!owner || owner->hosting) return;
            const std::vector<uint8_t> bytes = b64_decode(a.value("d", std::string()));
            owner->world->deserialize_chunk(json_ivec(a.value("c", nlohmann::json())), bytes.data(), bytes.size());
            if (a.value("s", false)) ++owner->received;
        });
    }

    ~VoxelSyncObject() override {
        if (owner && owner->object == this) {
            owner->object = nullptr;
            owner->reset_client();
        }
    }

    // A client said hello: its catch-up starts with the types, then chunks
    // from update(). From now on it also gets every change.
    void welcome(int conn) {
        VoxelSyncImpl& o = *owner;
        VoxelWorld types_only;
        types_only.copy_block_types(*o.world);
        types_only.voxel_size = o.world->voxel_size;
        types_only.origin = o.world->origin;
        types_only.rotation = o.world->rotation;
        std::vector<ivec3> chunks = o.world->chunks();
        call_target_rpc(conn, "world", {{"types", b64_encode(types_only.serialize())}, {"chunks", static_cast<int>(chunks.size())}});
        o.streams.erase(std::remove_if(o.streams.begin(), o.streams.end(), [&](const auto& s) { return s.conn == conn; }), o.streams.end());
        o.streams.push_back({conn, std::move(chunks), 0});
        if (std::find(o.joined.begin(), o.joined.end(), conn) == o.joined.end()) o.joined.push_back(conn);
    }

    void say_hello() { call_command("hello"); }
    void ask_set(ivec3 b, BlockId id) { call_command("set", {{"b", ivec_json(b)}, {"id", id}}); }

    void send_chunk(int conn, ivec3 c, const std::string& data, bool stream, bool& alive) {
        alive = call_target_rpc(conn, "chunk", {{"c", ivec_json(c)}, {"d", data}, {"s", stream}});
    }
};

} // namespace

struct VoxelSyncAccess {
    static void server_update(VoxelSyncImpl& o) {
        VoxelSyncObject& obj = *o.object;
        VoxelWorld& w = *o.world;
        // Changes since last time, to everyone who has joined.
        const uint64_t revision = detail::VoxelWorldAccess::revision(w);
        if (revision != o.world_revision) {
            o.world_revision = revision;
            std::vector<std::pair<ivec3, uint64_t>> chunks;
            detail::VoxelWorldAccess::chunks(w, chunks);
            std::vector<ivec3> changed;
            std::unordered_map<uint64_t, uint64_t> now;
            for (const auto& [c, rev] : chunks) {
                now[pack(c)] = rev;
                auto it = o.sent.find(pack(c));
                if (it == o.sent.end() || it->second != rev) changed.push_back(c);
            }
            for (const auto& [key, rev] : o.sent) {
                if (!now.count(key)) changed.push_back(unpack(key)); // emptied: send it empty
            }
            o.sent = std::move(now);
            for (const ivec3& c : changed) {
                const std::string data = b64_encode(w.serialize_chunk(c));
                for (size_t i = 0; i < o.joined.size();) {
                    bool alive = true;
                    obj.send_chunk(o.joined[i], c, data, false, alive);
                    if (alive) ++i;
                    else o.joined.erase(o.joined.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
        }
        // Catch-up streams, a byte budget each per update.
        for (size_t s = 0; s < o.streams.size();) {
            VoxelSyncImpl::Stream& st = o.streams[s];
            int budget = std::max(1024, o.self->stream_bytes_per_update);
            bool alive = true;
            while (st.next < st.queue.size() && budget > 0 && alive) {
                const ivec3 c = st.queue[st.next++];
                const std::string data = b64_encode(w.serialize_chunk(c));
                obj.send_chunk(st.conn, c, data, true, alive);
                budget -= static_cast<int>(data.size()) + 64;
            }
            if (!alive || st.next >= st.queue.size()) o.streams.erase(o.streams.begin() + static_cast<std::ptrdiff_t>(s));
            else ++s;
        }
    }
};

VoxelSync::VoxelSync(VoxelWorld& world, const std::string& name) : impl_(std::make_unique<VoxelSyncImpl>()) {
    impl_->world = &world;
    impl_->name = name;
    impl_->self = this;
    registry().clients[name].push_back(impl_.get());
    net_register_class(class_for(name), [name] {
        auto obj = std::make_unique<VoxelSyncObject>();
        Registry& r = registry();
        VoxelSyncImpl* owner = r.hosting && r.hosting->name == name ? r.hosting : nullptr;
        if (!owner) {
            auto& list = r.clients[name];
            owner = list.empty() ? nullptr : list.back();
            if (owner) owner->reset_client();
        }
        obj->owner = owner;
        if (owner) owner->object = obj.get();
        return obj;
    });
}

VoxelSync::~VoxelSync() {
    auto& list = registry().clients[impl_->name];
    list.erase(std::remove(list.begin(), list.end(), impl_.get()), list.end());
    if (impl_->object) {
        impl_->object->owner = nullptr;
        if (impl_->hosting && net::is_server()) net_despawn(impl_->object);
    }
}

void VoxelSync::host() {
    if (impl_->hosting) return;
    if (!net::is_server()) {
        log_warn("VoxelSync::host: no NetServer is listening yet");
        return;
    }
    auto& list = registry().clients[impl_->name];
    list.erase(std::remove(list.begin(), list.end(), impl_.get()), list.end());
    impl_->hosting = true;
    registry().hosting = impl_.get();
    net_spawn(class_for(impl_->name));
    registry().hosting = nullptr;
    // Clients catch up through their stream; only changes from here on are broadcast.
    std::vector<std::pair<ivec3, uint64_t>> chunks;
    detail::VoxelWorldAccess::chunks(*impl_->world, chunks);
    for (const auto& [c, rev] : chunks) impl_->sent[pack(c)] = rev;
    impl_->world_revision = detail::VoxelWorldAccess::revision(*impl_->world);
}

void VoxelSync::update() {
    VoxelSyncImpl& o = *impl_;
    if (!o.object) return;
    if (o.hosting) {
        VoxelSyncAccess::server_update(o);
    } else if (!o.said_hello) {
        o.said_hello = true;
        o.object->say_hello();
    }
}

void VoxelSync::request_set(ivec3 block, BlockId id) {
    VoxelSyncImpl& o = *impl_;
    o.world->set(block, id); // the server's version replaces it if it disagrees
    if (o.hosting || !o.object) return;
    o.object->ask_set(block, id);
}

bool VoxelSync::ready() const {
    const VoxelSyncImpl& o = *impl_;
    return o.hosting || (o.got_header && o.received >= o.expected);
}

float VoxelSync::progress() const {
    const VoxelSyncImpl& o = *impl_;
    if (o.hosting) return 1.0f;
    if (!o.got_header) return 0.0f;
    return o.expected > 0 ? std::min(1.0f, static_cast<float>(o.received) / static_cast<float>(o.expected)) : 1.0f;
}

// --- TransformInterpolator ---------------------------------------------------------------------

void TransformInterpolator::add(const Transform& transform, double time) {
    if (!samples_.empty() && time <= samples_.back().time) {
        samples_.back().transform = transform; // same moment (two updates in a frame): the newer wins
        return;
    }
    samples_.push_back({time, transform});
    // Keep a second or so of history behind the playback point.
    while (samples_.size() > 2 && samples_[1].time < time - delay - 1.0) samples_.erase(samples_.begin());
}

Transform TransformInterpolator::sample(double time) const {
    if (samples_.empty()) return {};
    const double at = time - delay;
    if (at <= samples_.front().time) return samples_.front().transform;
    if (at >= samples_.back().time) return samples_.back().transform;
    size_t i = 0;
    while (i + 1 < samples_.size() && samples_[i + 1].time <= at) ++i;
    const Sample& a = samples_[i];
    const Sample& b = samples_[i + 1];
    const float t = static_cast<float>((at - a.time) / (b.time - a.time));
    return Transform{lerp(a.transform.position, b.transform.position, t), slerp(a.transform.rotation, b.transform.rotation, t),
                     lerp(a.transform.scale, b.transform.scale, t)};
}

} // namespace thistle::three
