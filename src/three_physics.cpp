// three::Physics3D on Jolt Physics. Only compiled with THISTLE_PHYSICS3D on
// (three_physics_off.cpp stands in otherwise).
//
// How it maps onto Jolt:
// - Jolt's global state (allocator hooks, the type factory, a worker
//   thread pool) is set up by the first Physics3D and torn down with the
//   last one.
// - Object layers encode the game's layer (0..31) and what kind of body it
//   is: layer * 4 + {static, moving, trigger}. Statics never test against
//   statics and triggers never test against statics or other triggers; the
//   rest goes by the layer matrix set_layers_collide() edits.
// - Contact callbacks arrive on Jolt's worker threads, so the listener only
//   records them. step() then turns them into per-body-pair "started
//   touching" / "entered" / "left" events on the game's thread.
// - All body access uses Jolt's no-lock interfaces: Physics3D is used from
//   one thread, and nothing touches bodies while Update() runs.
#include "thistle_internal.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace thistle::three {

namespace {

// --- Jolt's process-wide state -------------------------------------------------------

std::mutex g_jolt_mutex;
int g_jolt_users = 0;
JPH::JobSystemThreadPool* g_jobs = nullptr;

void jolt_trace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    log_warn(std::string("Jolt: ") + buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool jolt_assert(const char* expr, const char* msg, const char* file, JPH::uint line) {
    log_error(std::string("Jolt assert: ") + expr + " " + (msg ? msg : "") + " (" + file + ":" + std::to_string(line) + ")");
    return true; // break into the debugger
}
#endif

void acquire_jolt() {
    std::lock_guard<std::mutex> lock(g_jolt_mutex);
    if (g_jolt_users++ > 0) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = jolt_trace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    // Leave a core for the game's own thread (which also works on physics
    // jobs while it waits for a step), and don't take over a 32-core
    // machine for what's usually a few hundred bodies.
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    g_jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::clamp(cores - 1, 1, 8));
}

void release_jolt() {
    std::lock_guard<std::mutex> lock(g_jolt_mutex);
    if (--g_jolt_users > 0) return;
    delete g_jobs;
    g_jobs = nullptr;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// --- conversions -------------------------------------------------------------------------

JPH::Vec3 jv(vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat jq(quat q) {
    const JPH::Quat r(q.x, q.y, q.z, q.w);
    const float len = r.Length();
    return len > 1e-6f ? r / len : JPH::Quat::sIdentity();
}
vec3 tv(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
#ifdef JPH_DOUBLE_PRECISION
vec3 tv(JPH::RVec3Arg v) { return {static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ())}; }
#endif
quat tq(JPH::QuatArg q) { return quat(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

JPH::BodyID to_id(RigidBody b) { return JPH::BodyID(b.id - 1); } // handle 0 -> Jolt's invalid id (all bits set)
RigidBody to_body(const JPH::BodyID& id) { return RigidBody{id.GetIndexAndSequenceNumber() + 1}; }
uint32_t raw(const JPH::BodyID& id) { return id.GetIndexAndSequenceNumber(); }
uint64_t pair_key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

// --- layers ------------------------------------------------------------------------------

enum : JPH::ObjectLayer { kStatic = 0, kMoving = 1, kTrigger = 2 };
constexpr int kLayers = 32;

JPH::ObjectLayer object_layer(int layer, JPH::ObjectLayer kind) {
    return static_cast<JPH::ObjectLayer>(std::clamp(layer, 0, kLayers - 1) * 4 + kind);
}

namespace bp {
constexpr JPH::BroadPhaseLayer still(0);
constexpr JPH::BroadPhaseLayer moving(1);
} // namespace bp

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return (layer & 3) == kStatic ? bp::still : bp::moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp::still ? "static" : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        // Moving bodies test against everything; statics and triggers only
        // care about things that move.
        return (layer & 3) == kMoving || broad == bp::moving;
    }
};

class LayerPairs final : public JPH::ObjectLayerPairFilter {
public:
    explicit LayerPairs(const uint32_t* matrix) : matrix_(matrix) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if ((a & 3) != kMoving && (b & 3) != kMoving) return false; // static/trigger pairs
        return (matrix_[a >> 2] >> (b >> 2)) & 1u;
    }

private:
    const uint32_t* matrix_;
};

// --- contact recording (worker threads) -----------------------------------------------------

struct RawEvent {
    uint32_t a = 0, b = 0;
    bool added = false;
    bool trigger = false;   // one of the two is a trigger
    bool a_is_trigger = false;
    vec3 point, normal;
    float speed = 0.0f;
};

class Listener final : public JPH::ContactListener {
public:
    std::mutex mutex;
    std::vector<RawEvent> events;

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        RawEvent e;
        e.a = raw(b1.GetID());
        e.b = raw(b2.GetID());
        e.added = true;
        e.trigger = b1.IsSensor() || b2.IsSensor();
        e.a_is_trigger = b1.IsSensor();
        if (!e.trigger && m.mRelativeContactPointsOn1.size() > 0) {
            const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
            e.point = tv(p);
            e.normal = tv(m.mWorldSpaceNormal); // from b1 toward b2
            // Velocities are still the ones from before the impact here.
            const JPH::Vec3 closing = b1.GetPointVelocity(p) - b2.GetPointVelocity(p);
            e.speed = std::max(0.0f, closing.Dot(m.mWorldSpaceNormal));
        }
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        RawEvent e;
        e.a = raw(pair.GetBody1ID());
        e.b = raw(pair.GetBody2ID());
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }
};

// --- shapes --------------------------------------------------------------------------------

struct ShapeKey {
    int kind;
    float x, y, z;
    bool operator==(const ShapeKey& o) const { return kind == o.kind && x == o.x && y == o.y && z == o.z; }
};
struct ShapeKeyHash {
    size_t operator()(const ShapeKey& k) const {
        uint32_t w[3];
        std::memcpy(w, &k.x, sizeof(float));
        std::memcpy(w + 1, &k.y, sizeof(float));
        std::memcpy(w + 2, &k.z, sizeof(float));
        size_t h = static_cast<size_t>(k.kind);
        for (uint32_t v : w) h = h * 1000003u ^ v;
        return h;
    }
};

bool is_identity(vec3 offset, quat rotation) {
    return offset == vec3{} && rotation.x == 0.0f && rotation.y == 0.0f && rotation.z == 0.0f && rotation.w == 1.0f;
}

} // namespace

