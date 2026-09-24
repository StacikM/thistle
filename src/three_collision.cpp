#include "thistle_internal.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace thistle::three {

namespace {
float comp(const vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
float& comp(vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
int icomp(const ivec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
int& icomp(ivec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
int ifloor(float v) { return static_cast<int>(std::floor(v)); }
} // namespace

// --- voxel raycast --------------------------------------------------------------------

VoxelWorld::Hit VoxelWorld::raycast(const Ray& ray, float max_distance, bool solid_only) const {
    // Amanatides & Woo: step from cell to cell across whichever grid plane
    // the ray reaches next, so no block is ever skipped or visited twice.
    Hit hit;
    const vec3 dir = normalize(ray.direction);
    if (dir == vec3{0, 0, 0} || voxel_size <= 0.0f) return hit;
    const vec3 o = (ray.origin - origin) / voxel_size;
    const float max_t = max_distance / voxel_size;
    ivec3 cell{ifloor(o.x), ifloor(o.y), ifloor(o.z)};
    ivec3 step;
    vec3 t_max, t_delta;
    for (int a = 0; a < 3; ++a) {
        const float d = comp(dir, a);
        const float p = comp(o, a);
        icomp(step, a) = d > 0.0f ? 1 : (d < 0.0f ? -1 : 0);
        if (d == 0.0f) {
            comp(t_max, a) = no_limit;
            comp(t_delta, a) = no_limit;
        } else {
            const float boundary = d > 0.0f ? std::floor(p) + 1.0f : std::floor(p);
            comp(t_max, a) = (boundary - p) / d;
            comp(t_delta, a) = std::fabs(1.0f / d);
        }
    }
    ivec3 normal{0, 0, 0};
    float t = 0.0f;
    for (int guard = 0; guard < 100000 && t <= max_t; ++guard) {
        const BlockId id = get(cell);
        if (id != 0 && (!solid_only || block_type(id).solid)) {
            hit.hit = true;
            hit.block = cell;
            hit.normal = normal;
            hit.id = id;
            hit.distance = t * voxel_size;
            hit.point = ray.origin + dir * hit.distance;
            return hit;
        }
        int a = 0;
        if (t_max.y < comp(t_max, a)) a = 1;
        if (t_max.z < comp(t_max, a)) a = 2;
        t = comp(t_max, a);
        icomp(cell, a) += icomp(step, a);
        comp(t_max, a) += comp(t_delta, a);
        normal = ivec3{0, 0, 0};
        icomp(normal, a) = -icomp(step, a);
    }
    return hit;
}

bool VoxelWorld::overlaps_solid(const Bounds& box) const {
    if (!box.valid()) return false;
    constexpr float eps = 1e-4f;
    const vec3 lo = (box.min - origin) / voxel_size, hi = (box.max - origin) / voxel_size;
    for (int z = ifloor(lo.z + eps); z <= ifloor(hi.z - eps); ++z)
        for (int y = ifloor(lo.y + eps); y <= ifloor(hi.y - eps); ++y)
            for (int x = ifloor(lo.x + eps); x <= ifloor(hi.x - eps); ++x) {
                const BlockId id = get(x, y, z);
                if (id != 0 && block_type(id).solid) return true;
            }
    return false;
}

// --- collision world --------------------------------------------------------------------

namespace {

// Box-vs-triangle separating-axis test (Akenine-Moller): the box's 3 axes,
// the triangle's normal, and the 9 cross products of their edges.
bool box_triangle_overlap(vec3 center, vec3 half, vec3 a, vec3 b, vec3 c) {
    const vec3 v0 = a - center, v1 = b - center, v2 = c - center;
    const vec3 e0 = v1 - v0, e1 = v2 - v1, e2 = v0 - v2;
    auto axis_test = [&](vec3 axis) {
        const float p0 = dot(v0, axis), p1 = dot(v1, axis), p2 = dot(v2, axis);
        const float r = half.x * std::fabs(axis.x) + half.y * std::fabs(axis.y) + half.z * std::fabs(axis.z);
        return !(std::fmin(p0, std::fmin(p1, p2)) > r || std::fmax(p0, std::fmax(p1, p2)) < -r);
    };
    const vec3 box_axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const vec3& e : {e0, e1, e2})
        for (const vec3& ax : box_axes)
            if (!axis_test(cross(ax, e))) return false;
    for (int i = 0; i < 3; ++i) {
        const float lo = std::fmin(comp(v0, i), std::fmin(comp(v1, i), comp(v2, i)));
        const float hi = std::fmax(comp(v0, i), std::fmax(comp(v1, i), comp(v2, i)));
        if (lo > comp(half, i) || hi < -comp(half, i)) return false;
    }
    return axis_test(cross(e0, e1));
}


constexpr float kCell = 2.0f; // broadphase grid cell for mesh triangles, in meters

int64_t cell_key(int x, int y, int z) {
    constexpr int64_t bias = 1 << 20;
    return ((static_cast<int64_t>(x) + bias) << 42) | ((static_cast<int64_t>(y) + bias) << 21) | (static_cast<int64_t>(z) + bias);
}

} // namespace

struct CollisionWorldImpl {
    struct Voxels { int id; const VoxelWorld* world; };
    struct Mesh {
        int id;
        std::vector<vec3> tris; // 3 per triangle
        Bounds bounds;
        std::unordered_map<int64_t, std::vector<uint32_t>> grid; // cell -> triangle indices
        mutable std::vector<uint32_t> stamp; // per-triangle "already tested this query"
        mutable uint32_t query = 0;
    };
    struct Box { int id; Bounds box; };
    std::vector<Voxels> voxels;
    std::vector<Mesh> meshes;
    std::vector<Box> boxes;
    int next_id = 1;

    template <typename F> void each_triangle_near(const Mesh& m, const Bounds& region, F&& fn) const {
        if (!m.bounds.overlaps(region)) return;
        ++m.query;
        for (int z = ifloor(region.min.z / kCell); z <= ifloor(region.max.z / kCell); ++z)
            for (int y = ifloor(region.min.y / kCell); y <= ifloor(region.max.y / kCell); ++y)
                for (int x = ifloor(region.min.x / kCell); x <= ifloor(region.max.x / kCell); ++x) {
                    auto it = m.grid.find(cell_key(x, y, z));
                    if (it == m.grid.end()) continue;
                    for (uint32_t t : it->second) {
                        if (m.stamp[t] == m.query) continue;
                        m.stamp[t] = m.query;
                        if (fn(m.tris[t * 3], m.tris[t * 3 + 1], m.tris[t * 3 + 2])) return;
                        // (fn takes const vec3& — the address of its first argument
                        // is how callers tell which triangle this is.)
                    }
                }
    }

    // Triangles are identified as (mesh index << 32 | triangle index).
    bool mesh_overlaps(const Bounds& box, const std::vector<uint64_t>* ignore = nullptr) const {
        const vec3 center = box.center(), half = box.size() * 0.5f;
        bool found = false;
        for (size_t mi = 0; mi < meshes.size() && !found; ++mi) {
            const Mesh& m = meshes[mi];
            each_triangle_near(m, box, [&](const vec3& a, const vec3& b, const vec3& c) {
                if (!box_triangle_overlap(center, half, a, b, c)) return false;
                if (ignore) {
                    const uint32_t t = static_cast<uint32_t>(&a - m.tris.data()) / 3;
                    if (std::binary_search(ignore->begin(), ignore->end(), (static_cast<uint64_t>(mi) << 32) | t)) return false;
                }
                found = true;
                return true;
            });
        }
        return found;
    }

    std::vector<uint64_t> overlapping_triangles(const Bounds& box) const {
        std::vector<uint64_t> out;
        const vec3 center = box.center(), half = box.size() * 0.5f;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            const Mesh& m = meshes[mi];
            each_triangle_near(m, box, [&](const vec3& a, const vec3& b, const vec3& c) {
                if (box_triangle_overlap(center, half, a, b, c)) {
                    out.push_back((static_cast<uint64_t>(mi) << 32) | (static_cast<uint32_t>(&a - m.tris.data()) / 3));
                }
                return false;
            });
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

CollisionWorld::CollisionWorld() : impl_(std::make_unique<CollisionWorldImpl>()) {}
CollisionWorld::~CollisionWorld() = default;
CollisionWorld::CollisionWorld(CollisionWorld&&) noexcept = default;
CollisionWorld& CollisionWorld::operator=(CollisionWorld&&) noexcept = default;

int CollisionWorld::add(const VoxelWorld& voxels) {
    const int id = impl_->next_id++;
    impl_->voxels.push_back({id, &voxels});
    return id;
}

int CollisionWorld::add(Model model, const Transform& transform) {
    CollisionWorldImpl::Mesh m;
    m.id = impl_->next_id++;
    detail::model_world_triangles(model, transform.matrix(), m.tris);
    const uint32_t count = static_cast<uint32_t>(m.tris.size() / 3);
    m.stamp.assign(count, 0);
    for (uint32_t t = 0; t < count; ++t) {
        Bounds tb;
        for (int k = 0; k < 3; ++k) { tb.add(m.tris[t * 3 + k]); m.bounds.add(m.tris[t * 3 + k]); }
        for (int z = ifloor(tb.min.z / kCell); z <= ifloor(tb.max.z / kCell); ++z)
            for (int y = ifloor(tb.min.y / kCell); y <= ifloor(tb.max.y / kCell); ++y)
                for (int x = ifloor(tb.min.x / kCell); x <= ifloor(tb.max.x / kCell); ++x)
                    m.grid[cell_key(x, y, z)].push_back(t);
    }
    if (count == 0) log_warn("CollisionWorld::add: model has no triangles");
    impl_->meshes.push_back(std::move(m));
    return impl_->meshes.back().id;
}

int CollisionWorld::add_box(const Bounds& box) {
    const int id = impl_->next_id++;
    impl_->boxes.push_back({id, box});
    return id;
}

void CollisionWorld::remove(int id) {
    auto drop = [id](auto& v) { v.erase(std::remove_if(v.begin(), v.end(), [id](const auto& e) { return e.id == id; }), v.end()); };
    drop(impl_->voxels);
    drop(impl_->meshes);
    drop(impl_->boxes);
}

void CollisionWorld::clear() {
    impl_->voxels.clear();
    impl_->meshes.clear();
    impl_->boxes.clear();
}

bool CollisionWorld::overlaps(const Bounds& box) const {
    for (const auto& v : impl_->voxels) if (v.world->overlaps_solid(box)) return true;
    for (const auto& b : impl_->boxes) if (b.box.overlaps(box)) return true;
    return impl_->mesh_overlaps(box);
}

RaycastHit CollisionWorld::raycast(const Ray& ray, float max_distance) const {
    RaycastHit best;
    auto consider = [&](const RaycastHit& h) {
        if (h.hit && (!best.hit || h.distance < best.distance)) best = h;
    };
    for (const auto& v : impl_->voxels) {
        const VoxelWorld::Hit h = v.world->raycast(ray, max_distance, true);
        if (h) {
            RaycastHit r;
            r.hit = true;
            r.distance = h.distance;
            r.point = h.point;
            r.normal = {static_cast<float>(h.normal.x), static_cast<float>(h.normal.y), static_cast<float>(h.normal.z)};
            consider(r);
        }
    }
    for (const auto& b : impl_->boxes) consider(three::raycast(ray, b.box, max_distance));
    for (const auto& m : impl_->meshes) {
        if (!detail::ray_reaches_box(ray, m.bounds, best.hit ? best.distance : max_distance)) continue;
        for (size_t t = 0; t + 2 < m.tris.size(); t += 3) {
            consider(raycast_triangle(ray, m.tris[t], m.tris[t + 1], m.tris[t + 2], best.hit ? best.distance : max_distance));
        }
    }
    return best;
}

// --- character controller ----------------------------------------------------------------

namespace {

constexpr float kSkin = 1e-3f; // stop this far short of surfaces so float error never leaves us touching

// How far a box can move along one axis (up to `want`, signed) before
// something solid blocks it.
float sweep_axis(const CollisionWorldImpl& w, const Bounds& box, int axis, float want) {
    if (want == 0.0f) return 0.0f;
    const float dir = want > 0.0f ? 1.0f : -1.0f;
    float allowed = want;

    // Voxels and boxes: exact — find the nearest face in the way.
    for (const auto& v : w.voxels) {
        const VoxelWorld& vw = *v.world;
        const float s = vw.voxel_size;
        const vec3 lo = (box.min - vw.origin) / s, hi = (box.max - vw.origin) / s;
        const float lead = dir > 0.0f ? comp(hi, axis) : comp(lo, axis); // the face moving forward
        const float reach = lead + allowed / s;
        const int start = dir > 0.0f ? ifloor(lead + 1e-4f) : ifloor(lead - 1e-4f);
        const int end = dir > 0.0f ? ifloor(reach - 1e-4f) : ifloor(reach + 1e-4f);
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        for (int c = start; dir > 0.0f ? c <= end : c >= end; c += static_cast<int>(dir)) {
            bool blocked = false;
            for (int j = ifloor(comp(lo, a2) + 1e-4f); j <= ifloor(comp(hi, a2) - 1e-4f) && !blocked; ++j) {
                for (int i = ifloor(comp(lo, a1) + 1e-4f); i <= ifloor(comp(hi, a1) - 1e-4f) && !blocked; ++i) {
                    ivec3 cell;
                    icomp(cell, axis) = c;
                    icomp(cell, a1) = i;
                    icomp(cell, a2) = j;
                    const BlockId id = vw.get(cell);
                    blocked = id != 0 && vw.block_type(id).solid;
                }
            }
            if (blocked) {
                const float face = dir > 0.0f ? static_cast<float>(c) : static_cast<float>(c + 1);
                const float limit = (face - lead) * s - dir * kSkin;
                if (std::fabs(limit) < std::fabs(allowed)) allowed = (limit * dir > 0.0f) ? limit : 0.0f;
                break;
            }
        }
    }
    for (const auto& b : w.boxes) {
        const Bounds& o = b.box;
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        const bool side_overlap = comp(box.min, a1) < comp(o.max, a1) && comp(box.max, a1) > comp(o.min, a1) &&
                                  comp(box.min, a2) < comp(o.max, a2) && comp(box.max, a2) > comp(o.min, a2);
        if (!side_overlap) continue;
        const float gap = dir > 0.0f ? comp(o.min, axis) - comp(box.max, axis) : comp(o.max, axis) - comp(box.min, axis);
        if (gap * dir < -kSkin) continue; // it's behind us
        const float limit = gap - dir * kSkin;
        if (std::fabs(limit) < std::fabs(allowed)) allowed = (limit * dir > 0.0f) ? limit : 0.0f;
    }

    // Meshes: no exact sweep for boxes against arbitrary triangles, so binary
    // search the largest free fraction of the move. Sub-millimeter after
    // 12 halvings of any step a character takes in one frame.
    if (!w.meshes.empty() && allowed != 0.0f) {
        // The box is shrunk slightly on the two axes it isn't moving along,
        // so resting on a floor doesn't count as touching it while walking —
        // otherwise every horizontal step would start "inside" the floor.
        auto moved = [&](float d) {
            Bounds b = box;
            comp(b.min, axis) += d;
            comp(b.max, axis) += d;
            for (int other = 0; other < 3; ++other) {
                if (other == axis) continue;
                comp(b.min, other) += 2.0f * kSkin;
                comp(b.max, other) -= 2.0f * kSkin;
            }
            return b;
        };
        // Triangles the box is already inside (spawned in a wall, pushed by
        // something) are ignored so it can move out of them — but only
        // those: everything else still blocks.
        const std::vector<uint64_t> stuck_in = w.overlapping_triangles(moved(0.0f));
        if (w.mesh_overlaps(moved(allowed), &stuck_in)) {
            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 12; ++i) {
                const float mid = (lo + hi) * 0.5f;
                if (w.mesh_overlaps(moved(allowed * mid), &stuck_in)) hi = mid; else lo = mid;
            }
            allowed *= lo;
            allowed = std::fabs(allowed) > kSkin ? allowed - dir * kSkin : 0.0f; // stop just short
        }
    }
    return allowed;
}

} // namespace

vec3 CharacterController::move(const CollisionWorld& world, vec3 delta) {
    vec3 moved{0, 0, 0};
    // Vertical first, then the two horizontal axes: the standard order for
    // box characters, so landing on a floor never eats horizontal speed.
    for (int axis : {1, 0, 2}) {
        const float got = sweep_axis(*world.impl_, bounds(), axis, comp(delta, axis));
        comp(position, axis) += got;
        comp(moved, axis) = got;
    }
    return moved;
}

void CharacterController::update(const CollisionWorld& world, vec3 wish, bool jump, float dt) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 0.1f); // a hitch shouldn't launch us through the floor
    since_ground_ += dt;
    since_jump_press_ += dt;
    if (jump) since_jump_press_ = 0.0f;

    vec3 flat{wish.x, 0.0f, wish.z};
    if (length(flat) > 1.0f) flat = normalize(flat);
    const vec3 target = flat * move_speed;
    const float accel = acceleration * (grounded_ ? 1.0f : air_control) * dt;
    vec3 dv{target.x - velocity.x, 0.0f, target.z - velocity.z};
    const float dv_len = length(dv);
    if (dv_len > accel) dv = dv * (accel / dv_len);
    velocity.x += dv.x;
    velocity.z += dv.z;
    velocity.y -= gravity * dt;

    if (since_jump_press_ <= jump_buffer && since_ground_ <= coyote_time) {
        velocity.y = jump_speed;
        since_jump_press_ = since_ground_ = 1e9f;
        grounded_ = false;
    }

    const vec3 delta = velocity * dt;
    const bool was_grounded = grounded_;

    // Vertical.
    const float dy = sweep_axis(*world.impl_, bounds(), 1, delta.y);
    position.y += dy;
    if (delta.y < 0.0f && dy > delta.y + 1e-6f) {
        grounded_ = true;
        since_ground_ = 0.0f;
        velocity.y = 0.0f;
    } else {
        grounded_ = false;
        if (delta.y > 0.0f && dy < delta.y - 1e-6f) velocity.y = 0.0f; // head hit a ceiling
    }

    // Horizontal, trying a step-up if a wall stops us while walking.
    const vec3 start = position;
    const vec3 flat_delta{delta.x, 0.0f, delta.z};
    const vec3 plain = move(world, flat_delta);
    const float want = length(flat_delta);
    if ((was_grounded || grounded_) && step_height > 0.0f && length(plain) + 1e-4f < want) {
        const vec3 plain_end = position;
        position = start;
        const float up = sweep_axis(*world.impl_, bounds(), 1, step_height);
        position.y += up;
        const vec3 stepped = move(world, flat_delta);
        const float down = sweep_axis(*world.impl_, bounds(), 1, -(up + 0.05f));
        position.y += down;
        bool landed_on_step = down > -(up + 0.05f) + 1e-6f;
        if (landed_on_step) {
            // Only a flat-enough landing counts: without this, stepping up a
            // little every frame climbs any slope short of vertical. Probe
            // where the box actually rests (its corners, not just its
            // center, which can still be over the floor below the slope).
            // On a steep face the box rests on its very edge, so the probes
            // sit at the edges and corners (5 mm in), plus the middle.
            bool flat_support = false;
            const float in = radius - 0.005f;
            const vec3 probes[9] = {{0, 0, 0},   {in, 0, in}, {-in, 0, in}, {in, 0, -in}, {-in, 0, -in},
                                    {in, 0, 0},  {-in, 0, 0}, {0, 0, in},   {0, 0, -in}};
            // Probes start above the step so none begins inside a steep
            // solid (a ray from inside hits the far side and reads as flat);
            // only hits right at foot level count as what we're resting on.
            const float top = up + 0.05f;
            for (const vec3& o : probes) {
                const RaycastHit g = world.raycast(Ray{position + o + vec3{0.0f, top, 0.0f}, {0.0f, -1.0f, 0.0f}}, top + 0.08f);
                if (g && std::fabs(g.point.y - position.y) <= 0.06f && g.normal.y >= std::cos(max_slope)) flat_support = true;
            }
            landed_on_step = flat_support;
        }
        if (landed_on_step && length(stepped) > length(plain) + 1e-4f) {
            grounded_ = true;
            since_ground_ = 0.0f;
        } else {
            position = plain_end;
        }
    }
    const vec3 actual = position - start;
    if (std::fabs(actual.x) + 1e-5f < std::fabs(delta.x)) velocity.x = actual.x / dt;
    if (std::fabs(actual.z) + 1e-5f < std::fabs(delta.z)) velocity.z = actual.z / dt;
}

} // namespace thistle::three
