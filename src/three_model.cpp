#include "thistle_core.h"

#include "cgltf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>

namespace thistle::three {

Bounds ModelData::bounds() const {
    Bounds b;
    for (const Part& part : parts) {
        const Bounds pb = part.mesh.bounds().transformed(part.transform);
        if (pb.valid()) { b.add(pb.min); b.add(pb.max); }
    }
    return b;
}

namespace {

std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string lowercase_extension(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// glTF stores colors in linear space; everything in the public API
// (Material::color, Vertex::color) is sRGB, like the 2D side.
float linear_to_srgb(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f); }
rgba linear_to_srgb(rgba c) { return {linear_to_srgb(c.r), linear_to_srgb(c.g), linear_to_srgb(c.b), c.a}; }

// --- OBJ + MTL -----------------------------------------------------------------

struct ObjMaterial {
    Material material;
};

// Texture paths in .mtl files may carry options (-s 1 1 1 -bm 0.5 file.png);
// the file name is always the last token.
std::string last_token(std::istringstream& ls) {
    std::string tok, last;
    while (ls >> tok) last = tok;
    return last;
}

std::map<std::string, ObjMaterial> parse_mtl(const std::string& path) {
    std::map<std::string, ObjMaterial> out;
    std::string content;
    if (!detail::read_file_text(path, content)) {
        log_warn("load_model: could not open material library " + path);
        return out;
    }
    const std::string dir = directory_of(path);
    ObjMaterial* cur = nullptr;
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "newmtl") {
            std::string name;
            ls >> name;
            cur = &out[name];
        } else if (!cur) {
            continue;
        } else if (tag == "Kd") {
            ls >> cur->material.color.r >> cur->material.color.g >> cur->material.color.b;
        } else if (tag == "Ke") {
            ls >> cur->material.emissive.r >> cur->material.emissive.g >> cur->material.emissive.b;
        } else if (tag == "Ks") {
            float r = 0, g = 0, b = 0;
            ls >> r >> g >> b;
            cur->material.specular = std::clamp((r + g + b) / 3.0f, 0.0f, 1.0f);
        } else if (tag == "Ns") {
            float ns = 0;
            ls >> ns;
            cur->material.shininess = std::clamp(ns, 1.0f, 256.0f);
        } else if (tag == "d" || tag == "Tr") {
            float v = 1.0f;
            ls >> v;
            cur->material.color.a = tag == "d" ? v : 1.0f - v;
            if (cur->material.color.a < 0.999f) cur->material.alpha = AlphaMode::Blend;
        } else if (tag == "map_Ke") {
            const std::string file = last_token(ls);
            if (!file.empty()) cur->material.emissive_texture = load_texture(dir + file);
        } else if (tag == "map_Kd") {
            const std::string file = last_token(ls);
            if (!file.empty()) {
                cur->material.texture = load_texture(dir + file);
                if (!cur->material.texture.valid()) log_warn("load_model: could not load texture " + dir + file);
            }
        }
    }
    return out;
}

struct ObjIndex { int p = -1, t = -1, n = -1; };

int resolve_obj_index(int raw, int count) {
    if (raw == 0) return -1;
    return raw > 0 ? raw - 1 : count + raw;
}

ObjIndex parse_obj_corner(const std::string& tok, int pc, int tc, int nc) {
    ObjIndex out;
    int field = 0;
    size_t start = 0;
    while (start <= tok.size()) {
        const size_t slash = tok.find('/', start);
        const std::string part = tok.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            const int raw = std::atoi(part.c_str());
            if (field == 0) out.p = resolve_obj_index(raw, pc);
            else if (field == 1) out.t = resolve_obj_index(raw, tc);
            else if (field == 2) out.n = resolve_obj_index(raw, nc);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
        ++field;
    }
    return out;
}

