// Checks three::VoxelWorld's storage and mesher on the CPU: block ids across
// chunk boundaries and negative coordinates, hidden-face removal (including
// between chunks, and glass-against-glass), greedy merging of flat faces
// (and not of textured ones), ambient-occlusion darkening in corners, and
// that every emitted triangle faces outward. No GPU: mesh_chunk() returns
// plain ModelData, the same thing the renderer uploads. Constructs an App
// (never run) only because the textured-block check needs make_texture().
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <string>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}

// Quads = triangles / 2 over every part of every chunk.
int count_quads(const VoxelWorld& w, ivec3 chunk) {
    int tris = 0;
    for (const ModelData::Part& p : w.mesh_chunk(chunk).parts) tris += static_cast<int>(p.mesh.indices.size() / 3);
    return tris / 2;
}

// Every triangle's winding must agree with its stored normal (the renderer
// culls back faces, so a reversed one would be an invisible face).
bool all_outward(const ModelData& d) {
    for (const ModelData::Part& p : d.parts) {
        for (size_t i = 0; i + 2 < p.mesh.indices.size(); i += 3) {
            const Vertex& a = p.mesh.vertices[p.mesh.indices[i]];
            const Vertex& b = p.mesh.vertices[p.mesh.indices[i + 1]];
            const Vertex& c = p.mesh.vertices[p.mesh.indices[i + 2]];
            if (dot(cross(b.position - a.position, c.position - a.position), a.normal) <= 0.0f) return false;
        }
    }
    return true;
}

float min_vertex_brightness(const ModelData& d) {
    float m = 1.0f;
    for (const ModelData::Part& p : d.parts)
        for (const Vertex& v : p.mesh.vertices) m = std::fmin(m, v.color.r);
    return m;
}
} // namespace

