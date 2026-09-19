// Thistle Editor — a minimal prop-placement tool: spawn a built-in primitive
// or an .obj file under assets/, nudge it around (keyboard or the Inspector's
// stepper buttons), group props into real parent/child hierarchies, save/load
// the layout as a Node tree via save_scene()/load_scene(). Shift+click a
// palette entry while something's selected to spawn it as a CHILD of the
// selection instead of at the scene root — Node::draw_meshes() composes a
// child's mesh_pos/rotation/scale onto its parent's, so this is real
// grouping: move/rotate/scale the parent and its children follow.
//
// Layout takes real inspiration from Blender's default window (an actual
// screenshot of it — docs.blender.org's Window System Introduction page —
// was checked before writing this, not worked from memory): a dark
// neutral-gray theme, a viewport grid with colored X/Z axis lines instead of
// a flat-shaded ground plane, an Outliner-style indented hierarchy tree on
// the left, and a Properties-style Transform panel with per-axis numeric
// fields on the right. Adapted to what Thistle's immediate-mode UI actually
// has, though: no text input anywhere in the engine, so numeric fields are
// read-only text plus +/- stepper buttons rather than click-to-type boxes,
// and there's no orbiting 3D gizmo ball (no way to draw a fixed screen-space
// overlay independent of the main camera) — just axis-colored lines through
// the origin instead.
//
// This is NOT a level compiler (no BSP, no lighting bake, nothing Hammer's
// actual value proposition rests on) — see docs/ui-and-scenes.md's "Placing
// minor-3D props on a Node" section for what save_scene() actually captures.
// It's the thinnest real GUI on top of that: a way to stop hand-writing the
// JSON yourself. A desktop dev tool, not something meant to run on mobile.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
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
// parent), recording each node's depth for the Outliner's indentation.
void flatten(Node* n, int depth, std::vector<std::pair<Node*, int>>& out) {
    for (std::size_t i = 0; i < n->child_count(); ++i) {
        Node* c = n->child(i);
        out.emplace_back(c, depth);
        flatten(c, depth + 1, out);
    }
}

// Node::draw_meshes() composes a child's mesh_pos/rotation/scale onto its
// parent's (see its doc comment in thistle.hpp) — this mirrors that exact
// math to find a selected node's actual WORLD transform, purely so the
// selection cage and Inspector are drawn/computed relative to what actually
// rendered, not the node's own parent-relative fields. Duplicated rather
// than exposed from the engine: it's a one-off editor need, not something
// worth new public API for.
vec3 rotate_euler(vec3 v, vec3 rot) {
    const float cx = std::cos(rot.x), sx = std::sin(rot.x);
    const float cy = std::cos(rot.y), sy = std::sin(rot.y);
    const float cz = std::cos(rot.z), sz = std::sin(rot.z);
    const vec3 after_x{v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
    const vec3 after_y{after_x.x * cy + after_x.z * sy, after_x.y, -after_x.x * sy + after_x.z * cy};
    const vec3 after_z{after_y.x * cz - after_y.y * sz, after_y.x * sz + after_y.y * cz, after_y.z};
    return after_z;
}

struct WorldTransform { vec3 pos{0, 0, 0}, rot{0, 0, 0}, scale{1, 1, 1}; };

WorldTransform world_mesh_transform(Node* n) {
    std::vector<Node*> chain;
    for (Node* cur = n; cur; cur = cur->parent()) chain.push_back(cur);
    WorldTransform t;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Node* cur = *it;
        const vec3 scaled_local{cur->mesh_pos.x * t.scale.x, cur->mesh_pos.y * t.scale.y, cur->mesh_pos.z * t.scale.z};
        t.pos = t.pos + rotate_euler(scaled_local, t.rot);
        t.rot = t.rot + cur->mesh_rotation;
        t.scale = {t.scale.x * cur->mesh_scale.x, t.scale.y * cur->mesh_scale.y, t.scale.z * cur->mesh_scale.z};
    }
    return t;
}

// A viewport grid instead of a flat-shaded ground plane — closer to how
// Blender's/Unity's viewports actually read at a glance. X is red, Z is
// blue (this engine is Y-up, so the ground plane is XZ, not Blender's XY —
// colors otherwise follow the same red/green/blue = X/Y/Z convention every
// one of these tools uses). The short green stub at the origin stands in
// for Y, which has no ground line of its own.
void draw_grid(Frame& f, float half_size, float step) {
    const rgba line_color = rgb(0.32f, 0.32f, 0.35f);
    for (float v = -half_size; v <= half_size + 0.001f; v += step) {
        if (std::fabs(v) < 0.001f) continue; // the two axis lines are drawn separately, in color
        f.line3d({v, 0, -half_size}, {v, 0, half_size}, line_color);
        f.line3d({-half_size, 0, v}, {half_size, 0, v}, line_color);
    }
    f.line3d({-half_size, 0, 0}, {half_size, 0, 0}, rgb(0.75f, 0.28f, 0.28f)); // X
    f.line3d({0, 0, -half_size}, {0, 0, half_size}, rgb(0.3f, 0.45f, 0.85f)); // Z
    f.line3d({0, 0, 0}, {0, half_size * 0.1f, 0}, rgb(0.35f, 0.75f, 0.4f));   // Y stub
}

