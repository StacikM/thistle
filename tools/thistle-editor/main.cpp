// Thistle Editor — a prop-placement tool: right-click the Outliner (empty
// space to add at the scene root, an existing row to add a child of it or
// delete it) to spawn a built-in primitive (including an invisible Trigger
// volume) or an .obj file under assets/, move/select things with the mouse
// (click to select+drag along the ground, right-drag empty viewport space to
// orbit the camera, scroll to zoom) or the keyboard (WASD/R/F/Q/E/Z/X, still
// there for precise nudging), group props into real parent/child
// hierarchies, save/load the layout as a Node tree via save_scene()/
// load_scene(). Ctrl+click a different Outliner row re-parents the current
// selection onto it.
//
// Layout takes real inspiration from Blender's default window (an actual
// screenshot of it — docs.blender.org's Window System Introduction page —
// was checked before writing this, not worked from memory): a dark
// neutral-gray theme, a viewport grid with colored X/Z axis lines instead of
// a flat-shaded ground plane, an Outliner-style indented hierarchy tree on
// the left, and a Properties-style Transform panel with per-axis numeric
// fields on the right. The Inspector's Name field uses Thistle's real
// begin_text_input()/text_input() capture (there IS text input in the
// engine, just no built-in visual widget for it — this is that widget, for
// exactly one field); the numeric Position/Rotation/Scale fields stay
// steppers rather than click-to-type, since a mouse-drag already covers
// coarse repositioning and steppers are enough for fine nudges. There's
// still no orbiting 3D gizmo ball (no way to draw a fixed screen-space
// overlay independent of the main camera) — just the axis-colored grid
// lines for orientation.
//
// This is NOT a level compiler (no BSP, no lighting bake, nothing Hammer's
// actual value proposition rests on) — see docs/ui-and-scenes.md's "Placing
// minor-3D props on a Node" section for what save_scene() actually captures.
// The Trigger primitive is the same story as a Hammer brush on its own: it's
// geometry (a position + size) and a name, nothing more. Thistle does have
// basic 3D overlap tests now (Box3D/Sphere3D, box3d_sphere3d_overlap(), etc.
// — see docs/drawing.md's "3D collision" section), but nothing calls them
// automatically — no "on enter" callback, no event. Your own game code
// builds a Box3D from a mesh_prim == Prim::Trigger node's
// world_mesh_transform() and calls the overlap test itself, every frame,
// exactly the way Source (not Hammer) is what actually processes a trigger
// brush at runtime.
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
// obj_path/texture_path used) or a built-in primitive/Trigger (prim set,
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
// Trigger is last and visually distinct (cyan, not one of the "real shape"
// colors) since it's a different kind of thing: pure data, never drawn as a
// solid mesh by Node::draw_meshes(), just a named volume your own game code
// interprets — see the Prim::Trigger doc comment in thistle.hpp.
std::vector<PaletteEntry> primitive_palette() {
    return {
        {Prim::Cube,     "", "", "Cube",     rgb(0.85f, 0.35f, 0.3f)},
        {Prim::Sphere,   "", "", "Sphere",   rgb(0.9f, 0.8f, 0.2f)},
        {Prim::Cylinder, "", "", "Cylinder", rgb(0.4f, 0.7f, 0.9f)},
        {Prim::Cone,     "", "", "Cone",     rgb(0.8f, 0.4f, 0.8f)},
        {Prim::Plane,    "", "", "Plane",    rgb(0.4f, 0.7f, 0.4f)},
        {Prim::Trigger,  "", "", "Trigger",  rgb(0.25f, 0.9f, 0.85f)},
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

// True if `ancestor` is somewhere above `node` in the tree — checked before
// letting a Ctrl+click reparent `node` onto one of its own descendants,
// which would otherwise orphan a whole subtree (or, if the reparent target
// IS a descendant of the thing being moved, silently create a cycle).
bool is_ancestor_of(Node* ancestor, Node* node) {
    for (Node* cur = node->parent(); cur; cur = cur->parent()) {
        if (cur == ancestor) return true;
    }
    return false;
}

// Node::draw_meshes() composes a child's mesh_pos/rotation/scale onto its
// parent's (see its doc comment in thistle.hpp) — this mirrors that exact
// math to find a node's actual WORLD transform, purely for editor-side needs
// (the selection cage, mouse picking, drag math). Duplicated rather than
// exposed from the engine: it's editor-specific, not something worth new
// public API for.
vec3 rotate_euler(vec3 v, vec3 rot) {
    const float cx = std::cos(rot.x), sx = std::sin(rot.x);
    const float cy = std::cos(rot.y), sy = std::sin(rot.y);
    const float cz = std::cos(rot.z), sz = std::sin(rot.z);
    const vec3 after_x{v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
    const vec3 after_y{after_x.x * cy + after_x.z * sy, after_x.y, -after_x.x * sy + after_x.z * cy};
    const vec3 after_z{after_y.x * cz - after_y.y * sz, after_y.x * sz + after_y.y * cz, after_y.z};
    return after_z;
}

// The exact inverse of rotate_euler() — undoes Z, then Y, then X (reverse
// order, negated angles), which is what correctly undoes a forward X-then-Y-
// then-Z rotation. Verified against rotate_euler() with a standalone
// round-trip test (36 rotation/vector combinations) before this was wired
// into any drag/reparent math, since rotation order is exactly the kind of
// thing that's easy to get subtly backwards.
vec3 rotate_euler_inverse(vec3 v, vec3 rot) {
    const float cz = std::cos(-rot.z), sz = std::sin(-rot.z);
    const float cy = std::cos(-rot.y), sy = std::sin(-rot.y);
    const float cx = std::cos(-rot.x), sx = std::sin(-rot.x);
    const vec3 after_z{v.x * cz - v.y * sz, v.x * sz + v.y * cz, v.z};
    const vec3 after_y{after_z.x * cy + after_z.z * sy, after_z.y, -after_z.x * sy + after_z.z * cy};
    const vec3 after_x{after_y.x, after_y.y * cx - after_y.z * sx, after_y.y * sx + after_y.z * cx};
    return after_x;
}

// Inverse of the composition Node::world_mesh_transform() performs one level at a
// time: given a WORLD point and a parent's already-known world transform,
// finds the parent-relative mesh_pos that would produce it. Used for both
// mouse-dragging (convert a ground-plane hit back into the selected node's
// own mesh_pos) and reparenting (keep an object's world position stable
// across a parent change). Verified via rotate_euler_inverse()'s own
// round-trip test — this is just that plus un-scaling.
vec3 world_to_local(vec3 world_point, const WorldMeshTransform& parent) {
    const vec3 rotated = world_point - parent.pos;
    const vec3 scaled_local = rotate_euler_inverse(rotated, parent.rotation);
    return {
        parent.scale.x != 0.0f ? scaled_local.x / parent.scale.x : scaled_local.x,
        parent.scale.y != 0.0f ? scaled_local.y / parent.scale.y : scaled_local.y,
        parent.scale.z != 0.0f ? scaled_local.z / parent.scale.z : scaled_local.z,
    };
}

// --- camera-basis ray casting, for mouse picking/dragging. Built directly
// from the camera's own eye/target/up/fov rather than a general 4x4 matrix
// (projection + inverse) — there's exactly one camera here, so a matrix
// library would be a lot of new code to do the same job. Both directions
// (world_to_screen and screen_to_ray) were checked together with a
// standalone round-trip test (project a point, unproject the resulting
// screen position, confirm it lands back on the original point) before
// being wired into anything, for the same reason as the rotation inverse
// above: this is exactly the kind of math that silently comes out backwards.
struct CameraBasis { vec3 eye, forward, right, up; float tan_half_fov, aspect; };

CameraBasis compute_camera_basis(const Camera3D& cam, float aspect) {
    const vec3 f = normalize(cam.target - cam.eye);
    const vec3 r = normalize(cross(f, cam.up));
    const vec3 u = cross(r, f);
    const float tan_half = std::tan(cam.fov_deg * (3.14159265f / 180.0f) * 0.5f);
    return {cam.eye, f, r, u, tan_half, aspect};
}

struct Ray { vec3 origin, dir; };

Ray screen_to_ray(const CameraBasis& cb, float sx, float sy, float width, float height) {
    const float ndc_x = (2.0f * sx / width - 1.0f) * cb.aspect * cb.tan_half_fov;
    const float ndc_y = (1.0f - 2.0f * sy / height) * cb.tan_half_fov;
    const vec3 dir = normalize(cb.forward + cb.right * ndc_x + cb.up * ndc_y);
    return {cb.eye, dir};
}

bool world_to_screen(const CameraBasis& cb, vec3 p, float width, float height, vec2& out) {
    const vec3 v = p - cb.eye;
    const float d = dot(v, cb.forward);
    if (d <= 0.01f) return false; // behind the camera
    const float x_view = dot(v, cb.right);
    const float y_view = dot(v, cb.up);
    const float ndc_x = x_view / (d * cb.tan_half_fov * cb.aspect);
    const float ndc_y = y_view / (d * cb.tan_half_fov);
    out = {(ndc_x * 0.5f + 0.5f) * width, (1.0f - (ndc_y * 0.5f + 0.5f)) * height};
    return true;
}

bool ray_plane_y(const Ray& ray, float plane_y, vec3& out) {
    if (std::fabs(ray.dir.y) < 1e-5f) return false; // parallel to the plane
    const float t = (plane_y - ray.origin.y) / ray.dir.y;
    if (t < 0.0f) return false; // plane is behind the ray's origin
    out = ray.origin + ray.dir * t;
    return true;
}

// A wireframe box, rotation-aware (unlike a naive axis-aligned cage) — used
// for both the selection highlight (stands in for Blender's orange outline;
// there's no outline-shader equivalent here, no shader stage at all) and
// every Trigger volume's visualization, since a trigger that's been rotated
// needs to visibly LOOK rotated or its bounds are actively misleading.
void draw_wire_box(Frame& f, vec3 center, vec3 rot, vec3 half_extent, rgba color) {
    const float hx = half_extent.x, hy = half_extent.y, hz = half_extent.z;
    const vec3 local[8] = {
        {-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {-hx, hy, -hz},
        {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz},
    };
    vec3 c[8];
    for (int i = 0; i < 8; ++i) c[i] = center + rotate_euler(local[i], rot);
    static constexpr int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto& e : edges) f.line3d(c[e[0]], c[e[1]], color);
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

// One-line display label for a node, used by both the Outliner and the
// Inspector: the shape/kind, plus ": name" if one's been set via the
// Inspector's Name field.
std::string node_label(Node* n) {
    std::string base;
    if (n->mesh_prim == Prim::Trigger) {
        base = "Trigger";
    } else if (n->mesh_prim != Prim::None) {
        base = n->mesh_prim == Prim::Cube ? "Cube" : n->mesh_prim == Prim::Sphere ? "Sphere"
             : n->mesh_prim == Prim::Cylinder ? "Cylinder" : n->mesh_prim == Prim::Cone ? "Cone" : "Plane";
        if (!n->mesh.valid()) base = "(invalid) " + base;
    } else {
        base = fs::path(n->mesh_path).stem().string();
        if (base.empty()) base = "(unnamed)";
    }
    return n->name.empty() ? base : base + ": " + n->name;
}

// Dark neutral-gray palette (sampled from an actual Blender screenshot, not
// guessed from memory) plus one warm accent color for selection/primary
// actions — Blender's own selection color is closer to orange, used here too.
namespace theme {
const rgba chrome   = rgb(0.09f, 0.09f, 0.10f);  // top bar, outer panel background
const rgba panel    = rgb(0.15f, 0.15f, 0.16f);  // hierarchy/inspector body
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
    // editor's own UI font (Inter Regular, OFL — the same font Blender's own
    // UI uses), unrelated to whatever font the props being placed might want.
    load_font("editor_assets/inter-regular.ttf");

    const std::vector<PaletteEntry> file_palette = scan_palette("assets");
    const std::vector<PaletteEntry> primitives = primitive_palette();
    // What the right-click context menu offers to spawn — primitives first,
    // then whatever real models were found under assets/.
    std::vector<PaletteEntry> spawnable = primitives;
    spawnable.insert(spawnable.end(), file_palette.begin(), file_palette.end());

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

    auto spawn = [&](const PaletteEntry& pe, Node* parent) {
        Node* n = parent->add_child();
        if (pe.prim != Prim::None) {
            n->mesh_prim = pe.prim;
            switch (pe.prim) {
                case Prim::Cube:     n->mesh = make_cube_mesh(); break;
                case Prim::Sphere:   n->mesh = make_sphere_mesh(); break;
                case Prim::Cylinder: n->mesh = make_cylinder_mesh(); break;
                case Prim::Cone:     n->mesh = make_cone_mesh(); break;
                case Prim::Plane:    n->mesh = make_plane_mesh(); break;
                case Prim::Trigger:  break; // pure data — no geometry, ever
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

    // Re-parents `child` onto `new_parent`, preserving its world position
    // (rotation/scale are NOT re-based — child keeps its own mesh_rotation/
    // mesh_scale values, which now apply on top of new_parent's, so its
    // world rotation/scale can visibly shift if new_parent has a non-
    // identity one; fixing that too means solving for a compensating local
    // rotation/scale, which is real extra math for a case that's not the
    // common one — reparenting under a plain, unrotated/unscaled group node
    // works exactly as expected).
    auto reparent = [&](Node* child, Node* new_parent) {
        const WorldMeshTransform child_world = child->world_mesh_transform();
        Node* old_parent = child->parent();
        std::unique_ptr<Node> detached = old_parent->detach_child(child);
        if (!detached) return;
        Node* raw = new_parent->add_child(std::move(detached));
        const WorldMeshTransform new_parent_world = new_parent->world_mesh_transform();
        raw->mesh_pos = world_to_local(child_world.pos, new_parent_world);
        selected = raw;
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
    float cam_pitch = 0.5236f; // ~30 degrees, matches the old fixed-eye-height default view
    float cam_dist = 9.0f;
    bool orbiting = false;
    vec2 orbit_last_mouse{};
    bool dragging_object = false;
    vec3 drag_offset{};   // world-space offset between the ground hit and the object's position, captured at drag start
    float drag_plane_y = 0.0f;
    bool editing_name = false; // begin_text_input() is active for selected->name

    // Right-click context menu state (Outliner only). AddAtRoot: right-
    // clicked empty Outliner space, menu offers every spawnable entry,
    // spawning at the scene root. NodeContext: right-clicked an existing
    // row, menu offers Delete plus every spawnable entry as a child of
    // that row specifically (not necessarily the current selection).
    enum class MenuKind { None, AddAtRoot, NodeContext };
    MenuKind menu_kind = MenuKind::None;
    vec2 menu_pos{};
    Node* menu_target = nullptr;

    // Layout constants — sized for the 1280x800 default window; everything
    // below is computed off f.width/f.height so it still lays out sanely if
    // the window is resized.
    constexpr float TOP_H = 42.0f;
    constexpr float LEFT_W = 230.0f;
    constexpr float RIGHT_W = 260.0f;

    app.update([&](Frame f) {
        const float win_w = static_cast<float>(f.width);
        const float win_h = static_cast<float>(f.height);
        const auto in_viewport = [&](vec2 p) {
            return p.x > LEFT_W && p.x < win_w - RIGHT_W && p.y > TOP_H;
        };

        f.clear(theme::viewport_clear);

        // Whole-tree flatten (with depth, for the Outliner's indentation) —
        // needed before mouse picking below, not just for the Outliner
        // itself further down. Cheap at editor-scale prop counts.
        std::vector<std::pair<Node*, int>> all;
        flatten(scene.get(), 0, all);

        // ---- camera: spherical orbit around the origin. Left/Right/Up/Down
        // keys or right-click-drag for yaw/pitch, scroll wheel or Up/Down
        // for zoom. ----
        cam_pitch = std::max(-1.5f, std::min(1.5f, cam_pitch));
        Camera3D cam;
        cam.eye = {
            cam_dist * std::cos(cam_pitch) * std::sin(cam_yaw),
            cam_dist * std::sin(cam_pitch),
            cam_dist * std::cos(cam_pitch) * std::cos(cam_yaw),
        };
        cam.target = {0.0f, 0.0f, 0.0f};
        const float aspect = win_h > 0.0f ? win_w / win_h : 1.0f;
        const CameraBasis basis = compute_camera_basis(cam, aspect);

        const bool ctrl = f.key_down(Key::LeftControl);

        // A text field (the Inspector's Name box) is capturing keystrokes —
        // suppress every other keyboard/mouse shortcut below so typing a
        // name doesn't also move the selected prop, delete it, save, etc.
        // The engine's own text_capturing mechanism keeps accumulating
        // characters into text_input() regardless of what we do here.
        if (!editing_name) {
            if (f.key_down(Key::Left))  cam_yaw -= f.dt * 1.5f;
            if (f.key_down(Key::Right)) cam_yaw += f.dt * 1.5f;
            if (f.key_down(Key::Up))    cam_dist = std::max(2.0f, cam_dist - f.dt * 6.0f);
            if (f.key_down(Key::Down))  cam_dist = std::min(30.0f, cam_dist + f.dt * 6.0f);

            const float scroll = f.mouse_scroll();
            if (scroll != 0.0f) cam_dist = std::max(2.0f, std::min(30.0f, cam_dist - scroll * 0.5f));

            if (f.mouse_pressed(Mouse::Right)) { orbiting = true; orbit_last_mouse = f.mouse(); }
            if (!f.mouse_down(Mouse::Right)) orbiting = false;
            if (orbiting) {
                const vec2 m = f.mouse();
                cam_yaw += (m.x - orbit_last_mouse.x) * 0.005f;
                cam_pitch = std::max(-1.5f, std::min(1.5f, cam_pitch - (m.y - orbit_last_mouse.y) * 0.005f));
                orbit_last_mouse = m;
            }

            // ---- mouse: click to select (nearest projected node within a
            // pixel threshold) + drag along the ground plane at the node's
            // current height. Clicking empty viewport space deselects,
            // matching Blender's/Unity's own convention. ----
            if (f.mouse_pressed(Mouse::Left) && in_viewport(f.mouse())) {
                Node* best = nullptr;
                float best_dist = 40.0f; // px
                for (auto& [node, depth] : all) {
                    const WorldMeshTransform wt = node->world_mesh_transform();
                    vec2 screen;
                    if (!world_to_screen(basis, wt.pos, win_w, win_h, screen)) continue;
                    const float dx = screen.x - f.mouse().x, dy = screen.y - f.mouse().y;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d < best_dist) { best_dist = d; best = node; }
                }
                selected = best;
                dragging_object = false;
                if (selected) {
                    const WorldMeshTransform wt = selected->world_mesh_transform();
                    drag_plane_y = wt.pos.y;
                    const Ray ray = screen_to_ray(basis, f.mouse().x, f.mouse().y, win_w, win_h);
                    vec3 hit;
                    if (ray_plane_y(ray, drag_plane_y, hit)) {
                        drag_offset = wt.pos - hit;
                        dragging_object = true;
                    }
                }
            }
            if (!f.mouse_down(Mouse::Left)) dragging_object = false;
            if (dragging_object && selected) {
                const Ray ray = screen_to_ray(basis, f.mouse().x, f.mouse().y, win_w, win_h);
                vec3 hit;
                if (ray_plane_y(ray, drag_plane_y, hit)) {
                    const vec3 new_world_pos = hit + drag_offset;
                    const WorldMeshTransform parent_wt = selected->parent()->world_mesh_transform();
                    const vec3 local = world_to_local(new_world_pos, parent_wt);
                    selected->mesh_pos.x = local.x;
                    selected->mesh_pos.z = local.z;
                }
            }

            // ---- keyboard: manipulate the selected prop (relative to its
            // own parent, per Node::draw_meshes()'s composition — moving a
            // group's parent moves the whole group) ----
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
                    // Deletes the whole subtree, not just the one node —
                    // Node owns its children via unique_ptr.
                    selected->parent()->remove_child(selected);
                    selected = nullptr;
                }
            }

            if (f.key_pressed(Key::Tab) && !all.empty()) {
                std::size_t next = 0;
                for (std::size_t i = 0; i < all.size(); ++i) {
                    if (all[i].first == selected) { next = (i + 1) % all.size(); break; }
                }
                selected = all[next].first;
            }
            if (f.key_pressed(Key::Escape) && menu_kind == MenuKind::None) selected = nullptr;
            if (ctrl && f.key_pressed(Key::S)) do_save();
            if (ctrl && f.key_pressed(Key::O)) do_load();
        }

        // ---- 3D viewport (fills the whole window; UI chrome draws on top
        // after, covering the edges — there's no viewport/scissor rect to
        // clip 3D drawing to a sub-region, so this is the same technique the
        // very first version of this editor already used) ----
        f.camera3d(cam);
        draw_grid(f, 12.0f, 1.0f);
        scene->draw_meshes(f);
        for (auto& [node, depth] : all) {
            if (node->mesh_prim != Prim::Trigger) continue;
            const WorldMeshTransform wt = node->world_mesh_transform();
            const vec3 half{0.5f * wt.scale.x, 0.5f * wt.scale.y, 0.5f * wt.scale.z};
            draw_wire_box(f, wt.pos, wt.rotation, half, node->mesh_tint);
        }
        if (selected) {
            const WorldMeshTransform wt = selected->world_mesh_transform();
            const vec3 half{0.55f * wt.scale.x, 0.55f * wt.scale.y, 0.55f * wt.scale.z};
            draw_wire_box(f, wt.pos, wt.rotation, half, theme::accent);
        }
        f.camera({0, 0}); // MANDATORY before any 2D drawing below

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

        // ---- left panel: Outliner (hierarchy tree). Ctrl+click a row while
        // something else is selected re-parents the selection onto that row
        // (preserving its world position) instead of selecting it. ----
        f.rect({0, TOP_H}, {LEFT_W, win_h - TOP_H}, theme::panel);
        f.text("Outliner", {12, TOP_H + 8}, {.size = 16, .color = theme::text_dim});
        {
            float y = TOP_H + 34.0f;
            const float row_h = 26.0f;
            bool right_click_consumed = false;
            if (all.empty()) {
                f.text("Nothing in the scene yet —", {12, y}, {.size = 13, .color = theme::text_dim2});
                f.text("right-click to add something.", {12, y + 16}, {.size = 13, .color = theme::text_dim2});
            }
            for (const auto& [node, depth] : all) {
                if (y > win_h - row_h) break; // no scrolling — just stop, rather than draw off-panel
                const bool is_selected = (node == selected);
                if (is_selected) f.rect({0, y}, {LEFT_W, row_h}, rgba{theme::accent.r, theme::accent.g, theme::accent.b, 0.35f});
                const std::string shown = node_label(node);
                const float indent = 12.0f + static_cast<float>(depth) * 16.0f;
                const float row_w = LEFT_W - indent - 8.0f;
                ButtonStyle row_style;
                row_style.bg = rgba{0, 0, 0, 0};
                row_style.bg_hover = rgba{1, 1, 1, 0.06f};
                row_style.bg_press = rgba{1, 1, 1, 0.1f};
                row_style.text = is_selected ? white : rgb(0.82f, 0.82f, 0.82f);
                row_style.text_size = 15.0f;
                const vec2 measured = f.measure_text(shown, {.size = row_style.text_size});
                if (measured.x > row_w - 8.0f) row_style.text_size *= (row_w - 8.0f) / measured.x;
                const Rect row_rect{{indent, y}, {row_w, row_h}};
                if (f.button(shown, row_rect, row_style) && !editing_name) {
                    if (ctrl && selected && selected != node && !is_ancestor_of(selected, node)) {
                        reparent(selected, node);
                    } else {
                        selected = node;
                    }
                }
                // Right-click a row: context menu for THAT row specifically
                // (add a child of it, or delete it) — independent of
                // whatever's currently selected.
                if (!editing_name && f.mouse_pressed(Mouse::Right) && row_rect.contains(f.mouse())) {
                    menu_kind = MenuKind::NodeContext;
                    menu_pos = f.mouse();
                    menu_target = node;
                    right_click_consumed = true;
                }
                y += row_h;
            }
            // Right-click anywhere else in the panel (not on a row): add at
            // the scene root.
            if (!editing_name && !right_click_consumed && f.mouse_pressed(Mouse::Right) &&
                f.mouse().x >= 0.0f && f.mouse().x <= LEFT_W && f.mouse().y >= TOP_H) {
                menu_kind = MenuKind::AddAtRoot;
                menu_pos = f.mouse();
                menu_target = nullptr;
            }
        }

        // ---- right panel: Inspector (selected node's name + transform) ----
        f.rect({win_w - RIGHT_W, TOP_H}, {RIGHT_W, win_h - TOP_H}, theme::panel);
        {
            const float px = win_w - RIGHT_W + 12.0f;
            const float pw = RIGHT_W - 24.0f;
            float y = TOP_H + 8.0f;
            f.text("Inspector", {px, y}, {.size = 16, .color = theme::text_dim});
            y += 26.0f;
            if (!selected) {
                f.text("Nothing selected.", {px, y}, {.size = 14, .color = theme::text_dim2});
                f.text("Click a prop, a row in the", {px, y + 20}, {.size = 13, .color = theme::text_dim2});
                f.text("Outliner, or Tab-cycle.", {px, y + 36}, {.size = 13, .color = theme::text_dim2});
            } else {
                const std::string kind = selected->mesh_prim == Prim::Trigger ? "Trigger Volume"
                    : selected->mesh_prim != Prim::None ? "Primitive" : "Model";
                f.text(kind, {px, y}, {.size = 13, .color = theme::accent});
                y += 22.0f;

                // Name: a real text field, using Thistle's actual
                // begin_text_input()/text_input() capture (see the header
                // comment — the engine does have text input, just no
                // built-in visual widget; this is that widget for one field).
                f.text("Name", {px, y}, {.size = 12, .color = theme::text_dim});
                y += 16.0f;
                {
                    ButtonStyle name_style;
                    name_style.text_size = 14.0f;
                    std::string shown_name;
                    if (editing_name) {
                        shown_name = text_input() + "_";
                        name_style.bg = rgb(0.12f, 0.28f, 0.38f);
                        name_style.bg_hover = name_style.bg;
                        name_style.bg_press = name_style.bg;
                        name_style.text = white;
                    } else {
                        shown_name = selected->name.empty() ? "(click to name)" : selected->name;
                        name_style.text = selected->name.empty() ? theme::text_dim2 : white;
                    }
                    if (f.button(shown_name, Rect{{px, y}, {pw, 28}}, name_style) && !editing_name) {
                        begin_text_input(selected->name);
                        editing_name = true;
                    }
                    y += 32.0f;
                    if (editing_name) {
                        f.text("Enter: save   Esc: cancel", {px, y}, {.size = 11, .color = theme::text_dim2});
                        y += 20.0f;
                        if (f.key_pressed(Key::Enter)) {
                            selected->name = text_input();
                            end_text_input();
                            editing_name = false;
                        } else if (f.key_pressed(Key::Escape)) {
                            end_text_input();
                            editing_name = false;
                        }
                    }
                }
                y += 6.0f;

                // A labeled X/Y/Z row with a live value readout and +/-
                // steppers. Position/scale can also be dragged with the
                // mouse in the viewport now; these are for precise nudges.
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
                if (f.button("Delete", Rect{{px, y}, {pw, 30}}, delete_style) && !editing_name) {
                    selected->parent()->remove_child(selected);
                    selected = nullptr;
                }
            }
        }

        // ---- compact keybinding hint, bottom-right corner of the viewport ----
        f.text("Right-click Outliner: add/delete   Click: select+drag   Right-drag: orbit   Scroll: zoom   WASD/R/F/Q/E/Z/X: nudge",
               {LEFT_W + 12, win_h - 24}, {.size = 12, .color = rgba{1, 1, 1, 0.5f}});

        // ---- right-click context menu (Outliner only): drawn last so it's
        // always on top of every panel, regardless of where it's positioned.
        // AddAtRoot: right-clicked empty Outliner space, every spawnable
        // entry spawns at the scene root. NodeContext: right-clicked an
        // existing row, offers Delete plus every spawnable entry as a CHILD
        // of that row specifically (not necessarily the current selection —
        // this is the actual replacement for the old Shift+click-to-spawn-
        // as-child palette workflow, and more flexible: it works on any row,
        // selected or not).
        if (menu_kind != MenuKind::None) {
            const float item_h = 26.0f;
            const float menu_w = 190.0f;
            int n_items = static_cast<int>(spawnable.size());
            if (menu_kind == MenuKind::NodeContext) n_items += 1; // Delete
            const float menu_h = static_cast<float>(n_items) * item_h;
            const float mx = std::min(menu_pos.x, win_w - menu_w - 4.0f);
            const float my = std::min(menu_pos.y, win_h - menu_h - 4.0f);
            const Rect menu_rect{{mx, my}, {menu_w, menu_h}};

            f.rect(menu_rect.pos, menu_rect.size, theme::chrome);
            float iy = my;
            if (menu_kind == MenuKind::NodeContext && menu_target) {
                ButtonStyle del_style;
                del_style.text = rgb(0.95f, 0.5f, 0.5f);
                del_style.text_size = 14.0f;
                if (f.button("Delete", Rect{{mx, iy}, {menu_w, item_h}}, del_style)) {
                    if (selected == menu_target) selected = nullptr;
                    menu_target->parent()->remove_child(menu_target);
                    menu_kind = MenuKind::None;
                }
                iy += item_h;
            }
            if (menu_kind != MenuKind::None) { // Delete above may have just closed it
                for (const PaletteEntry& pe : spawnable) {
                    const std::string label = (menu_kind == MenuKind::NodeContext ? "Add child: " : "Add: ") + pe.label;
                    ButtonStyle style;
                    style.text_size = 14.0f;
                    const vec2 measured = f.measure_text(label, {.size = style.text_size});
                    if (measured.x > menu_w - 12.0f) style.text_size *= (menu_w - 12.0f) / measured.x;
                    if (f.button(label, Rect{{mx, iy}, {menu_w, item_h}}, style)) {
                        spawn(pe, (menu_kind == MenuKind::NodeContext && menu_target) ? menu_target : scene.get());
                        menu_kind = MenuKind::None;
                    }
                    iy += item_h;
                }
            }

            // Click anywhere outside the menu (either mouse button) closes
            // it without acting. The click still falls through to whatever
            // it landed on underneath in the same frame (e.g. an Outliner
            // row also gets selected) — properly swallowing a click needs
            // more input-pipeline plumbing than this immediate-mode UI has;
            // this is the honest simplification, not a bug nobody noticed.
            if (menu_kind != MenuKind::None) {
                const bool clicked_outside = (f.mouse_pressed(Mouse::Left) || f.mouse_pressed(Mouse::Right)) && !menu_rect.contains(f.mouse());
                if (clicked_outside || f.key_pressed(Key::Escape)) menu_kind = MenuKind::None;
            }
        }
    });

    return app.run();
}
