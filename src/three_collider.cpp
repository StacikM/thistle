// three::Collider's builders. Plain data, so they're built either way:
// games that make colliders compile and run the same with Physics3D off.
#include "thistle_internal.h"

namespace thistle::three {

Collider Collider::box(vec3 size) {
    Collider c;
    c.kind = Kind::Box;
    c.size = size;
    return c;
}

Collider Collider::sphere(float radius) {
    Collider c;
    c.kind = Kind::Sphere;
    c.size = {radius, radius * 2.0f, radius};
    return c;
}

Collider Collider::capsule(float radius, float height) {
    Collider c;
    c.kind = Kind::Capsule;
    c.size = {radius, height, radius};
    return c;
}

Collider Collider::cylinder(float radius, float height) {
    Collider c;
    c.kind = Kind::Cylinder;
    c.size = {radius, height, radius};
    return c;
}

Collider Collider::convex(std::vector<vec3> points) {
    Collider c;
    c.kind = Kind::Convex;
    c.points = std::move(points);
    return c;
}

Collider Collider::convex(const MeshData& mesh) {
    std::vector<vec3> points;
    points.reserve(mesh.vertices.size());
    for (const Vertex& v : mesh.vertices) points.push_back(v.position);
    return convex(std::move(points));
}

Collider Collider::convex(Model model, vec3 scale) {
    Collider c;
    c.kind = Kind::Convex;
    detail::model_world_triangles(model, mat4::scale(scale), c.points); // duplicates are fine for a hull
    return c;
}

Collider Collider::mesh(const MeshData& mesh) {
    Collider c;
    c.kind = Kind::Mesh;
    c.points.reserve(mesh.indices.size());
    for (uint32_t i : mesh.indices) {
        if (i < mesh.vertices.size()) c.points.push_back(mesh.vertices[i].position);
    }
    c.points.resize(c.points.size() / 3 * 3);
    return c;
}

Collider Collider::mesh(Model model, vec3 scale) {
    Collider c;
    c.kind = Kind::Mesh;
    detail::model_world_triangles(model, mat4::scale(scale), c.points);
    return c;
}

Collider Collider::compound(std::vector<Collider> parts) {
    Collider c;
    c.kind = Kind::Compound;
    c.parts = std::move(parts);
    return c;
}

Collider Collider::voxels(const VoxelWorld& voxels) {
    Collider c;
    c.kind = Kind::Voxels;
    c.size = vec3{voxels.voxel_size, voxels.voxel_size, voxels.voxel_size};
    std::vector<std::pair<ivec3, uint64_t>> chunks;
    detail::VoxelWorldAccess::chunks(voxels, chunks);
    std::vector<std::pair<ivec3, ivec3>> boxes;
    for (const auto& [chunk, revision] : chunks) detail::voxel_chunk_boxes(voxels, chunk, boxes);
    c.points.reserve(boxes.size() * 2);
    const float s = voxels.voxel_size;
    for (const auto& [lo, hi] : boxes) {
        c.points.push_back(vec3{static_cast<float>(lo.x), static_cast<float>(lo.y), static_cast<float>(lo.z)} * s);
        c.points.push_back(vec3{static_cast<float>(hi.x), static_cast<float>(hi.y), static_cast<float>(hi.z)} * s);
    }
    return c;
}

Collider Collider::at(vec3 offset_, quat rotation_) const {
    Collider c = *this;
    c.offset = offset_;
    c.rotation = rotation_;
    return c;
}

} // namespace thistle::three

namespace thistle::detail {

void voxel_chunk_boxes(const three::VoxelWorld& world, three::ivec3 chunk, std::vector<std::pair<three::ivec3, three::ivec3>>& out) {
    using three::ivec3;
    constexpr int n = three::VoxelWorld::chunk_size;
    // Greedy: grow each box along x, then y, then z while every block it
    // would take in is solid and not in a box yet. Not the fewest boxes
    // possible, but close for walls, floors and terrain, and O(blocks).
    std::vector<uint8_t> solid(n * n * n), used(n * n * n, 0);
    std::vector<int8_t> solid_type; // cache: is block id i solid? (-1 = not looked up yet)
    const ivec3 base = chunk * n;
    auto at = [](int x, int y, int z) { return x + n * (y + n * z); };
    for (int z = 0; z < n; ++z) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const three::BlockId id = world.get(base.x + x, base.y + y, base.z + z);
                if (id >= solid_type.size()) solid_type.resize(id + 1, -1);
                if (solid_type[id] < 0) solid_type[id] = id != 0 && world.block_type(id).solid;
                solid[at(x, y, z)] = static_cast<uint8_t>(solid_type[id]);
            }
        }
    }
    auto free = [&](int x, int y, int z) { return solid[at(x, y, z)] && !used[at(x, y, z)]; };
    for (int z = 0; z < n; ++z) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                if (!free(x, y, z)) continue;
                int x1 = x, y1 = y, z1 = z;
                while (x1 + 1 < n && free(x1 + 1, y, z)) ++x1;
                for (bool grow = true; grow && y1 + 1 < n;) {
                    for (int i = x; i <= x1 && grow; ++i) grow = free(i, y1 + 1, z);
                    if (grow) ++y1;
                }
                for (bool grow = true; grow && z1 + 1 < n;) {
                    for (int j = y; j <= y1 && grow; ++j)
                        for (int i = x; i <= x1 && grow; ++i) grow = free(i, j, z1 + 1);
                    if (grow) ++z1;
                }
                for (int k = z; k <= z1; ++k)
                    for (int j = y; j <= y1; ++j)
                        for (int i = x; i <= x1; ++i) used[at(i, j, k)] = 1;
                out.push_back({base + ivec3{x, y, z}, base + ivec3{x1 + 1, y1 + 1, z1 + 1}});
            }
        }
    }
}

} // namespace thistle::detail
