// Thistle Editor — a minimal prop-placement tool: browse .obj files (or use
// a built-in primitive) under assets/, click one to drop it into the scene,
// nudge it around with the keyboard, save/load the layout as a Node tree via
// save_scene()/load_scene(). Shift+click a palette entry while something's
// selected to spawn it as a CHILD of the selection instead of at the scene
// root — Node::draw_meshes() composes a child's mesh_pos/rotation/scale onto
// its parent's, so this is real grouping: move/rotate/scale the parent and
// its children follow.
//
// This is NOT a level compiler (no BSP, no lighting bake, nothing Hammer's
// actual value proposition rests on) — see docs/ui-and-scenes.md's "Placing
// minor-3D props on a Node" section for what save_scene() actually captures.
// It's the thinnest real GUI on top of that: a way to stop hand-writing the
// JSON yourself. A desktop dev tool, not something meant to run on mobile.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#endif

using namespace thistle;
namespace fs = std::filesystem;

namespace {

// Launching via a terminal already `cd`'d into the build output leaves the
// working directory right where "assets/"/"editor_assets/..." expect it —
// but launching the same binary via Finder/double-click (the normal way an
// end user runs a .app) starts it with an arbitrary working directory (the
// user's home directory, typically), not the bundle's own folder. Every
// relative path in this file silently resolves against the wrong place in
// that case. Fix it once, here, by finding the actual running executable
// and chdir'ing next to it, instead of assuming the caller got cwd right.
void chdir_to_executable_dir() {
#if defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return;
    std::error_code ec;
    fs::current_path(fs::path(resolved).parent_path(), ec);
#elif defined(_WIN32)
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) return;
    std::error_code ec;
    fs::current_path(fs::path(path).parent_path(), ec);
#elif defined(__linux__)
    char path[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return;
    path[len] = '\0';
    std::error_code ec;
    fs::current_path(fs::path(path).parent_path(), ec);
#endif
}

// A spawnable palette entry — either a file-based prop (prim == Prim::None,
// obj_path/texture_path used) or a built-in primitive shape (prim set,
// obj_path/texture_path unused, tint used instead since generated shapes
// have no natural texture atlas convention).
struct PaletteEntry {
    Prim prim = Prim::None;
    std::string obj_path;
    std::string texture_path;
    std::string label;
    rgba tint = white;
};

// Scans `dir` for .obj files. Texture for each is resolved by convention,
// since load_mesh() doesn't read .mtl: same-basename .png next to the .obj
// first (chest.obj -> chest.png), else a shared atlas.png/colormap.png in
// that same folder (the common case for a kit like Kenney's, one atlas
// shared across many props), else no texture at all.
std::vector<PaletteEntry> scan_palette(const std::string& dir) {
    std::vector<PaletteEntry> out;
    if (!fs::exists(dir)) return out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".obj") continue;
        PaletteEntry pe;
        pe.obj_path = entry.path().string();
        pe.label = entry.path().stem().string();
        fs::path same_name = entry.path(); same_name.replace_extension(".png");
        fs::path atlas_a = entry.path().parent_path() / "atlas.png";
        fs::path atlas_b = entry.path().parent_path() / "colormap.png";
        if (fs::exists(same_name)) pe.texture_path = same_name.string();
        else if (fs::exists(atlas_a)) pe.texture_path = atlas_a.string();
        else if (fs::exists(atlas_b)) pe.texture_path = atlas_b.string();
        out.push_back(std::move(pe));
    }
    std::sort(out.begin(), out.end(), [](const PaletteEntry& a, const PaletteEntry& b) { return a.label < b.label; });
    return out;
}

// Always-available built-in shapes — these don't depend on assets/ at all.
std::vector<PaletteEntry> primitive_palette() {
    return {
        {Prim::Cube,     "", "", "Cube",     rgb(0.85f, 0.35f, 0.3f)},
        {Prim::Sphere,   "", "", "Sphere",   rgb(0.9f, 0.8f, 0.2f)},
        {Prim::Cylinder, "", "", "Cylinder", rgb(0.4f, 0.7f, 0.9f)},
        {Prim::Cone,     "", "", "Cone",     rgb(0.8f, 0.4f, 0.8f)},
        {Prim::Plane,    "", "", "Plane",    rgb(0.4f, 0.7f, 0.4f)},
    };
}