ModelData load_obj(const std::string& path) {
    ModelData data;
    std::string content;
    if (!detail::read_file_text(path, content)) {
        log_warn("load_model: could not open " + path);
        return data;
    }
    const std::string dir = directory_of(path);
    std::vector<vec3> positions;
    std::vector<rgba> colors; // parallel to positions; the "v x y z r g b" extension
    std::vector<vec2> uvs;
    std::vector<vec3> normals;
    std::map<std::string, ObjMaterial> materials;

    struct Building {
        ModelData::Part part;
        std::unordered_map<uint64_t, uint32_t> dedupe;
    };
    std::vector<Building> parts;
    std::map<std::string, size_t> part_for_material;
    size_t current = SIZE_MAX;

    auto select_material = [&](const std::string& name) {
        auto it = part_for_material.find(name);
        if (it != part_for_material.end()) { current = it->second; return; }
        Building b;
        b.part.name = name;
        auto m = materials.find(name);
        if (m != materials.end()) b.part.material = m->second.material;
        parts.push_back(std::move(b));
        current = parts.size() - 1;
        part_for_material[name] = current;
    };

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            vec3 p;
            ls >> p.x >> p.y >> p.z;
            positions.push_back(p);
            rgba c = white;
            if (ls >> c.r >> c.g >> c.b) colors.push_back(c);
            else colors.push_back(white);
        } else if (tag == "vt") {
            float u = 0.0f, v = 0.0f;
            ls >> u >> v;
            uvs.push_back({u, 1.0f - v}); // OBJ's v=0 is the bottom of the image; ours is the top row
        } else if (tag == "vn") {
            vec3 n;
            ls >> n.x >> n.y >> n.z;
            normals.push_back(normalize(n));
        } else if (tag == "mtllib") {
            const std::string file = last_token(ls);
            for (auto& [name, mat] : parse_mtl(dir + file)) materials[name] = mat;
        } else if (tag == "usemtl") {
            std::string name;
            ls >> name;
            select_material(name);
        } else if (tag == "f") {
            if (current == SIZE_MAX) select_material("");
            Building& b = parts[current];
            std::vector<ObjIndex> face;
            std::string tok;
            while (ls >> tok) {
                face.push_back(parse_obj_corner(tok, static_cast<int>(positions.size()), static_cast<int>(uvs.size()),
                                                 static_cast<int>(normals.size())));
            }
            if (face.size() < 3) continue;
            auto pos_of = [&](const ObjIndex& c) {
                return (c.p >= 0 && c.p < static_cast<int>(positions.size())) ? positions[c.p] : vec3{};
            };
            for (size_t i = 1; i + 1 < face.size(); ++i) { // fan-triangulate quads and n-gons
                const ObjIndex tri[3] = {face[0], face[i], face[i + 1]};
                const vec3 flat = normalize(cross(pos_of(tri[1]) - pos_of(tri[0]), pos_of(tri[2]) - pos_of(tri[0])));
                for (const ObjIndex& c : tri) {
                    const bool has_normal = c.n >= 0 && c.n < static_cast<int>(normals.size());
                    // Corners with a real normal are shared between faces; corners
                    // without one get the face's own flat normal, so they can't be.
                    const uint64_t key = (static_cast<uint64_t>(c.p + 1) << 42) ^ (static_cast<uint64_t>(c.t + 1) << 21) ^
                                         static_cast<uint64_t>(c.n + 1);
                    if (has_normal) {
                        auto it = b.dedupe.find(key);
                        if (it != b.dedupe.end()) { b.part.mesh.indices.push_back(it->second); continue; }
                    }
                    Vertex v;
                    v.position = pos_of(c);
                    v.normal = has_normal ? normals[c.n] : flat;
                    if (c.t >= 0 && c.t < static_cast<int>(uvs.size())) v.uv = uvs[c.t];
                    if (c.p >= 0 && c.p < static_cast<int>(colors.size())) v.color = colors[c.p];
                    const uint32_t idx = static_cast<uint32_t>(b.part.mesh.vertices.size());
                    b.part.mesh.vertices.push_back(v);
                    b.part.mesh.indices.push_back(idx);
                    if (has_normal) b.dedupe[key] = idx;
                }
            }
        }
    }
    for (Building& b : parts) {
        if (!b.part.mesh.indices.empty()) data.parts.push_back(std::move(b.part));
    }
    if (data.parts.empty()) log_warn("load_model: no triangles in " + path);
    return data;
}

// --- glTF / GLB ------------------------------------------------------------------

