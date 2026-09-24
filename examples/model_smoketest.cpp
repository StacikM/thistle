// Checks three::load_model_data() against real files it writes itself: an
// OBJ + MTL (materials, vertex colors, quads, missing normals, UV flip, a
// texture), a .gltf with everything embedded as base64 data URIs (node
// hierarchy, linear->sRGB colors, alpha mask, unlit, nearest sampler), and
// the same scene as a binary .glb with the texture inside the BIN chunk.
// Headless: constructs App (textures need the engine's state) but never
// runs it, the same established pattern as scene_smoketest.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
bool near(float a, float b, float eps = 2e-3f) { return std::fabs(a - b) <= eps; }
bool near(vec3 a, vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

// A 2x2 RGBA PNG: red, green / blue, half-transparent white.
const unsigned char kPng[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,0,0,0,19,73,68,65,84,120,156,99,248,207,192,240,31,12,129,52,8,52,0,0,73,73,9,120,40,160,219,119,0,0,0,0,73,69,78,68,174,66,96,130};

void write_file(const std::filesystem::path& p, const void* data, size_t size) {
    std::ofstream out(p, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}
void write_text(const std::filesystem::path& p, const std::string& s) { write_file(p, s.data(), s.size()); }

std::string base64(const unsigned char* data, size_t n) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (data[i] << 16) | ((i + 1 < n ? data[i + 1] : 0) << 8) | (i + 2 < n ? data[i + 2] : 0);
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += i + 1 < n ? abc[(v >> 6) & 63] : '=';
        out += i + 2 < n ? abc[v & 63] : '=';
    }
    return out;
}

// One triangle: positions (0,0,0) (1,0,0) (0,1,0), uint16 indices, a
// linear-gray COLOR_0 — laid out in one buffer the way exporters do it.
std::vector<unsigned char> triangle_buffer() {
    std::vector<unsigned char> b;
    auto put = [&](const void* p, size_t n) { const auto* c = static_cast<const unsigned char*>(p); b.insert(b.end(), c, c + n); };
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float col[12] = {0.5f, 0.5f, 0.5f, 1, 0.5f, 0.5f, 0.5f, 1, 0.5f, 0.5f, 0.5f, 1};
    const uint16_t idx[4] = {0, 1, 2, 0}; // padded to 8 bytes
    put(pos, sizeof(pos));  // offset 0, 36 bytes
    put(col, sizeof(col));  // offset 36, 48 bytes
    put(idx, sizeof(idx));  // offset 84, 6 bytes used
    return b;
}

std::string gltf_json(const std::string& buffer_entry, const std::string& image_entry, bool png_in_buffer) {
    std::string views = R"([{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":48},{"buffer":0,"byteOffset":84,"byteLength":6})";
    if (png_in_buffer) views += R"(,{"buffer":0,"byteOffset":92,"byteLength":)" + std::to_string(sizeof(kPng)) + "}";
    views += "]";
    return std::string(R"({
      "asset":{"version":"2.0"},
      "extensionsUsed":["KHR_materials_unlit"],
      "scene":0,
      "scenes":[{"nodes":[0]}],
      "nodes":[
        {"name":"Parent","translation":[10,0,0],"children":[1]},
        {"name":"Child","scale":[2,2,2],"mesh":0}
      ],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1},"indices":2,"material":0}]}],
      "materials":[{
        "pbrMetallicRoughness":{"baseColorFactor":[0.214,0.214,0.214,1],"baseColorTexture":{"index":0},"roughnessFactor":0.5},
        "alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true,
        "extensions":{"KHR_materials_unlit":{}}
      }],
      "textures":[{"source":0,"sampler":0}],
      "samplers":[{"magFilter":9728,"minFilter":9728}],
      "images":[)") + image_entry + R"(],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
        {"bufferView":1,"componentType":5126,"count":3,"type":"VEC4"},
        {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}
      ],
      "bufferViews":)" + views + R"(,
      "buffers":[)" + buffer_entry + R"(]
    })";
}

void check_gltf_scene(const char* label, const ModelData& d) {
    const std::string l = label;
    check(d.parts.size() == 1, l + ": one part");
    if (d.parts.size() != 1) return;
    const ModelData::Part& p = d.parts[0];
    check(p.name == "Child", l + ": part named after its node");
    check(p.mesh.vertices.size() == 3 && p.mesh.indices.size() == 3, l + ": one triangle");
    check(near(p.transform.transform_point(p.mesh.vertices[1].position), {12, 0, 0}),
          l + ": node hierarchy applied (parent translate 10, child scale 2)");
    check(near(p.mesh.vertices[0].normal, {0, 0, 1}), l + ": no NORMAL attribute -> flat normals, facing +Z");
    check(near(p.mesh.vertices[0].color.r, 0.7297f), l + ": COLOR_0 converted from linear 0.5 to sRGB");
    check(near(p.material.color.r, 0.4962f), l + ": baseColorFactor converted from linear to sRGB");
    check(p.material.alpha == AlphaMode::Cutout && near(p.material.alpha_cutoff, 0.3f), l + ": alphaMode MASK -> Cutout with its cutoff");
    check(p.material.double_sided && p.material.unlit, l + ": doubleSided and KHR_materials_unlit");
    check(p.material.filter == TextureFilter::Nearest, l + ": NEAREST sampler -> TextureFilter::Nearest");
    check(p.material.texture.valid() && p.material.texture.width == 2 && p.material.texture.height == 2,
          l + ": embedded 2x2 PNG texture loaded");
    check(near(d.bounds().max, {12, 2, 0}) && near(d.bounds().min, {10, 0, 0}), l + ": ModelData::bounds in model space");
}
} // namespace

