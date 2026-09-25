// three::Scene3D: the level format the Thistle Editor writes, and the code a
// game uses to read and draw it. JSON, so it diffs well in version control
// and a person can fix one by hand.
#include "thistle_internal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace thistle::three {

namespace {

constexpr int kFormat = 1;

const char* kind_name(SceneEntity::Kind k) {
    switch (k) {
        case SceneEntity::Kind::Empty: return "empty";
        case SceneEntity::Kind::Model: return "model";
        case SceneEntity::Kind::Box: return "box";
        case SceneEntity::Kind::Sphere: return "sphere";
        case SceneEntity::Kind::Cylinder: return "cylinder";
        case SceneEntity::Kind::Cone: return "cone";
        case SceneEntity::Kind::Plane: return "plane";
        case SceneEntity::Kind::PointLight: return "point_light";
        case SceneEntity::Kind::SpotLight: return "spot_light";
        case SceneEntity::Kind::Trigger: return "trigger";
        case SceneEntity::Kind::Spawn: return "spawn";
        case SceneEntity::Kind::Voxels: return "voxels";
    }
    return "empty";
}

SceneEntity::Kind kind_from(const std::string& s) {
    for (int k = 0; k <= static_cast<int>(SceneEntity::Kind::Voxels); ++k) {
        if (s == kind_name(static_cast<SceneEntity::Kind>(k))) return static_cast<SceneEntity::Kind>(k);
    }
    return SceneEntity::Kind::Empty;
}

// Models are shared between every scene (and every reload of one): the
// editor reloads the scene on every undo.
// Keyed by the full path: scene files use paths relative to the working
// directory, so "assets/models/tree.glb" is a different file after a
// program (the editor, switching projects) changes it.
std::string cache_key(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::absolute(path, ec);
    return ec ? path : full.lexically_normal().generic_string();
}

Model cached_model(const std::string& path) {
    static std::unordered_map<std::string, Model> cache;
    const std::string key = cache_key(path);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, load_model(path)).first;
    return it->second;
}

// Block atlases likewise: several block objects usually share one.
Texture cached_texture(const std::string& path) {
    static std::unordered_map<std::string, Texture> cache;
    const std::string key = cache_key(path);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, load_texture(path)).first;
    return it->second;
}

template <class T>
T get_or(const nlohmann::json& j, const char* key, const T& fallback) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}

} // namespace

std::string SceneEntity::property(const std::string& key, const std::string& fallback) const {
    for (const auto& [k, v] : properties) {
        if (k == key) return v;
    }
    return fallback;
}

void SceneEntity::set_property(const std::string& key, const std::string& value) {
    for (auto& [k, v] : properties) {
        if (k == key) { v = value; return; }
    }
    properties.emplace_back(key, value);
}

std::string Scene3D::to_json(bool voxel_blocks) const {
    nlohmann::json j;
    j["thistle_scene"] = kFormat;
    j["environment"] = {
        {"sun", {{"direction", sun.direction}, {"color", sun.color}, {"intensity", sun.intensity}, {"shadows", sun.shadows},
                 {"shadow_distance", sun.shadow_distance}, {"shadow_strength", sun.shadow_strength}}},
        {"sky", {{"top", sky.top}, {"horizon", sky.horizon}, {"ground", sky.ground}, {"visible", sky.visible}, {"sun_disc", sky.sun_disc}}},
        {"fog", {{"enabled", fog.enabled}, {"start", fog.start}, {"end", fog.end}, {"color", fog.color}, {"match_sky", fog.match_sky}}},
        {"ambient", ambient},
    };
    nlohmann::json list = nlohmann::json::array();
    for (const SceneEntity& e : entities) {
        nlohmann::json je = {{"name", e.name}, {"kind", kind_name(e.kind)}, {"parent", e.parent}, {"position", e.transform.position},
                             {"rotation", e.transform.rotation}, {"scale", e.transform.scale}, {"color", e.color}};
        if (e.kind == SceneEntity::Kind::Model) je["model"] = e.model;
        if (e.kind == SceneEntity::Kind::PointLight || e.kind == SceneEntity::Kind::SpotLight) {
            je["intensity"] = e.intensity;
            je["range"] = e.range;
        }
        if (e.kind == SceneEntity::Kind::SpotLight) je["spot_angle"] = e.spot_angle;
        if (e.kind == SceneEntity::Kind::Voxels) {
            if (voxel_blocks) je["blocks"] = e.voxels ? detail::b64_encode(e.voxels->serialize()) : std::string();
            if (!e.atlas.empty()) {
                je["atlas"] = e.atlas;
                je["atlas_tile"] = e.atlas_tile;
            }
        }
        if (!e.properties.empty()) {
            nlohmann::json props = nlohmann::json::array();
            for (const auto& [k, v] : e.properties) props.push_back({k, v}); // an array keeps their order
            je["properties"] = props;
        }
        list.push_back(je);
    }
    j["entities"] = list;
    return j.dump(2);
}