cgltf_result gltf_read_file(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data) {
    std::vector<unsigned char> bytes;
    if (!detail::read_file_bytes(path, bytes)) return cgltf_result_file_not_found;
    void* mem = std::malloc(bytes.empty() ? 1 : bytes.size());
    if (!mem) return cgltf_result_out_of_memory;
    if (!bytes.empty()) std::memcpy(mem, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = mem;
    return cgltf_result_success;
}

void gltf_release_file(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

struct GltfLoader {
    const std::string dir;
    std::unordered_map<const cgltf_image*, Texture> textures;

    Texture texture_for(const cgltf_image* image) {
        if (!image) return {};
        auto cached = textures.find(image);
        if (cached != textures.end()) return cached->second;
        Texture tex;
        if (image->buffer_view && image->buffer_view->buffer->data) {
            const auto* base = static_cast<const unsigned char*>(image->buffer_view->buffer->data);
            tex = load_texture_from_memory(base + image->buffer_view->offset, image->buffer_view->size);
        } else if (image->uri && std::strncmp(image->uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image->uri, ',');
            if (comma && std::strstr(image->uri, ";base64,")) {
                const char* b64 = comma + 1;
                size_t b64_len = std::strlen(b64);
                size_t padding = 0;
                while (b64_len > 0 && b64[b64_len - 1] == '=') { --b64_len; ++padding; }
                cgltf_options opts = {};
                void* decoded = nullptr;
                const cgltf_size size = (b64_len + padding) / 4 * 3 - padding;
                if (cgltf_load_buffer_base64(&opts, size, b64, &decoded) == cgltf_result_success) {
                    tex = load_texture_from_memory(decoded, size);
                    std::free(decoded);
                }
            }
        } else if (image->uri) {
            std::string uri = image->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            tex = load_texture(dir + uri);
        }
        if (!tex.valid()) log_warn("load_model: could not load a glTF texture" + std::string(image->uri && std::strncmp(image->uri, "data:", 5) != 0 ? std::string(" (") + image->uri + ")" : ""));
        textures[image] = tex;
        return tex;
    }

    Material material_for(const cgltf_material* m) {
        Material out;
        if (!m) return out;
        if (m->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = m->pbr_metallic_roughness;
            out.color = linear_to_srgb(rgba{pbr.base_color_factor[0], pbr.base_color_factor[1],
                                            pbr.base_color_factor[2], pbr.base_color_factor[3]});
            if (pbr.base_color_texture.texture) {
                out.texture = texture_for(pbr.base_color_texture.texture->image);
                const cgltf_sampler* smp = pbr.base_color_texture.texture->sampler;
                if (smp && smp->mag_filter == 9728 /* NEAREST */) out.filter = TextureFilter::Nearest;
                if (pbr.base_color_texture.has_transform) {
                    out.uv_scale = {pbr.base_color_texture.transform.scale[0], pbr.base_color_texture.transform.scale[1]};
                }
            }
            // No PBR here: map roughness onto the Blinn-Phong highlight so a
            // glossy Blender material still reads as glossy.
            const float r = std::clamp(pbr.roughness_factor, 0.05f, 1.0f);
            out.specular = 0.5f * (1.0f - r);
            out.shininess = std::clamp(2.0f / (r * r * r * r) - 2.0f, 2.0f, 256.0f);
        }
        const float strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
        out.emissive = linear_to_srgb(rgba{m->emissive_factor[0] * strength, m->emissive_factor[1] * strength,
                                           m->emissive_factor[2] * strength, 1.0f});
        if (m->emissive_texture.texture) out.emissive_texture = texture_for(m->emissive_texture.texture->image);
        out.double_sided = m->double_sided;
        out.unlit = m->unlit;
        if (m->alpha_mode == cgltf_alpha_mode_mask) {
            out.alpha = AlphaMode::Cutout;
            out.alpha_cutoff = m->alpha_cutoff;
        } else if (m->alpha_mode == cgltf_alpha_mode_blend) {
            out.alpha = AlphaMode::Blend;
        }
        return out;
    }

    bool read_primitive(const cgltf_primitive& prim, MeshData& mesh, bool skinned) {
        if (prim.type != cgltf_primitive_type_triangles) return false;
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *col = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
            const cgltf_attribute& a = prim.attributes[i];
            if (a.type == cgltf_attribute_type_position) pos = a.data;
            else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
            else if (a.type == cgltf_attribute_type_color && a.index == 0) col = a.data;
            else if (a.type == cgltf_attribute_type_joints && a.index == 0) jnt = a.data;
            else if (a.type == cgltf_attribute_type_weights && a.index == 0) wgt = a.data;
        }
        if (!skinned) jnt = wgt = nullptr;
        if (!pos || pos->count == 0) return false;
        mesh.vertices.resize(pos->count);
        for (cgltf_size i = 0; i < pos->count; ++i) {
            Vertex& v = mesh.vertices[i];
            float f[4] = {0, 0, 0, 1};
            cgltf_accessor_read_float(pos, i, f, 3);
            v.position = {f[0], f[1], f[2]};
            if (nrm && cgltf_accessor_read_float(nrm, i, f, 3)) v.normal = normalize(vec3{f[0], f[1], f[2]});
            if (uv && cgltf_accessor_read_float(uv, i, f, 2)) v.uv = {f[0], f[1]};
            if (col) {
                f[3] = 1.0f;
                cgltf_accessor_read_float(col, i, f, cgltf_num_components(col->type));
                v.color = linear_to_srgb(rgba{f[0], f[1], f[2], f[3]});
            }
            if (jnt && wgt) {
                float j[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
                cgltf_accessor_read_float(jnt, i, j, 4); // joint indices come back as their plain values
                cgltf_accessor_read_float(wgt, i, w, 4);
                v.joints = {j[0], j[1], j[2], j[3]};
                v.weights = {w[0], w[1], w[2], w[3]};
            }
        }
        if (prim.indices) {
            mesh.indices.resize(prim.indices->count);
            for (cgltf_size i = 0; i < prim.indices->count; ++i) {
                mesh.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
            }
        } else {
            mesh.indices.resize(pos->count);
            for (cgltf_size i = 0; i < pos->count; ++i) mesh.indices[i] = static_cast<uint32_t>(i);
        }
        mesh.indices.resize(mesh.indices.size() / 3 * 3);
        if (!nrm) mesh.make_flat(); // the glTF spec's rule for normal-less meshes: flat shading
        return true;
    }

    const cgltf_skin* skin = nullptr; // the one skin this model is animated by (the file's first)
    bool warned_other_skin = false;

    void add_node(const cgltf_node* node, ModelData& data) {
        if (node->mesh) {
            float world[16];
            cgltf_node_transform_world(node, world);
            mat4 m;
            std::memcpy(m.m, world, sizeof(world));
            const bool skinned = node->skin && node->skin == skin;
            if (node->skin && !skinned && !warned_other_skin) {
                warned_other_skin = true;
                log_warn("load_model: this file has more than one skin; meshes on the others are loaded unanimated");
            }
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& prim = node->mesh->primitives[p];
                ModelData::Part part;
                if (!read_primitive(prim, part.mesh, skinned)) continue;
                part.name = node->name ? node->name : (node->mesh->name ? node->mesh->name : "");
                part.material = material_for(prim.material);
                // glTF: a skinned mesh's own node transform doesn't apply;
                // its joints place it. (Applying it put Fox.glb in the wrong spot.)
                part.transform = skinned ? mat4{} : m;
                data.parts.push_back(std::move(part));
            }
        }
        for (cgltf_size c = 0; c < node->children_count; ++c) add_node(node->children[c], data);
    }
};

mat4 node_world(const cgltf_node* node) {
    mat4 m;
    if (node) cgltf_node_transform_world(node, m.m);
    return m;
}

int joint_index(const cgltf_skin& skin, const cgltf_node* node) {
    for (cgltf_size i = 0; i < skin.joints_count; ++i) {
        if (skin.joints[i] == node) return static_cast<int>(i);
    }
    return -1;
}

void read_skeleton(const cgltf_skin& skin, Skeleton& out) {
    out.joints.resize(skin.joints_count);
    for (cgltf_size i = 0; i < skin.joints_count; ++i) {
        const cgltf_node* node = skin.joints[i];
        Skeleton::Joint& j = out.joints[i];
        j.name = node->name ? node->name : "joint " + std::to_string(i);
        j.parent = node->parent ? joint_index(skin, node->parent) : -1;
        if (j.parent < 0) j.root_offset = node_world(node->parent); // armature nodes above the skeleton
        if (node->has_matrix) {
            mat4 m;
            std::memcpy(m.m, node->matrix, sizeof(m.m));
            detail::decompose(m, j.rest.position, j.rest.rotation, j.rest.scale);
        } else {
            if (node->has_translation) j.rest.position = {node->translation[0], node->translation[1], node->translation[2]};
            if (node->has_rotation) j.rest.rotation = quat(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]);
            if (node->has_scale) j.rest.scale = {node->scale[0], node->scale[1], node->scale[2]};
        }
        if (skin.inverse_bind_matrices) cgltf_accessor_read_float(skin.inverse_bind_matrices, i, j.inverse_bind.m, 16);
    }
}

