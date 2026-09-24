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
#include <vector>
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

    // --- save / load round trip ---
    {
        VoxelWorld a;
        a.voxel_size = 0.25f;
        a.origin = {1, 2, 3};
        const BlockId s1 = a.add_block({.name = "stone", .color = rgb(0.5f, 0.5f, 0.5f)});
        const BlockId s2 = a.add_block(BlockType{.name = "glow", .alpha = AlphaMode::Cutout, .emissive = coral, .solid = false}.set_tiles(1, 2, 3));
        a.fill({-40, -3, -40}, {40, 3, 40}, s1); // spans many chunks, negative ones too
        a.set(7, 100, -9, s2);
        const std::vector<uint8_t> bytes = a.serialize();
        check(bytes.size() < 20000, "a 81x7x81 slab serializes compactly (" + std::to_string(bytes.size()) + " bytes, run-length)");
        VoxelWorld b;
        check(b.deserialize(bytes.data(), bytes.size()), "deserialize accepts what serialize wrote");
        check(b.get(-40, -3, -40) == s1 && b.get(40, 3, 40) == s1 && b.get(0, 4, 0) == 0 && b.get(7, 100, -9) == s2,
              "every block survives the round trip");
        const BlockType& t = b.block_type(s2);
        check(t.name == "glow" && t.alpha == AlphaMode::Cutout && !t.solid && t.tile_top == 1 && t.tile_bottom == 3 &&
              std::fabs(t.emissive.r - coral.r) < 1e-6f, "block types survive the round trip");
        check(std::fabs(b.voxel_size - 0.25f) < 1e-6f && b.origin == vec3{1, 2, 3}, "voxel_size and origin survive");
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + bytes.size() / 2);
        check(!b.deserialize(cut.data(), cut.size()) && b.chunk_count() == 0, "truncated data is rejected and leaves the world empty");
        std::vector<uint8_t> bad = bytes;
        bad[0] ^= 0xFF;
        check(!b.deserialize(bad.data(), bad.size()), "wrong magic is rejected");
    }

    // --- MagicaVoxel .vox ---
    {
        std::vector<uint8_t> v;
        auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); };
        auto tag = [&](const char* t) { v.insert(v.end(), t, t + 4); };
        auto str = [&](const std::string& s2) { u32(static_cast<uint32_t>(s2.size())); v.insert(v.end(), s2.begin(), s2.end()); };
        auto chunk = [&](const char* id, const std::vector<uint8_t>& body) { tag(id); u32(static_cast<uint32_t>(body.size())); u32(0); v.insert(v.end(), body.begin(), body.end()); };
        auto body = [&](auto fill) { std::vector<uint8_t> saved; saved.swap(v); fill(); std::vector<uint8_t> b; b.swap(v); v.swap(saved); return b; };
        tag("VOX "); u32(150);
        tag("MAIN"); u32(0); u32(0);
        chunk("SIZE", body([&] { u32(2); u32(3); u32(4); }));
        // Three voxels: (0,0,0) red, (1,0,0) glass, (0,2,3) glowing. MagicaVoxel is Z-up.
        chunk("XYZI", body([&] { u32(3); v.insert(v.end(), {0, 0, 0, 1, 1, 0, 0, 2, 0, 2, 3, 3}); }));
        chunk("RGBA", body([&] {
            for (int i = 0; i < 256; ++i) {
                const uint8_t c[4] = {static_cast<uint8_t>(i == 0 ? 255 : 10), static_cast<uint8_t>(i == 1 ? 255 : 10), static_cast<uint8_t>(i == 2 ? 255 : 10), 255};
                v.insert(v.end(), c, c + 4);
            }
        }));
        chunk("MATL", body([&] { u32(2); u32(2); str("_type"); str("_glass"); str("_trans"); str("0.75"); }));
        chunk("MATL", body([&] { u32(3); u32(2); str("_type"); str("_emit"); str("_emit"); str("1.0"); }));
        // Scene: root transform -> group -> transform (moved +10 on MagicaVoxel X) -> shape(model 0).
        chunk("nTRN", body([&] { u32(0); u32(0); u32(1); u32(0xFFFFFFFF); u32(0xFFFFFFFF); u32(1); u32(0); }));
        chunk("nGRP", body([&] { u32(1); u32(0); u32(1); u32(2); }));
        chunk("nTRN", body([&] { u32(2); u32(0); u32(3); u32(0xFFFFFFFF); u32(0); u32(1); u32(1); str("_t"); str("10 0 0"); }));
        chunk("nSHP", body([&] { u32(3); u32(0); u32(1); u32(0); u32(0); }));
        const std::string path = "thistle_voxel_smoketest.vox";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(v.data(), 1, v.size(), f); std::fclose(f); }

        VoxelWorld w2;
        check(w2.load_vox(path, {100, 0, 100}), "load_vox reads a hand-built .vox file");
        // MagicaVoxel pivots a model on size/2: (0,0,0)-(1,1,2) moved +10 on X is (9,-1,-2),
        // which is (9,-2,1) once Z-up becomes Y-up via (x, z, -y); the lowest corner of all three
        // voxels is (9,-2,-1), so this one lands at `at` + (0, 0, 2).
        const BlockId red = w2.get(100, 0, 102);
        check(red != 0 && std::fabs(w2.block_type(red).color.r - 1.0f) < 1e-3f && std::fabs(w2.block_type(red).color.g - 10 / 255.0f) < 1e-3f,
              "palette slot 1 -> the red block, placed with Z-up turned to Y-up (min corner at `at`)");
        const BlockId glass2 = w2.get(101, 0, 102);
        check(glass2 != 0 && w2.block_type(glass2).alpha == AlphaMode::Blend && std::fabs(w2.block_type(glass2).color.a - 0.25f) < 1e-3f,
              "a _glass material becomes a see-through block (alpha = 1 - _trans)");
        const BlockId glow2 = w2.get(100, 3, 100);
        check(glow2 != 0 && w2.block_type(glow2).emissive.b > 0.9f, "an _emit material glows");
        check(w2.block_type_count() == 3, "one block type per palette slot used");
        VoxelWorld w3;
        w3.load_vox(path, {0, 0, 0});
        w3.load_vox(path, {20, 0, 0});
        check(w3.block_type_count() == 3, "importing twice reuses the same block types");
        std::remove(path.c_str());
        check(!w3.load_vox("does_not_exist.vox"), "a missing .vox file fails cleanly");
    }

    // --- a rotated grid ---
    {
        VoxelWorld r;
        const BlockId s1 = r.add_block({.name = "s"});
        r.set(2, 0, 0, s1);
        r.origin = {10, 0, 0};
        r.rotation = quat::axis_angle({0, 1, 0}, radians(90)); // grid +X now points along world -Z
        const vec3 c = r.block_center({2, 0, 0});
        check(std::fabs(c.x - 10.5f) < 1e-4f && std::fabs(c.z + 2.5f) < 1e-4f, "block_center follows the rotation");
        check(r.to_block(c) == ivec3{2, 0, 0}, "to_block undoes it");
        const auto h = r.raycast(Ray{{10.5f, 0.5f, 5.0f}, {0, 0, -1}});
        check(h.hit && h.block == ivec3{2, 0, 0} && std::fabs(h.distance - 7.0f) < 1e-3f, "raycast walks the rotated grid");
    }

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all voxel checks passed\n");
    return 0;
}
