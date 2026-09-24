// Thistle Editor — a 3D level editor for thistle::three scenes (Scene3D).
//
// Place models, shapes, lights, trigger volumes and spawn points; move,
// rotate and scale them with gizmos; group them into hierarchies; give
// them properties your game reads; set the sun, sky and fog; save as
// .scene.json, which a game loads with Scene3D::load() and draws with
// Scene3D::draw() (or reads to place its own things).
//
//   thistle-editor [project folder]
//
// It works inside a project: models come from its assets/ folder (.glb,
// .gltf, .obj), scenes are saved to assets/scenes/, so they ship with the
// game like everything else in assets/. Without a folder it uses the
// current one when that looks like a project (has assets/ or thistle.json),
// else the folder the editor itself is in. If the editor
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
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX // windows.h's min/max macros would break std::min/std::max
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#endif

using namespace thistle;
using namespace thistle::three;
namespace fs = std::filesystem;

namespace {

// Where the editor's own files (its UI font) are: next to the executable,
// wherever the project being edited is.
fs::path executable_dir() {
#if defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    char resolved[PATH_MAX];
    if (_NSGetExecutablePath(path, &size) == 0 && realpath(path, resolved)) return fs::path(resolved).parent_path();
#elif defined(_WIN32)
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) != 0) return fs::path(path).parent_path();
#elif defined(__linux__)
    char path[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        return fs::path(path).parent_path();
    }
#endif
    return fs::current_path();
}

namespace theme {
const rgba chrome = rgb(0.10f, 0.10f, 0.11f);
const rgba panel = rgb(0.16f, 0.16f, 0.17f);
const rgba field = rgb(0.22f, 0.22f, 0.24f);
const rgba field_hover = rgb(0.27f, 0.27f, 0.30f);
const rgba field_active = rgb(0.18f, 0.30f, 0.40f);
const rgba text = rgb(0.88f, 0.88f, 0.88f);
const rgba dim = rgb(0.58f, 0.58f, 0.62f);
const rgba faint = rgb(0.42f, 0.42f, 0.45f);
const rgba accent = rgb(0.95f, 0.55f, 0.18f); // Blender-ish orange: the selection
const rgba axis_x = rgb(0.92f, 0.28f, 0.30f);
const rgba axis_y = rgb(0.45f, 0.82f, 0.25f);
const rgba axis_z = rgb(0.25f, 0.52f, 0.95f);
const rgba hover = rgb(1.0f, 0.88f, 0.25f);
} // namespace theme

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

} // namespace