void read_animations(const cgltf_data& gltf, const cgltf_skin& skin, std::vector<AnimationClip>& out) {
    for (cgltf_size a = 0; a < gltf.animations_count; ++a) {
        const cgltf_animation& anim = gltf.animations[a];
        AnimationClip clip;
        clip.name = anim.name ? anim.name : "animation " + std::to_string(a);
        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            const int joint = joint_index(skin, ch.target_node);
            if (joint < 0 || !ch.sampler || !ch.sampler->input || !ch.sampler->output) continue; // non-joint nodes, morph weights: not supported
            AnimationClip::Channel out_ch;
            out_ch.joint = joint;
            int comps = 3;
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: out_ch.path = AnimationClip::Channel::Path::Translation; break;
                case cgltf_animation_path_type_rotation: out_ch.path = AnimationClip::Channel::Path::Rotation; comps = 4; break;
                case cgltf_animation_path_type_scale: out_ch.path = AnimationClip::Channel::Path::Scale; break;
                default: continue;
            }
            const cgltf_accessor* in = ch.sampler->input;
            const cgltf_accessor* vals = ch.sampler->output;
            const bool cubic = ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            out_ch.step = ch.sampler->interpolation == cgltf_interpolation_type_step;
            for (cgltf_size k = 0; k < in->count; ++k) {
                float t = 0.0f;
                cgltf_accessor_read_float(in, k, &t, 1);
                float v[4] = {0, 0, 0, 1};
                // Cubic splines store (in-tangent, value, out-tangent) per key:
                // take the values and blend them linearly (close, not exact).
                cgltf_accessor_read_float(vals, cubic ? k * 3 + 1 : k, v, static_cast<cgltf_size>(comps));
                out_ch.times.push_back(t);
                out_ch.values.push_back({v[0], v[1], v[2], comps == 4 ? v[3] : 0.0f});
                clip.duration = std::max(clip.duration, t);
            }
            clip.channels.push_back(std::move(out_ch));
        }
        out.push_back(std::move(clip));
    }
}