bool Scene3D::from_json(const std::string& text) {
    *this = Scene3D{};
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (...) {
        return false;
    }
    if (!j.is_object() || !j.contains("thistle_scene") || !j.contains("entities")) return false;
    const nlohmann::json env = j.value("environment", nlohmann::json::object());
    const nlohmann::json js = env.value("sun", nlohmann::json::object());
    sun.direction = get_or(js, "direction", sun.direction);
    sun.color = get_or(js, "color", sun.color);
    sun.intensity = get_or(js, "intensity", sun.intensity);
    sun.shadows = get_or(js, "shadows", sun.shadows);
    sun.shadow_distance = get_or(js, "shadow_distance", sun.shadow_distance);
    sun.shadow_strength = get_or(js, "shadow_strength", sun.shadow_strength);
    const nlohmann::json jk = env.value("sky", nlohmann::json::object());
    sky.top = get_or(jk, "top", sky.top);
    sky.horizon = get_or(jk, "horizon", sky.horizon);
    sky.ground = get_or(jk, "ground", sky.ground);
    sky.visible = get_or(jk, "visible", sky.visible);
    sky.sun_disc = get_or(jk, "sun_disc", sky.sun_disc);
    const nlohmann::json jf = env.value("fog", nlohmann::json::object());
    fog.enabled = get_or(jf, "enabled", fog.enabled);
    fog.start = get_or(jf, "start", fog.start);
    fog.end = get_or(jf, "end", fog.end);
    fog.color = get_or(jf, "color", fog.color);
    fog.match_sky = get_or(jf, "match_sky", fog.match_sky);
    ambient = get_or(env, "ambient", ambient);
    for (const nlohmann::json& je : j["entities"]) {
        if (!je.is_object()) continue;
        SceneEntity e;
        e.name = get_or<std::string>(je, "name", "");
        e.kind = kind_from(get_or<std::string>(je, "kind", "empty"));
        e.parent = get_or(je, "parent", -1);
        e.transform.position = get_or(je, "position", vec3{});
        // Only fix rotations that aren't unit length: renormalizing a good
        // one nudges its last digits, and then saving what was loaded
        // wouldn't give back the same file (noise in every version-control diff).
        const quat r = get_or(je, "rotation", quat{});
        const float len2 = r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w;
        e.transform.rotation = std::fabs(len2 - 1.0f) > 1e-4f ? normalize(r) : r;
        e.transform.scale = get_or(je, "scale", vec3{1, 1, 1});
        e.model = get_or<std::string>(je, "model", "");
        e.color = get_or(je, "color", white);
        e.intensity = get_or(je, "intensity", e.intensity);
        e.range = get_or(je, "range", e.range);
        e.spot_angle = get_or(je, "spot_angle", e.spot_angle);
        if (e.kind == SceneEntity::Kind::Voxels) {
            e.voxels = std::make_shared<VoxelWorld>();
            const std::vector<uint8_t> bytes = detail::b64_decode(get_or<std::string>(je, "blocks", ""));
            if (!bytes.empty() && !e.voxels->deserialize(bytes.data(), bytes.size())) {
                log_warn("Scene3D: the blocks of '" + e.name + "' didn't load (damaged data); it's empty");
            }
            e.atlas = get_or<std::string>(je, "atlas", "");
            e.atlas_tile = std::max(1, get_or(je, "atlas_tile", 16));
            if (!e.atlas.empty()) e.voxels->set_atlas(cached_texture(e.atlas), e.atlas_tile);
        }
        if (auto p = je.find("properties"); p != je.end() && p->is_array()) {
            for (const auto& kv : *p) {
                if (kv.is_array() && kv.size() == 2 && kv[0].is_string() && kv[1].is_string()) {
                    e.properties.emplace_back(kv[0].get<std::string>(), kv[1].get<std::string>());
                }
            }
        }
        entities.push_back(std::move(e));
    }
    // A parent must exist and not lead back around to the entity itself.
    const int n = static_cast<int>(entities.size());
    for (int i = 0; i < n; ++i) {
        int& p = entities[static_cast<size_t>(i)].parent;
        if (p < 0 || p >= n || p == i) { p = -1; continue; }
        int walk = p, steps = 0;
        while (walk >= 0 && steps++ <= n) {
            if (walk == i) { p = -1; break; }
            walk = entities[static_cast<size_t>(walk)].parent;
            if (walk >= n) break;
        }
    }
    place_voxels();
    return true;
}