struct Physics3DImpl {
    struct PairState {
        int count = 0;
        bool trigger = false;
        RigidBody trigger_body, other;
    };
    struct Exiting {
        RigidBody trigger_body, other;
        int age = 0;
    };

    uint32_t layer_matrix[kLayers];
    BroadPhaseLayers broad_layers;
    ObjectVsBroadPhase object_vs_broad;
    LayerPairs layer_pairs{layer_matrix};
    Listener listener;
    std::unique_ptr<JPH::TempAllocator> temp;
    std::unique_ptr<JPH::PhysicsSystem> system;

    std::unordered_map<uint64_t, PairState> pairs;
    std::unordered_map<uint64_t, Exiting> exiting;
    std::unordered_set<uint32_t> removed;         // since the last step
    std::unordered_set<uint32_t> static_triggers; // added as Static; simulated as Kinematic
    std::vector<TriggerEvent> pending_triggers;   // exits caused by remove(), reported next step
    std::vector<Contact> contacts;
    std::vector<TriggerEvent> triggers;
    std::function<void(const Contact&)> contact_fn;
    std::function<void(const TriggerEvent&)> trigger_fn;
    std::unordered_map<ShapeKey, JPH::ShapeRefC, ShapeKeyHash> shape_cache;
    int added_since_optimize = 0;
    bool warned_full = false;

    // Block worlds added with add_static(): one static body per chunk.
    struct VoxelLink {
        const VoxelWorld* world = nullptr;
        uint64_t revision = ~0ull;
        vec3 origin;
        quat rotation;
        float voxel_size = 0.0f;
        std::unordered_map<uint64_t, std::pair<RigidBody, uint64_t>> chunks; // packed chunk coords -> body, chunk revision
    };
    std::vector<VoxelLink> voxel_links;

    explicit Physics3DImpl(int max_bodies) {
        std::fill(std::begin(layer_matrix), std::end(layer_matrix), 0xFFFFFFFFu);
        acquire_jolt();
        temp = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(16 * 1024 * 1024);
        system = std::make_unique<JPH::PhysicsSystem>();
        const JPH::uint bodies = static_cast<JPH::uint>(std::clamp(max_bodies, 16, 1 << 22));
        system->Init(bodies, 0, bodies, std::max(1024u, bodies / 4), broad_layers, object_vs_broad, layer_pairs);
        system->SetContactListener(&listener);
        // Jolt lets resting bodies sink up to 2 cm into what they rest on
        // (it's steadier that way). At game scale that shows: a crate
        // visibly sunk into the floor. 5 mm is still steady.
        JPH::PhysicsSettings settings = system->GetPhysicsSettings();
        settings.mPenetrationSlop = 0.005f;
        system->SetPhysicsSettings(settings);
    }

    ~Physics3DImpl() {
        shape_cache.clear();
        system.reset(); // destroys its bodies
        temp.reset();
        release_jolt();
    }

    JPH::BodyInterface& bodies() { return system->GetBodyInterfaceNoLock(); }
    const JPH::BodyLockInterface& locks() const { return system->GetBodyLockInterfaceNoLock(); }

    JPH::ShapeRefC hull(std::vector<vec3> points) {
        // Duplicate points (every model triangle repeats its corners) only
        // slow the hull builder down.
        std::sort(points.begin(), points.end(), [](vec3 a, vec3 b) {
            return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
        });
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (points.empty()) return nullptr;
        JPH::Array<JPH::Vec3> pts;
        pts.reserve(points.size());
        Bounds b;
        for (vec3 p : points) {
            pts.push_back(jv(p));
            b.add(p);
        }
        JPH::ConvexHullShapeSettings settings(pts);
        JPH::ShapeSettings::ShapeResult r = settings.Create();
        if (r.IsValid()) return r.Get();
        // Flat or degenerate clouds (a single quad) have no hull: fall back
        // to their bounding box with a little thickness.
        const vec3 size = b.size();
        return new JPH::BoxShape(JPH::Vec3(std::max(size.x, 0.02f), std::max(size.y, 0.02f), std::max(size.z, 0.02f)) * 0.5f, 0.0f);
    }

