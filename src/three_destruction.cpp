// three::VoxelDestruction: carving a block world, finding what that left
// unsupported, and turning it into falling debris (a small VoxelWorld per
// piece, moved by a Physics3D body with a Collider::voxels shape).
//
// Support is connectivity: after a carve, every solid block next to a
// removed one starts a search through solid neighbors. Reaching the ground
// (or an anchor block, or more than max_piece blocks) means the whole group
// is held up; finishing without that means it's loose. The search goes
// downward first, so the common case, a wall still standing on the floor,
// ends after a few dozen blocks instead of exploring the whole building.
#include "thistle_internal.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace thistle::three {

namespace {

uint64_t pack(ivec3 p) {
    constexpr int64_t bias = 1 << 20;
    return (static_cast<uint64_t>(p.x + bias) << 42) | (static_cast<uint64_t>(p.y + bias) << 21) | static_cast<uint64_t>(p.z + bias);
}

// Tried in reverse order (it's a stack): down first, up last.
constexpr ivec3 kNeighbors[6] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};

vec3 fvec(ivec3 p) { return {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)}; }

// A sphere in world space, as seen from inside a (possibly rotated) grid.
struct LocalSphere {
    vec3 center;  // block units
    float radius; // block units
};
LocalSphere to_local(const VoxelWorld& w, vec3 center, float radius) {
    return {(w.rotation.inverse() * (center - w.origin)) / w.voxel_size, radius / w.voxel_size};
}

struct Removed {
    ivec3 block;
    BlockId id;
};

// Clears every block whose center is inside the sphere; returns them.
std::vector<Removed> carve_blocks(VoxelWorld& w, const LocalSphere& s) {
    std::vector<Removed> out;
    const float r2 = s.radius * s.radius;
    const ivec3 lo{static_cast<int>(std::floor(s.center.x - s.radius)), static_cast<int>(std::floor(s.center.y - s.radius)),
                   static_cast<int>(std::floor(s.center.z - s.radius))};
    const ivec3 hi{static_cast<int>(std::floor(s.center.x + s.radius)), static_cast<int>(std::floor(s.center.y + s.radius)),
                   static_cast<int>(std::floor(s.center.z + s.radius))};
    for (int z = lo.z; z <= hi.z; ++z) {
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const vec3 d = vec3{x + 0.5f, y + 0.5f, z + 0.5f} - s.center;
                if (dot(d, d) > r2) continue;
                const BlockId id = w.get(x, y, z);
                if (id == 0) continue;
                out.push_back({{x, y, z}, id});
                w.set(x, y, z, 0);
            }
        }
    }
    return out;
}

Texture chip_texture() {
    // Square chips read as bits of block; the default particle is a soft dot (dust).
    static Texture tex = [] {
        const unsigned char white[4 * 4 * 4] = {
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
        return make_texture(4, 4, white);
    }();
    return tex;
}

} // namespace

struct VoxelDestructionImpl {
    struct Debris {
        VoxelWorld voxels;
        RigidBody body;
    };

    VoxelWorld* world;
    Physics3D* physics;
    std::vector<std::unique_ptr<Debris>> debris; // oldest first

    bool is_anchor(const VoxelDestruction& d, ivec3 p, BlockId id) const {
        return p.y <= d.ground_y || std::find(d.anchors.begin(), d.anchors.end(), id) != d.anchors.end();
    }

    static bool solid(const VoxelWorld& w, BlockId id) { return id != 0 && w.block_type(id).solid; }

    void sync(Debris& piece) const {
        const Transform t = physics->transform(piece.body);
        piece.voxels.origin = t.position;
        piece.voxels.rotation = t.rotation;
    }

    void emit_chips(VoxelDestruction& d, const VoxelWorld& w, const std::vector<Removed>& removed, int budget, vec3 kick = {}) {
        if (removed.empty() || d.chips.max_particles == 0) return;
        const int step = std::max(1, static_cast<int>(removed.size()) / std::max(1, budget));
        const float s = w.voxel_size;
        for (size_t i = 0; i < removed.size(); i += step) {
            const BlockType& type = w.block_type(removed[i].id);
            ParticleSettings look;
            look.texture = chip_texture();
            look.start_color = type.color;
            look.start_color.a = 1.0f;
            look.end_color = look.start_color;
            look.end_color.a = 0.0f;
            look.start_size = s * 0.7f;
            look.end_size = s * 0.35f;
            look.lifetime = 1.4f;
            look.lifetime_jitter = 0.4f;
            look.velocity = kick + vec3{0.0f, 2.0f, 0.0f};
            look.spread = 4.5f;
            look.gravity = {0.0f, -9.81f, 0.0f};
            look.drag = 0.4f;
            look.spin = 8.0f;
            d.chips.emit(w.block_center(removed[i].block), 1, look);
        }
    }