int main(int argc, char** argv) {
    const fs::path editor_dir = executable_dir();
    {
        std::error_code ec;
        const fs::path cwd = fs::current_path(ec);
        // A folder on the command line is the game to edit. (Finder may add a
        // "-psn_..." argument to an app it launches; that isn't one.)
        if (argc >= 2 && std::string(argv[1]).rfind("-psn", 0) != 0) fs::current_path(argv[1], ec);
        else if (!(fs::exists(cwd / "assets") || fs::exists(cwd / "thistle.json"))) fs::current_path(editor_dir, ec);
        if (ec) log_warn("Thistle Editor: couldn't open the project folder: " + ec.message());
    }

    App app{{.title = "Thistle Editor", .width = 1440, .height = 860}};
    load_font((editor_dir / "editor_assets" / "inter-regular.ttf").string());

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

    // Undo: whole-scene JSON snapshots.
    std::vector<std::string> undo_stack, redo_stack;
    std::string edit_before; // snapshot taken when a drag/edit began
    bool editing = false;
    auto record = [&] { // call right before a one-shot change
        undo_stack.push_back(scene.to_json());
        if (undo_stack.size() > 200) undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
        dirty = true;
    };
    auto begin_edit = [&] {
        if (editing) return;
        edit_before = scene.to_json();
        editing = true;
    };
    auto end_edit = [&] {
        if (!editing) return;
        editing = false;
        if (scene.to_json() == edit_before) return; // a click without a change
        undo_stack.push_back(edit_before);
        if (undo_stack.size() > 200) undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
        dirty = true;
    };
    auto clamp_selection = [&] {
        const int n = static_cast<int>(scene.entities.size());
        selection.erase(std::remove_if(selection.begin(), selection.end(), [&](int i) { return i < 0 || i >= n; }), selection.end());
    };
    auto undo = [&] {
        if (undo_stack.empty()) return say("nothing to undo");
        redo_stack.push_back(scene.to_json());
        scene.from_json(undo_stack.back());
        undo_stack.pop_back();
        clamp_selection();
        dirty = true;
    };
    auto redo = [&] {
        if (redo_stack.empty()) return say("nothing to redo");
        undo_stack.push_back(scene.to_json());
        scene.from_json(redo_stack.back());
        redo_stack.pop_back();
        clamp_selection();
        dirty = true;
    };

    std::vector<std::string> model_files = scan("assets", {".glb", ".gltf", ".obj"});
    std::vector<std::string> scene_files;
    auto rescan = [&] {
        model_files = scan("assets", {".glb", ".gltf", ".obj"});
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
        dirty = false;
        say("opened " + path);
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

    auto delete_selection = [&] {
        if (selection.empty()) return;
        record();
        std::vector<int> doomed = selection;
        std::sort(doomed.begin(), doomed.end(), std::greater<int>());
        for (int i : doomed) {
            if (i < static_cast<int>(scene.entities.size())) scene.remove(i); // (a child may already be gone with its parent)
        }
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
    auto frame_selection = [&] {
        const Bounds b = selection.empty() ? Bounds{} : selection_bounds();
        if (!b.valid()) {
            target = {0, 0.5f, 0};
            distance = 14.0f;
            return;
        }
        target = b.center();
        const vec3 s = b.size();
        distance = std::clamp(std::max({s.x, s.y, s.z}) * 1.6f + 1.5f, 1.5f, 400.0f);
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
    enum class Popup { None, Open, Add, AddModel, SaveAs, Context, ChangeModel, Confirm } popup = Popup::None;
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
    int outliner_drag = -1;
    vec2 outliner_press{};
    bool outliner_dragging = false;
    float outliner_scroll = 0.0f;
    bool prev_left = false;

    app.update([&](Frame f) {
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
        const bool typing = !text_field.empty();
        const bool popup_open = popup != Popup::None;
        const bool over_view = view.contains(m) && !popup_open;

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

        // ------------------------------------------------------ gizmo geometry
        const std::vector<int> roots = selection_roots();
        vec3 pivot{};
        for (int i : roots) pivot = pivot + scene.world_transform(i).position;
        if (!roots.empty()) pivot = pivot / static_cast<float>(roots.size());
        vec2 pivot_s{};
        const bool gizmo_visible = !roots.empty() && camera.world_to_screen(pivot, view, pivot_s);
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
        if (left_pressed && over_view && !flying && !alt && !typing) {
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
                    }
                    scene.set_world_transform(i, t);
                }
            }
        }

        // ------------------------------------------------------ shortcuts
        if (!typing && !flying) {
            if (!ctrl && !alt) {
                if (f.key_pressed(Key::W)) tool = Tool::Move;
                if (f.key_pressed(Key::E)) tool = Tool::Rotate;
                if (f.key_pressed(Key::R)) tool = Tool::Scale;
                if (f.key_pressed(Key::F)) frame_selection();
                if (f.key_pressed(Key::Delete) || f.key_pressed(Key::X) || f.key_pressed(Key::Backspace)) delete_selection();
                if (shift && f.key_pressed(Key::A)) { popup = Popup::Add; popup_at = m; }
            }
            if (ctrl) {
                if (f.key_pressed(Key::Z)) { if (shift) redo(); else undo(); }
                if (f.key_pressed(Key::Y)) redo();
                if (f.key_pressed(Key::D)) duplicate_selection();
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
        auto number = [&](const std::string& id, Rect r, float& v, float speed, int decimals = 2, rgba tint = theme::field) {
            const bool hover = r.contains(m) && !popup_open;
            const bool is_text = text_field == id;
            if (typed_id == id) {
                record();
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
                    begin_text_input(fmt(v, decimals));
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
                begin_edit();
            }
            if (active_field == id) {
                if (!left_down) {
                    active_field.clear();
                    end_edit();
                } else if (d.x != 0.0f) {
                    v += d.x * speed * (shift ? 0.1f : 1.0f);
                }
            }
        };
        auto text_box = [&](const std::string& id, Rect r, const std::string& value, std::function<void(const std::string&)> commit) {
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
                begin_text_input(value);
                text_commit = [&, commit](const std::string& s) {
                    record();
                    commit(s);
                };
            }
        };
        auto color_row = [&](const std::string& id, float x, float y, float w, rgba& c) {
            f.rect({x, y}, {22, 22}, rgba{c.r, c.g, c.b, 1.0f});
            const float fw = (w - 30) / 3.0f;
            number(id + ".r", Rect{{x + 28, y}, {fw - 3, 22}}, c.r, 0.004f, 2, rgb(0.30f, 0.20f, 0.20f));
            number(id + ".g", Rect{{x + 28 + fw, y}, {fw - 3, 22}}, c.g, 0.004f, 2, rgb(0.20f, 0.28f, 0.20f));
            number(id + ".b", Rect{{x + 28 + fw * 2, y}, {fw - 3, 22}}, c.b, 0.004f, 2, rgb(0.20f, 0.22f, 0.32f));
            c.r = std::clamp(c.r, 0.0f, 1.0f);
            c.g = std::clamp(c.g, 0.0f, 1.0f);
            c.b = std::clamp(c.b, 0.0f, 1.0f);
        };

        // Text entry: Enter commits, Escape cancels, clicking outside the field
        // commits (except Save as, which only saves on Enter). text_rect is
        // from last frame's drawing, which is where the user clicked.
        if (typing) {
            const bool clicked_away = left_pressed && !text_rect.contains(m) && text_field != "#saveas";
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
        {
            const std::string name = (file_path.empty() ? std::string("untitled") : fs::path(file_path).filename().string()) + (dirty ? " *" : "");
            const vec2 tsz = f.measure_text(name, {.size = 15});
            f.text(name, {W - tsz.x - 14, 12}, {.size = 15, .color = dirty ? theme::accent : theme::dim});
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
        if (selection.size() == 1) {
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
            vec_row("scl", "Scale", ent.transform.scale, 0.01f, 2);
            y += 6;
            if (ent.kind != SceneEntity::Kind::Empty) {
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
        const std::string hints = "Middle-drag orbit  |  Shift+middle pan  |  Wheel zoom  |  Hold right: fly (WASD QE)  |  "
                                  "F frame  |  W/E/R gizmo (Ctrl snaps)  |  Ctrl+Z/Ctrl+Shift+Z  |  Ctrl+D  |  Del";
        label(hints, {10, H - BOTTOM + 6}, theme::faint, 12);
        if (!status.empty() && now_seconds() - status_time < 5.0) {
            const vec2 ssz = f.measure_text(status, {.size = 13});
            label(status, {W - ssz.x - 12, H - BOTTOM + 5}, theme::accent, 13);
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
                             "Model..."};
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
                    begin_text_input(current.substr(0, current.find('.'))); // empty when untitled: the placeholder shows
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
                        else { popup = Popup::AddModel; popup_at = at; }
                    } else if (was == Popup::AddModel) {
                        add_model(item);
                    } else if (was == Popup::ChangeModel && selection.size() == 1) {
                        record();
                        scene.entities[static_cast<size_t>(selection[0])].model = item;
                    } else if (was == Popup::Context && context_entity >= 0 && context_entity < static_cast<int>(scene.entities.size())) {
                        if (item == "Rename") {
                            selection = {context_entity};
                            text_field = "name";
                            begin_text_input(scene.entities[static_cast<size_t>(context_entity)].name);
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
        if (popup != Popup::SaveAs && text_field == "#saveas") { // cancelled
            end_text_input();
            text_field.clear();
        }
    });
    return app.run();
}