bool Scene3D::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << to_json();
    return static_cast<bool>(out);
}

bool Scene3D::load(const std::string& path) {
    std::string text;
    if (!detail::read_file_text(path, text)) {
        *this = Scene3D{};
        return false;
    }
    return from_json(text);
}

int Scene3D::add(const SceneEntity& entity) {
    entities.push_back(entity);
    int& p = entities.back().parent;
    if (p >= static_cast<int>(entities.size()) - 1) p = -1;
    return static_cast<int>(entities.size()) - 1;
}

void Scene3D::remove(int index) { remove(std::vector<int>{index}); }

void Scene3D::remove(const std::vector<int>& indices) {
    const int n = static_cast<int>(entities.size());
    std::vector<uint8_t> gone(static_cast<size_t>(n), 0);
    bool any = false;
    for (int index : indices) {
        if (index < 0 || index >= n) continue;
        gone[static_cast<size_t>(index)] = 1;
        any = true;
    }
    if (!any) return;
    // Descendants: repeat until nothing new is marked (parents can come after children).
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            const int p = entities[static_cast<size_t>(i)].parent;
            if (!gone[static_cast<size_t>(i)] && p >= 0 && p < n && gone[static_cast<size_t>(p)]) {
                gone[static_cast<size_t>(i)] = 1;
                changed = true;
            }
        }
    }
    std::vector<int> remap(static_cast<size_t>(n), -1);
    std::vector<SceneEntity> kept;
    for (int i = 0; i < n; ++i) {
        if (gone[static_cast<size_t>(i)]) continue;
        remap[static_cast<size_t>(i)] = static_cast<int>(kept.size());
        kept.push_back(std::move(entities[static_cast<size_t>(i)]));
    }
    for (SceneEntity& e : kept) e.parent = e.parent >= 0 ? remap[static_cast<size_t>(e.parent)] : -1;
    entities = std::move(kept);
}

bool Scene3D::set_parent(int child, int parent) {
    const int n = static_cast<int>(entities.size());
    if (child < 0 || child >= n || parent >= n) return false;
    for (int walk = parent, steps = 0; walk >= 0 && steps <= n; walk = entities[static_cast<size_t>(walk)].parent, ++steps) {
        if (walk == child) return false; // under itself
    }
    const Transform world = world_transform(child);
    entities[static_cast<size_t>(child)].parent = parent < 0 ? -1 : parent;
    set_world_transform(child, world);
    return true;
}

std::vector<int> Scene3D::children(int index) const {
    std::vector<int> out;
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].parent == index) out.push_back(static_cast<int>(i));
    }
    return out;
}