    JPH::ShapeRefC primitive(const Collider& c) {
        const ShapeKey key{static_cast<int>(c.kind), c.size.x, c.size.y, c.size.z};
        if (auto it = shape_cache.find(key); it != shape_cache.end()) return it->second;
        JPH::ShapeRefC shape;
        switch (c.kind) {
            case Collider::Kind::Box: {
                const JPH::Vec3 half = JPH::Vec3::sMax(jv(c.size) * 0.5f, JPH::Vec3::sReplicate(0.005f));
                // Jolt rounds box corners by the convex radius, which must fit inside the box.
                shape = new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, half.ReduceMin() * 0.5f));
                break;
            }
            case Collider::Kind::Sphere: shape = new JPH::SphereShape(std::max(c.size.x, 0.005f)); break;
            case Collider::Kind::Capsule: {
                const float r = std::max(c.size.x, 0.005f);
                const float half_cylinder = c.size.y * 0.5f - r;
                if (half_cylinder > 1e-4f) shape = new JPH::CapsuleShape(half_cylinder, r);
                else shape = new JPH::SphereShape(r);
                break;
            }
            default: { // Cylinder
                const float r = std::max(c.size.x, 0.005f), half = std::max(c.size.y * 0.5f, 0.005f);
                shape = new JPH::CylinderShape(half, r, std::min(JPH::cDefaultConvexRadius, std::min(half, r) * 0.5f));
                break;
            }
        }
        if (shape_cache.size() > 4096) shape_cache.clear(); // lots of one-off sizes (debris): don't grow forever
        shape_cache.emplace(key, shape);
        return shape;
    }

    // Boxes given as min/max corner pairs, glued into one shape.
    JPH::ShapeRefC voxel_boxes(const std::vector<vec3>& corners, float voxel_size) {
        if (corners.size() < 2) return nullptr;
        // Rounded corners help Jolt's collision detection, but a voxel's
        // edges should stay sharp-ish: at most a tenth of a block.
        const float rounding = std::min(JPH::cDefaultConvexRadius, 0.1f * std::max(voxel_size, 1e-3f));
        std::unordered_map<ShapeKey, JPH::ShapeRefC, ShapeKeyHash> boxes; // most boxes repeat a few sizes
        auto box = [&](vec3 size) {
            const ShapeKey key{0, size.x, size.y, size.z};
            if (auto it = boxes.find(key); it != boxes.end()) return it->second;
            const JPH::Vec3 half = jv(size) * 0.5f;
            JPH::ShapeRefC b = new JPH::BoxShape(half, std::min(rounding, half.ReduceMin() * 0.5f));
            boxes.emplace(key, b);
            return b;
        };
        if (corners.size() == 2) {
            const vec3 lo = corners[0], hi = corners[1];
            return new JPH::RotatedTranslatedShape(jv((lo + hi) * 0.5f), JPH::Quat::sIdentity(), box(hi - lo));
        }
        JPH::StaticCompoundShapeSettings settings;
        for (size_t i = 0; i + 1 < corners.size(); i += 2) {
            const vec3 lo = corners[i], hi = corners[i + 1];
            settings.AddShape(jv((lo + hi) * 0.5f), JPH::Quat::sIdentity(), box(hi - lo));
        }
        JPH::ShapeSettings::ShapeResult r = settings.Create();
        if (!r.IsValid()) {
            log_warn("Physics3D: couldn't build a voxel collider: " + std::string(r.GetError().c_str()));
            return nullptr;
        }
        return r.Get();
    }

    // The collider's own shape, ignoring its offset/rotation.
    JPH::ShapeRefC local_shape(const Collider& c, bool dynamic) {
        switch (c.kind) {
            case Collider::Kind::Box:
            case Collider::Kind::Sphere:
            case Collider::Kind::Capsule:
            case Collider::Kind::Cylinder: return primitive(c);
            case Collider::Kind::Convex: return hull(c.points);
            case Collider::Kind::Mesh: {
                if (dynamic) return hull(c.points); // Jolt can't collide two triangle meshes
                JPH::TriangleList tris;
                tris.reserve(c.points.size() / 3);
                for (size_t i = 0; i + 2 < c.points.size(); i += 3) {
                    const vec3 a = c.points[i], b = c.points[i + 1], d = c.points[i + 2];
                    tris.push_back(JPH::Triangle(JPH::Float3(a.x, a.y, a.z), JPH::Float3(b.x, b.y, b.z), JPH::Float3(d.x, d.y, d.z)));
                }
                if (tris.empty()) return nullptr;
                JPH::MeshShapeSettings settings(tris);
                JPH::ShapeSettings::ShapeResult r = settings.Create();
                if (!r.IsValid()) {
                    log_warn("Physics3D: couldn't build a mesh collider: " + std::string(r.GetError().c_str()));
                    return nullptr;
                }
                return r.Get();
            }
            case Collider::Kind::Voxels: return voxel_boxes(c.points, c.size.x);
            case Collider::Kind::Compound: {
                JPH::StaticCompoundShapeSettings settings;
                int count = 0;
                for (const Collider& part : c.parts) {
                    JPH::ShapeRefC s = local_shape(part, dynamic);
                    if (!s) continue;
                    settings.AddShape(jv(part.offset), jq(part.rotation), s);
                    ++count;
                }
                if (count == 0) return nullptr;
                JPH::ShapeSettings::ShapeResult r = settings.Create();
                if (!r.IsValid()) {
                    log_warn("Physics3D: couldn't build a compound collider: " + std::string(r.GetError().c_str()));
                    return nullptr;
                }
                return r.Get();
            }
        }
        return nullptr;
    }

    JPH::ShapeRefC shape(const Collider& c, bool dynamic) {
        JPH::ShapeRefC s = local_shape(c, dynamic);
        if (!s || is_identity(c.offset, c.rotation)) return s;
        return new JPH::RotatedTranslatedShape(jv(c.offset), jq(c.rotation), s);
    }

    static uint64_t pack(ivec3 c) {
        constexpr int64_t bias = 1 << 20;
        return (static_cast<uint64_t>(c.x + bias) << 42) | (static_cast<uint64_t>(c.y + bias) << 21) | static_cast<uint64_t>(c.z + bias);
    }

    void remove_body(RigidBody body) {
        const JPH::BodyID id = to_id(body);
        if (!body || !bodies().IsAdded(id)) return;
        bodies().RemoveBody(id);
        bodies().DestroyBody(id);
        removed.insert(raw(id));
    }

    // Brings each linked block world's chunk bodies up to date with its
    // edits. Runs before a step, so nothing simulates against stale blocks.
    void sync_voxels() {
        std::vector<std::pair<ivec3, uint64_t>> chunks;
        std::vector<std::pair<ivec3, ivec3>> boxes;
        for (VoxelLink& link : voxel_links) {
            const VoxelWorld& w = *link.world;
            const bool moved = !(w.origin == link.origin) || w.rotation.x != link.rotation.x || w.rotation.y != link.rotation.y ||
                               w.rotation.z != link.rotation.z || w.rotation.w != link.rotation.w || w.voxel_size != link.voxel_size;
            const uint64_t revision = detail::VoxelWorldAccess::revision(w);
            if (!moved && revision == link.revision) continue;
            if (moved) { // every body is placed by the world's transform: rebuild them all
                for (auto& [key, entry] : link.chunks) remove_body(entry.first);
                link.chunks.clear();
                link.origin = w.origin;
                link.rotation = w.rotation;
                link.voxel_size = w.voxel_size;
            }
            link.revision = revision;
            detail::VoxelWorldAccess::chunks(w, chunks);
            std::unordered_set<uint64_t> alive;
            const float s = w.voxel_size;
            const Transform place{w.origin, w.rotation};
            for (const auto& [chunk, chunk_revision] : chunks) {
                const uint64_t key = pack(chunk);
                alive.insert(key);
                auto it = link.chunks.find(key);
                if (it != link.chunks.end() && it->second.second == chunk_revision) continue;
                if (it != link.chunks.end()) remove_body(it->second.first);
                boxes.clear();
                detail::voxel_chunk_boxes(w, chunk, boxes);
                RigidBody body;
                if (!boxes.empty()) {
                    std::vector<vec3> corners;
                    corners.reserve(boxes.size() * 2);
                    for (const auto& [lo, hi] : boxes) {
                        corners.push_back(vec3{static_cast<float>(lo.x), static_cast<float>(lo.y), static_cast<float>(lo.z)} * s);
                        corners.push_back(vec3{static_cast<float>(hi.x), static_cast<float>(hi.y), static_cast<float>(hi.z)} * s);
                    }
                    if (JPH::ShapeRefC shape = voxel_boxes(corners, s)) {
                        JPH::BodyCreationSettings bcs(shape, JPH::RVec3(jv(w.origin)), jq(w.rotation), JPH::EMotionType::Static,
                                                      object_layer(0, kStatic));
                        bcs.mFriction = 0.6f;
                        if (JPH::Body* b = bodies().CreateBody(bcs)) {
                            bodies().AddBody(b->GetID(), JPH::EActivation::DontActivate);
                            body = to_body(b->GetID());
                            ++added_since_optimize;
                        }
                    }
                }
                link.chunks[key] = {body, chunk_revision};
                wake_chunk(w, chunk, place);
            }
            for (auto it = link.chunks.begin(); it != link.chunks.end();) {
                if (alive.count(it->first)) { ++it; continue; }
                remove_body(it->second.first);
                // Rebuild the chunk's key into coords to wake what rested on it.
                constexpr int64_t bias = 1 << 20, mask = (1 << 21) - 1;
                const ivec3 chunk{static_cast<int>(((it->first >> 42) & mask) - bias), static_cast<int>(((it->first >> 21) & mask) - bias),
                                  static_cast<int>((it->first & mask) - bias)};
                wake_chunk(w, chunk, place);
                it = link.chunks.erase(it);
            }
        }
    }

    // Sleeping bodies don't notice the ground changing under them.
    void wake_chunk(const VoxelWorld& w, ivec3 chunk, const Transform& place) {
        const float size = VoxelWorld::chunk_size * w.voxel_size;
        const vec3 lo = vec3{static_cast<float>(chunk.x), static_cast<float>(chunk.y), static_cast<float>(chunk.z)} * size;
        const Bounds local{lo - vec3{w.voxel_size, w.voxel_size, w.voxel_size}, lo + vec3{size, size, size} + vec3{w.voxel_size, w.voxel_size, w.voxel_size}};
        const Bounds b = local.transformed(place.matrix());
        bodies().ActivateBodiesInAABox(JPH::AABox(jv(b.min), jv(b.max)), {}, {});
    }

    void add_event_contact(const RawEvent& e) {
        Contact c;
        c.a = RigidBody{e.a + 1};
        c.b = RigidBody{e.b + 1};
        c.point = e.point;
        c.normal = e.normal;
        c.speed = e.speed;
        contacts.push_back(c);
    }

    // Forget pairs with bodies removed since the last step; a trigger
    // losing a body (or a removed trigger) reports it as leaving.
    void sweep_removed() {
        if (removed.empty()) return;
        for (auto it = pairs.begin(); it != pairs.end();) {
            const uint32_t a = static_cast<uint32_t>(it->first >> 32), b = static_cast<uint32_t>(it->first);
            if (removed.count(a) || removed.count(b)) {
                if (it->second.trigger) triggers.push_back({it->second.trigger_body, it->second.other, false});
                it = pairs.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = exiting.begin(); it != exiting.end();) {
            const uint32_t a = static_cast<uint32_t>(it->first >> 32), b = static_cast<uint32_t>(it->first);
            if (removed.count(a) || removed.count(b)) {
                triggers.push_back({it->second.trigger_body, it->second.other, false});
                it = exiting.erase(it);
            } else {
                ++it;
            }
        }
        removed.clear();
    }

    void process_events() {
        std::vector<RawEvent> events;
        {
            std::lock_guard<std::mutex> lock(listener.mutex);
            events.swap(listener.events);
        }
        for (const RawEvent& e : events) {
            const uint64_t key = pair_key(e.a, e.b);
            if (e.added) {
                PairState& st = pairs[key];
                if (st.count++ > 0) continue; // another sub-shape of a pair already touching
                if (e.trigger) {
                    st.trigger = true;
                    st.trigger_body = RigidBody{(e.a_is_trigger ? e.a : e.b) + 1};
                    st.other = RigidBody{(e.a_is_trigger ? e.b : e.a) + 1};
                    // Back within a step of leaving: that was Jolt dropping
                    // and re-finding the contact (a body falling asleep
                    // does that), not a real exit and re-entry.
                    if (exiting.erase(key) == 0) triggers.push_back({st.trigger_body, st.other, true});
                } else {
                    add_event_contact(e);
                }
            } else {
                auto it = pairs.find(key);
                if (it == pairs.end()) continue; // a pair we already forgot (a removed body)
                if (--it->second.count > 0) continue;
                if (it->second.trigger) exiting[key] = Exiting{it->second.trigger_body, it->second.other, 0};
                pairs.erase(it);
            }
        }
        for (auto it = exiting.begin(); it != exiting.end();) {
            if (it->second.age++ >= 1) {
                triggers.push_back({it->second.trigger_body, it->second.other, false});
                it = exiting.erase(it);
            } else {
                ++it;
            }
        }
    }
};