// A wireframe box around a world-space center/half-extent — stands in for
// Blender's orange selection outline (there's no outline-shader equivalent
// here, no shader stage at all actually, so a drawn cage is the honest
// substitute). Sized off the node's own scale, not a real computed mesh
// bounding box (mesh triangle data isn't exposed to draw a real AABB from) —
// approximate, not exact, for anything that isn't unit-sized to begin with.
void draw_selection_cage(Frame& f, vec3 center, vec3 half_extent, rgba color) {
    const float hx = half_extent.x, hy = half_extent.y, hz = half_extent.z;
    const vec3 c[8] = {
        {center.x - hx, center.y - hy, center.z - hz}, {center.x + hx, center.y - hy, center.z - hz},
        {center.x + hx, center.y + hy, center.z - hz}, {center.x - hx, center.y + hy, center.z - hz},
        {center.x - hx, center.y - hy, center.z + hz}, {center.x + hx, center.y - hy, center.z + hz},
        {center.x + hx, center.y + hy, center.z + hz}, {center.x - hx, center.y + hy, center.z + hz},
    };
    static constexpr int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto& e : edges) f.line3d(c[e[0]], c[e[1]], color);
}

// Dark neutral-gray palette (sampled from an actual Blender screenshot, not
// guessed from memory) plus one warm accent color for selection/primary
// actions — Blender's own selection color is closer to orange, used here too.
namespace theme {
const rgba chrome   = rgb(0.09f, 0.09f, 0.10f);  // top bar, outer panel background
const rgba panel    = rgb(0.15f, 0.15f, 0.16f);  // hierarchy/inspector body
const rgba panel_alt = rgb(0.19f, 0.19f, 0.21f); // alternating row background
const rgba viewport_clear = rgb(0.14f, 0.15f, 0.17f);
const rgba text_dim = rgb(0.6f, 0.6f, 0.65f);
const rgba text_dim2 = rgb(0.5f, 0.5f, 0.5f);
const rgba accent   = rgb(0.85f, 0.5f, 0.18f);   // Blender-orange selection accent
} // namespace theme

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

    // Layout constants — sized for the 1280x800 default window; everything
    // below is computed off f.width/f.height so it still lays out sanely if
    // the window is resized.
    constexpr float TOP_H = 42.0f;
    constexpr float BOTTOM_H = 92.0f;
    constexpr float LEFT_W = 230.0f;
    constexpr float RIGHT_W = 260.0f;

    app.update([&](Frame f) {
        const float win_w = static_cast<float>(f.width);
        const float win_h = static_cast<float>(f.height);

        f.clear(theme::viewport_clear);

        // ---- 3D viewport (fills the whole window; UI chrome draws on top
        // after, covering the edges — there's no viewport/scissor rect to
        // clip 3D drawing to a sub-region, so this is the same technique the
        // very first version of this editor already used) ----
        Camera3D cam;
        cam.eye = {cam_dist * std::sin(cam_yaw), 4.5f, cam_dist * std::cos(cam_yaw)};
        cam.target = {0.0f, 0.0f, 0.0f};
        f.camera3d(cam);
        draw_grid(f, 12.0f, 1.0f);
        scene->draw_meshes(f);
        WorldTransform sel_world;
        if (selected) {
            sel_world = world_mesh_transform(selected);
            const vec3 half{0.55f * sel_world.scale.x, 0.55f * sel_world.scale.y, 0.55f * sel_world.scale.z};
            draw_selection_cage(f, sel_world.pos, half, theme::accent);
        }
        f.camera({0, 0}); // MANDATORY before any 2D drawing below

        // Whole-tree flatten (with depth, for the Outliner's indentation) —
        // once per frame, cheap at editor-scale prop counts.
        std::vector<std::pair<Node*, int>> all;
        flatten(scene.get(), 0, all);

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
                if (all[i].first == selected) { next = (i + 1) % all.size(); break; }
            }
            selected = all[next].first;
        }
        if (f.key_pressed(Key::Escape)) selected = nullptr;
        const bool ctrl = f.key_down(Key::LeftControl);
        const bool shift = f.key_down(Key::LeftShift);
        if (ctrl && f.key_pressed(Key::S)) do_save();
        if (ctrl && f.key_pressed(Key::O)) do_load();

        // ================= UI chrome =================

        // ---- top bar ----
        f.rect({0, 0}, {win_w, TOP_H}, theme::chrome);
        f.text("Thistle Editor", {14, 10}, {.size = 20});
        {
            ButtonStyle save_style;
            save_style.bg_press = theme::accent;
            if (f.button("Save", Rect{{win_w - 190, 6}, {84, 30}}, save_style)) do_save();
            if (f.button("Load", Rect{{win_w - 100, 6}, {84, 30}})) do_load();
        }

        // ---- left panel: Outliner (hierarchy tree) ----
        f.rect({0, TOP_H}, {LEFT_W, win_h - TOP_H - BOTTOM_H}, theme::panel);
        f.text("Outliner", {12, TOP_H + 8}, {.size = 16, .color = theme::text_dim});
        {
            float y = TOP_H + 34.0f;
            const float row_h = 26.0f;
            if (all.empty()) {
                f.text("Nothing in the scene yet —", {12, y}, {.size = 13, .color = theme::text_dim2});
                f.text("spawn something below.", {12, y + 16}, {.size = 13, .color = theme::text_dim2});
            }
            for (const auto& [node, depth] : all) {
                if (y > win_h - BOTTOM_H - row_h) break; // no scrolling — just stop, rather than draw off-panel
                const bool is_selected = (node == selected);
                if (is_selected) f.rect({0, y}, {LEFT_W, row_h}, rgba{theme::accent.r, theme::accent.g, theme::accent.b, 0.35f});
                const std::string label = node->mesh_prim != Prim::None
                    ? std::string(node->mesh.valid() ? "" : "(invalid) ") +
                          (node->mesh_prim == Prim::Cube ? "Cube" : node->mesh_prim == Prim::Sphere ? "Sphere"
                           : node->mesh_prim == Prim::Cylinder ? "Cylinder" : node->mesh_prim == Prim::Cone ? "Cone" : "Plane")
                    : fs::path(node->mesh_path).stem().string();
                const float indent = 12.0f + static_cast<float>(depth) * 16.0f;
                const float row_w = LEFT_W - indent - 8.0f;
                const std::string shown = label.empty() ? "(unnamed)" : label;
                // The whole row is a click target (an invisible-background
                // button spanning the panel width), so clicking anywhere on
                // the row selects it, not just the label text itself.
                ButtonStyle row_style;
                row_style.bg = rgba{0, 0, 0, 0};
                row_style.bg_hover = rgba{1, 1, 1, 0.06f};
                row_style.bg_press = rgba{1, 1, 1, 0.1f};
                row_style.text = is_selected ? white : rgb(0.82f, 0.82f, 0.82f);
                row_style.text_size = 15.0f;
                const vec2 measured = f.measure_text(shown, {.size = row_style.text_size});
                if (measured.x > row_w - 8.0f) row_style.text_size *= (row_w - 8.0f) / measured.x;
                if (f.button(shown, Rect{{indent, y}, {row_w, row_h}}, row_style)) {
                    selected = node;
                }
                y += row_h;
            }
        }

        // ---- right panel: Inspector (selected node's transform) ----
        f.rect({win_w - RIGHT_W, TOP_H}, {RIGHT_W, win_h - TOP_H - BOTTOM_H}, theme::panel);
        {
            const float px = win_w - RIGHT_W + 12.0f;
            const float pw = RIGHT_W - 24.0f;
            float y = TOP_H + 8.0f;
            f.text("Inspector", {px, y}, {.size = 16, .color = theme::text_dim});
            y += 26.0f;
            if (!selected) {
                f.text("Nothing selected.", {px, y}, {.size = 14, .color = theme::text_dim2});
                f.text("Click a row in the Outliner,", {px, y + 20}, {.size = 13, .color = theme::text_dim2});
                f.text("or a prop in the viewport's", {px, y + 36}, {.size = 13, .color = theme::text_dim2});
                f.text("Tab-cycle order.", {px, y + 52}, {.size = 13, .color = theme::text_dim2});
            } else {
                const std::string kind = selected->mesh_prim != Prim::None ? "Primitive" : "Model";
                f.text(kind, {px, y}, {.size = 13, .color = theme::accent});
                y += 22.0f;

                // A labeled X/Y/Z row with a live value readout and +/-
                // steppers — Thistle has no text-input widget at all (see
                // docs/ui-and-scenes.md), so this is the closest honest
                // equivalent to Blender's/Unity's click-to-type number
                // fields: precise, discoverable, no keyboard focus needed.
                auto axis_row = [&](const char* label, float y_pos, float& value, float step, float min_value) {
                    f.text(label, {px, y_pos + 6}, {.size = 13});
                    f.text([&]{ char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value)); return std::string(buf); }(),
                           {px + 22, y_pos + 6}, {.size = 13, .color = rgb(0.85f, 0.85f, 0.85f)});
                    ButtonStyle step_style;
                    step_style.text_size = 16.0f;
                    if (f.button("-", Rect{{px + pw - 64, y_pos}, {28, 24}}, step_style)) value = std::max(min_value, value - step);
                    if (f.button("+", Rect{{px + pw - 30, y_pos}, {28, 24}}, step_style)) value += step;
                };

                f.text("Position", {px, y}, {.size = 13, .color = theme::text_dim});
                y += 18.0f;
                axis_row("X", y, selected->mesh_pos.x, 0.1f, -1000.0f); y += 28.0f;
                axis_row("Y", y, selected->mesh_pos.y, 0.1f, -1000.0f); y += 28.0f;
                axis_row("Z", y, selected->mesh_pos.z, 0.1f, -1000.0f); y += 34.0f;

                f.text("Rotation (Y)", {px, y}, {.size = 13, .color = theme::text_dim});
                y += 18.0f;
                axis_row("Y", y, selected->mesh_rotation.y, 0.1745f, -1000.0f); y += 34.0f;

                f.text("Scale", {px, y}, {.size = 13, .color = theme::text_dim});
                y += 18.0f;
                axis_row("X", y, selected->mesh_scale.x, 0.1f, 0.05f); y += 28.0f;
                axis_row("Y", y, selected->mesh_scale.y, 0.1f, 0.05f); y += 28.0f;
                axis_row("Z", y, selected->mesh_scale.z, 0.1f, 0.05f); y += 34.0f;

                f.rect({px, y}, {24, 24}, selected->mesh_tint);
                f.text("Tint", {px + 32, y + 4}, {.size = 13, .color = theme::text_dim});
                y += 36.0f;

                if (selected->child_count() > 0) {
                    f.text("Children: " + std::to_string(selected->child_count()), {px, y}, {.size = 13, .color = theme::text_dim});
                    y += 24.0f;
                }

                ButtonStyle delete_style;
                delete_style.bg = rgb(0.35f, 0.16f, 0.14f);
                delete_style.bg_hover = rgb(0.45f, 0.2f, 0.17f);
                delete_style.bg_press = rgb(0.6f, 0.25f, 0.2f);
                if (f.button("Delete", Rect{{px, y}, {pw, 30}}, delete_style)) {
                    Node* parent = selected->parent() ? selected->parent() : scene.get();
                    parent->remove_child(selected);
                    selected = nullptr;
                }
            }
        }

        // ---- bottom bar: spawnable palette (primitives + models) ----
        f.rect({0, win_h - BOTTOM_H}, {win_w, BOTTOM_H}, theme::chrome);
        {
            const float item_w = 100.0f, item_h = 32.0f, gap = 8.0f;
            const float label_y = win_h - BOTTOM_H + 6.0f;
            const float row_y = label_y + 16.0f;
            float x = 12.0f;

            // f.button() doesn't wrap or clip text (no layout system, see
            // docs/drawing.md) — a label longer than the button just draws
            // past both edges, which is exactly the overlapping-text bug an
            // earlier version of this redesign shipped with (Cylinder/
            // Character-orc bled into their neighbors). Shrink to fit,
            // starting from a sane base size instead of ButtonStyle's
            // default 28 (much too large for a 100px-wide palette slot).
            auto palette_button = [&](const PaletteEntry& pe, float bx, ButtonStyle style = {}) {
                style.text_size = 15.0f;
                const vec2 measured = f.measure_text(pe.label, {.size = style.text_size});
                if (measured.x > item_w - 12.0f) style.text_size *= (item_w - 12.0f) / measured.x;
                return f.button(pe.label, Rect{{bx, row_y}, {item_w, item_h}}, style);
            };

            f.text("PRIMITIVES", {x, label_y}, {.size = 11, .color = theme::text_dim});
            for (const PaletteEntry& pe : primitives) {
                ButtonStyle style;
                style.bg_press = pe.tint;
                if (palette_button(pe, x, style)) spawn(pe, shift);
                x += item_w + gap;
            }

            x += 20.0f;
            f.text("MODELS (assets/)", {x, label_y}, {.size = 11, .color = theme::text_dim});
            if (file_palette.empty()) {
                f.text("none found", {x, row_y + 8}, {.size = 13, .color = theme::text_dim2});
            } else {
                for (const PaletteEntry& pe : file_palette) {
                    if (palette_button(pe, x)) spawn(pe, shift);
                    x += item_w + gap;
                }
            }
        }

        // ---- compact keybinding hint, bottom-right corner of the viewport ----
        f.text("WASD/R/F move   Q/E rotate   Z/X scale   Tab select   Shift+click: child of selection",
               {LEFT_W + 12, win_h - BOTTOM_H - 20}, {.size = 12, .color = rgba{1, 1, 1, 0.5f}});
    });

    return app.run();
}