int Scene3D::find(const std::string& name) const {
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Transform Scene3D::world_transform(int index) const {
    const int n = static_cast<int>(entities.size());
    if (index < 0 || index >= n) return {};
    Transform t = entities[static_cast<size_t>(index)].transform;
    int p = entities[static_cast<size_t>(index)].parent;
    for (int steps = 0; p >= 0 && p < n && steps < n; ++steps) {
        t = entities[static_cast<size_t>(p)].transform * t;
        p = entities[static_cast<size_t>(p)].parent;
    }
    return t;
}

void Scene3D::set_world_transform(int index, const Transform& world) {
    if (index < 0 || index >= static_cast<int>(entities.size())) return;
    SceneEntity& e = entities[static_cast<size_t>(index)];
    if (e.parent < 0) {
        e.transform = world;
        return;
    }
    // The exact inverse of Transform's parent * child.
    const Transform p = world_transform(e.parent);
    const quat inv = p.rotation.inverse();
    auto safe_div = [](float a, float b) { return std::fabs(b) > 1e-8f ? a / b : a; };
    const vec3 local = inv * (world.position - p.position);
    e.transform.position = {safe_div(local.x, p.scale.x), safe_div(local.y, p.scale.y), safe_div(local.z, p.scale.z)};
    e.transform.rotation = normalize(inv * world.rotation);
    e.transform.scale = {safe_div(world.scale.x, p.scale.x), safe_div(world.scale.y, p.scale.y), safe_div(world.scale.z, p.scale.z)};
}

Bounds Scene3D::local_bounds(int index) const {
    if (index < 0 || index >= static_cast<int>(entities.size())) return {};
    const SceneEntity& e = entities[static_cast<size_t>(index)];
    switch (e.kind) {
        case SceneEntity::Kind::Model: {
            const Bounds b = model_bounds(model(index));
            return b.valid() ? b : Bounds{{-0.25f, -0.25f, -0.25f}, {0.25f, 0.25f, 0.25f}};
        }
        case SceneEntity::Kind::Box:
        case SceneEntity::Kind::Sphere:
        case SceneEntity::Kind::Cylinder:
        case SceneEntity::Kind::Cone:
        case SceneEntity::Kind::Trigger: return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        case SceneEntity::Kind::Plane: return {{-0.5f, -0.01f, -0.5f}, {0.5f, 0.01f, 0.5f}};
        case SceneEntity::Kind::Spawn: return {{-0.25f, 0.0f, -0.25f}, {0.25f, 1.8f, 0.25f}}; // a person standing there
        case SceneEntity::Kind::Voxels: {
            const Bounds b = e.voxels ? e.voxels->grid_bounds() : Bounds{};
            return b.valid() ? b : Bounds{{0, 0, 0}, {0.5f, 0.5f, 0.5f}};
        }
        default: return {{-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}};
    }
}

bool Scene3D::inside(int index, vec3 point) const {
    if (index < 0 || index >= static_cast<int>(entities.size())) return false;
    const Transform t = world_transform(index);
    const vec3 local = t.rotation.inverse() * (point - t.position);
    const Bounds b = local_bounds(index);
    const float p[3] = {local.x, local.y, local.z}, s[3] = {t.scale.x, t.scale.y, t.scale.z};
    const float lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(s[a]) < 1e-6f) return false; // squashed flat: nothing is inside
        const float v = p[a] / s[a];
        if (v < lo[a] || v > hi[a]) return false;
    }
    return true;
}

Model Scene3D::model(int index) const {
    if (index < 0 || index >= static_cast<int>(entities.size())) return {};
    const SceneEntity& e = entities[static_cast<size_t>(index)];
    if (e.kind != SceneEntity::Kind::Model || e.model.empty()) return {};
    return cached_model(e.model);
}

void Scene3D::place_voxels() {
    for (size_t i = 0; i < entities.size(); ++i) {
        SceneEntity& e = entities[i];
        if (e.kind != SceneEntity::Kind::Voxels || !e.voxels) continue;
        const Transform t = world_transform(static_cast<int>(i));
        e.voxels->origin = t.position;
        e.voxels->rotation = t.rotation;
    }
}

