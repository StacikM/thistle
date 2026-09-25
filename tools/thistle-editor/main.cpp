// Thistle Editor — a 3D level editor for thistle::three scenes (Scene3D).
//
// Place models, shapes, block objects, lights, trigger volumes and spawn
// points; move, rotate and scale them with gizmos; group them into
// hierarchies; give them properties your game reads; set the sun, sky and
// fog; save as .scene.json, which a game loads with Scene3D::load() and
// draws with Scene3D::draw() (or reads to place its own things).
//
// Block objects (voxels) are edited in block mode (Tab): add, erase and
// paint with a brush or a box, a stroke staying in the layer it started
// in. Files dropped on the window (or typed into Import) are brought into
// the project: models copied into assets/models/ with the files they
// refer to, .vox files turned into block objects.
//
//   thistle-editor [project folder]
//
// It works inside a project: models come from its assets/ folder (.glb,
// .gltf, .obj), scenes are saved to assets/scenes/, so they ship with the
// game like everything else in assets/. It starts on the projects screen
// (hub.cpp: recent projects, New project, Open folder), or straight in the
// project given on the command line or the one it was started in (a
// folder with a thistle.json). If the editor
// that came before this one (a prop placer that saved a Node tree to one
// file in the save folder) left a layout behind, Open offers to import it.
//
// Navigation is Blender's (middle-drag orbits, Shift+middle-drag pans, the
// wheel zooms; Alt+left-drag orbits too, for laptops) plus Unity's/Unreal's
// fly mode (hold the right button: mouse looks, WASD/QE move). F frames the
// selection, numpad 1/3/7 look from the front/right/top. W/E/R pick the
// move/rotate/scale gizmo; holding Ctrl while dragging one snaps. Every
// change can be undone (Ctrl+Z / Ctrl+Shift+Z): the whole scene is
// snapshotted as its JSON before each change, which is simple enough to be
// obviously right, and small enough at editor-sized scenes.
//
// The UI is drawn with Thistle's own 2D API (no Dear ImGui): the editor
// should look like a tool made with the engine, not a debug overlay.
#include <thistle.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hub.hpp"
#include "platform.hpp"
#include "theme.hpp"

using namespace thistle;
using namespace thistle::three;
namespace fs = std::filesystem;

namespace {

using editor::executable_dir;

namespace theme = editor::theme;

const char* kind_label(SceneEntity::Kind k) {
    switch (k) {
        case SceneEntity::Kind::Empty: return "Empty";
        case SceneEntity::Kind::Model: return "Model";
        case SceneEntity::Kind::Box: return "Box";
        case SceneEntity::Kind::Sphere: return "Sphere";
        case SceneEntity::Kind::Cylinder: return "Cylinder";
        case SceneEntity::Kind::Cone: return "Cone";
        case SceneEntity::Kind::Plane: return "Plane";
        case SceneEntity::Kind::PointLight: return "Point light";
        case SceneEntity::Kind::SpotLight: return "Spot light";
        case SceneEntity::Kind::Trigger: return "Trigger";
        case SceneEntity::Kind::Spawn: return "Spawn point";
        case SceneEntity::Kind::Voxels: return "Blocks";
    }
    return "?";
}

std::string fmt(float v, int decimals = 2) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, static_cast<double>(v));
    std::string s = buf;
    if (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos) s.erase(0, 1); // "-0.0" -> "0.0"
    return s;
}