    // All blocks of `w` connected (through solid blocks) to `start`.
    // Stops early and returns false as soon as it finds support, if `d`.
    bool flood(const VoxelWorld& w, ivec3 start, const VoxelDestruction* d, std::vector<ivec3>& out,
               std::unordered_set<uint64_t>& seen) const {
        std::vector<ivec3> stack{start};
        seen.insert(pack(start));
        while (!stack.empty()) {
            const ivec3 p = stack.back();
            stack.pop_back();
            out.push_back(p);
            if (d && (is_anchor(*d, p, w.get(p)) || static_cast<int>(out.size()) > d->max_piece)) {
                // Everything queued is connected to this too.
                out.insert(out.end(), stack.begin(), stack.end());
                return false;
            }
            for (const ivec3& n : kNeighbors) {
                const ivec3 q = p + n;
                if (!solid(w, w.get(q)) || !seen.insert(pack(q)).second) continue;
                stack.push_back(q);
            }
        }
        return true;
    }

    // A new piece of debris from blocks of `from` (which lose them). Blocks
    // keep their coordinates; the piece's grid sits where `from`'s does.
    void spawn(VoxelDestruction& d, VoxelWorld& from, const std::vector<ivec3>& blocks, vec3 velocity) {
        auto piece = std::make_unique<Debris>();
        VoxelWorld& v = piece->voxels;
        v.copy_block_types(from);
        v.voxel_size = from.voxel_size;
        v.ambient_occlusion = from.ambient_occlusion;
        v.origin = from.origin;
        v.rotation = from.rotation;
        v.max_remesh_per_frame = 0;
        int solid_blocks = 0;
        for (const ivec3& p : blocks) {
            const BlockId id = from.get(p);
            v.set(p, id);
            from.set(p, 0);
            solid_blocks += solid(v, id);
        }
        const float block_volume = v.voxel_size * v.voxel_size * v.voxel_size;
        piece->body = physics->add(BodySettings{.collider = Collider::voxels(v), .position = v.origin, .rotation = v.rotation,
                                                .velocity = velocity, .mass = std::max(1, solid_blocks) * block_volume * d.density,
                                                .friction = 0.7f});
        if (!piece->body) return; // (physics off: the blocks are gone from `from`, but nothing to show them)
        debris.push_back(std::move(piece));
    }

    void crumble(VoxelDestruction& d, VoxelWorld& from, const std::vector<ivec3>& blocks) {
        std::vector<Removed> removed;
        removed.reserve(blocks.size());
        for (const ivec3& p : blocks) {
            removed.push_back({p, from.get(p)});
            from.set(p, 0);
        }
        emit_chips(d, from, removed, static_cast<int>(removed.size()));
    }

    // After carving the level: let go of whatever lost its support.
    void release_loose(VoxelDestruction& d, const std::vector<Removed>& removed) {
        VoxelWorld& w = *world;
        std::unordered_set<uint64_t> held, loose;
        for (const Removed& r : removed) {
            for (const ivec3& n : kNeighbors) {
                const ivec3 seed = r.block + n;
                const uint64_t key = pack(seed);
                if (!solid(w, w.get(seed)) || held.count(key) || loose.count(key)) continue;
                std::vector<ivec3> group;
                std::unordered_set<uint64_t> seen;
                if (!flood(w, seed, &d, group, seen)) {
                    held.insert(seen.begin(), seen.end());
                    continue;
                }
                loose.insert(seen.begin(), seen.end());
                if (static_cast<int>(group.size()) < d.min_piece) crumble(d, w, group);
                else spawn(d, w, group, {});
            }
        }
    }