std::vector<int> Scene3D::add_colliders(CollisionWorld& world) const {
    // The same unit shapes World::box() and friends draw, as models the
    // collision world can read triangles from. Made once, shared.
    static Model shapes[5];
    static bool made = false;
    if (!made) {
        shapes[0] = make_model(box_mesh());
        shapes[1] = make_model(sphere_mesh());
        shapes[2] = make_model(cylinder_mesh());
        shapes[3] = make_model(cone_mesh());
        shapes[4] = make_model(plane_mesh());
        made = true;
    }
    std::vector<int> ids;
    for (size_t i = 0; i < entities.size(); ++i) {
        const SceneEntity& e = entities[i];
        if (e.property("solid") == "false") continue;
        const Transform t = world_transform(static_cast<int>(i));
        switch (e.kind) {
            case SceneEntity::Kind::Box: ids.push_back(world.add(shapes[0], t)); break;
            case SceneEntity::Kind::Sphere: ids.push_back(world.add(shapes[1], t)); break;
            case SceneEntity::Kind::Cylinder: ids.push_back(world.add(shapes[2], t)); break;
            case SceneEntity::Kind::Cone: ids.push_back(world.add(shapes[3], t)); break;
            case SceneEntity::Kind::Plane: ids.push_back(world.add(shapes[4], t)); break;
            case SceneEntity::Kind::Model:
                if (Model m = model(static_cast<int>(i)); m.valid()) ids.push_back(world.add(m, t));
                break;
            case SceneEntity::Kind::Voxels:
                if (e.voxels) {
                    if (std::fabs(std::fabs(t.rotation.w) - 1.0f) > 1e-4f) {
                        log_warn("Scene3D::add_colliders: '" + e.name + "' is turned, but its blocks collide as if it weren't");
                    }
                    e.voxels->origin = t.position; // (place_voxels() isn't const)
                    e.voxels->rotation = t.rotation;
                    ids.push_back(world.add(*e.voxels));
                }
                break;
            default: break;
        }
    }
    return ids;
}

void Scene3D::draw(World& world, bool environment) const {
    if (environment) {
        const Skybox keep = world.sky.skybox; // not part of the file: leave whatever the game set
        world.sun = sun;
        world.sky = sky;
        world.sky.skybox = keep;
        world.fog = fog;
        world.ambient = ambient;
    }
    for (size_t i = 0; i < entities.size(); ++i) {
        const SceneEntity& e = entities[i];
        const Transform t = world_transform(static_cast<int>(i));
        switch (e.kind) {
            case SceneEntity::Kind::Model: world.draw(model(static_cast<int>(i)), t, e.color); break;
            case SceneEntity::Kind::Box: world.box(t, e.color); break;
            case SceneEntity::Kind::Sphere: world.sphere(t, e.color); break;
            case SceneEntity::Kind::Cylinder: world.cylinder(t, e.color); break;
            case SceneEntity::Kind::Cone: world.cone(t, e.color); break;
            case SceneEntity::Kind::Plane: world.plane(t, e.color); break;
            case SceneEntity::Kind::Voxels:
                if (e.voxels) {
                    e.voxels->origin = t.position; // as place_voxels(), which isn't const
                    e.voxels->rotation = t.rotation;
                    world.draw(*e.voxels);
                }
                break;
            case SceneEntity::Kind::PointLight: world.light(PointLight{t.position, e.color, e.range, e.intensity}); break;
            case SceneEntity::Kind::SpotLight: {
                SpotLight l;
                l.position = t.position;
                l.direction = t.rotation.forward();
                l.color = e.color;
                l.range = e.range;
                l.intensity = e.intensity;
                l.angle = e.spot_angle;
                world.light(l);
                break;
            }
            default: break; // triggers, spawns, empties: data for the game
        }
    }
}

} // namespace thistle::three