float parse_float(const std::string& s, float fallback) {
    try {
        size_t used = 0;
        const float v = std::stof(s, &used);
        return used > 0 ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Every file under `dir` (recursively) with one of these extensions, as
// paths relative to the project folder — the form saved into scenes.
std::vector<std::string> scan(const fs::path& dir, std::initializer_list<const char*> exts) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        const std::string name = it->path().filename().string();
        for (const char* ext : exts) {
            const std::string e = ext;
            if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) {
                out.push_back(fs::relative(it->path(), fs::current_path(), ec).generic_string());
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Closest approach between a ray and an infinite line: the parameter along
// the line (from `origin`, in units of `dir`).
float ray_line_param(const Ray& ray, vec3 origin, vec3 dir) {
    const vec3 w = origin - ray.origin;
    const float a = dot(dir, dir), b = dot(dir, ray.direction), c = dot(ray.direction, ray.direction);
    const float d = dot(dir, w), e = dot(ray.direction, w);
    const float denom = a * c - b * b;
    if (std::fabs(denom) < 1e-8f) return 0.0f; // parallel
    return (b * e - c * d) / denom;
}

float point_segment_distance(vec2 p, vec2 a, vec2 b) {
    const vec2 ab{b.x - a.x, b.y - a.y};
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    float t = len2 > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = p.x - (a.x + ab.x * t), dy = p.y - (a.y + ab.y * t);
    return std::sqrt(dx * dx + dy * dy);
}

bool point_in_triangle(vec2 p, vec2 a, vec2 b, vec2 c) {
    auto side = [](vec2 p1, vec2 p2, vec2 p3) { return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y); };
    const float d1 = side(p, a, b), d2 = side(p, b, c), d3 = side(p, c, a);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

bool detail_read(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool same_file_contents(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
}

// "My%20Model.bin" -> "My Model.bin": glTF URIs are percent-encoded.
std::string uri_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The files a model file refers to, relative to its folder: a .gltf's
// buffers and images, an .obj's material libraries and their textures.
std::vector<std::string> model_dependencies(const fs::path& file) {
    std::vector<std::string> out;
    const std::string ext = lower(file.extension().string());
    std::string text;
    if (!detail_read(file, text)) return out;
    if (ext == ".gltf") {
        try {
            const nlohmann::json j = nlohmann::json::parse(text);
            for (const char* list : {"buffers", "images"}) {
                if (!j.contains(list)) continue;
                for (const auto& item : j[list]) {
                    const std::string uri = item.value("uri", std::string());
                    if (!uri.empty() && uri.rfind("data:", 0) != 0) out.push_back(uri_decode(uri));
                }
            }
        } catch (...) {
        }
    } else if (ext == ".obj") {
        std::istringstream lines(text);
        std::string line;
        std::vector<std::string> mtls;
        while (std::getline(lines, line)) {
            std::istringstream words(line);
            std::string word;
            words >> word;
            if (word != "mtllib") continue;
            std::string rest;
            std::getline(words, rest);
            rest.erase(0, rest.find_first_not_of(" \t"));
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
            if (!rest.empty()) mtls.push_back(rest); // one name, possibly with spaces (what most exporters write)
        }
        for (const std::string& mtl : mtls) {
            out.push_back(mtl);
            std::string mtext;
            if (!detail_read(file.parent_path() / mtl, mtext)) continue;
            std::istringstream mlines(mtext);
            while (std::getline(mlines, line)) {
                std::istringstream words(line);
                std::string key;
                words >> key;
                static const char* maps[] = {"map_Kd", "map_Ka", "map_Ks", "map_Ke", "map_Ns", "map_d", "map_Bump", "map_bump", "bump", "norm", "disp"};
                if (std::find_if(std::begin(maps), std::end(maps), [&](const char* m) { return key == m; }) == std::end(maps)) continue;
                std::string last, word; // options like "-bm 0.5" come first: the file name is the last word
                while (words >> word) last = word;
                if (!last.empty()) out.push_back(last);
            }
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path editor_dir = executable_dir();
    // A folder on the command line is the game to edit (Finder may add a
    // "-psn_..." argument to an app it launches; that isn't one), else the
    // project it was started in. Neither: the projects screen.
    std::optional<fs::path> start_project;
    {
        std::error_code ec;
        const fs::path cwd = fs::current_path(ec);
        if (argc >= 2 && std::string(argv[1]).rfind("-psn", 0) != 0) start_project = fs::absolute(argv[1], ec);
        else if (!ec && fs::exists(cwd / "thistle.json", ec)) start_project = cwd;
    }

    App app{{.title = "Thistle Editor", .width = 1440, .height = 860}};
    load_font((editor_dir / "editor_assets" / "inter-regular.ttf").string());

    // The projects screen, and which project is open.
    editor::Hub hub;
    bool in_hub = true;
    if (start_project) hub.request_open(*start_project); // checked like any other: straight in if it's a Thistle project
    fs::path project_root;
    std::string project_name;
    bool project_is_thistle = false;
    editor::BuildRunner runner;
    bool show_build_log = false;
    float build_log_scroll = 0.0f;

    // ---------------------------------------------------------------- state
    Scene3D scene;
    std::vector<int> selection;
    enum class Tool { Move, Rotate, Scale } tool = Tool::Move;
    std::string file_path;
    bool dirty = false;
    std::string status;
    double status_time = 0.0;
    auto say = [&](const std::string& s) {
        status = s;
        status_time = now_seconds();
        log_info("Thistle Editor: " + s);
    };

    // Camera: orbits `target` (Blender), or flies (right button held).
    vec3 target{0, 0.5f, 0};
    float yaw = radians(35.0f), pitch = radians(-28.0f), distance = 14.0f;
    bool flying = false;
    Camera camera;

    // Undo. A step is either a copy of the whole scene (entities, settings),
    // or edits to one block object's blocks: copies of the scene share their
    // VoxelWorlds (shared_ptr), so blocks aren't copied with every step,
    // and a brush stroke keeps only the blocks it changed. That works because
    // steps are undone strictly in reverse: when a scene copy comes back,
    // every block edit made after it has already been undone in the worlds it
    // shares.
    struct BlockEdit {
        ivec3 at;
        BlockId before;
    };
    struct UndoStep {
        std::optional<Scene3D> scene;
        std::shared_ptr<VoxelWorld> world;
        std::vector<BlockEdit> blocks;
        std::vector<std::pair<BlockId, BlockType>> types; // block types to put back
        float voxel_size = 0.0f;                          // > 0: the block size to put back
    };
    std::vector<UndoStep> undo_stack, redo_stack;
    Scene3D edit_before; // snapshot taken when a drag/edit began
    bool editing = false;
    auto push_undo = [&](UndoStep step) {
        undo_stack.push_back(std::move(step));
        if (undo_stack.size() > 200) undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
        dirty = true;
    };
    auto record = [&] { // call right before a one-shot change
        UndoStep step;
        step.scene = scene;
        push_undo(std::move(step));
    };
    auto begin_edit = [&] {
        if (editing) return;
        edit_before = scene;
        editing = true;
    };
    auto end_edit = [&] {
        if (!editing) return;
        editing = false;
        if (scene.to_json(false) == edit_before.to_json(false)) return; // a click without a change
        UndoStep step;
        step.scene = std::move(edit_before);
        push_undo(std::move(step));
    };
    // Puts a step's state back and returns the step that goes the other way.
    auto apply_step = [&](UndoStep step) {
        UndoStep back;
        if (step.scene) {
            back.scene = scene;
            scene = std::move(*step.scene);
        }
        if (step.world) {
            VoxelWorld& w = *step.world;
            back.world = step.world;
            for (const BlockEdit& b : step.blocks) {
                back.blocks.push_back({b.at, w.get(b.at)});
                w.set(b.at, b.before);
            }
            for (const auto& [id, type] : step.types) {
                back.types.push_back({id, w.block_type(id)});
                w.set_block_type(id, type);
            }
            if (step.voxel_size > 0.0f) {
                back.voxel_size = w.voxel_size;
                w.voxel_size = step.voxel_size;
            }
        }
        return back;
    };
    auto clamp_selection = [&] {
        const int n = static_cast<int>(scene.entities.size());
        selection.erase(std::remove_if(selection.begin(), selection.end(), [&](int i) { return i < 0 || i >= n; }), selection.end());
    };
    // Atlases are applied to the (shared) worlds only when an entity's
    // atlas setting differs from what its world has: set_atlas re-meshes
    // everything, which shouldn't happen on every undo.
    std::unordered_map<const VoxelWorld*, std::pair<std::string, int>> applied_atlas;
    auto sync_atlases = [&] {
        for (SceneEntity& e : scene.entities) {
            if (e.kind != SceneEntity::Kind::Voxels || !e.voxels) continue;
            auto it = applied_atlas.find(e.voxels.get());
            if (it == applied_atlas.end()) { // first seen: as loaded (Scene3D::load applies atlases) or made here
                applied_atlas[e.voxels.get()] = {e.atlas, e.atlas_tile};
                continue;
            }
            if (it->second.first == e.atlas && it->second.second == e.atlas_tile) continue;
            it->second = {e.atlas, e.atlas_tile};
            e.voxels->set_atlas(e.atlas.empty() ? Texture{} : load_texture(e.atlas), e.atlas_tile);
        }
    };
    bool block_mode = false; // editing the selected block object's blocks (Tab)
    bool frame_opened = false; // a scene was just opened: point the camera at it on the next frame
    auto undo = [&] {
        if (undo_stack.empty()) return say("nothing to undo");
        UndoStep step = std::move(undo_stack.back());
        undo_stack.pop_back();
        redo_stack.push_back(apply_step(std::move(step)));
        clamp_selection();
        sync_atlases();
        dirty = true;
    };
    auto redo = [&] {
        if (redo_stack.empty()) return say("nothing to redo");
        UndoStep step = std::move(redo_stack.back());
        redo_stack.pop_back();
        undo_stack.push_back(apply_step(std::move(step)));
        clamp_selection();
        sync_atlases();
        dirty = true;
    };

    std::vector<std::string> model_files = scan("assets", {".glb", ".gltf", ".obj"});
    std::vector<std::string> scene_files, vox_files, image_files;
    auto rescan = [&] {
        model_files = scan("assets", {".glb", ".gltf", ".obj"});
        vox_files = scan("assets", {".vox"});
        image_files = scan("assets", {".png", ".jpg", ".jpeg"});
        // Scenes live in assets/ so they ship with the game (only assets/ is
        // bundled). Also any lying right in the project folder, but not deeper:
        // build folders hold far too many files to walk every time.
        scene_files = scan("assets", {".scene.json"});
        std::error_code ec;
        for (auto it = fs::directory_iterator(".", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(ec) && name.size() > 11 && name.compare(name.size() - 11, 11, ".scene.json") == 0) scene_files.push_back(name);
        }
    };
    rescan();

    auto unique_name = [&](const std::string& base) {
        if (scene.find(base) < 0) return base;
        for (int i = 2;; ++i) {
            const std::string n = base + " " + std::to_string(i);
            if (scene.find(n) < 0) return n;
        }
    };

    auto new_scene = [&] {
        scene = Scene3D{};
        SceneEntity ground;
        ground.name = "Ground";
        ground.kind = SceneEntity::Kind::Plane;
        ground.transform.scale = {30, 1, 30};
        ground.color = rgb(0.42f, 0.55f, 0.36f);
        scene.add(ground);
        SceneEntity start;
        start.name = "player_start";
        start.kind = SceneEntity::Kind::Spawn;
        start.color = rgb(0.3f, 0.9f, 0.4f);
        start.transform.position = {0, 0, 4};
        scene.add(start);
        selection.clear();
        file_path.clear();
        undo_stack.clear();
        redo_stack.clear();
        applied_atlas.clear();
        block_mode = false;
        dirty = false;
    };
    new_scene();

    auto save_to = [&](const std::string& path) {
        std::error_code ec;
        if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
        if (scene.save(path)) {
            file_path = path;
            dirty = false;
            say("saved " + path);
            rescan();
            if (!project_root.empty()) hub.remember(project_root, path);
        } else {
            say("couldn't save " + path);
        }
    };
    auto open_file = [&](const std::string& path) {
        Scene3D loaded;
        if (!loaded.load(path)) return say("couldn't read " + path + " as a scene");
        scene = std::move(loaded);
        file_path = path;
        selection.clear();
        undo_stack.clear();
        redo_stack.clear();
        applied_atlas.clear();
        block_mode = false;
        dirty = false;
        frame_opened = true;
        say("opened " + path);
        if (!project_root.empty()) hub.remember(project_root, path);
    };

    // The editor before this one placed minor-3D props as a Node tree, one
    // layout kept in the save folder. Offer to bring that across.
    const std::string old_layout = [] {
        const std::string s = save::path();
        return s.substr(0, s.find_last_of("/\\") + 1) + "thistle_editor_scene.json";
    }();
    const std::string import_label = "Import the old editor's layout";
    auto import_old_layout = [&] {
        std::unique_ptr<Node> root;
        try {
            root = load_scene(old_layout);
        } catch (const std::exception&) { // load_scene() throws on a malformed file
        }
        if (!root) return say("couldn't read the old layout (" + old_layout + ")");
        Scene3D s;
        // Node meshes turn X, then Y, then Z (Frame::mesh3d's sgl_rotate order).
        auto old_rotation = [](vec3 e) {
            return quat::axis_angle({1, 0, 0}, e.x) * quat::axis_angle({0, 1, 0}, e.y) * quat::axis_angle({0, 0, 1}, e.z);
        };
        std::function<void(const Node&, int)> walk = [&](const Node& n, int parent) {
            for (size_t i = 0; i < n.child_count(); ++i) {
                const Node& c = *n.child(i);
                SceneEntity e;
                e.parent = parent;
                e.color = c.mesh_tint;
                switch (c.mesh_prim) {
                    case Prim::Cube: e.kind = SceneEntity::Kind::Box; break;
                    case Prim::Sphere: e.kind = SceneEntity::Kind::Sphere; break;
                    case Prim::Cylinder: e.kind = SceneEntity::Kind::Cylinder; break;
                    case Prim::Cone: e.kind = SceneEntity::Kind::Cone; break;
                    case Prim::Plane: e.kind = SceneEntity::Kind::Plane; break;
                    case Prim::Trigger: e.kind = SceneEntity::Kind::Trigger; break;
                    case Prim::None: e.kind = c.mesh_path.empty() ? SceneEntity::Kind::Empty : SceneEntity::Kind::Model; break;
                }
                if (e.kind == SceneEntity::Kind::Model) {
                    std::error_code ec;
                    const fs::path rel = fs::path(c.mesh_path).is_absolute() ? fs::relative(c.mesh_path, fs::current_path(), ec) : fs::path(c.mesh_path);
                    e.model = (ec || rel.empty() ? fs::path(c.mesh_path) : rel).generic_string();
                }
                e.name = !c.name.empty() ? c.name : e.kind == SceneEntity::Kind::Model ? fs::path(c.mesh_path).stem().string() : kind_label(e.kind);
                const int idx = s.add(e);
                // Node trees add rotations per axis down the chain; Scene3D
                // composes them properly. Placing each one at the world
                // transform the old editor showed keeps things where they were.
                const WorldMeshTransform w = c.world_mesh_transform();
                s.set_world_transform(idx, Transform{w.pos, old_rotation(w.rotation), w.scale});
                walk(c, idx);
            }
        };
        walk(*root, -1);
        scene = std::move(s);
        file_path.clear();
        selection.clear();
        undo_stack.clear();
        redo_stack.clear();
        applied_atlas.clear();
        block_mode = false;
        dirty = true;
        say("imported " + std::to_string(scene.entities.size()) + " things from the old editor: Save as to keep them");
    };

    // Selected entities whose ancestors aren't also selected: moving a
    // parent already moves its children.
    auto selection_roots = [&] {
        std::vector<int> roots;
        std::unordered_set<int> sel(selection.begin(), selection.end());
        for (int i : selection) {
            bool covered = false;
            for (int p = scene.entities[static_cast<size_t>(i)].parent, steps = 0; p >= 0 && steps < 10000; p = scene.entities[static_cast<size_t>(p)].parent, ++steps) {
                if (sel.count(p)) { covered = true; break; }
            }
            if (!covered) roots.push_back(i);
        }
        return roots;
    };
    auto world_box = [&](int i) { // the entity's box in the world (axis-aligned around its oriented box)
        const Transform t = scene.world_transform(i);
        return scene.local_bounds(i).transformed(t.matrix());
    };
    auto selection_bounds = [&] {
        Bounds b;
        for (int i : selection) {
            const Bounds w = world_box(i);
            if (w.valid()) { b.add(w.min); b.add(w.max); }
        }
        return b;
    };

    auto add_entity = [&](SceneEntity e) {
        record();
        e.name = unique_name(e.name.empty() ? kind_label(e.kind) : e.name);
        // Dropped where the view is centered, resting on the ground.
        const vec3 at{target.x, 0.0f, target.z};
        Bounds lb;
        const int index = scene.add(e);
        lb = scene.local_bounds(index);
        const float lift = e.kind == SceneEntity::Kind::PointLight || e.kind == SceneEntity::Kind::SpotLight ? 3.0f
                           : lb.valid() ? -lb.min.y * e.transform.scale.y : 0.0f;
        scene.entities.back().transform.position = at + vec3{0, lift, 0};
        selection = {index};
    };
    auto add_kind = [&](SceneEntity::Kind k) {
        SceneEntity e;
        e.kind = k;
        switch (k) {
            case SceneEntity::Kind::Box: e.color = rgb(0.75f, 0.6f, 0.45f); break;
            case SceneEntity::Kind::Sphere: e.color = rgb(0.85f, 0.35f, 0.3f); break;
            case SceneEntity::Kind::Cylinder: e.color = rgb(0.4f, 0.6f, 0.85f); break;
            case SceneEntity::Kind::Cone: e.color = rgb(0.8f, 0.7f, 0.3f); break;
            case SceneEntity::Kind::Plane: e.color = rgb(0.6f, 0.6f, 0.6f); e.transform.scale = {4, 1, 4}; break;
            case SceneEntity::Kind::PointLight: e.color = rgb(1.0f, 0.85f, 0.6f); e.intensity = 2.0f; break;
            case SceneEntity::Kind::SpotLight:
                e.color = rgb(1.0f, 0.95f, 0.85f);
                e.intensity = 3.0f;
                e.range = 15.0f;
                e.transform.rotation = quat::euler(radians(-60.0f), 0);
                break;
            case SceneEntity::Kind::Trigger: e.color = rgb(0.25f, 0.9f, 0.85f); e.transform.scale = {3, 2, 3}; break;
            case SceneEntity::Kind::Spawn: e.color = rgb(0.3f, 0.9f, 0.4f); break;
            default: break;
        }
        add_entity(e);
    };
    auto add_model = [&](const std::string& path) {
        SceneEntity e;
        e.kind = SceneEntity::Kind::Model;
        e.model = path;
        e.name = fs::path(path).stem().string();
        add_entity(e);
    };

    enum class Popup { None, Open, Add, AddModel, AddVox, SaveAs, Import, Context, ChangeModel, Atlas, Confirm } popup = Popup::None;
    vec2 popup_at{};
    int context_entity = -1;
    // New/Open with unsaved changes asks first: they clear the undo history,
    // so there'd be no getting the changes back.
    std::string confirm_text;
    std::function<void()> confirm_action;
    auto unless_unsaved = [&](const std::string& what, std::function<void()> action) {
        if (!dirty) return action();
        confirm_text = what;
        confirm_action = std::move(action);
        popup = Popup::Confirm;
    };

    // Block objects. A new one comes with block types to paint with and a
    // small platform, so there's something to see and click.
    auto add_blocks = [&](std::shared_ptr<VoxelWorld> w, const std::string& name) {
        record();
        SceneEntity e;
        e.kind = SceneEntity::Kind::Voxels;
        e.name = unique_name(name);
        const Bounds gb = w->grid_bounds();
        // Centered under the view, standing on the ground.
        e.transform.position = vec3{target.x, 0.0f, target.z} - (gb.valid() ? vec3{gb.center().x, gb.min.y, gb.center().z} : vec3{});
        e.voxels = std::move(w);
        const int index = scene.add(e);
        selection = {index};
        return index;
    };
    auto new_block_object = [&] {
        auto w = std::make_shared<VoxelWorld>();
        w->voxel_size = 0.5f;
        w->add_block({.name = "stone", .color = rgb(0.52f, 0.52f, 0.54f)});
        w->add_block({.name = "dirt", .color = rgb(0.45f, 0.32f, 0.22f)});
        w->add_block({.name = "grass", .color = rgb(0.36f, 0.62f, 0.27f)});
        w->add_block({.name = "wood", .color = rgb(0.50f, 0.34f, 0.20f)});
        w->add_block({.name = "planks", .color = rgb(0.76f, 0.60f, 0.38f)});
        w->add_block({.name = "leaves", .color = rgb(0.25f, 0.50f, 0.20f)});
        w->add_block({.name = "sand", .color = rgb(0.86f, 0.80f, 0.58f)});
        w->add_block({.name = "brick", .color = rgb(0.66f, 0.30f, 0.24f)});
        w->add_block({.name = "white", .color = rgb(0.92f, 0.92f, 0.90f)});
        w->add_block({.name = "dark", .color = rgb(0.16f, 0.16f, 0.18f)});
        w->add_block({.name = "glass", .color = rgba{0.70f, 0.85f, 1.0f, 0.35f}, .alpha = AlphaMode::Blend});
        w->add_block({.name = "water", .color = rgba{0.20f, 0.45f, 0.80f, 0.6f}, .alpha = AlphaMode::Blend, .solid = false});
        w->add_block({.name = "lamp", .color = rgb(1.0f, 0.85f, 0.5f), .emissive = rgb(1.0f, 0.8f, 0.45f)});
        w->fill({0, 0, 0}, {7, 0, 7}, w->find_block("grass"));
        add_blocks(w, "Blocks");
        block_mode = true;
        say("block mode: click to add blocks (Tab when done)");
    };
    auto import_vox = [&](const std::string& path) {
        auto w = std::make_shared<VoxelWorld>();
        w->voxel_size = 0.1f; // MagicaVoxel models are usually made at about Teardown's scale
        if (!w->load_vox(path)) {
            say("couldn't read " + path + " as a MagicaVoxel file");
            return false;
        }
        const int blocks = w->block_count();
        add_blocks(w, fs::path(path).stem().string());
        say("imported " + fs::path(path).filename().string() + ": " + std::to_string(blocks) + " blocks, 0.1 m each");
        return true;
    };

    // Importing: a model from anywhere is copied into assets/models/ with
    // the files it refers to (so the project has everything it needs to
    // ship), and placed. A file already under assets/ is used where it is.
    auto inside_assets = [&](const fs::path& p) {
        std::error_code ec;
        const fs::path rel = fs::relative(fs::weakly_canonical(p, ec), fs::weakly_canonical("assets", ec), ec);
        return !ec && !rel.empty() && rel.native()[0] != '.';
    };
    auto project_relative = [&](const fs::path& p) {
        std::error_code ec;
        return fs::relative(fs::weakly_canonical(p, ec), fs::current_path(), ec).generic_string();
    };
    // Copies `src` into `dir`, keeping an identical file that's already
    // there, renaming ("name 2.glb") when a different one is.
    auto copy_in = [&](const fs::path& src, const fs::path& dir) -> fs::path {
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path dst = dir / src.filename();
        for (int n = 2; fs::exists(dst) && !same_file_contents(src, dst); ++n) {
            dst = dir / (src.stem().string() + " " + std::to_string(n) + src.extension().string());
        }
        if (!fs::exists(dst)) fs::copy_file(src, dst, ec);
        return ec ? fs::path{} : dst;
    };
    auto import_model = [&](const fs::path& src, int& missing) -> std::string {
        missing = 0;
        if (inside_assets(src)) return project_relative(src);
        const std::string ext = lower(src.extension().string());
        if (ext == ".glb") { // everything's inside
            const fs::path dst = copy_in(src, fs::path("assets") / "models");
            return dst.empty() ? std::string() : dst.generic_string();
        }
        // .gltf and .obj refer to other files by relative path: they get a
        // folder of their own, laid out the same way.
        fs::path dir = fs::path("assets") / "models" / src.stem();
        for (int n = 2; fs::exists(dir / src.filename()) && !same_file_contents(src, dir / src.filename()); ++n) {
            dir = fs::path("assets") / "models" / (src.stem().string() + " " + std::to_string(n));
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        for (const std::string& dep : model_dependencies(src)) {
            const fs::path from = src.parent_path() / dep;
            const fs::path to = dir / dep;
            if (!fs::exists(from) || fs::path(dep).is_absolute() || dep.find("..") != std::string::npos) {
                ++missing;
                continue;
            }
            fs::create_directories(to.parent_path(), ec);
            if (!fs::exists(to)) fs::copy_file(from, to, ec);
        }
        if (!fs::exists(dir / src.filename())) fs::copy_file(src, dir / src.filename(), ec);
        if (ec) return {};
        return (dir / src.filename()).generic_string();
    };
    // Anything dropped on the window or typed into Import.
    auto import_path = [&](std::string raw) {
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\n' || raw.back() == '\r' || raw.back() == '"' || raw.back() == '\'')) raw.pop_back();
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '"' || raw.front() == '\'')) raw.erase(0, 1);
        if (raw.rfind("file://", 0) == 0) raw = uri_decode(raw.substr(7));
        const fs::path src(raw);
        std::error_code ec;
        if (raw.empty() || !fs::is_regular_file(src, ec)) {
            say("no file at " + raw);
            return false;
        }
        const std::string name = lower(src.filename().string());
        const std::string ext = lower(src.extension().string());
        if (ext == ".vox") return import_vox(src.string());
        if (ext == ".glb" || ext == ".gltf" || ext == ".obj") {
            int missing = 0;
            const std::string path = import_model(src, missing);
            if (path.empty()) {
                say("couldn't copy " + src.filename().string() + " into assets/models/");
                return false;
            }
            rescan();
            add_model(path);
            say(missing == 0 ? "added " + path
                             : "added " + path + ", but " + std::to_string(missing) + " file(s) it refers to weren't found or are outside its folder");
            return true;
        }
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
            const fs::path dst = inside_assets(src) ? src : copy_in(src, fs::path("assets") / "textures");
            rescan();
            say(dst.empty() ? "couldn't copy " + src.filename().string() : "copied to " + project_relative(dst) + " (a block object can use it as its texture atlas)");
            return !dst.empty();
        }
        if (name.size() > 11 && name.compare(name.size() - 11, 11, ".scene.json") == 0) {
            const std::string path = inside_assets(src) ? project_relative(src) : src.string();
            unless_unsaved("open " + src.filename().string(), [&, path] { open_file(path); });
            return true;
        }
        say("can't import " + (ext.empty() ? src.filename().string() : ext) + " files (models: .glb .gltf .obj; blocks: .vox; images: .png .jpg)");
        return false;
    };

    auto delete_selection = [&] {
        if (selection.empty()) return;
        record();
        scene.remove(selection); // all at once: removing one shifts the indices after it
        selection.clear();
    };
    auto duplicate_selection = [&] {
        const std::vector<int> roots = selection_roots();
        if (roots.empty()) return;
        record();
        std::vector<int> fresh;
        std::function<void(int, int)> copy = [&](int src, int parent) {
            SceneEntity e = scene.entities[static_cast<size_t>(src)];
            e.name = unique_name(e.name);
            if (e.voxels) e.voxels = std::make_shared<VoxelWorld>(e.voxels->copy()); // its own blocks, not shared
            const int kept_parent = e.parent;
            e.parent = parent == -2 ? kept_parent : parent;
            const int dst = scene.add(e);
            if (parent == -2) fresh.push_back(dst);
            for (int c : scene.children(src)) {
                if (c != dst) copy(c, dst);
            }
        };
        for (int r : roots) copy(r, -2); // -2: keep the original's parent
        selection = fresh;
        say("duplicated " + std::to_string(fresh.size()) + " (move them with the gizmo)");
    };
    auto frame_bounds = [&](const Bounds& b) {
        if (!b.valid()) {
            target = {0, 0.5f, 0};
            distance = 14.0f;
            return;
        }
        target = b.center();
        const vec3 s = b.size();
        distance = std::clamp(std::max({s.x, s.y, s.z}) * 1.6f + 1.5f, 1.5f, 400.0f);
    };
    auto frame_selection = [&] { frame_bounds(selection.empty() ? Bounds{} : selection_bounds()); };
    // Everything in the scene, except ground and sea planes, which would
    // zoom out to the horizon (unless planes are all there is). Otherwise
    // the view stays wherever the last scene left it, which could be inside
    // a building.
    auto frame_scene = [&] {
        Bounds b;
        for (int pass = 0; pass < 2 && !b.valid(); ++pass) {
            for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
                if (pass == 0 && scene.entities[i].kind == SceneEntity::Kind::Plane) continue;
                const Bounds w = world_box(i);
                if (w.valid()) { b.add(w.min); b.add(w.max); }
            }
        }
        frame_bounds(b);
    };

    // ------------------------------------------------------ gizmo state
    enum Handle { None = -1, AxisX, AxisY, AxisZ, PlaneYZ, PlaneXZ, PlaneXY, Uniform };
    int hot_handle = None, active_handle = None;
    struct DragStart {
        vec2 mouse;
        vec3 pivot;
        float axis_param = 0.0f;
        vec3 plane_hit;
        std::vector<std::pair<int, Transform>> worlds;
    } drag;

    // --------------------------------------------------- widget state
    std::string active_field;   // a number field being dragged
    std::string text_field;     // a text field being typed into ("" = none)
    std::function<void(const std::string&)> text_commit;
    Rect text_rect{};           // where the field being typed into is, this frame
    // A typed number goes back to its field by id and is applied there, the
    // way a drag is: some fields edit values rebuilt every frame (rotation
    // as angles, the sun's direction), so there's nothing lasting to point at.
    std::string typed_id, typed_text;
    double last_click = 0.0;
    std::string last_click_id;
    // Block mode: tools, and the stroke in progress (what every block it
    // changed was before, for its one undo step; the layer it stays in).
    enum class BlockTool { Add, Erase, Paint } block_tool = BlockTool::Add;
    bool block_box = false, brush_sphere = false;
    int brush = 1;
    BlockId current_block = 1;
    bool stroking = false;
    std::shared_ptr<VoxelWorld> stroke_world;
    std::map<std::tuple<int, int, int>, BlockId> stroke_before;
    int lock_axis = -1, lock_layer = 0;
    ivec3 box_a{}, box_b{}, box_normal{0, 1, 0}, last_cell{};
    bool have_last = false;
    float palette_scroll = 0.0f;
    bool type_edit_open = false; // a block type's edit already has its undo step
    std::unordered_map<std::string, Texture> swatch_atlases; // for drawing textured types' tiles in the palette
    int outliner_drag = -1;
    vec2 outliner_press{};
    bool outliner_dragging = false;
    float outliner_scroll = 0.0f;
    bool prev_left = false;

    // Build & run: `thistle run` in the project. The game loads the scene
    // as saved, so a saved scene with changes is saved first.
    auto start_build = [&] {
        if (!project_is_thistle) return say("Build & run needs a Thistle project (one with a thistle.json)");
        if (dirty && !file_path.empty()) save_to(file_path);
        else if (dirty) say("this scene isn't saved, so the game won't have it");
        show_build_log = false;
        build_log_scroll = 0.0f;
        runner.start(project_root);
    };

    // Opening a project: work in its folder, start with the scene it had
    // open last (or its only one), and forget everything from the one before.
    auto open_project = [&](const editor::OpenRequest& req) {
        std::error_code ec;
        fs::current_path(req.folder, ec);
        if (ec) {
            hub.say("Couldn't open " + editor::pretty_path(req.folder) + ": " + ec.message());
            return;
        }
        if (runner.active() && runner.project() != req.folder) {
            runner.stop();
            runner.dismiss();
        }
        if (!text_field.empty()) {
            end_text_input();
            text_field.clear();
        }
        popup = Popup::None;
        project_root = req.folder;
        project_is_thistle = fs::exists(req.folder / "thistle.json", ec);
        project_name = req.folder.filename().string();
        if (project_is_thistle) {
            try {
                std::ifstream in(req.folder / "thistle.json");
                project_name = nlohmann::json::parse(in).value("name", project_name);
            } catch (const std::exception&) {
            }
        }
        new_scene();
        swatch_atlases.clear();
        rescan();
        auto usable = [&](const std::string& s) { return !s.empty() && fs::is_regular_file(s, ec); };
        std::string first = req.scene;
        if (!usable(first)) first = hub.last_scene(req.folder);
        if (!usable(first) && scene_files.size() == 1) first = scene_files[0];
        if (usable(first)) {
            open_file(first);
        } else {
            target = {0, 0.5f, 0};
            distance = 14.0f;
        }
        hub.remember(req.folder, file_path);
        in_hub = false;
        say(project_is_thistle ? "opened " + project_name : "opened " + project_name + " (not a Thistle project, so no Build & run)");
        if (req.build_and_run) start_build();
    };

    app.update([&](Frame f) {
        runner.update();
        if (in_hub) {
            if (auto req = hub.update(f)) open_project(*req);
            return;
        }
        if (frame_opened) {
            frame_scene();
            frame_opened = false;
        }
        const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
        constexpr float TOP = 40.0f, LEFT = 250.0f, RIGHT = 310.0f, BOTTOM = 26.0f;
        const Rect view{{LEFT, TOP}, {std::max(W - LEFT - RIGHT, 50.0f), std::max(H - TOP - BOTTOM, 50.0f)}};
        const vec2 m = f.mouse();
        const bool left_down = f.mouse_down(Mouse::Left);
        const bool left_pressed = f.mouse_pressed(Mouse::Left);
        const bool left_released = prev_left && !left_down;
        prev_left = left_down;
        const Popup popup_before = popup; // a popup opened by this frame's click mustn't close on that same click
        const bool ctrl = f.key_down(Key::LeftControl) || f.key_down(Key::RightControl) || f.key_down(Key::LeftSuper);
        const bool shift = f.key_down(Key::LeftShift) || f.key_down(Key::RightShift);
        const bool alt = f.key_down(Key::LeftAlt) || f.key_down(Key::RightAlt);
        // Files dropped on the window: imported, side by side.
        if (!f.dropped_files().empty()) {
            float offset = 0.0f;
            for (const std::string& path : f.dropped_files()) {
                std::error_code dir_ec;
                if (fs::is_directory(path, dir_ec)) { // a folder: open it as the project (checked like any other)
                    const std::string folder = path;
                    unless_unsaved("open another folder", [&, folder] {
                        in_hub = true;
                        hub.request_open(folder);
                    });
                    break;
                }
                const size_t before = scene.entities.size();
                if (import_path(path) && scene.entities.size() > before) {
                    const int e = static_cast<int>(scene.entities.size()) - 1;
                    scene.entities.back().transform.position.x += offset;
                    const Bounds b = scene.local_bounds(e);
                    offset += (b.valid() ? b.size().x * scene.entities.back().transform.scale.x : 1.0f) + 0.5f;
                }
            }
        }
        const bool typing = !text_field.empty();
        const bool popup_open = popup != Popup::None;
        // The Build & run strip (and its log) cover the bottom of the view.
        const bool over_build_ui = runner.active() && (show_build_log || m.y > H - BOTTOM - 34);
        const bool over_view = view.contains(m) && !popup_open && !over_build_ui;

        // ------------------------------------------------------ camera
        const vec2 d = f.mouse_delta();
        if (f.mouse_pressed(Mouse::Right) && over_view) {
            flying = true;
            lock_mouse(true);
        }
        if (flying && !f.mouse_down(Mouse::Right)) {
            flying = false;
            lock_mouse(false);
        }
        if (flying) {
            yaw -= d.x * 0.0035f;
            pitch = std::clamp(pitch - d.y * 0.0035f, radians(-89.0f), radians(89.0f));
            const quat r = quat::euler(pitch, yaw);
            vec3 move{};
            if (f.key_down(Key::W)) move = move + r.forward();
            if (f.key_down(Key::S)) move = move - r.forward();
            if (f.key_down(Key::D)) move = move + r.right();
            if (f.key_down(Key::A)) move = move - r.right();
            if (f.key_down(Key::E)) move = move + vec3{0, 1, 0};
            if (f.key_down(Key::Q)) move = move - vec3{0, 1, 0};
            const float speed = std::max(3.0f, distance * 0.9f) * (shift ? 3.0f : 1.0f);
            if (length(move) > 0.0f) target = target + normalize(move) * (speed * f.dt);
        } else if (over_view || active_field == "#orbit" || active_field == "#pan") {
            const bool mid = f.mouse_down(Mouse::Middle);
            const bool orbit_drag = (mid && !shift) || (alt && left_down && !shift);
            const bool pan_drag = (mid && shift) || (alt && left_down && shift);
            if (orbit_drag) {
                yaw -= d.x * 0.006f;
                pitch = std::clamp(pitch - d.y * 0.006f, radians(-89.0f), radians(89.0f));
            } else if (pan_drag) {
                const quat r = quat::euler(pitch, yaw);
                const float k = distance * 0.0016f;
                target = target - r.right() * (d.x * k) + r.up() * (d.y * k);
            }
            if (over_view && f.mouse_scroll() != 0.0f) distance = std::clamp(distance * (1.0f - f.mouse_scroll() * 0.12f), 0.5f, 800.0f);
        }
        if (!typing && !flying && over_view) {
            if (f.key_pressed(Key::Keypad1)) { yaw = 0; pitch = 0; }
            if (f.key_pressed(Key::Keypad3)) { yaw = radians(90.0f); pitch = 0; }
            if (f.key_pressed(Key::Keypad7)) { yaw = 0; pitch = radians(-89.9f); }
        }
        camera.rotation = quat::euler(pitch, yaw);
        camera.position = target - camera.rotation.forward() * distance;
        camera.near_z = std::max(0.02f, distance * 0.005f);
        camera.far_z = std::max(500.0f, distance * 20.0f);

        scene.place_voxels(); // raycasts below need them where the transforms (and gizmo drags) put them
        sync_atlases();
        SceneEntity* blocks_ent = nullptr; // the block object being edited, in block mode
        if (block_mode) {
            if (selection.size() == 1 && scene.entities[static_cast<size_t>(selection[0])].kind == SceneEntity::Kind::Voxels &&
                scene.entities[static_cast<size_t>(selection[0])].voxels) {
                blocks_ent = &scene.entities[static_cast<size_t>(selection[0])];
            } else {
                block_mode = false;
            }
        }

        // ------------------------------------------------------ gizmo geometry
        const std::vector<int> roots = selection_roots();
        vec3 pivot{};
        for (int i : roots) pivot = pivot + scene.world_transform(i).position;
        if (!roots.empty()) pivot = pivot / static_cast<float>(roots.size());
        vec2 pivot_s{};
        const bool gizmo_visible = !block_mode && !roots.empty() && camera.world_to_screen(pivot, view, pivot_s);
        const float gizmo_len = length(camera.position - pivot) * 0.17f;
        // Move and rotate go along the world's axes; scale along the object's
        // own (a single selection's), since that's what scale means.
        vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (tool == Tool::Scale && roots.size() == 1) {
            const quat r = scene.world_transform(roots[0]).rotation;
            axes[0] = r * vec3{1, 0, 0};
            axes[1] = r * vec3{0, 1, 0};
            axes[2] = r * vec3{0, 0, 1};
        }
        auto project = [&](vec3 p) {
            vec2 s;
            return camera.world_to_screen(p, view, s) ? s : pivot_s;
        };
        auto ring_points = [&](int axis) {
            std::vector<vec2> pts;
            const vec3 a = axes[(axis + 1) % 3], b = axes[(axis + 2) % 3];
            for (int k = 0; k <= 48; ++k) {
                const float t = 2.0f * pi * k / 48.0f;
                pts.push_back(project(pivot + (a * std::cos(t) + b * std::sin(t)) * gizmo_len));
            }
            return pts;
        };
        auto plane_corners = [&](int handle, vec2 out[4]) {
            const int n = handle - PlaneYZ; // the axis the plane is perpendicular to
            const vec3 a = axes[(n + 1) % 3], b = axes[(n + 2) % 3];
            const float s0 = gizmo_len * 0.22f, s1 = gizmo_len * 0.42f;
            out[0] = project(pivot + a * s0 + b * s0);
            out[1] = project(pivot + a * s1 + b * s0);
            out[2] = project(pivot + a * s1 + b * s1);
            out[3] = project(pivot + a * s0 + b * s1);
        };
        auto handle_under = [&](vec2 p) -> int {
            if (!gizmo_visible) return None;
            if (tool == Tool::Rotate) {
                int best = None;
                float best_d = 8.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const std::vector<vec2> pts = ring_points(axis);
                    for (size_t k = 0; k + 1 < pts.size(); ++k) {
                        const float dd = point_segment_distance(p, pts[k], pts[k + 1]);
                        if (dd < best_d) { best_d = dd; best = axis; }
                    }
                }
                return best;
            }
            if (tool == Tool::Scale && std::hypot(p.x - pivot_s.x, p.y - pivot_s.y) < 11.0f) return Uniform;
            if (tool == Tool::Move) {
                for (int h = PlaneYZ; h <= PlaneXY; ++h) {
                    vec2 c[4];
                    plane_corners(h, c);
                    if (point_in_triangle(p, c[0], c[1], c[2]) || point_in_triangle(p, c[0], c[2], c[3])) return h;
                }
            }
            int best = None;
            float best_d = 9.0f;
            for (int axis = 0; axis < 3; ++axis) {
                const float dd = point_segment_distance(p, pivot_s, project(pivot + axes[axis] * gizmo_len));
                if (dd < best_d) { best_d = dd; best = axis; }
            }
            return best;
        };

        // ------------------------------------------------------ viewport clicks
        hot_handle = active_handle != None ? active_handle : (over_view && !flying && !alt ? handle_under(m) : None);
        if (left_pressed && over_view && !flying && !alt && !typing && !block_mode) {
            if (hot_handle != None) {
                // Start dragging a gizmo handle.
                active_handle = hot_handle;
                begin_edit();
                drag = DragStart{};
                drag.mouse = m;
                drag.pivot = pivot;
                const Ray ray = camera.screen_ray(m, view);
                if (tool == Tool::Move && active_handle <= AxisZ) drag.axis_param = ray_line_param(ray, pivot, axes[active_handle]);
                if (tool == Tool::Move && active_handle >= PlaneYZ && active_handle <= PlaneXY) {
                    const RaycastHit h = raycast_plane(ray, pivot, axes[active_handle - PlaneYZ]);
                    drag.plane_hit = h ? h.point : pivot;
                }
                for (int i : roots) drag.worlds.push_back({i, scene.world_transform(i)});
            } else {
                // Select what's under the cursor: the nearest hit.
                const Ray ray = camera.screen_ray(m, view);
                int best = -1;
                float best_d = no_limit;
                for (size_t i = 0; i < scene.entities.size(); ++i) {
                    const int e = static_cast<int>(i);
                    const Transform t = scene.world_transform(e);
                    const mat4 inv = inverse(t.matrix());
                    const vec3 o = inv.transform_point(ray.origin);
                    const vec3 dir = inv.transform_direction(ray.direction);
                    const float len = length(dir);
                    if (len < 1e-8f) continue;
                    const RaycastHit box_hit = raycast(Ray{o, dir / len}, scene.local_bounds(e));
                    if (!box_hit) continue;
                    float hit_d = length(t.matrix().transform_point(box_hit.point) - ray.origin);
                    if (scene.entities[i].kind == SceneEntity::Kind::Model) { // exact, once the box says maybe
                        const RaycastHit exact = raycast(ray, scene.model(e), t);
                        if (!exact) continue;
                        hit_d = exact.distance;
                    } else if (scene.entities[i].kind == SceneEntity::Kind::Voxels && scene.entities[i].voxels) {
                        const VoxelWorld::Hit exact = scene.entities[i].voxels->raycast(ray, 5000.0f);
                        if (!exact) continue;
                        hit_d = exact.distance;
                    }
                    if (hit_d < best_d) { best_d = hit_d; best = e; }
                }
                if (shift || ctrl) {
                    if (best >= 0) {
                        auto it = std::find(selection.begin(), selection.end(), best);
                        if (it == selection.end()) selection.push_back(best);
                        else selection.erase(it);
                    }
                } else {
                    selection = best >= 0 ? std::vector<int>{best} : std::vector<int>{};
                }
            }
        }
        if (active_handle != None) {
            if (!left_down) {
                active_handle = None;
                end_edit();
            } else {
                const Ray ray = camera.screen_ray(m, view);
                for (const auto& [i, start] : drag.worlds) {
                    Transform t = start;
                    if (tool == Tool::Move) {
                        vec3 delta{};
                        if (active_handle <= AxisZ) {
                            float step = ray_line_param(ray, drag.pivot, axes[active_handle]) - drag.axis_param;
                            if (ctrl) step = std::round(step * 2.0f) / 2.0f; // half-meter snaps
                            delta = axes[active_handle] * step;
                        } else {
                            const RaycastHit h = raycast_plane(ray, drag.pivot, axes[active_handle - PlaneYZ]);
                            if (h) delta = h.point - drag.plane_hit;
                            if (ctrl) delta = {std::round(delta.x * 2) / 2, std::round(delta.y * 2) / 2, std::round(delta.z * 2) / 2};
                        }
                        t.position = start.position + delta;
                    } else if (tool == Tool::Rotate) {
                        const vec2 a{drag.mouse.x - pivot_s.x, drag.mouse.y - pivot_s.y}, b{m.x - pivot_s.x, m.y - pivot_s.y};
                        // Screen y points down: a counter-clockwise turn on
                        // screen has a negative cross product. Counter-clockwise
                        // seen from an axis' tip is a positive turn about it.
                        float angle = -std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
                        if (dot(axes[active_handle], camera.rotation.forward()) > 0.0f) angle = -angle;
                        if (ctrl) angle = radians(15.0f) * std::round(angle / radians(15.0f));
                        const quat q = quat::axis_angle(axes[active_handle], angle);
                        t.position = drag.pivot + q * (start.position - drag.pivot);
                        t.rotation = normalize(q * start.rotation);
                    } else { // scale
                        float s = 1.0f;
                        const vec2 a{drag.mouse.x - pivot_s.x, drag.mouse.y - pivot_s.y}, b{m.x - pivot_s.x, m.y - pivot_s.y};
                        if (active_handle == Uniform) {
                            const float la = std::hypot(a.x, a.y);
                            s = la > 1.0f ? std::hypot(b.x, b.y) / la : 1.0f;
                        } else {
                            const vec2 end = project(drag.pivot + axes[active_handle] * gizmo_len);
                            vec2 dir{end.x - pivot_s.x, end.y - pivot_s.y};
                            const float dl = std::hypot(dir.x, dir.y);
                            if (dl > 1.0f) {
                                dir = {dir.x / dl, dir.y / dl};
                                const float pa = a.x * dir.x + a.y * dir.y;
                                if (std::fabs(pa) > 1.0f) s = (b.x * dir.x + b.y * dir.y) / pa;
                            }
                        }
                        s = std::max(s, 0.01f);
                        if (ctrl) s = std::max(0.1f, std::round(s * 10.0f) / 10.0f);
                        if (active_handle == Uniform) {
                            t.scale = start.scale * s;
                            t.position = drag.pivot + (start.position - drag.pivot) * s;
                        } else {
                            vec3 k{1, 1, 1};
                            (active_handle == AxisX ? k.x : active_handle == AxisY ? k.y : k.z) = s;
                            t.scale = {start.scale.x * k.x, start.scale.y * k.y, start.scale.z * k.z};
                        }
                        if (scene.entities[static_cast<size_t>(i)].kind == SceneEntity::Kind::Voxels) t.scale = start.scale; // sized by their block size
                    }
                    scene.set_world_transform(i, t);
                }
            }
        }

        // ------------------------------------------------------ block mode
        // Where the tool would act: the block under the cursor (Erase,
        // Paint) or the empty cell in front of its face (Add). Past the
        // blocks, the object's floor counts as a surface, so an empty object
        // can be started. A stroke stays in the layer it started in, so
        // dragging draws a floor or a wall instead of stacking blocks toward
        // the camera (or digging a pit, erasing).
        bool op_ok = false, on_block = false;
        ivec3 op_cell{}, op_normal{0, 1, 0};
        BlockId under_id = 0;
        if (blocks_ent && over_view && !flying && !alt) {
            VoxelWorld& w = *blocks_ent->voxels;
            const Ray ray = camera.screen_ray(m, view);
            if (stroking && lock_axis >= 0) {
                const quat inv = w.rotation.inverse();
                const vec3 o = (inv * (ray.origin - w.origin)) / w.voxel_size;
                const vec3 dir = inv * ray.direction;
                const float oa = lock_axis == 0 ? o.x : lock_axis == 1 ? o.y : o.z;
                const float da = lock_axis == 0 ? dir.x : lock_axis == 1 ? dir.y : dir.z;
                if (std::fabs(da) > 1e-5f) {
                    const float t = (lock_layer + 0.5f - oa) / da;
                    if (t > 0.0f) {
                        const vec3 hp = o + dir * t;
                        op_cell = {static_cast<int>(std::floor(hp.x)), static_cast<int>(std::floor(hp.y)), static_cast<int>(std::floor(hp.z))};
                        (lock_axis == 0 ? op_cell.x : lock_axis == 1 ? op_cell.y : op_cell.z) = lock_layer;
                        op_ok = true;
                        op_normal = box_normal;
                    }
                }
            } else if (const VoxelWorld::Hit h = w.raycast(ray, 5000.0f)) {
                on_block = true;
                under_id = h.id;
                op_normal = h.normal;
                op_cell = block_tool == BlockTool::Add ? h.block + h.normal : h.block;
                op_ok = true;
            } else if (block_tool == BlockTool::Add) {
                const vec3 up = w.rotation * vec3{0, 1, 0};
                if (const RaycastHit ph = raycast_plane(ray, w.origin, up)) {
                    const ivec3 c = w.to_block(ph.point + up * (w.voxel_size * 0.5f));
                    op_cell = {c.x, 0, c.z};
                    op_ok = true;
                }
            }
        }
        auto apply_cell = [&](VoxelWorld& w, ivec3 c) {
            const BlockId now = w.get(c);
            BlockId want = now;
            if (block_tool == BlockTool::Add) want = now == 0 ? current_block : now;
            else if (block_tool == BlockTool::Erase) want = 0;
            else if (now != 0) want = current_block;
            if (want == now) return;
            stroke_before.emplace(std::make_tuple(c.x, c.y, c.z), now); // keeps the first "before"
            w.set(c, want);
        };
        // The cells a brush of `brush` blocks covers around `center`.
        auto brush_range = [&](ivec3 center, ivec3& lo, ivec3& hi) {
            const int r0 = -(brush - 1) / 2, r1 = brush / 2;
            lo = center + ivec3{r0, r0, r0};
            hi = center + ivec3{r1, r1, r1};
        };
        auto apply_brush = [&](VoxelWorld& w, ivec3 center) {
            ivec3 lo, hi;
            brush_range(center, lo, hi);
            const vec3 mid = vec3{lo.x + hi.x + 1.0f, lo.y + hi.y + 1.0f, lo.z + hi.z + 1.0f} * 0.5f;
            const float r2 = brush * brush * 0.25f + 0.01f;
            for (int z = lo.z; z <= hi.z; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x) {
                        const vec3 dc = vec3{x + 0.5f, y + 0.5f, z + 0.5f} - mid;
                        if (brush_sphere && dot(dc, dc) > r2) continue;
                        apply_cell(w, {x, y, z});
                    }
        };
        // The box tool's cells: the rectangle between the corners, `brush`
        // blocks thick, outward from the surface when adding, into it otherwise.
        auto box_range = [&](ivec3& lo, ivec3& hi) {
            lo = {std::min(box_a.x, box_b.x), std::min(box_a.y, box_b.y), std::min(box_a.z, box_b.z)};
            hi = {std::max(box_a.x, box_b.x), std::max(box_a.y, box_b.y), std::max(box_a.z, box_b.z)};
            const ivec3 cap{255, 255, 255};
            hi = {std::min(hi.x, lo.x + cap.x), std::min(hi.y, lo.y + cap.y), std::min(hi.z, lo.z + cap.z)};
            const ivec3 n = block_tool == BlockTool::Add ? box_normal : ivec3{-box_normal.x, -box_normal.y, -box_normal.z};
            const int extra = brush - 1;
            if (n.x > 0) hi.x += extra; else if (n.x < 0) lo.x -= extra;
            if (n.y > 0) hi.y += extra; else if (n.y < 0) lo.y -= extra;
            if (n.z > 0) hi.z += extra; else if (n.z < 0) lo.z -= extra;
        };
        if (blocks_ent && left_pressed && over_view && !flying && !alt && !typing) {
            if (shift) { // eyedropper
                if (on_block && under_id != 0) {
                    current_block = under_id;
                    say("picked " + blocks_ent->voxels->block_type(under_id).name);
                }
            } else if (op_ok) {
                stroking = true;
                stroke_world = blocks_ent->voxels;
                stroke_before.clear();
                box_normal = op_normal;
                lock_axis = op_normal.x != 0 ? 0 : op_normal.y != 0 ? 1 : 2;
                lock_layer = lock_axis == 0 ? op_cell.x : lock_axis == 1 ? op_cell.y : op_cell.z;
                box_a = box_b = last_cell = op_cell;
                have_last = true;
                if (!block_box) apply_brush(*stroke_world, op_cell);
            }
        }
        if (stroking) {
            if (op_ok && have_last && !(op_cell == last_cell)) {
                if (block_box) {
                    box_b = op_cell;
                } else {
                    // Fill in between, so a fast drag leaves a line, not dots.
                    const ivec3 dlt = op_cell - last_cell;
                    const int steps = std::max({std::abs(dlt.x), std::abs(dlt.y), std::abs(dlt.z)});
                    for (int k = 1; k <= steps; ++k) {
                        const float t = static_cast<float>(k) / steps;
                        apply_brush(*stroke_world, {last_cell.x + static_cast<int>(std::lround(dlt.x * t)), last_cell.y + static_cast<int>(std::lround(dlt.y * t)),
                                                    last_cell.z + static_cast<int>(std::lround(dlt.z * t))});
                    }
                }
                last_cell = op_cell;
            }
            if (!left_down) {
                if (block_box) {
                    ivec3 lo, hi;
                    box_range(lo, hi);
                    for (int z = lo.z; z <= hi.z; ++z)
                        for (int y = lo.y; y <= hi.y; ++y)
                            for (int x = lo.x; x <= hi.x; ++x) apply_cell(*stroke_world, {x, y, z});
                }
                if (!stroke_before.empty()) {
                    UndoStep step;
                    step.world = stroke_world;
                    for (const auto& [key, before] : stroke_before) step.blocks.push_back({{std::get<0>(key), std::get<1>(key), std::get<2>(key)}, before});
                    push_undo(std::move(step));
                }
                stroking = false;
                lock_axis = -1;
                have_last = false;
                stroke_world.reset();
                stroke_before.clear();
            }
        }

        // ------------------------------------------------------ shortcuts
        if (!typing && !flying) {
            if (block_mode && !ctrl && !alt) {
                if (f.key_pressed(Key::Num1)) block_tool = BlockTool::Add;
                if (f.key_pressed(Key::Num2)) block_tool = BlockTool::Erase;
                if (f.key_pressed(Key::Num3)) block_tool = BlockTool::Paint;
                if (f.key_pressed(Key::B)) block_box = !block_box;
                if (f.key_pressed(Key::LeftBracket)) brush = std::max(1, brush - 1);
                if (f.key_pressed(Key::RightBracket)) brush = std::min(16, brush + 1);
            }
            if (!ctrl && !alt && f.key_pressed(Key::Tab) && !popup_open) {
                if (block_mode) block_mode = false;
                else if (selection.size() == 1 && scene.entities[static_cast<size_t>(selection[0])].kind == SceneEntity::Kind::Voxels) block_mode = true;
            }
            if (!ctrl && !alt) {
                if (f.key_pressed(Key::W)) tool = Tool::Move;
                if (f.key_pressed(Key::E)) tool = Tool::Rotate;
                if (f.key_pressed(Key::R)) tool = Tool::Scale;
                if (f.key_pressed(Key::F)) frame_selection();
                if (f.key_pressed(Key::Home)) frame_scene();
                if (!block_mode && (f.key_pressed(Key::Delete) || f.key_pressed(Key::X) || f.key_pressed(Key::Backspace))) delete_selection();
                if (shift && f.key_pressed(Key::A)) { popup = Popup::Add; popup_at = m; }
            }
            if (ctrl) {
                if (f.key_pressed(Key::Z)) { if (shift) redo(); else undo(); }
                if (f.key_pressed(Key::Y)) redo();
                if (f.key_pressed(Key::D) && !block_mode) duplicate_selection();
                if (f.key_pressed(Key::A)) {
                    selection.clear();
                    for (size_t i = 0; i < scene.entities.size(); ++i) selection.push_back(static_cast<int>(i));
                }
                if (f.key_pressed(Key::S)) {
                    if (shift || file_path.empty()) { popup = Popup::SaveAs; popup_at = {W * 0.5f - 160, H * 0.3f}; }
                    else save_to(file_path);
                }
                if (f.key_pressed(Key::O)) { rescan(); popup = Popup::Open; popup_at = {60, TOP}; }
                if (f.key_pressed(Key::N)) unless_unsaved("start a new scene", new_scene);
            }
            if (f.key_pressed(Key::Escape)) {
                if (popup_open) popup = Popup::None;
                else if (block_mode) block_mode = false;
                else selection.clear();
            }
        }

        // ------------------------------------------------------ the 3D view
        World world;
        scene.draw(world);
        world.grid({std::round(target.x), 0.002f, std::round(target.z)}, 60.0f, 1.0f, rgba{1, 1, 1, 0.08f});
        world.line({-500, 0.004f, 0}, {500, 0.004f, 0}, rgba{theme::axis_x.r, theme::axis_x.g, theme::axis_x.b, 0.6f});
        world.line({0, 0.004f, -500}, {0, 0.004f, 500}, rgba{theme::axis_z.r, theme::axis_z.g, theme::axis_z.b, 0.6f});
        for (size_t i = 0; i < scene.entities.size(); ++i) {
            const SceneEntity& e = scene.entities[i];
            const Transform t = scene.world_transform(static_cast<int>(i));
            const bool selected = std::find(selection.begin(), selection.end(), static_cast<int>(i)) != selection.end();
            switch (e.kind) {
                case SceneEntity::Kind::PointLight:
                    world.sphere(t.position, 0.12f, e.color);
                    world.wire_sphere(t.position, selected ? e.range : 0.35f, rgba{e.color.r, e.color.g, e.color.b, 0.5f});
                    break;
                case SceneEntity::Kind::SpotLight: {
                    world.sphere(t.position, 0.12f, e.color);
                    const float reach = selected ? e.range : 1.5f;
                    const vec3 fwd = t.rotation.forward(), r = t.rotation.right(), u = t.rotation.up();
                    const float spread = std::tan(e.spot_angle) * reach;
                    for (int k = 0; k < 4; ++k) {
                        const vec3 side = (k % 2 ? r : u) * (k < 2 ? spread : -spread);
                        world.line(t.position, t.position + fwd * reach + side, rgba{e.color.r, e.color.g, e.color.b, 0.6f});
                    }
                    break;
                }
                case SceneEntity::Kind::Trigger: world.wire_box(t, e.color); break;
                case SceneEntity::Kind::Spawn: {
                    world.wire_box(Transform{t.apply({0, 0.9f, 0}), t.rotation, {0.5f, 1.8f, 0.5f}}, e.color);
                    world.line(t.apply({0, 0.05f, 0}), t.apply({0, 0.05f, -0.9f}), e.color); // facing
                    break;
                }
                case SceneEntity::Kind::Empty:
                    world.line(t.position - vec3{0.3f, 0, 0}, t.position + vec3{0.3f, 0, 0}, theme::faint);
                    world.line(t.position - vec3{0, 0.3f, 0}, t.position + vec3{0, 0.3f, 0}, theme::faint);
                    world.line(t.position - vec3{0, 0, 0.3f}, t.position + vec3{0, 0, 0.3f}, theme::faint);
                    break;
                default: break;
            }
            if (selected) {
                const Bounds lb = scene.local_bounds(static_cast<int>(i));
                world.wire_box(Transform{t.apply(lb.center()), t.rotation, {lb.size().x * t.scale.x, lb.size().y * t.scale.y, lb.size().z * t.scale.z}},
                               theme::accent, true);
            }
        }
        if (blocks_ent) {
            const VoxelWorld& w = *blocks_ent->voxels;
            auto cells_box = [&](ivec3 lo, ivec3 hi, rgba c) {
                const vec3 a{static_cast<float>(lo.x), static_cast<float>(lo.y), static_cast<float>(lo.z)};
                const vec3 b{hi.x + 1.0f, hi.y + 1.0f, hi.z + 1.0f};
                world.wire_box(Transform{w.origin + w.rotation * ((a + b) * (0.5f * w.voxel_size)), w.rotation, (b - a) * w.voxel_size}, c, true);
            };
            const rgba tool_color = block_tool == BlockTool::Add ? rgba{1, 1, 1, 0.9f}
                                    : block_tool == BlockTool::Erase ? rgba{1.0f, 0.3f, 0.3f, 0.9f}
                                                                     : w.block_type(current_block).color;
            if (stroking && block_box) {
                ivec3 lo, hi;
                box_range(lo, hi);
                cells_box(lo, hi, tool_color);
            } else if (op_ok) {
                ivec3 lo, hi;
                if (block_box) lo = hi = op_cell;
                else brush_range(op_cell, lo, hi);
                cells_box(lo, hi, tool_color);
            }
        }
        world.render(f, camera, view);

        // Gizmo, in screen space on top of the view: crisp at any distance.
        if (gizmo_visible) {
            const rgba colors[3] = {theme::axis_x, theme::axis_y, theme::axis_z};
            auto col = [&](int h, rgba c) { return h == hot_handle ? theme::hover : c; };
            if (tool == Tool::Rotate) {
                for (int axis = 0; axis < 3; ++axis) {
                    const std::vector<vec2> pts = ring_points(axis);
                    for (size_t k = 0; k + 1 < pts.size(); ++k) f.line(pts[k], pts[k + 1], col(axis, colors[axis]), axis == hot_handle ? 3.5f : 2.5f);
                }
                f.circle(pivot_s, 3.0f, white);
            } else {
                if (tool == Tool::Move) {
                    for (int h = PlaneYZ; h <= PlaneXY; ++h) {
                        vec2 c[4];
                        plane_corners(h, c);
                        rgba fill = col(h, colors[h - PlaneYZ]);
                        fill.a = h == hot_handle ? 0.7f : 0.35f;
                        f.triangle(c[0], c[1], c[2], fill);
                        f.triangle(c[0], c[2], c[3], fill);
                    }
                }
                for (int axis = 0; axis < 3; ++axis) {
                    const vec2 end = project(pivot + axes[axis] * gizmo_len);
                    const rgba c = col(axis, colors[axis]);
                    f.line(pivot_s, end, c, axis == hot_handle ? 4.0f : 3.0f);
                    vec2 dir{end.x - pivot_s.x, end.y - pivot_s.y};
                    const float len = std::hypot(dir.x, dir.y);
                    if (len < 1.0f) continue;
                    dir = {dir.x / len, dir.y / len};
                    const vec2 side{-dir.y, dir.x};
                    if (tool == Tool::Move) { // arrowhead
                        f.triangle({end.x + dir.x * 14, end.y + dir.y * 14}, {end.x + side.x * 6, end.y + side.y * 6}, {end.x - side.x * 6, end.y - side.y * 6}, c);
                    } else { // scale: a square end
                        f.rect({end.x - 6, end.y - 6}, {12, 12}, c);
                    }
                }
                f.rect({pivot_s.x - 5, pivot_s.y - 5}, {10, 10}, tool == Tool::Scale ? col(Uniform, white) : white);
            }
        }
        if (flying) f.text("flying: WASD / Q E, Shift = faster", {view.pos.x + 12, view.pos.y + 10}, {.size = 14, .color = rgba{1, 1, 1, 0.7f}});

        // ------------------------------------------------------ widgets
        auto button = [&](const std::string& label, Rect r, bool on = false, float size = 14.0f) {
            ButtonStyle s;
            s.bg = on ? theme::field_active : theme::field;
            s.bg_hover = on ? theme::field_active : theme::field_hover;
            s.bg_press = theme::accent;
            s.text = theme::text;
            s.text_size = size;
            return f.button(label, r, s) && !typing;
        };
        auto label = [&](const std::string& t, vec2 p, rgba c = theme::dim, float size = 13.0f) { f.text(t, p, {.size = size, .color = c}); };
        // A number: drag left/right to change, double-click to type.
        // undoable = false: the caller makes its own undo step (block types
        // live in a shared VoxelWorld, which a scene snapshot doesn't copy).
        auto number = [&](const std::string& id, Rect r, float& v, float speed, int decimals = 2, rgba tint = theme::field, bool undoable = true) {
            const bool hover = r.contains(m) && !popup_open;
            const bool is_text = text_field == id;
            if (typed_id == id) {
                if (undoable) record();
                v = parse_float(typed_text, v);
                typed_id.clear();
            }
            if (is_text) text_rect = r;
            f.rect(r.pos, r.size, is_text ? theme::field_active : active_field == id ? theme::field_active : hover ? theme::field_hover : tint);
            const std::string shown = is_text ? text_input() + "|" : fmt(v, decimals);
            const vec2 tsz = f.measure_text(shown, {.size = 13});
            f.text(shown, {r.pos.x + (r.size.x - tsz.x) * 0.5f, r.pos.y + (r.size.y - 13) * 0.5f - 1}, {.size = 13, .color = theme::text});
            if (is_text) return;
            if (left_pressed && hover && !typing) {
                if (last_click_id == id && now_seconds() - last_click < 0.35) { // double-click: type a value
                    text_field = id;
                    begin_text_input(fmt(v, decimals), 64);
                    text_commit = [&, id](const std::string& s) {
                        typed_id = id;
                        typed_text = s;
                    };
                    last_click_id.clear();
                    return;
                }
                last_click_id = id;
                last_click = now_seconds();
                active_field = id;
                if (undoable) begin_edit();
            }
            if (active_field == id) {
                if (!left_down) {
                    active_field.clear();
                    if (undoable) end_edit();
                } else if (d.x != 0.0f) {
                    v += d.x * speed * (shift ? 0.1f : 1.0f);
                }
            }
        };
        auto text_box = [&](const std::string& id, Rect r, const std::string& value, std::function<void(const std::string&)> commit, bool undoable = true) {
            const bool is_text = text_field == id;
            const bool hover = r.contains(m) && !popup_open;
            if (is_text) text_rect = r;
            f.rect(r.pos, r.size, is_text ? theme::field_active : hover ? theme::field_hover : theme::field);
            std::string shown = is_text ? text_input() + "|" : value;
            float size = 13.0f;
            const vec2 tsz = f.measure_text(shown, {.size = size});
            if (tsz.x > r.size.x - 10) size *= (r.size.x - 10) / tsz.x;
            f.text(shown, {r.pos.x + 6, r.pos.y + (r.size.y - size) * 0.5f - 1}, {.size = size, .color = theme::text});
            if (!is_text && left_pressed && hover && !typing) {
                text_field = id;
                begin_text_input(value, 256);
                text_commit = [&, commit, undoable](const std::string& s) {
                    if (undoable) record();
                    commit(s);
                };
            }
        };
        auto color_row = [&](const std::string& id, float x, float y, float w, rgba& c, bool undoable = true) {
            f.rect({x, y}, {22, 22}, rgba{c.r, c.g, c.b, 1.0f});
            const float fw = (w - 30) / 3.0f;
            number(id + ".r", Rect{{x + 28, y}, {fw - 3, 22}}, c.r, 0.004f, 2, rgb(0.30f, 0.20f, 0.20f), undoable);
            number(id + ".g", Rect{{x + 28 + fw, y}, {fw - 3, 22}}, c.g, 0.004f, 2, rgb(0.20f, 0.28f, 0.20f), undoable);
            number(id + ".b", Rect{{x + 28 + fw * 2, y}, {fw - 3, 22}}, c.b, 0.004f, 2, rgb(0.20f, 0.22f, 0.32f), undoable);
            c.r = std::clamp(c.r, 0.0f, 1.0f);
            c.g = std::clamp(c.g, 0.0f, 1.0f);
            c.b = std::clamp(c.b, 0.0f, 1.0f);
        };

        // Text entry: Enter commits, Escape cancels, clicking outside the field
        // commits (except Save as and Import, which only act on Enter). text_rect is
        // from last frame's drawing, which is where the user clicked.
        if (typing) {
            const bool clicked_away = left_pressed && !text_rect.contains(m) && text_field != "#saveas" && text_field != "#import";
            if (f.key_pressed(Key::Enter) || f.key_pressed(Key::KeypadEnter) || clicked_away) {
                const std::string v = text_input();
                end_text_input();
                auto commit = text_commit;
                text_field.clear();
                if (commit) commit(v);
            } else if (f.key_pressed(Key::Escape)) {
                end_text_input();
                text_field.clear();
            }
        }

        // ------------------------------------------------------ top bar
        f.rect({0, 0}, {W, TOP}, theme::chrome);
        float x = 10;
        auto top_button = [&](const std::string& t, float w, bool on = false) {
            const bool r = button(t, Rect{{x, 6}, {w, TOP - 12}}, on);
            x += w + 4;
            return r;
        };
        if (top_button("Projects", 80)) unless_unsaved("go back to the projects", [&] { in_hub = true; });
        x += 14;
        if (top_button("New", 52)) unless_unsaved("start a new scene", new_scene);
        if (top_button("Open", 60)) { rescan(); popup = Popup::Open; popup_at = {x - 64, TOP}; }
        if (top_button("Save", 56)) {
            if (file_path.empty()) { popup = Popup::SaveAs; popup_at = {W * 0.5f - 160, H * 0.3f}; }
            else save_to(file_path);
        }
        if (top_button("Save as", 72)) { popup = Popup::SaveAs; popup_at = {W * 0.5f - 160, H * 0.3f}; }
        x += 14;
        if (top_button("Undo", 56)) undo();
        if (top_button("Redo", 56)) redo();
        x += 14;
        if (top_button("Move (W)", 86, tool == Tool::Move)) tool = Tool::Move;
        if (top_button("Rotate (E)", 90, tool == Tool::Rotate)) tool = Tool::Rotate;
        if (top_button("Scale (R)", 86, tool == Tool::Scale)) tool = Tool::Scale;
        x += 14;
        if (top_button("+ Add", 70)) { rescan(); popup = Popup::Add; popup_at = {x - 74, TOP}; }
        if (top_button("Import", 70)) popup = Popup::Import;
        if (project_is_thistle) {
            x += 14;
            const bool going = runner.state() == editor::BuildRunner::State::Building || runner.state() == editor::BuildRunner::State::Running;
            if (going ? top_button("Stop", 64, true) : top_button("Build & run", 104)) {
                if (going) runner.stop();
                else start_build();
            }
        }
        {
            const std::string name = (file_path.empty() ? std::string("untitled") : fs::path(file_path).filename().string()) + (dirty ? " *" : "");
            const vec2 tsz = f.measure_text(name, {.size = 15});
            f.text(name, {W - tsz.x - 14, 12}, {.size = 15, .color = dirty ? theme::accent : theme::dim});
            const vec2 psz = f.measure_text(project_name + "  /", {.size = 15});
            f.text(project_name + "  /", {W - tsz.x - psz.x - 20, 12}, {.size = 15, .color = theme::faint});
        }

        // ------------------------------------------------------ outliner
        f.rect({0, TOP}, {LEFT, H - TOP - BOTTOM}, theme::panel);
        label("Outliner", {12, TOP + 10}, theme::dim, 14);
        {
            // Depth-first, children under their parents.
            std::vector<std::pair<int, int>> rows; // entity, depth
            std::function<void(int, int)> walk = [&](int parent, int depth) {
                for (int c : scene.children(parent)) {
                    rows.push_back({c, depth});
                    if (depth < 32) walk(c, depth + 1);
                }
            };
            walk(-1, 0);
            const float row_h = 24.0f, top_y = TOP + 34.0f, bottom_y = H - BOTTOM - 4;
            const float max_scroll = std::max(0.0f, rows.size() * row_h - (bottom_y - top_y));
            const Rect list{{0, top_y}, {LEFT, bottom_y - top_y}};
            if (list.contains(m) && f.mouse_scroll() != 0.0f && !popup_open) outliner_scroll = std::clamp(outliner_scroll - f.mouse_scroll() * 40.0f, 0.0f, max_scroll);
            outliner_scroll = std::clamp(outliner_scroll, 0.0f, max_scroll);
            int hover_row = -1;
            for (size_t r = 0; r < rows.size(); ++r) {
                const float y = top_y + r * row_h - outliner_scroll;
                if (y < top_y - row_h || y > bottom_y) continue;
                const auto [e, depth] = rows[r];
                const Rect rr{{0, y}, {LEFT, row_h}};
                const bool sel = std::find(selection.begin(), selection.end(), e) != selection.end();
                const bool hov = rr.contains(m) && list.contains(m) && !popup_open;
                if (hov) hover_row = e;
                if (sel) f.rect(rr.pos, rr.size, rgba{theme::accent.r, theme::accent.g, theme::accent.b, 0.30f});
                else if (hov) f.rect(rr.pos, rr.size, rgba{1, 1, 1, 0.05f});
                const SceneEntity& ent = scene.entities[static_cast<size_t>(e)];
                const float ix = 14 + depth * 14.0f;
                label(ent.name.empty() ? "(unnamed)" : ent.name, {ix, y + 5}, sel ? white : theme::text, 13);
                const std::string k = kind_label(ent.kind);
                const vec2 ksz = f.measure_text(k, {.size = 11});
                label(k, {LEFT - ksz.x - 10, y + 7}, theme::faint, 11);
            }
            if (rows.empty()) label("Empty. Add something with + Add.", {12, top_y + 4}, theme::faint, 12);
            // Click selects (Ctrl/Shift: add/remove); drag a row onto
            // another to parent it there, onto empty space to unparent it.
            if (left_pressed && list.contains(m) && !popup_open && !typing) {
                if (hover_row >= 0) {
                    if (ctrl || shift) {
                        auto it = std::find(selection.begin(), selection.end(), hover_row);
                        if (it == selection.end()) selection.push_back(hover_row);
                        else selection.erase(it);
                    } else if (std::find(selection.begin(), selection.end(), hover_row) == selection.end()) {
                        selection = {hover_row};
                    }
                    outliner_drag = hover_row;
                    outliner_press = m;
                    outliner_dragging = false;
                } else {
                    selection.clear();
                }
            }
            if (outliner_drag >= 0 && left_down && std::hypot(m.x - outliner_press.x, m.y - outliner_press.y) > 6.0f) outliner_dragging = true;
            if (outliner_dragging && outliner_drag >= 0) {
                std::string hint = "move to the top level";
                if (hover_row >= 0) {
                    hint = "move under " + scene.entities[static_cast<size_t>(hover_row)].name;
                    for (size_t r = 0; r < rows.size(); ++r) {
                        if (rows[r].first != hover_row) continue;
                        const float ry = top_y + r * row_h - outliner_scroll;
                        f.rect({1, ry}, {LEFT - 2, 2}, theme::accent); // outline the row it'll go under
                        f.rect({1, ry + row_h - 2}, {LEFT - 2, 2}, theme::accent);
                        f.rect({1, ry}, {2, row_h}, theme::accent);
                        f.rect({LEFT - 3, ry}, {2, row_h}, theme::accent);
                    }
                }
                if (!list.contains(m)) hint = "(let go outside to cancel)";
                label(hint, {m.x + 14, m.y + 4}, theme::accent, 12);
            }
            if (left_released && outliner_drag >= 0) {
                if (outliner_dragging) {
                    const int new_parent = list.contains(m) ? hover_row : -2;
                    if (new_parent != -2 && new_parent != outliner_drag) {
                        record();
                        std::vector<int> moving = selection_roots();
                        if (std::find(moving.begin(), moving.end(), outliner_drag) == moving.end()) moving = {outliner_drag};
                        int moved = 0;
                        for (int e : moving) moved += scene.set_parent(e, new_parent);
                        if (moved == 0) {
                            undo_stack.pop_back(); // nothing changed, so nothing to undo
                            say("can't put something under itself");
                        }
                        else if (new_parent < 0) say("moved to the top level");
                        else say("moved under " + scene.entities[static_cast<size_t>(new_parent)].name);
                    }
                } else if (!ctrl && !shift && outliner_drag >= 0) {
                    selection = {outliner_drag};
                }
                outliner_drag = -1;
                outliner_dragging = false;
            }
            if (f.mouse_pressed(Mouse::Right) && list.contains(m) && !typing) {
                context_entity = hover_row;
                if (hover_row >= 0 && std::find(selection.begin(), selection.end(), hover_row) == selection.end()) selection = {hover_row};
                popup = hover_row >= 0 ? Popup::Context : Popup::Add;
                popup_at = m;
            }
        }

        // ------------------------------------------------------ inspector
        const float px = W - RIGHT + 12, pw = RIGHT - 24;
        f.rect({W - RIGHT, TOP}, {RIGHT, H - TOP - BOTTOM}, theme::panel);
        float y = TOP + 10;
        auto heading = [&](const std::string& t) {
            label(t, {px, y}, theme::accent, 13);
            y += 22;
        };
        auto vec_row = [&](const std::string& id, const std::string& name, vec3& v, float speed, int decimals) {
            label(name, {px, y + 4}, theme::dim, 12);
            const float fx = px + 62, fw = (pw - 62) / 3.0f;
            number(id + ".x", Rect{{fx, y}, {fw - 3, 22}}, v.x, speed, decimals, rgb(0.30f, 0.20f, 0.20f));
            number(id + ".y", Rect{{fx + fw, y}, {fw - 3, 22}}, v.y, speed, decimals, rgb(0.20f, 0.28f, 0.20f));
            number(id + ".z", Rect{{fx + fw * 2, y}, {fw - 3, 22}}, v.z, speed, decimals, rgb(0.20f, 0.22f, 0.32f));
            y += 28;
        };
        auto float_row = [&](const std::string& id, const std::string& name, float& v, float speed, int decimals = 2) {
            label(name, {px, y + 4}, theme::dim, 12);
            number(id, Rect{{px + 110, y}, {pw - 110, 22}}, v, speed, decimals);
            y += 28;
        };
        if (blocks_ent) {
            VoxelWorld& w = *blocks_ent->voxels;
            const std::shared_ptr<VoxelWorld> wp = blocks_ent->voxels;
            label("Blocks: " + blocks_ent->name, {px, y}, theme::accent, 14);
            if (button("Done (Tab)", Rect{{px + pw - 90, y - 4}, {90, 24}}, false, 12)) block_mode = false;
            y += 28;
            const float bw = (pw - 8) / 3.0f;
            if (button("Add (1)", Rect{{px, y}, {bw, 26}}, block_tool == BlockTool::Add, 12)) block_tool = BlockTool::Add;
            if (button("Erase (2)", Rect{{px + bw + 4, y}, {bw, 26}}, block_tool == BlockTool::Erase, 12)) block_tool = BlockTool::Erase;
            if (button("Paint (3)", Rect{{px + 2 * (bw + 4), y}, {bw, 26}}, block_tool == BlockTool::Paint, 12)) block_tool = BlockTool::Paint;
            y += 32;
            if (button(block_box ? "Box (B): on" : "Box (B): off", Rect{{px, y}, {bw, 24}}, block_box, 12)) block_box = !block_box;
            label("Brush", {px + bw + 10, y + 5}, theme::dim, 12);
            if (button("-", Rect{{px + bw + 52, y}, {24, 24}}, false, 14)) brush = std::max(1, brush - 1);
            label(std::to_string(brush), {px + bw + 84, y + 4}, theme::text, 13);
            if (button("+", Rect{{px + bw + 104, y}, {24, 24}}, false, 14)) brush = std::min(16, brush + 1);
            if (button(brush_sphere ? "Round" : "Square", Rect{{px + pw - 62, y}, {62, 24}}, false, 12)) brush_sphere = !brush_sphere;
            y += 30;
            label(block_box ? "Drag from corner to corner; the brush size is its depth." : "Click or drag. Shift+click a block to pick its type.", {px, y}, theme::faint, 11);
            y += 20;

            heading("Block types");
            const int count = w.block_type_count();
            if (current_block < 1 || current_block > count) current_block = count > 0 ? 1 : 0;
            const float sw = 27.0f;
            const int per_row = static_cast<int>(pw / sw);
            const int rows = (count + per_row - 1) / per_row;
            const float area_h = std::min(rows * sw, sw * 5);
            const Rect area{{px, y}, {pw, area_h}};
            if (area.contains(m) && f.mouse_scroll() != 0.0f) palette_scroll -= f.mouse_scroll() * 20.0f;
            palette_scroll = std::clamp(palette_scroll, 0.0f, std::max(0.0f, rows * sw - area_h));
            std::string hover_name;
            Texture atlas_tex;
            if (!blocks_ent->atlas.empty()) {
                auto it = swatch_atlases.find(blocks_ent->atlas);
                if (it == swatch_atlases.end()) it = swatch_atlases.emplace(blocks_ent->atlas, load_texture(blocks_ent->atlas)).first;
                atlas_tex = it->second;
            }
            const int tile_px = std::max(1, blocks_ent->atlas_tile);
            const int tiles_across = atlas_tex.valid() ? std::max(1, atlas_tex.width / tile_px) : 1;
            for (int id = 1; id <= count; ++id) {
                const int k = id - 1;
                const float sx = px + (k % per_row) * sw, sy = y + (k / per_row) * sw - palette_scroll;
                if (sy < y - 1 || sy + sw - 3 > y + area_h + 1) continue;
                const BlockType& bt = w.block_type(static_cast<BlockId>(id));
                const Rect r{{sx, sy}, {sw - 3, sw - 3}};
                if (id == current_block) f.rect(r.pos - vec2{2, 2}, r.size + vec2{4, 4}, theme::accent);
                const int tile = bt.tile_side >= 0 ? bt.tile_side : bt.tile_top;
                if (atlas_tex.valid() && tile >= 0) {
                    SpriteOpts so;
                    so.size = r.size;
                    so.tint = rgba{bt.color.r, bt.color.g, bt.color.b, 1.0f};
                    so.src = Rect{{static_cast<float>((tile % tiles_across) * tile_px), static_cast<float>((tile / tiles_across) * tile_px)},
                                  {static_cast<float>(tile_px), static_cast<float>(tile_px)}};
                    f.sprite(atlas_tex, r.pos, so);
                } else {
                    f.rect(r.pos, r.size, rgba{bt.color.r, bt.color.g, bt.color.b, 1.0f});
                }
                if (bt.alpha == AlphaMode::Blend) f.rect(r.pos + vec2{r.size.x * 0.5f, 0}, {r.size.x * 0.5f, r.size.y * 0.5f}, rgba{1, 1, 1, 0.35f});
                if (r.contains(m) && !popup_open) {
                    hover_name = bt.name;
                    if (left_pressed && !typing) current_block = static_cast<BlockId>(id);
                }
            }
            y += area_h + 6;
            label(hover_name.empty() ? (current_block > 0 ? w.block_type(current_block).name : "") : hover_name, {px, y}, hover_name.empty() ? theme::text : theme::dim, 12);
            if (button("+ type", Rect{{px + pw - 70, y - 4}, {70, 22}}, false, 12)) {
                const float hue = std::fmod(count * 0.618f, 1.0f); // spread new colors around
                const vec3 c = {0.5f + 0.4f * std::cos(6.2832f * hue), 0.5f + 0.4f * std::cos(6.2832f * (hue - 0.333f)), 0.5f + 0.4f * std::cos(6.2832f * (hue - 0.667f))};
                current_block = w.add_block({.name = "block " + std::to_string(count + 1), .color = rgb(c.x, c.y, c.z)});
                dirty = true;
            }
            y += 24;
            if (current_block > 0) {
                // The chosen type. Its edits go straight into the (shared)
                // world, with one undo step per drag or click.
                BlockType t = w.block_type(current_block);
                const BlockType before = t;
                const BlockId cb = current_block;
                label("Name", {px, y + 4}, theme::dim, 12);
                text_box("blockname", Rect{{px + 62, y}, {pw - 62, 22}}, t.name, [&, wp, cb](const std::string& v) {
                    BlockType named = wp->block_type(cb);
                    UndoStep step;
                    step.world = wp;
                    step.types = {{cb, named}};
                    push_undo(std::move(step));
                    named.name = v;
                    wp->set_block_type(cb, named);
                }, false);
                y += 28;
                label("Color", {px, y + 4}, theme::dim, 12);
                color_row("blockcolor", px + 62, y, pw - 62, t.color, false);
                y += 28;
                const float tw = (pw - 8) / 3.0f;
                const bool clear = t.alpha == AlphaMode::Blend, glows = t.emissive.r + t.emissive.g + t.emissive.b > 0.0f;
                if (button("See-through", Rect{{px, y}, {tw, 22}}, clear, 11)) {
                    t.alpha = clear ? AlphaMode::Opaque : AlphaMode::Blend;
                    t.color.a = clear ? 1.0f : 0.45f;
                }
                if (button("Glows", Rect{{px + tw + 4, y}, {tw, 22}}, glows, 11)) t.emissive = glows ? black : rgba{t.color.r, t.color.g, t.color.b, 1.0f};
                if (button("Solid", Rect{{px + 2 * (tw + 4), y}, {tw, 22}}, t.solid, 11)) t.solid = !t.solid;
                y += 28;
                if (!blocks_ent->atlas.empty()) {
                    label("Tiles", {px, y + 4}, theme::dim, 12);
                    float tt = static_cast<float>(t.tile_top), ts = static_cast<float>(t.tile_side), tb = static_cast<float>(t.tile_bottom);
                    const float fw = (pw - 62) / 3.0f;
                    number("tile.top", Rect{{px + 62, y}, {fw - 3, 22}}, tt, 0.05f, 0, theme::field, false);
                    number("tile.side", Rect{{px + 62 + fw, y}, {fw - 3, 22}}, ts, 0.05f, 0, theme::field, false);
                    number("tile.bottom", Rect{{px + 62 + fw * 2, y}, {fw - 3, 22}}, tb, 0.05f, 0, theme::field, false);
                    t.tile_top = std::max(-1, static_cast<int>(std::lround(tt)));
                    t.tile_side = std::max(-1, static_cast<int>(std::lround(ts)));
                    t.tile_bottom = std::max(-1, static_cast<int>(std::lround(tb)));
                    y += 18;
                    label("top / side / bottom tile (-1: plain color)", {px + 62, y + 6}, theme::faint, 10);
                    y += 22;
                }
                const bool changed = t.name != before.name || t.alpha != before.alpha || t.solid != before.solid || t.tile_top != before.tile_top ||
                                     t.tile_side != before.tile_side || t.tile_bottom != before.tile_bottom ||
                                     std::memcmp(&t.color, &before.color, sizeof(rgba)) != 0 || std::memcmp(&t.emissive, &before.emissive, sizeof(rgba)) != 0;
                if (changed) {
                    if (!type_edit_open) {
                        UndoStep step;
                        step.world = wp;
                        step.types = {{cb, before}};
                        push_undo(std::move(step));
                        type_edit_open = true;
                    }
                    w.set_block_type(cb, t);
                }
            }
            if (!left_down && !typing) type_edit_open = false;

            heading("Object");
            label("Block size", {px, y + 4}, theme::dim, 12);
            {
                const float sizes[] = {0.1f, 0.25f, 0.5f, 1.0f};
                const float qw = (pw - 70 - 12) / 4.0f;
                for (int k = 0; k < 4; ++k) {
                    const bool on = std::fabs(w.voxel_size - sizes[k]) < 1e-4f;
                    char txt[16];
                    std::snprintf(txt, sizeof txt, "%g m", static_cast<double>(sizes[k]));
                    if (button(txt, Rect{{px + 70 + k * (qw + 4), y}, {qw, 22}}, on, 11) && !on) {
                        UndoStep step;
                        step.world = wp;
                        step.voxel_size = w.voxel_size;
                        push_undo(std::move(step));
                        w.voxel_size = sizes[k];
                    }
                }
            }
            y += 28;
            label("Texture", {px, y + 4}, theme::dim, 12);
            if (button(blocks_ent->atlas.empty() ? "(plain colors)" : fs::path(blocks_ent->atlas).filename().string(), Rect{{px + 70, y}, {pw - 70, 22}}, false, 11)) {
                rescan();
                popup = Popup::Atlas;
                popup_at = {px - 140, y + 24};
            }
            y += 28;
            if (!blocks_ent->atlas.empty()) {
                float tile = static_cast<float>(blocks_ent->atlas_tile);
                float_row("atlas.tile", "Tile size (px)", tile, 0.1f, 0);
                blocks_ent->atlas_tile = std::clamp(static_cast<int>(std::lround(tile)), 1, 4096);
            }
            label(std::to_string(w.block_count()) + " blocks", {px, y + 2}, theme::faint, 12);
        } else if (selection.size() == 1) {
            const int e = selection[0];
            SceneEntity& ent = scene.entities[static_cast<size_t>(e)];
            label(kind_label(ent.kind), {px, y}, theme::accent, 14);
            y += 24;
            label("Name", {px, y + 4}, theme::dim, 12);
            text_box("name", Rect{{px + 62, y}, {pw - 62, 24}}, ent.name, [&, e](const std::string& s) {
                if (e < static_cast<int>(scene.entities.size())) scene.entities[static_cast<size_t>(e)].name = s;
            });
            y += 34;
            heading("Transform");
            vec_row("pos", "Position", ent.transform.position, 0.02f, 2);
            vec3 euler = to_euler(ent.transform.rotation);
            vec3 deg{degrees(euler.x), degrees(euler.y), degrees(euler.z)};
            const vec3 before = deg;
            vec_row("rot", "Rotation", deg, 0.5f, 1);
            if (!(deg == before)) ent.transform.rotation = quat::euler(radians(deg.x), radians(deg.y), radians(deg.z));
            if (ent.kind != SceneEntity::Kind::Voxels) vec_row("scl", "Scale", ent.transform.scale, 0.01f, 2);
            y += 6;
            if (ent.kind == SceneEntity::Kind::Voxels && ent.voxels) {
                heading("Blocks");
                label(std::to_string(ent.voxels->block_count()) + " blocks, " + fmt(ent.voxels->voxel_size, 2) + " m each", {px, y}, theme::dim, 12);
                y += 22;
                if (button("Edit blocks (Tab)", Rect{{px, y}, {pw, 28}}, false, 13)) block_mode = true;
                y += 38;
            } else if (ent.kind != SceneEntity::Kind::Empty) {
                heading(ent.kind == SceneEntity::Kind::PointLight || ent.kind == SceneEntity::Kind::SpotLight ? "Light" : "Look");
                label(ent.kind == SceneEntity::Kind::Model ? "Tint" : "Color", {px, y + 4}, theme::dim, 12);
                color_row("color", px + 62, y, pw - 62, ent.color);
                y += 30;
            }
            if (ent.kind == SceneEntity::Kind::Model) {
                label("File", {px, y + 4}, theme::dim, 12);
                if (button(ent.model.empty() ? "(choose)" : fs::path(ent.model).filename().string(), Rect{{px + 62, y}, {pw - 62, 24}}, false, 12)) {
                    rescan();
                    popup = Popup::ChangeModel;
                    popup_at = {px - 140, y + 26};
                }
                y += 32;
            }
            if (ent.kind == SceneEntity::Kind::PointLight || ent.kind == SceneEntity::Kind::SpotLight) {
                float_row("intensity", "Intensity", ent.intensity, 0.01f);
                float_row("range", "Range (m)", ent.range, 0.05f, 1);
                if (ent.kind == SceneEntity::Kind::SpotLight) {
                    float a = degrees(ent.spot_angle);
                    float_row("angle", "Cone (deg)", a, 0.3f, 1);
                    ent.spot_angle = radians(std::clamp(a, 1.0f, 89.0f));
                }
                ent.intensity = std::max(ent.intensity, 0.0f);
                ent.range = std::max(ent.range, 0.1f);
            }
            heading("Properties");
            label("for your game: level.entities[i].property(\"key\")", {px, y - 4}, theme::faint, 10);
            y += 12;
            int remove_at = -1;
            for (size_t k = 0; k < ent.properties.size(); ++k) {
                const float kw = (pw - 30) * 0.45f;
                text_box("pk" + std::to_string(k), Rect{{px, y}, {kw - 3, 22}}, ent.properties[k].first, [&, e, k](const std::string& s) {
                    if (e < static_cast<int>(scene.entities.size()) && k < scene.entities[static_cast<size_t>(e)].properties.size())
                        scene.entities[static_cast<size_t>(e)].properties[k].first = s;
                });
                text_box("pv" + std::to_string(k), Rect{{px + kw, y}, {pw - 30 - kw, 22}}, ent.properties[k].second, [&, e, k](const std::string& s) {
                    if (e < static_cast<int>(scene.entities.size()) && k < scene.entities[static_cast<size_t>(e)].properties.size())
                        scene.entities[static_cast<size_t>(e)].properties[k].second = s;
                });
                if (button("x", Rect{{px + pw - 24, y}, {24, 22}}, false, 12)) remove_at = static_cast<int>(k);
                y += 26;
            }
            if (remove_at >= 0) {
                record();
                ent.properties.erase(ent.properties.begin() + remove_at);
            }
            if (button("+ property", Rect{{px, y}, {110, 22}}, false, 12)) {
                record();
                ent.properties.push_back({"key", "value"});
            }
            y += 34;
            if (button("Duplicate (Ctrl+D)", Rect{{px, y}, {pw * 0.5f - 3, 26}}, false, 12)) duplicate_selection();
            if (button("Delete (Del)", Rect{{px + pw * 0.5f + 3, y}, {pw * 0.5f - 3, 26}}, false, 12)) delete_selection();
        } else if (selection.size() > 1) {
            label(std::to_string(selection.size()) + " selected", {px, y}, theme::accent, 14);
            y += 26;
            label("The gizmo moves, turns and scales them", {px, y}, theme::dim, 12);
            label("together, around their middle.", {px, y + 16}, theme::dim, 12);
            y += 44;
            if (button("Duplicate (Ctrl+D)", Rect{{px, y}, {pw * 0.5f - 3, 26}}, false, 12)) duplicate_selection();
            if (button("Delete (Del)", Rect{{px + pw * 0.5f + 3, y}, {pw * 0.5f - 3, 26}}, false, 12)) delete_selection();
        } else {
            label("Scene", {px, y}, theme::accent, 14);
            y += 26;
            heading("Sun");
            vec3 sdir = normalize(scene.sun.direction);
            float s_yaw = degrees(std::atan2(-sdir.x, -sdir.z)), s_height = degrees(std::asin(std::clamp(-sdir.y, -1.0f, 1.0f)));
            const float sy0 = s_yaw, sh0 = s_height;
            float_row("sun.yaw", "Direction", s_yaw, 0.5f, 0);
            float_row("sun.height", "Height (deg)", s_height, 0.3f, 0);
            if (s_yaw != sy0 || s_height != sh0) {
                s_height = std::clamp(s_height, 1.0f, 90.0f);
                const float yr = radians(s_yaw), hr = radians(s_height);
                scene.sun.direction = {-std::sin(yr) * std::cos(hr), -std::sin(hr), -std::cos(yr) * std::cos(hr)};
            }
            label("Color", {px, y + 4}, theme::dim, 12);
            color_row("sun.color", px + 62, y, pw - 62, scene.sun.color);
            y += 30;
            float_row("sun.intensity", "Intensity", scene.sun.intensity, 0.01f);
            {
                bool sh = scene.sun.shadows;
                if (button(sh ? "Shadows: on" : "Shadows: off", Rect{{px, y}, {pw, 24}}, sh, 12)) {
                    record();
                    scene.sun.shadows = !sh;
                }
                y += 32;
            }
            heading("Sky");
            label("Top", {px, y + 4}, theme::dim, 12);
            color_row("sky.top", px + 62, y, pw - 62, scene.sky.top);
            y += 28;
            label("Horizon", {px, y + 4}, theme::dim, 12);
            color_row("sky.horizon", px + 62, y, pw - 62, scene.sky.horizon);
            y += 28;
            label("Ground", {px, y + 4}, theme::dim, 12);
            color_row("sky.ground", px + 62, y, pw - 62, scene.sky.ground);
            y += 30;
            float_row("ambient", "Ambient", scene.ambient, 0.005f);
            heading("Fog");
            {
                bool fe = scene.fog.enabled;
                if (button(fe ? "Fog: on" : "Fog: off", Rect{{px, y}, {pw, 24}}, fe, 12)) {
                    record();
                    scene.fog.enabled = !fe;
                }
                y += 30;
            }
            if (scene.fog.enabled) {
                float_row("fog.start", "Starts at (m)", scene.fog.start, 0.2f, 0);
                float_row("fog.end", "Solid at (m)", scene.fog.end, 0.3f, 0);
                scene.fog.end = std::max(scene.fog.end, scene.fog.start + 1.0f);
            }
            y += 8;
            label("Click something to edit it.", {px, y}, theme::faint, 12);
        }

        // ------------------------------------------------------ status bar
        f.rect({0, H - BOTTOM}, {W, BOTTOM}, theme::chrome);
        const std::string hints = block_mode ? "BLOCKS  |  Click/drag: add, erase, paint  |  1/2/3 tool  |  B box  |  [ ] brush size  |  "
                                               "Shift+click: pick type  |  Ctrl+Z undo  |  Tab done"
                                             : "Middle-drag orbit  |  Shift+middle pan  |  Wheel zoom  |  Hold right: fly (WASD QE)  |  "
                                               "F frame  |  W/E/R gizmo (Ctrl snaps)  |  Ctrl+Z/Ctrl+Shift+Z  |  Ctrl+D  |  Del  |  Tab: edit blocks";
        label(hints, {10, H - BOTTOM + 6}, theme::faint, 12);
        if (!status.empty() && now_seconds() - status_time < 5.0) {
            const vec2 ssz = f.measure_text(status, {.size = 13});
            label(status, {W - ssz.x - 12, H - BOTTOM + 5}, theme::accent, 13);
        }

        // ------------------------------------------------------ build & run
        {
            using State = editor::BuildRunner::State;
            if ((runner.state() == State::Finished || runner.state() == State::Stopped) && runner.since_finished() > 6.0 && !show_build_log) {
                runner.dismiss(); // a clean finish goes away by itself; a failure stays until closed
            }
        }
        if (runner.active()) {
            using State = editor::BuildRunner::State;
            const State st = runner.state();
            const bool going = st == State::Building || st == State::Running;
            const rgba tone = st == State::Failed ? theme::bad : st == State::Running ? theme::good : st == State::Building ? theme::accent : theme::dim;
            const float py = H - BOTTOM - 34;
            f.rect({0, py}, {W, 34}, rgb(0.12f, 0.12f, 0.13f));
            f.rect({0, py}, {W, 2}, tone);
            float tx = 34;
            if (st == State::Building) {
                const Rect bar{{12, py + 12}, {150, 12}};
                f.rect(bar.pos, bar.size, theme::field);
                if (runner.progress() >= 0.0f) {
                    f.rect(bar.pos, {bar.size.x * std::clamp(runner.progress(), 0.0f, 1.0f), bar.size.y}, theme::accent);
                } else { // no percentage (MSBuild prints none): a block sliding back and forth
                    const float t = std::fabs(std::fmod(static_cast<float>(now_seconds()) * 0.8f, 2.0f) - 1.0f);
                    f.rect({bar.pos.x + (bar.size.x - 30) * t, bar.pos.y}, {30, bar.size.y}, theme::accent);
                }
                tx = 174;
            } else {
                f.circle({18, py + 18}, 6, tone);
            }
            std::string head = runner.headline();
            if (st == State::Building && runner.progress() >= 0.0f) head += "   " + std::to_string(static_cast<int>(runner.progress() * 100.0f)) + "%";
            f.text(head, {tx, py + 9}, {.size = 14, .color = theme::text});
            const float hx = tx + f.measure_text(head, {.size = 14}).x + 18;
            const Rect close_btn{{W - 86, py + 5}, {76, 24}};
            const Rect log_btn{{W - 168, py + 5}, {76, 24}};
            if (button(going ? "Stop" : "Close", close_btn, false, 13)) {
                if (going) {
                    runner.stop();
                } else {
                    runner.dismiss();
                    show_build_log = false;
                }
            }
            if (button(show_build_log ? "Hide log" : "Log", log_btn, show_build_log, 13)) show_build_log = !show_build_log;
            // Cut text to a width without measuring it a character at a time.
            auto fit = [&](std::string t, float room, float size) {
                const float w = f.measure_text(t, {.size = size}).x;
                if (w <= room || t.empty()) return t;
                t.resize(static_cast<size_t>(t.size() * room / w * 0.97f));
                return t + "...";
            };
            f.text(fit(runner.last_line(), log_btn.pos.x - hx - 12, 12), {hx, py + 11},
                   {.size = 12, .color = st == State::Failed ? theme::bad : theme::faint});
            if (show_build_log) {
                constexpr float LH = 16.0f;
                const float top = std::max(TOP + 10.0f, py - H * 0.55f);
                const Rect panel{{8, top}, {W - 16, py - top - 6}};
                f.rect(panel.pos - vec2{1, 1}, panel.size + vec2{2, 2}, theme::accent);
                f.rect(panel.pos, panel.size, rgba{0.07f, 0.07f, 0.08f, 0.97f});
                const std::vector<std::string>& lines = runner.log();
                const int rows = std::max(1, static_cast<int>((panel.size.y - 16) / LH));
                if (panel.contains(m) && f.mouse_scroll() != 0.0f) build_log_scroll += f.mouse_scroll() * 3.0f; // wheel up: older lines
                build_log_scroll = std::clamp(build_log_scroll, 0.0f, std::max(0.0f, static_cast<float>(lines.size()) - rows));
                const int last = static_cast<int>(lines.size()) - static_cast<int>(build_log_scroll);
                const int first = std::max(0, last - rows);
                for (int i = first; i < last; ++i) {
                    f.text(fit(lines[static_cast<size_t>(i)], panel.size.x - 20, 12), {panel.pos.x + 10, panel.pos.y + 8 + (i - first) * LH},
                           {.size = 12, .color = theme::dim});
                }
                if (lines.empty()) f.text("(nothing printed yet)", panel.pos + vec2{10, 8}, {.size = 12, .color = theme::faint});
            }
        }

        // ------------------------------------------------------ popups
        if (popup != Popup::None) {
            std::vector<std::string> items;
            std::string title;
            switch (popup) {
                case Popup::Open:
                    title = scene_files.empty() ? "No scenes yet (save one first)" : "Open";
                    items = scene_files;
                    if (fs::exists(old_layout)) items.push_back(import_label);
                    break;
                case Popup::Add:
                    title = "Add";
                    items = {"Box", "Sphere", "Cylinder", "Cone", "Plane", "Point light", "Spot light", "Trigger volume", "Spawn point", "Empty (group)",
                             "Blocks (voxels)", "Model...", "Voxel model (.vox)..."};
                    break;
                case Popup::AddVox:
                    title = vox_files.empty() ? "No .vox files in assets/" : "MagicaVoxel files in assets/";
                    items = vox_files;
                    break;
                case Popup::Atlas:
                    title = "Block texture atlas (images in assets/)";
                    items = {"(plain colors)"};
                    items.insert(items.end(), image_files.begin(), image_files.end());
                    break;
                case Popup::AddModel:
                case Popup::ChangeModel:
                    title = model_files.empty() ? "No models in assets/" : "Models in assets/";
                    items = model_files;
                    break;
                case Popup::Context:
                    title = context_entity >= 0 && context_entity < static_cast<int>(scene.entities.size()) ? scene.entities[static_cast<size_t>(context_entity)].name : "";
                    items = {"Rename", "Duplicate", "Delete", "Unparent", "Frame (F)"};
                    break;
                default: break;
            }
            if (popup == Popup::SaveAs) {
                const Rect box{popup_at, {320, 96}};
                f.rect(box.pos - vec2{1, 1}, box.size + vec2{2, 2}, theme::accent);
                f.rect(box.pos, box.size, theme::chrome);
                label("Save as (goes in assets/scenes/)", box.pos + vec2{12, 10}, theme::text, 14);
                if (text_field != "#saveas") {
                    text_field = "#saveas";
                    const std::string current = fs::path(file_path).filename().string();
                    begin_text_input(current.substr(0, current.find('.')), 128); // empty when untitled: the placeholder shows
                    text_commit = [&](const std::string& s) {
                        std::string name = s.empty() ? "level" : s;
                        if (name.size() < 11 || name.compare(name.size() - 11, 11, ".scene.json") != 0) name += ".scene.json";
                        popup = Popup::None;
                        save_to((fs::path("assets") / "scenes" / name).generic_string());
                    };
                }
                f.rect(box.pos + vec2{12, 36}, {296, 26}, theme::field_active);
                if (text_input().empty()) label("|level  .scene.json", box.pos + vec2{18, 41}, theme::faint, 14);
                else label(text_input() + "|  .scene.json", box.pos + vec2{18, 41}, theme::text, 14);
                label("Enter saves, Esc cancels", box.pos + vec2{12, 70}, theme::faint, 12);
                if (!text_field.empty() && f.key_pressed(Key::Escape)) popup = Popup::None;
            } else if (popup == Popup::Import) {
                const Rect box{{W * 0.5f - 260, H * 0.3f}, {520, 118}};
                f.rect(box.pos - vec2{1, 1}, box.size + vec2{2, 2}, theme::accent);
                f.rect(box.pos, box.size, theme::chrome);
                label("Import a file: type or paste its path", box.pos + vec2{12, 10}, theme::text, 14);
                if (text_field != "#import") {
                    text_field = "#import";
                    begin_text_input("", 2048);
                    text_commit = [&](const std::string& v) {
                        popup = Popup::None;
                        import_path(v);
                    };
                }
                f.rect(box.pos + vec2{12, 36}, {496, 26}, theme::field_active);
                std::string shown = text_input() + "|";
                const vec2 tsz = f.measure_text(shown, {.size = 14});
                if (tsz.x > 480) shown = "..." + shown.substr(shown.size() - static_cast<size_t>(shown.size() * 470 / tsz.x) + 3);
                label(shown, box.pos + vec2{18, 41}, theme::text, 14);
                label("Models (.glb .gltf .obj) are copied into assets/models/ with their textures.", box.pos + vec2{12, 70}, theme::faint, 12);
                label(".vox becomes a block object.  Or drag files onto the window.  Enter imports, Esc cancels.", box.pos + vec2{12, 88}, theme::faint, 12);
                if (!text_field.empty() && f.key_pressed(Key::Escape)) popup = Popup::None;
            } else if (popup == Popup::Confirm) {
                const Rect box{{W * 0.5f - 190, H * 0.3f}, {380, 112}};
                f.rect(box.pos - vec2{1, 1}, box.size + vec2{2, 2}, theme::accent);
                f.rect(box.pos, box.size, theme::chrome);
                const std::string name = file_path.empty() ? "this scene" : fs::path(file_path).filename().string();
                label("Unsaved changes to " + name + ".", box.pos + vec2{14, 12}, theme::text, 14);
                label("Throw them away and " + confirm_text + "?", box.pos + vec2{14, 34}, theme::dim, 13);
                if (button("Discard changes", Rect{box.pos + vec2{14, 70}, {170, 28}})) {
                    popup = Popup::None;
                    if (confirm_action) confirm_action();
                    confirm_action = nullptr;
                } else if (button("Cancel", Rect{box.pos + vec2{196, 70}, {170, 28}}) ||
                           ((left_pressed || f.mouse_pressed(Mouse::Right)) && !box.contains(m) && popup == popup_before)) {
                    popup = Popup::None;
                    confirm_action = nullptr;
                }
            } else {
                const float item_h = 24, w = 280;
                const int shown = std::min<int>(static_cast<int>(items.size()), 22);
                const float h = 30 + shown * item_h;
                const vec2 at{std::min(popup_at.x, W - w - 4), std::min(popup_at.y, H - h - 4)};
                const Rect box{at, {w, h}};
                f.rect(at - vec2{1, 1}, box.size + vec2{2, 2}, rgba{0, 0, 0, 0.6f});
                f.rect(at, box.size, theme::chrome);
                label(title, at + vec2{10, 7}, theme::dim, 12);
                int chosen = -1;
                for (int i = 0; i < shown; ++i) {
                    const Rect r{{at.x + 4, at.y + 28 + i * item_h}, {w - 8, item_h - 2}};
                    ButtonStyle s;
                    s.bg = rgba{0, 0, 0, 0};
                    s.bg_hover = theme::field_hover;
                    s.bg_press = theme::accent;
                    s.text = theme::text;
                    s.text_size = 13;
                    std::string t = items[static_cast<size_t>(i)];
                    const vec2 tsz = f.measure_text(t, {.size = 13});
                    if (tsz.x > w - 20) s.text_size = 13 * (w - 20) / tsz.x;
                    if (f.button(t, r, s)) chosen = i;
                }
                const Popup was = popup;
                if (chosen >= 0) {
                    popup = Popup::None;
                    const std::string& item = items[static_cast<size_t>(chosen)];
                    if (was == Popup::Open) {
                        if (item == import_label) unless_unsaved("import the old layout", import_old_layout);
                        else unless_unsaved("open " + fs::path(item).filename().string(), [&, item] { open_file(item); });
                    } else if (was == Popup::Add) {
                        const SceneEntity::Kind kinds[] = {SceneEntity::Kind::Box, SceneEntity::Kind::Sphere, SceneEntity::Kind::Cylinder,
                                                           SceneEntity::Kind::Cone, SceneEntity::Kind::Plane, SceneEntity::Kind::PointLight,
                                                           SceneEntity::Kind::SpotLight, SceneEntity::Kind::Trigger, SceneEntity::Kind::Spawn,
                                                           SceneEntity::Kind::Empty};
                        if (chosen < 10) add_kind(kinds[chosen]);
                        else if (chosen == 10) new_block_object();
                        else if (chosen == 11) { popup = Popup::AddModel; popup_at = at; }
                        else { popup = Popup::AddVox; popup_at = at; }
                    } else if (was == Popup::AddModel) {
                        add_model(item);
                    } else if (was == Popup::AddVox) {
                        import_vox(item);
                    } else if (was == Popup::Atlas && selection.size() == 1) {
                        record();
                        scene.entities[static_cast<size_t>(selection[0])].atlas = chosen == 0 ? std::string() : item;
                        sync_atlases();
                    } else if (was == Popup::ChangeModel && selection.size() == 1) {
                        record();
                        scene.entities[static_cast<size_t>(selection[0])].model = item;
                    } else if (was == Popup::Context && context_entity >= 0 && context_entity < static_cast<int>(scene.entities.size())) {
                        if (item == "Rename") {
                            selection = {context_entity};
                            text_field = "name";
                            begin_text_input(scene.entities[static_cast<size_t>(context_entity)].name, 256);
                            const int e = context_entity;
                            text_commit = [&, e](const std::string& s) {
                                record();
                                if (e < static_cast<int>(scene.entities.size())) scene.entities[static_cast<size_t>(e)].name = s;
                            };
                        } else if (item == "Duplicate") {
                            duplicate_selection();
                        } else if (item == "Delete") {
                            delete_selection();
                        } else if (item == "Unparent") {
                            record();
                            for (int e : selection) scene.set_parent(e, -1);
                        } else {
                            frame_selection();
                        }
                    }
                } else if ((left_pressed || f.mouse_pressed(Mouse::Right)) && !box.contains(m) && popup == popup_before) {
                    popup = Popup::None;
                }
            }
        }
        if ((popup != Popup::SaveAs && text_field == "#saveas") || (popup != Popup::Import && text_field == "#import")) { // cancelled
            end_text_input();
            text_field.clear();
        }
    });
    return app.run();
}