bool physics3d_available() { return true; }

Physics3D::Physics3D(int max_bodies) : impl_(std::make_unique<Physics3DImpl>(max_bodies)) {}
Physics3D::~Physics3D() = default;
Physics3D::Physics3D(Physics3D&&) noexcept = default;
Physics3D& Physics3D::operator=(Physics3D&&) noexcept = default;

RigidBody Physics3D::add(const BodySettings& s) {
    Physics3DImpl& I = *impl_;
    const bool dynamic = s.type == BodyType::Dynamic;
    JPH::ShapeRefC shape = I.shape(s.collider, dynamic);
    if (!shape) {
        log_warn("Physics3D::add: the collider is empty (no points, triangles or parts), so no body was added");
        return {};
    }
    JPH::EMotionType motion = s.type == BodyType::Static ? JPH::EMotionType::Static
                              : dynamic                  ? JPH::EMotionType::Dynamic
                                                         : JPH::EMotionType::Kinematic;
    // Static triggers only notice bodies that are awake; kinematic ones that
    // are kept awake notice sleeping bodies too, so a crate resting inside
    // doesn't "leave" when it falls asleep.
    if (s.trigger && motion == JPH::EMotionType::Static) motion = JPH::EMotionType::Kinematic;
    const JPH::ObjectLayer layer = object_layer(s.layer, s.trigger ? kTrigger : motion == JPH::EMotionType::Static ? kStatic : kMoving);

    JPH::BodyCreationSettings bcs(shape, JPH::RVec3(jv(s.position)), jq(s.rotation), motion, layer);
    bcs.mIsSensor = s.trigger;
    bcs.mAllowSleeping = s.can_sleep && !s.trigger;
    bcs.mMotionQuality = s.fast ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    bcs.mFriction = s.friction;
    bcs.mRestitution = s.bounciness;
    bcs.mLinearDamping = s.linear_damping;
    bcs.mAngularDamping = s.angular_damping;
    bcs.mGravityFactor = s.gravity_scale;
    bcs.mUserData = s.user;
    if (motion != JPH::EMotionType::Static) {
        bcs.mAllowDynamicOrKinematic = true; // so set_type() can switch it
        bcs.mLinearVelocity = jv(s.velocity);
        bcs.mAngularVelocity = jv(s.angular_velocity);
        // Mass from our density rather than Jolt's shape default (1000 kg/m^3);
        // shapes without volume (meshes on a kinematic body) keep Jolt's default.
        const float volume = shape->GetVolume();
        if (s.mass > 0.0f || volume > 0.0f) {
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = s.mass > 0.0f ? s.mass : std::max(volume * s.density, 1e-3f);
        }
    }
    JPH::Body* body = I.bodies().CreateBody(bcs);
    if (!body) {
        log_warn("Physics3D::add: the body limit is reached (see Physics3D's max_bodies), so no body was added");
        return {};
    }
    const JPH::BodyID id = body->GetID();
    I.bodies().AddBody(id, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (s.trigger && s.type == BodyType::Static) I.static_triggers.insert(raw(id));
    ++I.added_since_optimize;
    return to_body(id);
}

RigidBody Physics3D::add_box(vec3 center, vec3 size, BodyType type) {
    return add(BodySettings{.collider = Collider::box(size), .type = type, .position = center});
}

RigidBody Physics3D::add_sphere(vec3 center, float radius, BodyType type) {
    return add(BodySettings{.collider = Collider::sphere(radius), .type = type, .position = center});
}

RigidBody Physics3D::add_static(Model model, const Transform& transform) {
    Collider c;
    c.kind = Collider::Kind::Mesh;
    detail::model_world_triangles(model, transform.matrix(), c.points);
    return add(BodySettings{.collider = std::move(c), .type = BodyType::Static});
}

RigidBody Physics3D::add_static(const Terrain& terrain) {
    Collider c;
    c.kind = Collider::Kind::Mesh;
    detail::terrain_triangles(terrain, c.points);
    return add(BodySettings{.collider = std::move(c), .type = BodyType::Static});
}

void Physics3D::add_static(const VoxelWorld& voxels) {
    for (const auto& link : impl_->voxel_links) {
        if (link.world == &voxels) return;
    }
    Physics3DImpl::VoxelLink link;
    link.world = &voxels;
    link.origin = voxels.origin;
    link.rotation = voxels.rotation;
    link.voxel_size = voxels.voxel_size;
    impl_->voxel_links.push_back(std::move(link));
    impl_->sync_voxels(); // solid right away, not just from the next step
}

void Physics3D::remove_static(const VoxelWorld& voxels) {
    auto& links = impl_->voxel_links;
    for (auto it = links.begin(); it != links.end(); ++it) {
        if (it->world != &voxels) continue;
        for (auto& [key, entry] : it->chunks) impl_->remove_body(entry.first);
        links.erase(it);
        return;
    }
}

void Physics3D::remove(RigidBody body) {
    Physics3DImpl& I = *impl_;
    const JPH::BodyID id = to_id(body);
    if (!body || !I.bodies().IsAdded(id)) return; // stale handles must not reach DestroyBody
    I.bodies().RemoveBody(id);
    I.bodies().DestroyBody(id);
    I.removed.insert(raw(id));
    I.static_triggers.erase(raw(id));
}

void Physics3D::clear() {
    Physics3DImpl& I = *impl_;
    JPH::BodyIDVector ids;
    I.system->GetBodies(ids);
    if (!ids.empty()) {
        I.bodies().RemoveBodies(ids.data(), static_cast<int>(ids.size()));
        I.bodies().DestroyBodies(ids.data(), static_cast<int>(ids.size()));
    }
    for (const JPH::BodyID& id : ids) I.removed.insert(raw(id));
    I.static_triggers.clear();
    I.voxel_links.clear();
}

bool Physics3D::contains(RigidBody body) const { return body && impl_->bodies().IsAdded(to_id(body)); }
int Physics3D::body_count() const { return static_cast<int>(impl_->system->GetNumBodies()); }

void Physics3D::step(float dt) {
    Physics3DImpl& I = *impl_;
    I.contacts.clear();
    I.triggers.clear();
    I.sync_voxels();
    I.sweep_removed();
    if (dt > 0.0f) {
        constexpr float max_step = 1.0f / 60.0f;
        constexpr int max_steps = 4;
        const int steps = std::clamp(static_cast<int>(std::ceil(dt / max_step - 1e-3f)), 1, max_steps);
        dt = std::min(dt, max_step * max_steps);
        if (I.added_since_optimize > 500) {
            I.system->OptimizeBroadPhase(); // Jolt's advice after adding lots of bodies at once
            I.added_since_optimize = 0;
        }
        I.system->SetGravity(jv(gravity));
        const JPH::EPhysicsUpdateError err = I.system->Update(dt, steps, I.temp.get(), g_jobs);
        if (err != JPH::EPhysicsUpdateError::None && !I.warned_full) {
            I.warned_full = true;
            log_warn("Physics3D: too many bodies touching at once, so some contacts were skipped (objects may sink "
                     "into each other). Pass a bigger max_bodies to Physics3D.");
        }
        I.process_events();
    }
    for (size_t i = 0; i < I.contacts.size(); ++i) {
        if (I.contact_fn) I.contact_fn(I.contacts[i]);
    }
    for (size_t i = 0; i < I.triggers.size(); ++i) {
        if (I.trigger_fn) I.trigger_fn(I.triggers[i]);
    }
}

// --- one body --------------------------------------------------------------------------------

Transform Physics3D::transform(RigidBody body, vec3 scale) const {
    JPH::RVec3 p;
    JPH::Quat q;
    impl_->bodies().GetPositionAndRotation(to_id(body), p, q);
    return Transform{tv(p), tq(q), scale};
}

vec3 Physics3D::position(RigidBody body) const { return tv(impl_->bodies().GetPosition(to_id(body))); }
quat Physics3D::rotation(RigidBody body) const { return tq(impl_->bodies().GetRotation(to_id(body))); }

void Physics3D::set_position(RigidBody body, vec3 position) {
    impl_->bodies().SetPosition(to_id(body), JPH::RVec3(jv(position)), JPH::EActivation::Activate);
}

void Physics3D::set_rotation(RigidBody body, quat rotation) {
    impl_->bodies().SetRotation(to_id(body), jq(rotation), JPH::EActivation::Activate);
}

void Physics3D::move_kinematic(RigidBody body, vec3 position, quat rotation, float dt) {
    if (dt <= 0.0f) return;
    impl_->bodies().MoveKinematic(to_id(body), JPH::RVec3(jv(position)), jq(rotation), dt);
}

vec3 Physics3D::velocity(RigidBody body) const { return tv(impl_->bodies().GetLinearVelocity(to_id(body))); }
void Physics3D::set_velocity(RigidBody body, vec3 v) { impl_->bodies().SetLinearVelocity(to_id(body), jv(v)); }
vec3 Physics3D::angular_velocity(RigidBody body) const { return tv(impl_->bodies().GetAngularVelocity(to_id(body))); }
void Physics3D::set_angular_velocity(RigidBody body, vec3 v) { impl_->bodies().SetAngularVelocity(to_id(body), jv(v)); }
void Physics3D::apply_force(RigidBody body, vec3 force) { impl_->bodies().AddForce(to_id(body), jv(force)); }
void Physics3D::apply_torque(RigidBody body, vec3 torque) { impl_->bodies().AddTorque(to_id(body), jv(torque)); }
void Physics3D::apply_impulse(RigidBody body, vec3 impulse) { impl_->bodies().AddImpulse(to_id(body), jv(impulse)); }
void Physics3D::apply_impulse(RigidBody body, vec3 impulse, vec3 at_point) {
    impl_->bodies().AddImpulse(to_id(body), jv(impulse), JPH::RVec3(jv(at_point)));
}

float Physics3D::mass(RigidBody body) const {
    JPH::BodyLockRead lock(impl_->locks(), to_id(body));
    if (!lock.Succeeded() || lock.GetBody().IsStatic()) return 0.0f;
    const float inv = lock.GetBody().GetMotionProperties()->GetInverseMassUnchecked();
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

BodyType Physics3D::type(RigidBody body) const {
    if (impl_->static_triggers.count(raw(to_id(body)))) return BodyType::Static;
    switch (impl_->bodies().GetMotionType(to_id(body))) {
        case JPH::EMotionType::Dynamic: return BodyType::Dynamic;
        case JPH::EMotionType::Kinematic: return BodyType::Kinematic;
        default: return BodyType::Static;
    }
}

void Physics3D::set_type(RigidBody body, BodyType type) {
    Physics3DImpl& I = *impl_;
    const JPH::BodyID id = to_id(body);
    {
        JPH::BodyLockRead lock(I.locks(), id);
        if (!lock.Succeeded()) return;
        if (!lock.GetBody().CanBeKinematicOrDynamic()) {
            if (type != BodyType::Static) log_warn("Physics3D::set_type: a body added as Static can't start moving; add it as Kinematic instead");
            return;
        }
    }
    const JPH::EMotionType motion = type == BodyType::Dynamic   ? JPH::EMotionType::Dynamic
                                    : type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                  : JPH::EMotionType::Static;
    I.bodies().SetMotionType(id, motion, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    // The broad phase keeps statics apart from moving bodies, so the
    // object layer follows the motion type (triggers stay triggers).
    const JPH::ObjectLayer old_layer = I.bodies().GetObjectLayer(id);
    if ((old_layer & 3) != kTrigger) {
        I.bodies().SetObjectLayer(id, static_cast<JPH::ObjectLayer>((old_layer & ~3) | (motion == JPH::EMotionType::Static ? kStatic : kMoving)));
    }
}

bool Physics3D::sleeping(RigidBody body) const {
    JPH::BodyLockRead lock(impl_->locks(), to_id(body));
    return lock.Succeeded() && !lock.GetBody().IsStatic() && !lock.GetBody().IsActive();
}

void Physics3D::wake(RigidBody body) { impl_->bodies().ActivateBody(to_id(body)); }

Bounds Physics3D::bounds(RigidBody body) const {
    JPH::BodyLockRead lock(impl_->locks(), to_id(body));
    if (!lock.Succeeded()) return {};
    const JPH::AABox& b = lock.GetBody().GetWorldSpaceBounds();
    return Bounds{tv(b.mMin), tv(b.mMax)};
}

uint64_t Physics3D::user(RigidBody body) const { return impl_->bodies().GetUserData(to_id(body)); }

void Physics3D::set_collider(RigidBody body, const Collider& collider) {
    Physics3DImpl& I = *impl_;
    const JPH::BodyID id = to_id(body);
    float density = 0.0f;
    bool dynamic = false;
    {
        JPH::BodyLockRead lock(I.locks(), id);
        if (!lock.Succeeded()) return;
        const JPH::Body& b = lock.GetBody();
        dynamic = b.IsDynamic();
        if (dynamic) {
            const float inv = b.GetMotionProperties()->GetInverseMassUnchecked();
            const float volume = b.GetShape()->GetVolume();
            if (inv > 0.0f && volume > 0.0f) density = 1.0f / inv / volume;
        }
    }
    JPH::ShapeRefC shape = I.shape(collider, dynamic);
    if (!shape) {
        log_warn("Physics3D::set_collider: the collider is empty, so the body keeps its old shape");
        return;
    }
    I.bodies().SetShape(id, shape, false, JPH::EActivation::Activate);
    if (dynamic && density > 0.0f && shape->GetVolume() > 0.0f) {
        JPH::BodyLockWrite lock(I.system->GetBodyLockInterfaceNoLock(), id);
        if (lock.Succeeded()) {
            JPH::MassProperties mp = shape->GetMassProperties();
            mp.ScaleToMass(std::max(shape->GetVolume() * density, 1e-3f));
            lock.GetBody().GetMotionProperties()->SetMassProperties(JPH::EAllowedDOFs::All, mp);
        }
    }
}

// --- queries --------------------------------------------------------------------------------

namespace {

class SolidOnly final : public JPH::BodyFilter {
public:
    explicit SolidOnly(JPH::BodyID ignore) : ignore_(ignore) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != ignore_; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return !body.IsSensor(); }

private:
    JPH::BodyID ignore_;
};

} // namespace

PhysicsHit Physics3D::raycast(const Ray& ray, float max_distance, RigidBody ignore) const {
    PhysicsHit out;
    // Jolt measures along the direction vector, so give it the real length
    // (capped: a 1e30 m ray loses all float precision).
    const float len = std::min(max_distance, 1e5f);
    const vec3 dir = normalize(ray.direction);
    if (len <= 0.0f || length(dir) < 0.5f) return out;
    const JPH::RRayCast rc{JPH::RVec3(jv(ray.origin)), jv(dir * len)};
    JPH::RayCastResult hit;
    SolidOnly filter(to_id(ignore));
    if (!impl_->system->GetNarrowPhaseQueryNoLock().CastRay(rc, hit, {}, {}, filter)) return out;
    const JPH::RVec3 p = rc.GetPointOnRay(hit.mFraction);
    out.hit = true;
    out.distance = hit.mFraction * len;
    out.point = tv(p);
    out.body = to_body(hit.mBodyID);
    JPH::BodyLockRead lock(impl_->locks(), hit.mBodyID);
    if (lock.Succeeded()) {
        vec3 n = tv(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p));
        if (dot(n, dir) > 0.0f) n = -n;
        out.normal = n;
    }
    return out;
}

namespace {

std::vector<RigidBody> overlap(const JPH::PhysicsSystem& system, const JPH::Shape& shape, vec3 center) {
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    SolidOnly filter{JPH::BodyID()};
    system.GetNarrowPhaseQueryNoLock().CollideShape(&shape, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(jv(center))),
                                                    settings, JPH::RVec3::sZero(), collector, {}, {}, filter);
    std::vector<RigidBody> out;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const RigidBody b = to_body(r.mBodyID2);
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    return out;
}

} // namespace

