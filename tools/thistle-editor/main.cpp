// Thistle Editor — a minimal prop-placement tool: browse .obj files under
// assets/, click one to drop it into the scene, nudge it around with the
// keyboard, save/load the layout as a Node tree via save_scene()/load_scene().
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
#include <unordered_map>
#include <vector>
using namespace thistle;
namespace fs = std::filesystem;

namespace {

struct PaletteEntry {
    std::string obj_path;
    std::string texture_path; // empty = no texture, flat mesh_tint
    std::string label;
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

} // namespace

int main() {
    App app{{.title = "Thistle Editor", .width = 1280, .height = 800}};

    // Without a loaded font, f.text()/f.button() silently draw nothing —
    // there's no built-in fallback font anywhere in Thistle, by design (see
    // docs/drawing.md), so every real UI has to bring its own. This is the
    // editor's own UI font (Kenney Future, CC0), unrelated to whatever font
    // the props being placed might want.
    load_font("editor_assets/kenney-future.ttf");

    const std::vector<PaletteEntry> palette = scan_palette("assets");

    // Dedups GPU uploads across repeated spawns of the same palette entry —
    // load_scene() keeps its own separate caches internally, so a loaded
    // prop and a palette-spawned one sharing a path may double-upload; a
    // cosmetic inefficiency, not a correctness issue (same as sprite_path).
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

    auto spawn = [&](const PaletteEntry& pe) {
        Node* n = scene->add_child();
        n->mesh_path = pe.obj_path;
        n->mesh_texture_path = pe.texture_path;
        n->mesh = get_mesh(pe.obj_path);
        n->mesh_texture = get_tex(pe.texture_path);
        // Fan new spawns out in a grid instead of stacking them all at the
        // origin, so the last-placed one isn't hidden inside the others.
        const int i = static_cast<int>(scene->child_count()) - 1;
        n->mesh_pos = {static_cast<float>(i % 5) * 1.5f - 3.0f, 0.0f, static_cast<float>(i / 5) * 1.5f};
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
        if (selected) f.cube(selected->mesh_pos, {0.12f, 0.12f, 0.12f}, coral); // selection gizmo
        f.camera({0, 0}); // MANDATORY before any 2D drawing below

        // ---- input: camera orbit/zoom (always active) ----
        if (f.key_down(Key::Left))  cam_yaw -= f.dt * 1.5f;
        if (f.key_down(Key::Right)) cam_yaw += f.dt * 1.5f;
        if (f.key_down(Key::Up))    cam_dist = std::max(2.0f, cam_dist - f.dt * 6.0f);
        if (f.key_down(Key::Down))  cam_dist = std::min(30.0f, cam_dist + f.dt * 6.0f);

        // ---- input: manipulate the selected prop ----
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
                scene->remove_child(selected);
                selected = nullptr;
            }
        }

        // ---- input: selection cycling, save/load ----
        if (f.key_pressed(Key::Tab) && scene->child_count() > 0) {
            std::size_t next = 0;
            for (std::size_t i = 0; i < scene->child_count(); ++i) {
                if (scene->child(i) == selected) { next = (i + 1) % scene->child_count(); break; }
            }
            selected = scene->child(next);
        }
        if (f.key_pressed(Key::Escape)) selected = nullptr;
        const bool ctrl = f.key_down(Key::LeftControl);
        if (ctrl && f.key_pressed(Key::S)) do_save();
        if (ctrl && f.key_pressed(Key::O)) do_load();

        // ---- sidebar: palette ----
        const float sidebar_w = 220.0f;
        f.rect({0, 0}, {sidebar_w, static_cast<float>(f.height)}, rgb(0.11f, 0.12f, 0.16f));
        f.text("Thistle Editor", {14, 12}, {.size = 22});
        float y = 50.0f;
        if (palette.empty()) {
            f.text("No .obj files found", {14, y}, {.size = 16, .color = rgb(0.6f, 0.6f, 0.6f)});
            f.text("under ./assets", {14, y + 20}, {.size = 16, .color = rgb(0.6f, 0.6f, 0.6f)});
        }
        for (const PaletteEntry& pe : palette) {
            // f.button() doesn't wrap or clip text (no layout system, see
            // docs/drawing.md) — a label longer than the button just draws
            // past both edges. Shrink the font to fit instead of letting
            // that happen, the same technique measure_text() exists for.
            const float button_w = sidebar_w - 28.0f;
            const float pad = 16.0f;
            ButtonStyle style;
            const vec2 measured = f.measure_text(pe.label, {.size = style.text_size});
            if (measured.x > button_w - pad) style.text_size *= (button_w - pad) / measured.x;
            if (f.button(pe.label, Rect{{14, y}, {button_w, 36}}, style)) spawn(pe);
            y += 44.0f;
        }

        // ---- sidebar: help + status, bottom-anchored ----
        float hy = static_cast<float>(f.height) - 190.0f;
        f.text("Left/Right: orbit   Up/Down: zoom", {14, hy}, {.size = 14});
        f.text("WASD: move   R/F: height", {14, hy + 18}, {.size = 14});
        f.text("Q/E: rotate   Z/X: scale", {14, hy + 36}, {.size = 14});
        f.text("Tab: select next   Backspace: delete", {14, hy + 54}, {.size = 14});
        f.text("Ctrl+S: save   Ctrl+O: load", {14, hy + 72}, {.size = 14});
        f.text("Props: " + std::to_string(scene->child_count()), {14, hy + 100}, {.size = 14, .color = rgb(0.6f, 0.8f, 0.6f)});
        if (selected) {
            f.text("Selected: " + fs::path(selected->mesh_path).stem().string(), {14, hy + 118},
                   {.size = 14, .color = coral});
        }
    });

    return app.run();
}