ModelData load_gltf(const std::string& path) {
    ModelData data;
    std::vector<unsigned char> bytes;
    if (!detail::read_file_bytes(path, bytes)) {
        log_warn("load_model: could not open " + path);
        return data;
    }
    cgltf_options options = {};
    options.file.read = gltf_read_file;
    options.file.release = gltf_release_file;
    cgltf_data* gltf = nullptr;
    cgltf_result r = cgltf_parse(&options, bytes.data(), bytes.size(), &gltf);
    if (r == cgltf_result_success) r = cgltf_load_buffers(&options, gltf, path.c_str());
    if (r != cgltf_result_success) {
        log_warn("load_model: " + path + " is not a valid glTF/GLB file (cgltf error " + std::to_string(static_cast<int>(r)) + ")");
        if (gltf) cgltf_free(gltf);
        return data;
    }
    GltfLoader loader{directory_of(path), {}};
    if (gltf->skins_count > 0) {
        loader.skin = &gltf->skins[0];
        read_skeleton(*loader.skin, data.skeleton);
        read_animations(*gltf, *loader.skin, data.animations);
    }
    const cgltf_scene* scene = gltf->scene ? gltf->scene : (gltf->scenes_count > 0 ? &gltf->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) loader.add_node(scene->nodes[i], data);
    } else {
        for (cgltf_size i = 0; i < gltf->nodes_count; ++i) {
            if (!gltf->nodes[i].parent) loader.add_node(&gltf->nodes[i], data);
        }
    }
    cgltf_free(gltf);
    if (data.parts.empty()) log_warn("load_model: no triangle meshes in " + path);
    return data;
}

} // namespace

ModelData load_model_data(const std::string& path) {
    const std::string ext = lowercase_extension(path);
    if (ext == "obj") return load_obj(path);
    if (ext == "gltf" || ext == "glb") return load_gltf(path);
    log_warn("load_model: don't know how to load '" + path + "' (supported: .obj, .gltf, .glb)");
    return {};
}

Model load_model(const std::string& path) {
    const ModelData data = load_model_data(path);
    return data.empty() ? Model{} : make_model(data);
}

} // namespace thistle::three