std::vector<RigidBody> Physics3D::overlap_sphere(vec3 center, float radius) const {
    if (radius <= 0.0f) return {};
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded(); // on the stack: no reference counting
    return overlap(*impl_->system, sphere, center);
}

std::vector<RigidBody> Physics3D::overlap_box(const Bounds& box) const {
    if (!box.valid()) return {};
    const vec3 half = box.size() * 0.5f;
    JPH::BoxShape shape(JPH::Vec3::sMax(jv(half), JPH::Vec3::sReplicate(1e-4f)), 0.0f);
    shape.SetEmbedded();
    return overlap(*impl_->system, shape, box.center());
}

int Physics3D::explode(vec3 center, float radius, float speed) {
    if (radius <= 0.0f) return 0;
    Physics3DImpl& I = *impl_;
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    I.system->GetBroadPhaseQuery().CollideSphere(jv(center), radius, collector);
    struct Kick {
        JPH::BodyID id;
        JPH::Vec3 impulse;
        JPH::RVec3 at;
    };
    std::vector<Kick> kicks;
    for (const JPH::BodyID& id : collector.mHits) {
        JPH::BodyLockRead lock(I.locks(), id);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        if (!body.IsDynamic() || body.IsSensor()) continue;
        // Distance to the body's box rather than its middle, so a big wall
        // piece right next to the blast gets the full kick.
        const JPH::AABox& b = body.GetWorldSpaceBounds();
        const JPH::Vec3 c = jv(center);
        const JPH::Vec3 nearest = JPH::Vec3::sMin(JPH::Vec3::sMax(c, b.mMin), b.mMax);
        const float d = (nearest - c).Length();
        if (d > radius) continue;
        const JPH::Vec3 com(body.GetCenterOfMassPosition());
        JPH::Vec3 dir = com - c;
        dir = dir.Length() > 1e-4f ? dir.Normalized() : JPH::Vec3::sAxisY();
        const float inv_mass = body.GetMotionProperties()->GetInverseMassUnchecked();
        if (inv_mass <= 0.0f) continue;
        const float falloff = 1.0f - d / radius;
        // Pushed at the near side rather than the center of mass: off-center
        // pieces also get set spinning, which reads as a blast.
        kicks.push_back({id, dir * (speed * falloff / inv_mass), JPH::RVec3(nearest)});
    }
    for (const Kick& k : kicks) I.bodies().AddImpulse(k.id, k.impulse, k.at);
    return static_cast<int>(kicks.size());
}

