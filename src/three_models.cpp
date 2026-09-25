// The model registry: see three_models.h.
#include "three_models.h"

#include <algorithm>
#include <cmath>

namespace thistle::three {

namespace {
void (*g_release)(MeshRecord& mesh) = nullptr;

void release_meshes(ModelRecord& rec) {
    if (!g_release) return;
    for (MeshRecord& mesh : rec.meshes) g_release(mesh);
}

ModelRecord build_record(const ModelData& data);
} // namespace

std::vector<ModelRecord>& model_records() {
    static std::vector<ModelRecord> records;
    return records;
}

ModelRecord* model_record(Model m) {
    std::vector<ModelRecord>& records = model_records();
    if (m.id < 0 || m.id >= static_cast<int>(records.size())) return nullptr;
    ModelRecord& rec = records[static_cast<size_t>(m.id)];
    return rec.alive ? &rec : nullptr;
}

void set_mesh_release(void (*release)(MeshRecord& mesh)) { g_release = release; }

void unload_all_models() {
    for (ModelRecord& rec : model_records()) release_meshes(rec);
    model_records().clear();
}

Model make_model(const MeshData& mesh, const Material& material) {
    ModelRecord rec;
    MeshRecord m;
    m.cpu = mesh;
    m.bounds = mesh.bounds();
    m.positions.reserve(mesh.vertices.size());
    for (const Vertex& v : mesh.vertices) m.positions.push_back(v.position);
    m.triangles = mesh.indices;
    rec.bounds = m.bounds;
    rec.meshes.push_back(std::move(m));
    rec.parts.push_back({0, material, mat4{}});
    model_records().push_back(std::move(rec));
    return Model{static_cast<int>(model_records().size()) - 1};
}

// Normals use the same matrix as positions (fine for the rotations and
// uniform scales skeletons use).
void skin_vertices(const std::vector<Vertex>& bind, const std::vector<mat4>& skin, std::vector<Vertex>& out) {
    out.resize(bind.size());
    const int n = static_cast<int>(skin.size());
    for (size_t i = 0; i < bind.size(); ++i) {
        const Vertex& v = bind[i];
        const float w[4] = {v.weights.x, v.weights.y, v.weights.z, v.weights.w};
        const float j[4] = {v.joints.x, v.joints.y, v.joints.z, v.joints.w};
        mat4 m;
        float total = 0.0f;
        for (float& e : m.m) e = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const int joint = static_cast<int>(j[k]);
            if (w[k] <= 0.0f || joint < 0 || joint >= n) continue;
            for (int e = 0; e < 16; ++e) m.m[e] += skin[static_cast<size_t>(joint)].m[e] * w[k];
            total += w[k];
        }
        Vertex& o = out[i];
        o = v;
        if (total <= 0.0f) continue;
        if (std::fabs(total - 1.0f) > 1e-3f) {
            for (float& e : m.m) e /= total;
        }
        o.position = m.transform_point(v.position);
        o.normal = normalize(m.transform_direction(v.normal));
    }
}

Model make_model(const ModelData& data) {
    if (data.parts.empty()) return Model{};
    model_records().push_back(build_record(data));
    return Model{static_cast<int>(model_records().size()) - 1};
}

namespace {
ModelRecord build_record(const ModelData& data) {
    ModelRecord rec;
    rec.skeleton = data.skeleton;
    rec.animations = data.animations;
    std::vector<mat4> rest;
    if (!data.skeleton.empty()) detail::rest_skin_matrices(data.skeleton, rest);
    for (const ModelData::Part& part : data.parts) {
        MeshRecord m;
        m.cpu = part.mesh;
        const bool skinned = !rest.empty() && std::any_of(part.mesh.vertices.begin(), part.mesh.vertices.end(), [](const Vertex& v) {
            return v.weights.x + v.weights.y + v.weights.z + v.weights.w > 0.0f;
        });
        if (skinned) {
            // Keep the modeled vertices for posing; draw (and collide with)
            // the rest pose until an Animator says otherwise.
            m.bind = part.mesh.vertices;
            skin_vertices(m.bind, rest, m.cpu.vertices);
        }
        m.bounds = m.cpu.bounds();
        m.positions.reserve(m.cpu.vertices.size());
        for (const Vertex& v : m.cpu.vertices) m.positions.push_back(v.position);
        m.triangles = part.mesh.indices;
        const Bounds placed = m.bounds.transformed(part.transform);
        if (placed.valid()) { rec.bounds.add(placed.min); rec.bounds.add(placed.max); }
        rec.parts.push_back({static_cast<int>(rec.meshes.size()), part.material, part.transform});
        rec.meshes.push_back(std::move(m));
    }
    return rec;
}
} // namespace