// Flattens a tree into visitation order (depth-first, children after their
// parent) — needed because Tab-cycling and the "Props: N" count have to
// cover the WHOLE tree now that spawning-as-child makes real nesting
// possible, not just scene's direct children.
void flatten(Node* n, std::vector<Node*>& out) {
    for (std::size_t i = 0; i < n->child_count(); ++i) {
        Node* c = n->child(i);
        out.push_back(c);
        flatten(c, out);
    }
}

// Node::draw_meshes() composes a child's mesh_pos/rotation/scale onto its
// parent's (see its doc comment in thistle.hpp) — this mirrors that exact
// math to find a selected node's actual WORLD position, purely so the
// selection gizmo is drawn where the node actually rendered, not at its
// parent-relative mesh_pos. Duplicated rather than exposed from the engine:
// it's a one-off need for a gizmo, not something worth new public API for.
vec3 rotate_euler(vec3 v, vec3 rot) {
    const float cx = std::cos(rot.x), sx = std::sin(rot.x);
    const float cy = std::cos(rot.y), sy = std::sin(rot.y);
    const float cz = std::cos(rot.z), sz = std::sin(rot.z);
    const vec3 after_x{v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
    const vec3 after_y{after_x.x * cy + after_x.z * sy, after_x.y, -after_x.x * sy + after_x.z * cy};
    const vec3 after_z{after_y.x * cz - after_y.y * sz, after_y.x * sz + after_y.y * cz, after_y.z};
    return after_z;
}

vec3 world_mesh_pos(Node* n) {
    std::vector<Node*> chain;
    for (Node* cur = n; cur; cur = cur->parent()) chain.push_back(cur);
    vec3 pos{0, 0, 0}, rot{0, 0, 0}, scale{1, 1, 1};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Node* cur = *it;
        const vec3 scaled_local{cur->mesh_pos.x * scale.x, cur->mesh_pos.y * scale.y, cur->mesh_pos.z * scale.z};
        pos = pos + rotate_euler(scaled_local, rot);
        rot = rot + cur->mesh_rotation;
        scale = {scale.x * cur->mesh_scale.x, scale.y * cur->mesh_scale.y, scale.z * cur->mesh_scale.z};
    }
    return pos;
}

} // namespace