// --- events & layers ---------------------------------------------------------------------------

const std::vector<Contact>& Physics3D::contacts() const { return impl_->contacts; }
const std::vector<TriggerEvent>& Physics3D::trigger_events() const { return impl_->triggers; }
void Physics3D::on_contact(std::function<void(const Contact&)> fn) { impl_->contact_fn = std::move(fn); }
void Physics3D::on_trigger(std::function<void(const TriggerEvent&)> fn) { impl_->trigger_fn = std::move(fn); }

void Physics3D::set_layers_collide(int a, int b, bool collide) {
    if (a < 0 || b < 0 || a >= kLayers || b >= kLayers) return;
    uint32_t* m = impl_->layer_matrix;
    if (collide) {
        m[a] |= 1u << b;
        m[b] |= 1u << a;
    } else {
        m[a] &= ~(1u << b);
        m[b] &= ~(1u << a);
    }
}

bool Physics3D::layers_collide(int a, int b) const {
    if (a < 0 || b < 0 || a >= kLayers || b >= kLayers) return false;
    return (impl_->layer_matrix[a] >> b) & 1u;
}

// --- debug drawing -------------------------------------------------------------------------------

namespace {

void circle(World& world, const JPH::RMat44& m, float y, float r, rgba color, bool on_top) {
    constexpr int n = 20;
    for (int i = 0; i < n; ++i) {
        const float a0 = 2.0f * pi * i / n, a1 = 2.0f * pi * (i + 1) / n;
        const JPH::RVec3 p0 = m * JPH::Vec3(std::cos(a0) * r, y, std::sin(a0) * r);
        const JPH::RVec3 p1 = m * JPH::Vec3(std::cos(a1) * r, y, std::sin(a1) * r);
        world.line(tv(p0), tv(p1), color, on_top);
    }
}

// `m` is the shape's center-of-mass transform, which is how Jolt nests
// shapes (see its own Shape::Draw implementations).
void draw_shape(World& world, const JPH::Shape* shape, const JPH::RMat44& m, rgba color, bool on_top) {
    switch (shape->GetSubType()) {
        case JPH::EShapeSubType::Box: {
            const auto* box = static_cast<const JPH::BoxShape*>(shape);
            world.wire_box(Transform{tv(m.GetTranslation()), tq(m.GetQuaternion()), tv(box->GetHalfExtent() * 2.0f)}, color, on_top);
            return;
        }
        case JPH::EShapeSubType::Sphere:
            world.wire_sphere(tv(m.GetTranslation()), static_cast<const JPH::SphereShape*>(shape)->GetRadius(), color, on_top);
            return;
        case JPH::EShapeSubType::Capsule: {
            const auto* cap = static_cast<const JPH::CapsuleShape*>(shape);
            const float h = cap->GetHalfHeightOfCylinder(), r = cap->GetRadius();
            world.wire_sphere(tv(m * JPH::Vec3(0, h, 0)), r, color, on_top);
            world.wire_sphere(tv(m * JPH::Vec3(0, -h, 0)), r, color, on_top);
            for (int i = 0; i < 4; ++i) {
                const float a = pi * 0.5f * i;
                const JPH::Vec3 side(std::cos(a) * r, 0, std::sin(a) * r);
                world.line(tv(m * (side + JPH::Vec3(0, h, 0))), tv(m * (side - JPH::Vec3(0, h, 0))), color, on_top);
            }
            return;
        }
        case JPH::EShapeSubType::Cylinder: {
            const auto* cyl = static_cast<const JPH::CylinderShape*>(shape);
            const float h = cyl->GetHalfHeight(), r = cyl->GetRadius();
            circle(world, m, h, r, color, on_top);
            circle(world, m, -h, r, color, on_top);
            for (int i = 0; i < 4; ++i) {
                const float a = pi * 0.5f * i;
                const JPH::Vec3 side(std::cos(a) * r, 0, std::sin(a) * r);
                world.line(tv(m * (side + JPH::Vec3(0, h, 0))), tv(m * (side - JPH::Vec3(0, h, 0))), color, on_top);
            }
            return;
        }
        case JPH::EShapeSubType::ConvexHull: {
            const auto* hull = static_cast<const JPH::ConvexHullShape*>(shape);
            std::vector<JPH::uint> idx;
            for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f) {
                idx.resize(hull->GetNumVerticesInFace(f));
                const JPH::uint n = hull->GetFaceVertices(f, static_cast<JPH::uint>(idx.size()), idx.data());
                for (JPH::uint i = 0; i < n; ++i) {
                    world.line(tv(m * hull->GetPoint(idx[i])), tv(m * hull->GetPoint(idx[(i + 1) % n])), color, on_top);
                }
            }
            return;
        }
        case JPH::EShapeSubType::RotatedTranslated: {
            const auto* rt = static_cast<const JPH::RotatedTranslatedShape*>(shape);
            draw_shape(world, rt->GetInnerShape(), m * JPH::Mat44::sRotation(rt->GetRotation()), color, on_top);
            return;
        }
        case JPH::EShapeSubType::StaticCompound:
        case JPH::EShapeSubType::MutableCompound: {
            const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
            for (const JPH::CompoundShape::SubShape& sub : compound->GetSubShapes()) {
                draw_shape(world, sub.mShape, m * sub.GetLocalTransformNoScale(JPH::Vec3::sOne()), color, on_top);
            }
            return;
        }
        default: {
            // Triangle meshes: just their bounds (drawing every triangle of
            // a level would bury everything else).
            const JPH::AABox b = shape->GetWorldSpaceBounds(m, JPH::Vec3::sOne());
            world.wire_box(Bounds{tv(b.mMin), tv(b.mMax)}, color, on_top);
            return;
        }
    }
}

} // namespace

void Physics3D::draw_debug(World& world, bool on_top) const {
    JPH::BodyIDVector ids;
    impl_->system->GetBodies(ids);
    for (const JPH::BodyID& id : ids) {
        JPH::BodyLockRead lock(impl_->locks(), id);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        const rgba color = body.IsSensor()   ? rgb(1.0f, 0.85f, 0.2f)
                           : body.IsStatic() ? rgb(0.35f, 0.55f, 1.0f)
                           : body.IsActive() ? rgb(0.3f, 1.0f, 0.4f)
                                             : rgb(0.55f, 0.55f, 0.55f);
        draw_shape(world, body.GetShape(), body.GetCenterOfMassTransform(), color, on_top);
    }
}

} // namespace thistle::three