void unload_model(Model& model) {
    if (ModelRecord* rec = model_record(model)) {
        release_meshes(*rec);
        *rec = ModelRecord{};
        rec->alive = false;
    }
    model = Model{};
}

Bounds model_bounds(Model model) {
    const ModelRecord* rec = model_record(model);
    return rec ? rec->bounds : Bounds{};
}

int model_part_count(Model model) {
    const ModelRecord* rec = model_record(model);
    return rec ? static_cast<int>(rec->parts.size()) : 0;
}

Material model_material(Model model, int part) {
    const ModelRecord* rec = model_record(model);
    if (!rec || part < 0 || part >= static_cast<int>(rec->parts.size())) return {};
    return rec->parts[part].material;
}

void set_model_material(Model model, const Material& material, int part) {
    ModelRecord* rec = model_record(model);
    if (!rec) return;
    for (int i = 0; i < static_cast<int>(rec->parts.size()); ++i) {
        if (part < 0 || part == i) rec->parts[i].material = material;
    }
}

RaycastHit raycast(const Ray& ray, Model model, const Transform& transform, float max_distance) {
    RaycastHit best;
    const ModelRecord* rec = model_record(model);
    if (!rec) return best;
    const mat4 model_matrix = transform.matrix();
    for (const PartRecord& part : rec->parts) {
        const MeshRecord& mesh = rec->meshes[part.mesh];
        const mat4 world = model_matrix * part.local;
        const float limit = best.hit ? best.distance : max_distance;
        if (!detail::ray_reaches_box(ray, mesh.bounds.transformed(world), limit)) continue;
        // Triangles are tested in world space, so hit distances stay in the
        // caller's units even under non-uniform scale.
        for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
            const vec3 a = world.transform_point(mesh.positions[mesh.triangles[i]]);
            const vec3 b = world.transform_point(mesh.positions[mesh.triangles[i + 1]]);
            const vec3 c = world.transform_point(mesh.positions[mesh.triangles[i + 2]]);
            const RaycastHit h = raycast_triangle(ray, a, b, c, best.hit ? best.distance : max_distance);
            if (h.hit) best = h;
        }
    }
    return best;
}

const Skeleton* model_skeleton(Model model) {
    const ModelRecord* rec = model_record(model);
    return rec && !rec->skeleton.empty() ? &rec->skeleton : nullptr;
}

int model_animation_count(Model model) {
    const ModelRecord* rec = model_record(model);
    return rec ? static_cast<int>(rec->animations.size()) : 0;
}

const AnimationClip* model_animation(Model model, int index) {
    const ModelRecord* rec = model_record(model);
    if (!rec || index < 0 || index >= static_cast<int>(rec->animations.size())) return nullptr;
    return &rec->animations[static_cast<size_t>(index)];
}

int find_animation(Model model, const std::string& name) {
    const ModelRecord* rec = model_record(model);
    if (!rec) return -1;
    for (size_t i = 0; i < rec->animations.size(); ++i) {
        if (rec->animations[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

} // namespace thistle::three

namespace thistle::detail {

using namespace thistle::three;

void model_world_triangles(Model model, const mat4& transform, std::vector<vec3>& out) {
    const ModelRecord* rec = model_record(model);
    if (!rec) return;
    for (const PartRecord& part : rec->parts) {
        const MeshRecord& mesh = rec->meshes[part.mesh];
        const mat4 m = transform * part.local;
        for (uint32_t idx : mesh.triangles) out.push_back(m.transform_point(mesh.positions[idx]));
    }
}

void replace_model(Model& model, const ModelData& data) {
    ModelRecord* rec = model_record(model);
    if (data.parts.empty()) {
        if (rec) unload_model(model);
        return;
    }
    if (!rec) {
        model = make_model(data);
        return;
    }
    release_meshes(*rec);
    *rec = build_record(data);
}

} // namespace thistle::detail