int main() {
    App app{{.title = "VoxelSmoketest", .width = 320, .height = 240}};
    VoxelWorld w;
    const BlockId stone = w.add_block({.name = "stone", .color = white});
    const BlockId dirt = w.add_block({.name = "dirt", .color = rgb(0.5f, 0.35f, 0.2f)});
    const BlockId glass = w.add_block({.name = "glass", .color = rgba{0.8f, 0.9f, 1, 0.4f}, .alpha = AlphaMode::Blend});
    BlockType grass{.name = "grass"};
    grass.set_tiles(0, 1, 2);
    const BlockId grass_id = w.add_block(grass);

    check(stone == 1 && dirt == 2 && w.block_type_count() == 4, "block ids are handed out from 1");
    check(w.find_block("glass") == glass && w.find_block("nope") == 0, "find_block by name");
    check(w.block_type(0).name == "air" && !w.block_type(0).solid, "id 0 is air");

    // Storage
    w.set(0, 0, 0, stone);
    w.set(-1, -1, -1, dirt);
    w.set(31, 5, 7, stone);
    w.set(32, 5, 7, dirt);
    w.set(-33, 100, 64, glass);
    check(w.get(0, 0, 0) == stone && w.get(-1, -1, -1) == dirt, "get/set at the origin and just below it");
    check(w.get(31, 5, 7) == stone && w.get(32, 5, 7) == dirt, "neighbors across a chunk boundary are independent");
    check(w.get(-33, 100, 64) == glass, "far negative/positive coordinates");
    check(w.get(5, 5, 5) == 0, "untouched block is air");
    check(w.to_block({-0.5f, 0.2f, 1.7f}) == ivec3{-1, 0, 1}, "to_block floors (negative coords too)");
    w.clear();
    check(w.chunk_count() == 0 && w.get(0, 0, 0) == 0, "clear() empties everything");

    // One block: 6 faces, all outward.
    w.set(3, 3, 3, stone);
    check(count_quads(w, {0, 0, 0}) == 6, "a lone block has 6 faces");
    check(all_outward(w.mesh_chunk({0, 0, 0})), "a lone block's faces all point outward");

    // Greedy merging: a solid 4x4x4 cube is still 6 quads (flat, same shading).
    w.clear();
    w.ambient_occlusion = false;
    w.fill({0, 0, 0}, {3, 3, 3}, stone);
    check(count_quads(w, {0, 0, 0}) == 6, "a solid flat-colored 4x4x4 cube merges into 6 quads");
    w.set(1, 1, 1, dirt); // hidden inside: changes nothing visible
    check(count_quads(w, {0, 0, 0}) == 6, "a different block buried inside adds no faces");
    w.set(0, 3, 0, dirt); // different type on a corner of the surface
    check(count_quads(w, {0, 0, 0}) > 6, "a different surface block splits the merged faces");

    // Textured blocks are never merged (an atlas tile can't repeat across a quad).
    w.clear();
    w.set_atlas(make_texture(2, 2, std::vector<unsigned char>(16, 255).data()), 1);
    w.fill({0, 0, 0}, {1, 0, 0}, grass_id);
    check(count_quads(w, {0, 0, 0}) == 10, "two textured blocks side by side: 10 quads, top/bottom/sides not merged");

    // Hidden faces between chunks: blocks at x=31 and x=32 share a face.
    w.clear();
    w.set(31, 0, 0, stone);
    w.set(32, 0, 0, stone);
    check(count_quads(w, {0, 0, 0}) == 5 && count_quads(w, {1, 0, 0}) == 5, "the face between two chunks is hidden on both sides");

    // Transparency rules.
    w.clear();
    w.set(0, 0, 0, glass);
    w.set(1, 0, 0, glass);
    check(count_quads(w, {0, 0, 0}) == 6, "glass next to glass: no wall between them (merged into one box)");
    w.set(1, 0, 0, stone);
    int glass_quads = 0, stone_quads = 0;
    for (const ModelData::Part& p : w.mesh_chunk({0, 0, 0}).parts) {
        const int q = static_cast<int>(p.mesh.indices.size() / 6);
        if (p.material.alpha == AlphaMode::Blend) glass_quads += q; else stone_quads += q;
    }
    check(glass_quads == 5 && stone_quads == 6, "glass next to stone: glass hides its shared face, stone still shows it");

    // Ambient occlusion: a block sitting in an inside corner gets dark vertices.
    w.clear();
    w.ambient_occlusion = true;
    w.fill({0, 0, 0}, {4, 0, 4}, stone);  // floor
    w.fill({0, 1, 0}, {0, 3, 4}, stone);  // wall
    const ModelData cornered = w.mesh_chunk({0, 0, 0});
    check(min_vertex_brightness(cornered) < 0.9f, "vertices in the floor/wall corner are darkened");
    check(all_outward(cornered), "AO triangle flipping keeps every face outward");
    w.ambient_occlusion = false;
    check(min_vertex_brightness(w.mesh_chunk({0, 0, 0})) == 1.0f, "ambient_occlusion = false: no darkening");

    // fill_sphere carves.
    w.clear();
    w.fill({-8, -8, -8}, {8, 8, 8}, stone);
    w.fill_sphere({0.5f, 0.5f, 0.5f}, 4.0f, 0);
    check(w.get(0, 0, 0) == 0 && w.get(0, 3, 0) == 0 && w.get(0, 5, 0) == stone, "fill_sphere with air carves a ball");

    // bounds() in world units, respecting voxel_size and origin.
    w.clear();
    w.voxel_size = 0.5f;
    w.origin = {10, 0, 0};
    w.fill({0, 0, 0}, {3, 1, 0}, stone);
    const Bounds b = w.bounds();
    check(std::fabs(b.min.x - 10.0f) < 1e-4f && std::fabs(b.max.x - 12.0f) < 1e-4f && std::fabs(b.max.y - 1.0f) < 1e-4f,
          "bounds() applies voxel_size and origin");

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all voxel checks passed\n");
    return 0;
}