int main() {
    chdir_to_executable_dir();

    App app{{.title = "Thistle Editor", .width = 1280, .height = 800}};

    // Without a loaded font, f.text()/f.button() silently draw nothing —
    // there's no built-in fallback font anywhere in Thistle, by design (see
    // docs/drawing.md), so every real UI has to bring its own. This is the
    // editor's own UI font (Kenney Future, CC0), unrelated to whatever font
    // the props being placed might want.
    load_font("editor_assets/kenney-future.ttf");

    const std::vector<PaletteEntry> file_palette = scan_palette("assets");
    const std::vector<PaletteEntry> primitives = primitive_palette();

    // Dedups GPU uploads across repeated spawns of the same palette entry —
    // load_scene() keeps its own separate caches internally, so a loaded
    // prop and a palette-spawned one sharing a path may double-upload; a
    // cosmetic inefficiency, not a correctness issue (same as sprite_path).
    // Primitives aren't cached here — each spawn gets a fresh generated Mesh,
    // same as load_scene() does for a mesh_prim node.
    std::unordered_map<std::string, Mesh> mesh_cache;
    std::unordered_map<std::string, Texture> tex_cache;
    auto get_mesh = [&](const std::string& path) {
        auto it = mesh_cache.find(path);
        if (it == mesh_cache.end()) it = mesh_cache.emplace(path, load_mesh(path)).first;
        return it->second;
    };
    auto get_tex = [&](const std::string& path) -> Texture {
        if (path.empty()) return Texture{};
        auto it = tex_cache.find(path);
        if (it == tex_cache.end()) it = tex_cache.emplace(path, load_texture(path)).first;
        return it->second;
    };

    auto scene = std::make_unique<Node>();
    Node* selected = nullptr;

    const std::string save_file = save::path();
    const std::string scene_path = save_file.substr(0, save_file.find_last_of("/\\")) + "/thistle_editor_scene.json";

    auto spawn = [&](const PaletteEntry& pe, bool as_child_of_selection) {
        Node* parent = (as_child_of_selection && selected) ? selected : scene.get();
        Node* n = parent->add_child();
        if (pe.prim != Prim::None) {
            n->mesh_prim = pe.prim;
            switch (pe.prim) {
                case Prim::Cube:     n->mesh = make_cube_mesh(); break;
                case Prim::Sphere:   n->mesh = make_sphere_mesh(); break;
                case Prim::Cylinder: n->mesh = make_cylinder_mesh(); break;
                case Prim::Cone:     n->mesh = make_cone_mesh(); break;
                case Prim::Plane:    n->mesh = make_plane_mesh(); break;
                case Prim::None:     break;
            }
            n->mesh_tint = pe.tint;
        } else {
            n->mesh_path = pe.obj_path;
            n->mesh_texture_path = pe.texture_path;
            n->mesh = get_mesh(pe.obj_path);
            n->mesh_texture = get_tex(pe.texture_path);
        }
        if (parent == scene.get()) {
            // Fan root-level spawns out in a grid instead of stacking them
            // all at the origin, so the last-placed one isn't hidden inside
            // the others.
            const int i = static_cast<int>(parent->child_count()) - 1;
            n->mesh_pos = {static_cast<float>(i % 5) * 1.5f - 3.0f, 0.0f, static_cast<float>(i / 5) * 1.5f};
        } else {
            // Child spawns are relative to the parent (see Node::draw_meshes()) —
            // a small offset so it's visibly next to its parent, not exactly
            // overlapping it.
            n->mesh_pos = {0.4f, 0.4f, 0.0f};
        }
        selected = n;
    };

    auto do_save = [&] {
        const bool ok = save_scene(*scene, scene_path);
        log_info(std::string("Thistle Editor: save ") + (ok ? "ok -> " : "FAILED -> ") + scene_path);
    };
    auto do_load = [&] {
        std::unique_ptr<Node> loaded = load_scene(scene_path);
        if (!loaded) { log_info("Thistle Editor: nothing to load at " + scene_path); return; }
        scene = std::move(loaded);
        selected = nullptr;
        log_info("Thistle Editor: loaded " + scene_path);
    };

    float cam_yaw = 0.6f;
    float cam_dist = 9.0f;

    app.update([&](Frame f) {
        f.clear(rgb(0.07f, 0.08f, 0.11f));

        // ---- 3D viewport (whole window; the sidebar draws on top after) ----
        Camera3D cam;
        cam.eye = {cam_dist * std::sin(cam_yaw), 4.5f, cam_dist * std::cos(cam_yaw)};
        cam.target = {0.0f, 0.0f, 0.0f};
        f.camera3d(cam);
        f.plane3d({0, 0, 0}, 24.0f, 24.0f, rgb(0.2f, 0.22f, 0.28f));
        scene->draw_meshes(f);
        if (selected) f.cube(world_mesh_pos(selected), {0.12f, 0.12f, 0.12f}, coral); // selection gizmo
        f.camera({0, 0}); // MANDATORY before any 2D drawing below

        // Whole-tree flatten, once per frame — cheap at editor-scale prop
        // counts, and needed for both Tab-cycling and the props count below.
        std::vector<Node*> all;
        flatten(scene.get(), all);

        // ---- input: camera orbit/zoom (always active) ----
        if (f.key_down(Key::Left))  cam_yaw -= f.dt * 1.5f;
        if (f.key_down(Key::Right)) cam_yaw += f.dt * 1.5f;
        if (f.key_down(Key::Up))    cam_dist = std::max(2.0f, cam_dist - f.dt * 6.0f);
        if (f.key_down(Key::Down))  cam_dist = std::min(30.0f, cam_dist + f.dt * 6.0f);

        // ---- input: manipulate the selected prop (relative to its own
        // parent, per Node::draw_meshes()'s composition — moving a group's
        // parent moves the whole group) ----
        if (selected) {
            const float move_speed = 2.5f * f.dt;
            const float rot_speed = 1.8f * f.dt;
            if (f.key_down(Key::W)) selected->mesh_pos.z -= move_speed;
            if (f.key_down(Key::S)) selected->mesh_pos.z += move_speed;
            if (f.key_down(Key::A)) selected->mesh_pos.x -= move_speed;
            if (f.key_down(Key::D)) selected->mesh_pos.x += move_speed;
            if (f.key_down(Key::R)) selected->mesh_pos.y += move_speed;
            if (f.key_down(Key::F)) selected->mesh_pos.y -= move_speed;
            if (f.key_down(Key::Q)) selected->mesh_rotation.y -= rot_speed;
            if (f.key_down(Key::E)) selected->mesh_rotation.y += rot_speed;
            if (f.key_down(Key::Z)) selected->mesh_scale = selected->mesh_scale * (1.0f - f.dt);
            if (f.key_down(Key::X)) selected->mesh_scale = selected->mesh_scale * (1.0f + f.dt);
            if (f.key_pressed(Key::Backspace)) {
                // Deletes the whole subtree, not just the one node — Node
                // owns its children via unique_ptr, so removing it from its
                // actual parent (not always `scene` now that nesting is
                // real) destroys everything under it too.
                Node* parent = selected->parent() ? selected->parent() : scene.get();
                parent->remove_child(selected);
                selected = nullptr;
            }
        }

        // ---- input: selection cycling (whole tree), save/load ----
        if (f.key_pressed(Key::Tab) && !all.empty()) {
            std::size_t next = 0;
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (all[i] == selected) { next = (i + 1) % all.size(); break; }
            }
            selected = all[next];
        }
        if (f.key_pressed(Key::Escape)) selected = nullptr;
        const bool ctrl = f.key_down(Key::LeftControl);
        const bool shift = f.key_down(Key::LeftShift);
        if (ctrl && f.key_pressed(Key::S)) do_save();
        if (ctrl && f.key_pressed(Key::O)) do_load();

        // ---- sidebar ----
        const float sidebar_w = 220.0f;
        const float button_w = sidebar_w - 28.0f;
        const float pad = 16.0f;
        f.rect({0, 0}, {sidebar_w, static_cast<float>(f.height)}, rgb(0.11f, 0.12f, 0.16f));
        f.text("Thistle Editor", {14, 12}, {.size = 22});

        // f.button() doesn't wrap or clip text (no layout system, see
        // docs/drawing.md) — a label longer than the button just draws past
        // both edges. Shrink the font to fit instead of letting that happen,
        // the same technique measure_text() exists for.
        auto fitted_button = [&](const std::string& label, float y, ButtonStyle style = {}) {
            const vec2 measured = f.measure_text(label, {.size = style.text_size});
            if (measured.x > button_w - pad) style.text_size *= (button_w - pad) / measured.x;
            return f.button(label, Rect{{14, y}, {button_w, 36}}, style);
        };

        float y = 46.0f;
        f.text("Primitives", {14, y}, {.size = 14, .color = rgb(0.6f, 0.6f, 0.65f)});
        y += 20.0f;
        for (const PaletteEntry& pe : primitives) {
            ButtonStyle style;
            style.bg_press = pe.tint;
            if (fitted_button(pe.label, y, style)) spawn(pe, shift);
            y += 40.0f;
        }

        y += 10.0f;
        f.text("Models (assets/)", {14, y}, {.size = 14, .color = rgb(0.6f, 0.6f, 0.65f)});
        y += 20.0f;
        if (file_palette.empty()) {
            f.text("None found under", {14, y}, {.size = 14, .color = rgb(0.5f, 0.5f, 0.5f)});
            f.text("./assets", {14, y + 18}, {.size = 14, .color = rgb(0.5f, 0.5f, 0.5f)});
            y += 40.0f;
        } else {
            for (const PaletteEntry& pe : file_palette) {
                if (fitted_button(pe.label, y)) spawn(pe, shift);
                y += 40.0f;
            }
        }

        // ---- sidebar: help + status, bottom-anchored ----
        float hy = static_cast<float>(f.height) - 210.0f;
        f.text("Left/Right: orbit   Up/Down: zoom", {14, hy}, {.size = 14});
        f.text("WASD: move   R/F: height", {14, hy + 18}, {.size = 14});
        f.text("Q/E: rotate   Z/X: scale", {14, hy + 36}, {.size = 14});
        f.text("Tab: select next   Backspace: delete", {14, hy + 54}, {.size = 14});
        f.text("Shift+click: spawn as child of selection", {14, hy + 72}, {.size = 14});
        f.text("Ctrl+S: save   Ctrl+O: load", {14, hy + 90}, {.size = 14});
        f.text("Props: " + std::to_string(all.size()), {14, hy + 118}, {.size = 14, .color = rgb(0.6f, 0.8f, 0.6f)});
        if (selected) {
            const std::string label = selected->mesh_prim != Prim::None
                ? "primitive"
                : fs::path(selected->mesh_path).stem().string();
            std::string status = "Selected: " + label;
            if (selected->child_count() > 0) status += " (+" + std::to_string(selected->child_count()) + " child)";
            f.text(status, {14, hy + 136}, {.size = 14, .color = coral});
        }
    });

    return app.run();
}