    // After carving a piece of debris: it may have split in two (or more).
    void resplit(VoxelDestruction& d, size_t index) {
        Debris& piece = *debris[index];
        VoxelWorld& v = piece.voxels;
        std::vector<ivec3> blocks;
        v.each_block([&](ivec3 p, BlockId id) { if (solid(v, id)) blocks.push_back(p); });
        std::unordered_set<uint64_t> seen;
        std::vector<std::vector<ivec3>> groups;
        for (const ivec3& p : blocks) {
            if (seen.count(pack(p))) continue;
            groups.emplace_back();
            flood(v, p, nullptr, groups.back(), seen);
        }
        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
        const vec3 velocity = physics->velocity(piece.body);
        // Everything but the biggest group leaves this piece.
        for (size_t g = 1; g < groups.size(); ++g) {
            if (static_cast<int>(groups[g].size()) < d.min_piece) crumble(d, v, groups[g]);
            else spawn(d, v, groups[g], velocity); // may reallocate `debris`: don't touch `piece` via index after
        }
        Debris& kept = *debris[index];
        // Non-solid leftovers (a plant on the piece) aren't connected to anything: drop them too.
        std::vector<ivec3> stray;
        kept.voxels.each_block([&](ivec3 p, BlockId id) { if (!solid(kept.voxels, id)) stray.push_back(p); });
        for (const ivec3& p : stray) kept.voxels.set(p, 0);
        if (groups.empty() || static_cast<int>(groups[0].size()) < d.min_piece) {
            if (!groups.empty()) crumble(d, kept.voxels, groups[0]);
            physics->remove(kept.body);
            debris.erase(debris.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
        physics->set_collider(kept.body, Collider::voxels(kept.voxels));
    }

    void drop(VoxelDestruction& d, size_t index, bool chips) {
        Debris& piece = *debris[index];
        if (chips) {
            sync(piece);
            std::vector<Removed> all;
            piece.voxels.each_block([&](ivec3 p, BlockId id) { all.push_back({p, id}); });
            emit_chips(d, piece.voxels, all, 40);
        }
        physics->remove(piece.body);
        debris.erase(debris.begin() + static_cast<std::ptrdiff_t>(index));
    }
};

VoxelDestruction::VoxelDestruction(VoxelWorld& voxels, Physics3D& physics) : impl_(std::make_unique<VoxelDestructionImpl>()) {
    impl_->world = &voxels;
    impl_->physics = &physics;
    chips.max_particles = 6000;
    physics.add_static(voxels);
}

VoxelDestruction::~VoxelDestruction() {
    for (auto& piece : impl_->debris) impl_->physics->remove(piece->body);
}

int VoxelDestruction::carve(vec3 center, float radius) {
    VoxelDestructionImpl& I = *impl_;
    if (radius <= 0.0f) return 0;
    VoxelWorld& w = *I.world;
    const std::vector<Removed> removed = carve_blocks(w, to_local(w, center, radius));
    int count = static_cast<int>(removed.size());
    I.emit_chips(*this, w, removed, 120);
    if (physics3d_available()) I.release_loose(*this, removed);

    // Debris in reach. Walk backwards: resplit() can remove the piece (and
    // appends new ones, which were just carved and don't need it again).
    for (size_t i = I.debris.size(); i-- > 0;) {
        VoxelDestructionImpl::Debris& piece = *I.debris[i];
        const Bounds b = I.physics->bounds(piece.body);
        const vec3 nearest{std::clamp(center.x, b.min.x, b.max.x), std::clamp(center.y, b.min.y, b.max.y),
                           std::clamp(center.z, b.min.z, b.max.z)};
        if (length(nearest - center) > radius) continue;
        I.sync(piece);
        const std::vector<Removed> bits = carve_blocks(piece.voxels, to_local(piece.voxels, center, radius));
        if (bits.empty()) continue;
        count += static_cast<int>(bits.size());
        I.emit_chips(*this, piece.voxels, bits, 40);
        I.resplit(*this, i);
    }
    while (static_cast<int>(I.debris.size()) > std::max(0, max_debris)) I.drop(*this, 0, true);
    return count;
}

int VoxelDestruction::explode(vec3 center, float radius, float speed) {
    const int n = carve(center, radius);
    impl_->physics->explode(center, radius * 2.0f, speed);
    return n;
}

void VoxelDestruction::update(float dt) {
    VoxelDestructionImpl& I = *impl_;
    chips.update(dt);
    // Debris that fell out of the world (off an edge, through a hole in a
    // floating level) is gone for good. Bounds, not position: a piece's
    // body origin is its grid's origin, which can be far from its blocks.
    const VoxelWorld& w = *I.world;
    const float floor = (w.origin + w.rotation * vec3{0.0f, ground_y * w.voxel_size, 0.0f}).y - 100.0f;
    for (size_t i = I.debris.size(); i-- > 0;) {
        if (I.physics->bounds(I.debris[i]->body).max.y < floor) I.drop(*this, i, false);
    }
}

void VoxelDestruction::draw(World& world) {
    VoxelDestructionImpl& I = *impl_;
    world.draw(*I.world);
    for (auto& piece : I.debris) {
        I.sync(*piece);
        world.draw(piece->voxels);
    }
    world.draw(chips);
}

int VoxelDestruction::debris_count() const { return static_cast<int>(impl_->debris.size()); }

RigidBody VoxelDestruction::debris_body(int index) const {
    if (index < 0 || index >= debris_count()) return {};
    return impl_->debris[static_cast<size_t>(index)]->body;
}

const VoxelWorld& VoxelDestruction::debris_blocks(int index) const {
    static const VoxelWorld none;
    if (index < 0 || index >= debris_count()) return none;
    return impl_->debris[static_cast<size_t>(index)]->voxels;
}

bool VoxelDestruction::is_debris(RigidBody body) const {
    if (!body) return false;
    for (const auto& piece : impl_->debris) {
        if (piece->body == body) return true;
    }
    return false;
}

} // namespace thistle::three