int main() {
    App app{{.title = "ModelSmoketest", .width = 320, .height = 240}};
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "thistle_model_smoketest";
    std::filesystem::create_directories(dir);

    // --- OBJ + MTL ---
    write_file(dir / "tex.png", kPng, sizeof(kPng));
    write_text(dir / "test.mtl",
               "newmtl red\nKd 1 0 0\nNs 100\nKe 0.1 0.2 0.3\nmap_Ke tex.png\n\n"
               "newmtl glass\nKd 0.5 0.5 1\nd 0.4\nmap_Kd -s 1 1 1 tex.png\n");
    write_text(dir / "test.obj",
               "# a quad with normals + vertex colors, then a normal-less triangle\n"
               "mtllib test.mtl\n"
               "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 1 1 0 0 0 1\nv 0 1 0 1 1 1\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
               "usemtl red\nf 1/1/1 2/2/1 3/3/1 4/4/1\n"
               "usemtl glass\nf 1 2 3\n");
    {
        const ModelData d = load_model_data((dir / "test.obj").string());
        check(d.parts.size() == 2, "obj: one part per material");
        if (d.parts.size() == 2) {
            const ModelData::Part& red = d.parts[0];
            const ModelData::Part& glass = d.parts[1];
            check(red.name == "red" && glass.name == "glass", "obj: parts named after their materials, in file order");
            check(red.mesh.vertices.size() == 4 && red.mesh.indices.size() == 6, "obj: quad -> 2 triangles sharing 4 vertices");
            check(red.material.color.r == 1.0f && red.material.color.g == 0.0f, "mtl: Kd -> color");
            check(near(red.material.shininess, 100.0f) && near(red.material.emissive.b, 0.3f), "mtl: Ns -> shininess, Ke -> emissive");
            check(red.material.emissive_texture.valid(), "mtl: map_Ke -> emissive_texture");
            check(near(red.mesh.vertices[0].uv.y, 1.0f), "obj: vt v=0 flipped to the image's bottom row (v=1)");
            check(near(red.mesh.vertices[1].color.g, 1.0f) && near(red.mesh.vertices[1].color.r, 0.0f), "obj: 'v x y z r g b' vertex colors");
            check(glass.material.alpha == AlphaMode::Blend && near(glass.material.color.a, 0.4f), "mtl: d 0.4 -> see-through blend");
            check(glass.material.texture.valid() && glass.material.texture.width == 2, "mtl: map_Kd (with options before the file name) loads the texture");
            check(glass.mesh.vertices.size() == 3 && near(glass.mesh.vertices[0].normal, {0, 0, 1}), "obj: face without vn gets its flat normal");
        }
        const Model m = load_model((dir / "test.obj").string());
        check(m.valid() && model_part_count(m) == 2, "load_model: a drawable model with both parts (no GPU needed yet)");
        check(model_material(m, 1).alpha == AlphaMode::Blend, "model_material reads a part's material back");
    }

    // --- glTF with base64 data URIs ---
    {
        const std::vector<unsigned char> buf = triangle_buffer();
        const std::string json = gltf_json(
            R"({"byteLength":)" + std::to_string(buf.size()) + R"(,"uri":"data:application/octet-stream;base64,)" + base64(buf.data(), buf.size()) + "\"}",
            R"({"uri":"data:image/png;base64,)" + base64(kPng, sizeof(kPng)) + "\"}", false);
        write_text(dir / "embedded.gltf", json);
        check_gltf_scene("gltf", load_model_data((dir / "embedded.gltf").string()));
    }

    // --- the same scene as binary GLB, PNG inside the BIN chunk ---
    {
        std::vector<unsigned char> bin = triangle_buffer();
        bin.resize(92, 0);
        bin.insert(bin.end(), kPng, kPng + sizeof(kPng));
        while (bin.size() % 4) bin.push_back(0);
        std::string json = gltf_json(R"({"byteLength":)" + std::to_string(bin.size()) + "}",
                                     R"({"bufferView":3,"mimeType":"image/png"})", true);
        while (json.size() % 4) json += ' ';
        std::vector<unsigned char> glb;
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) glb.push_back(static_cast<unsigned char>(v >> (8 * i))); };
        u32(0x46546C67); u32(2); u32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
        u32(static_cast<uint32_t>(json.size())); u32(0x4E4F534A);
        glb.insert(glb.end(), json.begin(), json.end());
        u32(static_cast<uint32_t>(bin.size())); u32(0x004E4942);
        glb.insert(glb.end(), bin.begin(), bin.end());
        write_file(dir / "binary.glb", glb.data(), glb.size());
        check_gltf_scene("glb", load_model_data((dir / "binary.glb").string()));
        const Model m = load_model((dir / "binary.glb").string());
        check(near(model_bounds(m).max, {12, 2, 0}), "load_model: model_bounds includes the node transforms");
    }

    // --- failure paths: warn and return nothing, never crash ---
    check(load_model_data((dir / "missing.glb").string()).empty(), "missing file -> empty ModelData");
    check(!load_model((dir / "tex.png").string()).valid(), "unsupported extension -> invalid Model");
    write_text(dir / "broken.gltf", "{ this is not json");
    check(load_model_data((dir / "broken.gltf").string()).empty(), "malformed glTF -> empty ModelData");

    std::filesystem::remove_all(dir);
    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all model checks passed\n");
    return 0;
}
