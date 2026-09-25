#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <type_traits>

#include <nlohmann/json.hpp>

// Thistle — a small, cross-platform 2D game engine.
// Include <thistle> (the umbrella header) from game code and `using namespace thistle;`.

namespace thistle {

// --- math ---------------------------------------------------------------

struct vec2 {
    float x = 0.0f;
    float y = 0.0f;

    vec2& operator+=(vec2 o) { x += o.x; y += o.y; return *this; }
    vec2& operator-=(vec2 o) { x -= o.x; y -= o.y; return *this; }
    vec2& operator*=(float s) { x *= s; y *= s; return *this; }
};

inline vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline vec2 operator*(vec2 v, float s) { return {v.x * s, v.y * s}; }
inline vec2 operator*(float s, vec2 v) { return {v.x * s, v.y * s}; }

// ADL hooks so vec2 can be a NetVar<vec2> or go straight into a NetArgs
// payload — `nlohmann::json(v)` / `j.get<vec2>()` just work.
inline void to_json(nlohmann::json& j, const vec2& v) { j = {v.x, v.y}; }
inline void from_json(const nlohmann::json& j, vec2& v) { v.x = j.at(0).get<float>(); v.y = j.at(1).get<float>(); }

// A point/vector in 3D world space, for the minor-3D drawing calls below.
struct vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    vec3& operator+=(vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    vec3& operator-=(vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline vec3 operator*(float s, vec3 v) { return {v.x * s, v.y * s, v.z * s}; }
inline vec3 operator/(vec3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }
inline vec3 operator-(vec3 v) { return {-v.x, -v.y, -v.z}; }
// Component-wise, like GLSL's vec3 * vec3 — handy for scaling by a vec3.
inline vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline bool operator==(vec3 a, vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline bool operator!=(vec3 a, vec3 b) { return !(a == b); }

float dot(vec3 a, vec3 b);
vec3  cross(vec3 a, vec3 b);
float length(vec3 v);
float distance(vec3 a, vec3 b);
vec3  normalize(vec3 v); // a zero vector stays zero instead of becoming NaN
inline vec3 lerp(vec3 a, vec3 b, float t) { return a + (b - a) * t; }

inline void to_json(nlohmann::json& j, const vec3& v) { j = {v.x, v.y, v.z}; }
inline void from_json(const nlohmann::json& j, vec3& v) { v.x = j.at(0).get<float>(); v.y = j.at(1).get<float>(); v.z = j.at(2).get<float>(); }

// An axis-aligned rectangle in pixels.
struct Rect {
    vec2 pos;
    vec2 size;
    bool contains(vec2 p) const {
        return p.x >= pos.x && p.y >= pos.y &&
               p.x <= pos.x + size.x && p.y <= pos.y + size.y;
    }
};

inline void to_json(nlohmann::json& j, const Rect& r) { j = {{"pos", r.pos}, {"size", r.size}}; }
inline void from_json(const nlohmann::json& j, Rect& r) { j.at("pos").get_to(r.pos); j.at("size").get_to(r.size); }

// --- color --------------------------------------------------------------

struct rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// Custom color; for a specific alpha just write rgba{r, g, b, a} directly.
inline rgba rgb(float r, float g, float b) { return {r, g, b, 1.0f}; }

inline void to_json(nlohmann::json& j, const rgba& c) { j = {c.r, c.g, c.b, c.a}; }
inline void from_json(const nlohmann::json& j, rgba& c) {
    c.r = j.at(0).get<float>(); c.g = j.at(1).get<float>(); c.b = j.at(2).get<float>(); c.a = j.at(3).get<float>();
}

// Built-in colors are values, not function calls.
inline constexpr rgba midnight {0.055f, 0.067f, 0.098f, 1.0f};
inline constexpr rgba coral    {0.94f,  0.42f,  0.30f,  1.0f};
inline constexpr rgba white    {1.0f,   1.0f,   1.0f,   1.0f};
inline constexpr rgba black    {0.0f,   0.0f,   0.0f,   1.0f};

// --- easing & tweens ----------------------------------------------------

enum class Ease { Linear, InQuad, OutQuad, InOutQuad, OutCubic, OutBack, OutElastic, OutBounce };

float ease(Ease e, float t); // remaps t in 0..1 through the easing curve

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline vec2  lerp(vec2 a, vec2 b, float t)   { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
inline rgba  lerp(rgba a, rgba b, float t)   {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// A reusable 0..1 progress driver. Read t() (eased) and lerp yourself:
//   Tween tw(0.5f, Ease::OutBack); ... tw.update(dt); pos = lerp(a, b, tw.t());
class Tween {
public:
    Tween() = default;
    explicit Tween(float duration, Ease e = Ease::OutCubic, bool loop = false)
        : dur_(duration), ease_(e), loop_(loop) {}
    void update(float dt) {
        elapsed_ += dt;
        if (loop_ && dur_ > 0.0f) { while (elapsed_ > dur_) elapsed_ -= dur_; }
    }
    void restart() { elapsed_ = 0.0f; }
    float raw() const {
        if (dur_ <= 0.0f) return 1.0f;
        float t = elapsed_ / dur_;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    float t() const { return ease(ease_, raw()); }
    bool done() const { return !loop_ && elapsed_ >= dur_; }

private:
    float dur_ = 0.0f, elapsed_ = 0.0f;
    Ease ease_ = Ease::OutCubic;
    bool loop_ = false;
};


// Entrance animations for Menu.
enum class MenuAnim { None, FromTop, FromLeft, FromRight, FromBottom, FadeIn, SmallToBig };

// --- textures -----------------------------------------------------------

// A lightweight handle to a loaded image. Copyable; owns nothing itself.
struct Texture {
    int id = -1;
    int width = 0;
    int height = 0;

    bool valid() const { return id >= 0; }
};

// Loads a PNG/JPG/etc. into a texture. Safe to call before run(); the pixels
// are uploaded to the GPU lazily on first draw.
Texture load_texture(const std::string& path);
// Same, from an encoded image (PNG/JPG/...) already in memory.
Texture load_texture_from_memory(const void* data, size_t size);
// From raw RGBA8 pixels, row by row from the top — for textures you
// generate in code (the pixels are copied).
Texture make_texture(int width, int height, const unsigned char* rgba);

// Frees a texture's GPU image + any pending pixels and invalidates the handle.
// Call when unloading a level so textures don't leak.
void unload_texture(Texture& tex);

// Re-reads a texture from its original file and re-uploads it (hot reload).
void reload_texture(Texture tex);

// --- meshes ---------------------------------------------------------------

// A lightweight handle to mesh geometry loaded from a .obj file. Copyable,
// owns nothing itself — the triangle data lives engine-side, keyed by id.
struct Mesh {
    int id = -1;
    bool valid() const { return id >= 0; }
};

// Loads a Wavefront .obj as a flat triangle list (position + normal + UV per
// vertex). N-gon faces are fan-triangulated; any vertex missing a "vn" gets
// the flat face normal computed from its triangle instead, so a file with no
// normals at all still shades correctly (just faceted, not smoothed). No
// materials, no .mtl, no per-object/group split — every face in the file
// becomes one flat triangle list. See docs/drawing.md before assuming more.
Mesh load_mesh(const std::string& path);

// Frees a mesh's triangle data and invalidates the handle.
void unload_mesh(Mesh& mesh);

// Built-in primitive shapes as real Mesh handles — unit-sized (a 1x1x1 cube,
// radius-0.5 sphere, radius-0.5/height-1 cylinder and cone, a 1x1 plane),
// scale them via mesh3d()'s own `scale` (or Node::mesh_scale) rather than
// passing a size in here. The point of these over calling cube()/sphere3d()/
// etc. directly: those have no rotation parameter at all (there's nowhere to
// put one — they're single immediate-mode draw calls), while a Mesh drawn
// through mesh3d() gets full rotation support, correctly-rotated shading
// included. Each call generates a fresh Mesh — same as calling load_mesh()
// twice on one file would — so cache the handle yourself if you're spawning
// many of the same shape.
Mesh make_cube_mesh();
Mesh make_sphere_mesh();
Mesh make_cylinder_mesh();
Mesh make_cone_mesh();
Mesh make_plane_mesh();

// Which built-in primitive (if any) a Node's `mesh` was built from — set
// Node::mesh_prim alongside `mesh` if you assigned it from make_cube_mesh()
// etc. directly, the same reason mesh_path exists for a load_mesh()'d file:
// load_scene() needs to know how to rebuild `mesh` after a restart, since a
// Mesh handle is just a runtime id.
//
// Trigger is different from the rest: it's pure data, not a shape. A node
// with mesh_prim == Trigger never gets a real `mesh` and Node::draw_meshes()
// never draws it — mesh_pos/mesh_scale describe an invisible volume's
// center/half-extent, mesh_tint and name (below) are just for identifying it
// while editing. Thistle has no 3D collision system at all (see docs/
// drawing.md) — there's no overlap test, no callback, nothing that fires
// when something enters this volume. A trigger here is exactly what a brush
// in a level editor like Hammer actually is on its own: geometry and a name,
// with the *engine* (Source, in Hammer's case — your own game code, here)
// responsible for actually testing overlap against it and doing something.
enum class Prim { None, Cube, Sphere, Cylinder, Cone, Plane, Trigger };

// Optional per-sprite draw settings. Use designated initializers:
//   f.sprite(tex, pos, { .size = {64, 64}, .tint = coral, .rotation = 0.5f });
struct SpriteOpts {
    vec2 size{0.0f, 0.0f};      // {0, 0} means the source region's size
    rgba tint = white;
    float rotation = 0.0f;      // radians, around the sprite's center
    Rect src{{0, 0}, {0, 0}};   // sub-rectangle of the texture in pixels;
                                // zero size means the whole texture
    bool flip_x = false;
    bool flip_y = false;
};

// Blend mode for subsequent draws. Additive is great for glow/fire/hit flashes.
enum class Blend { Alpha, Additive };

// A frame animation over a sprite sheet laid out in a row-major grid of equal
// cells. Returns the source Rect for a given time; feed it into SpriteOpts::src:
//   Anim run = Anim(hero, 64, 64).frames(0, 8).fps(12);
//   f.sprite(hero, pos, { .src = run.frame_at(f.time) });
class Anim {
public:
    Anim() = default;
    Anim(Texture tex, int frame_w, int frame_h) : tex_(tex), fw_(frame_w), fh_(frame_h) {}

    Anim& frames(int first, int count) { first_ = first; count_ = count < 1 ? 1 : count; return *this; }
    Anim& fps(float f) { fps_ = f; return *this; }
    Anim& loop(bool l = true) { loop_ = l; return *this; }

    Texture texture() const { return tex_; }

    Rect frame_at(double time) const {
        int idx = fps_ > 0.0f ? static_cast<int>(time * fps_) : 0;
        if (loop_) { idx %= count_; if (idx < 0) idx += count_; }
        else if (idx >= count_) idx = count_ - 1;
        const int frame = first_ + idx;
        const int cols = (fw_ > 0 && tex_.width > 0) ? (tex_.width / fw_) : 1;
        const int col = cols > 0 ? frame % cols : 0;
        const int row = cols > 0 ? frame / cols : 0;
        return Rect{{static_cast<float>(col * fw_), static_cast<float>(row * fh_)},
                    {static_cast<float>(fw_), static_cast<float>(fh_)}};
    }

private:
    Texture tex_{};
    int fw_ = 0, fh_ = 0;
    int first_ = 0, count_ = 1;
    float fps_ = 12.0f;
    bool loop_ = true;
};

// --- text ---------------------------------------------------------------

// A handle to a loaded TTF font.
struct Font {
    int id = -1;
    bool valid() const { return id >= 0; }
};

// Loads a .ttf font. Safe to call before run() only after the App exists.
// The first font loaded becomes the default used when TextOpts::font is unset.
Font load_font(const std::string& path);

enum class Align { Left, Center, Right };

struct TextOpts {
    float size = 32.0f;   // pixel height
    rgba color = white;
    Font font = {};       // unset => the default (first-loaded) font
    float max_width = 0.0f;   // >0 word-wraps to this pixel width
    Align align = Align::Left; // alignment within max_width (or of the block)
    float line_spacing = 1.25f; // line height as a multiple of size
};

// --- ui -----------------------------------------------------------------

struct ButtonStyle {
    rgba bg       {0.16f, 0.17f, 0.24f, 1.0f};
    rgba bg_hover {0.24f, 0.26f, 0.36f, 1.0f};
    rgba bg_press = coral;
    rgba text     = white;
    float text_size = 28.0f;
    Font font = {};   // unset => the default (first-loaded) font, same as TextOpts
};

// --- input --------------------------------------------------------------

// Values match sokol/GLFW keycodes so no lookup table is needed internally.
enum class Key {
    Space = 32,
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Semicolon = 59, Equal = 61, LeftBracket = 91, Backslash = 92, RightBracket = 93,
    Grave = 96, // the ` / ~ key: the traditional debug-console key
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
    Right = 262, Left = 263, Down = 264, Up = 265,
    PageUp = 266, PageDown = 267, Home = 268, End = 269, CapsLock = 280,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Keypad0 = 320, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal = 330, KeypadDivide, KeypadMultiply, KeypadSubtract, KeypadAdd, KeypadEnter,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342, LeftSuper = 343,
    RightShift = 344, RightControl = 345, RightAlt = 346, RightSuper = 347,
};

enum class Mouse { Left = 0, Right = 1, Middle = 2 };

// Gamepad buttons, by physical position (Xbox-style names): A is the bottom
// face button on every brand. Use pad_label() for the brand-correct glyph.
enum class Pad { A, B, X, Y, Up, Down, Left, Right, L1, R1, Start, Back };

enum class PadKind { None, Xbox, PlayStation, Nintendo, Generic };

// --- minor 3D -------------------------------------------------------------
// A small perspective/depth-tested drawing mode layered onto the same
// immediate-mode Frame — for simple 3D objects (a spinning prop, a debug
// scene), not a full 3D pipeline: no lighting model, no meshes/materials,
// just flat-shaded boxes/planes/lines using a single fixed key light.

struct Camera3D {
    vec3 eye{0.0f, 0.0f, 5.0f};
    vec3 target{0.0f, 0.0f, 0.0f};
    vec3 up{0.0f, 1.0f, 0.0f};
    float fov_deg = 60.0f;
    float near_z = 0.05f;
    float far_z = 500.0f;
};

// --- frame --------------------------------------------------------------

// Passed to your update callback each frame. Pixels, origin top-left, y down.
class Frame {
public:
    float dt = 0.0f;    // seconds since previous frame
    double time = 0.0;  // seconds since app start
    int width = 0;      // framebuffer width in pixels
    int height = 0;     // framebuffer height in pixels

    void clear(rgba color);
    void rect(vec2 pos, vec2 size, rgba color);

    // A filled rectangle with rounded corners — pure geometry, no texture, so
    // it costs nothing but a few extra triangles. `radius` clamps to half the
    // shorter side, so passing anything large just gets you a pill/circle.
    // For a border, draw a slightly bigger one in the border color first and
    // a smaller one in the fill color on top — no separate outline call.
    void rounded_rect(vec2 pos, vec2 size, float radius, rgba color, int segments_per_corner = 8);

    // Vector shapes (filled/outline), for debug draws, HUDs, effects.
    void line(vec2 a, vec2 b, rgba color, float thickness = 2.0f);
    void triangle(vec2 a, vec2 b, vec2 c, rgba color);
    void circle(vec2 center, float radius, rgba color, int segments = 28);
    void circle_outline(vec2 center, float radius, rgba color, float thickness = 2.0f, int segments = 28);

    // Draw a texture. The first overload uses the image's native size.
    void sprite(Texture tex, vec2 pos);
    void sprite(Texture tex, vec2 pos, SpriteOpts opts);

    // 9-slice: stretches `tex` to fill `size` while keeping its four
    // `border`-pixel corners at native scale (only the edges/center stretch).
    // This is the actual way to get a fully custom-looking button/panel that
    // isn't a flat color — draw your own rounded/gradient/glowing frame once
    // in an image editor at any convenient size, and this scales it cleanly
    // to whatever button size you need instead of stretching it and blurring
    // the corners. Uses the whole texture; no sub-rect support (yet).
    void sprite9(Texture tex, vec2 pos, vec2 size, float border, rgba tint = white);

    // Draw text with its top-left corner at pos.
    void text(const std::string& str, vec2 pos, TextOpts opts = {});

    // Pixel width/height the string would occupy with the given options.
    vec2 measure_text(const std::string& str, TextOpts opts = {}) const;

    // Shift all subsequent draws this frame by offset (a simple 2D camera).
    // Pass {0, 0} to reset — e.g. before drawing a fixed HUD. Also switches
    // drawing back to 2D/orthographic mode if camera3d() was used earlier
    // this frame.
    void camera(vec2 offset);

    // Switches to a perspective-projected, depth-tested 3D pass. Draw with
    // cube()/plane3d()/line3d() (world-space vec3 positions) after calling
    // this; call camera({0, 0}) afterward to go back to 2D/UI drawing.
    void camera3d(const Camera3D& cam);

    // Flat-shaded box centered at `center`, `size` along each axis.
    void cube(vec3 center, vec3 size, rgba color);

    // Same box, textured instead of flat-colored — one texture wrapped
    // 0..1 per face (six faces, so it repeats per-face, not seamlessly
    // across the whole box). `tint` multiplies the sampled color.
    void cube(vec3 center, vec3 size, Texture tex, rgba tint = white);

    // A flat, horizontal (XZ) ground/wall-sized quad centered at `center`.
    void plane3d(vec3 center, float width, float depth, rgba color);

    // Same quad, textured 0..1 across its whole width/depth (no tiling —
    // scale width/depth or your texture if you want repetition).
    void plane3d(vec3 center, float width, float depth, Texture tex, rgba tint = white);

    // A 3D line (1px, GPU line width — thickness is best-effort/platform-dependent).
    void line3d(vec3 a, vec3 b, rgba color);

    // A smooth-shaded UV sphere (per-vertex normals, unlike the flat-per-face
    // cube). `rings`/`segments` are latitude/longitude subdivisions — more
    // means rounder and more triangles.
    void sphere3d(vec3 center, float radius, rgba color, int rings = 12, int segments = 16);
    void sphere3d(vec3 center, float radius, Texture tex, rgba tint = white, int rings = 12, int segments = 16);

    // A capped cylinder, axis along Y, centered at `center` (extends
    // `height` * 0.5 up and down from `center.y`).
    void cylinder3d(vec3 center, float radius, float height, rgba color, int segments = 16);
    void cylinder3d(vec3 center, float radius, float height, Texture tex, rgba tint = white, int segments = 16);

    // A capped cone, axis along Y, apex up: base sits at
    // `center.y - height * 0.5`, apex at `center.y + height * 0.5`.
    void cone3d(vec3 center, float radius, float height, rgba color, int segments = 16);
    void cone3d(vec3 center, float radius, float height, Texture tex, rgba tint = white, int segments = 16);

    // Draws a mesh loaded with load_mesh(), shaded with the same fixed key
    // light as cube()/sphere3d()/etc. `rotation_rad` is Euler angles (radians)
    // applied X, then Y, then Z. Scale isn't corrected for in the shading
    // normals, so a heavily non-uniform scale will shade a little wrong —
    // fine for gameplay-scale stretching, not something to lean on.
    void mesh3d(Mesh mesh, vec3 pos, vec3 rotation_rad = {0, 0, 0}, vec3 scale = {1, 1, 1}, rgba tint = white);
    void mesh3d(Mesh mesh, vec3 pos, vec3 rotation_rad, vec3 scale, Texture tex, rgba tint = white);

    // Transform stack (used by Node). Draws between push/pop are translated,
    // rotated (radians), then scaled; nest freely, pop what you push.
    void push_transform(vec2 pos, float rotation = 0.0f, vec2 scale = {1.0f, 1.0f});
    void pop_transform();

    // Immediate-mode button: draws it and returns true on the frame it's clicked
    // (or tapped, on mobile). Manage layout yourself, or use Menu for stacks.
    bool button(const std::string& label, Rect area, ButtonStyle style = {});

    // More immediate-mode widgets.
    bool checkbox(const std::string& label, bool& value, Rect area, ButtonStyle style = {});
    float slider(float value, float min_v, float max_v, Rect area, ButtonStyle style = {});
    void progress_bar(float t, Rect area, rgba fill, rgba bg = rgba{0.16f, 0.17f, 0.24f, 1.0f});

    // Blend mode for subsequent draws this frame (resets to Alpha next frame).
    void blend(Blend mode);

    // Input. *_down is true while held; *_pressed is true only on the frame
    // the key/button first went down.
    bool key_down(Key k) const;
    bool key_pressed(Key k) const;
    vec2 mouse() const; // cursor position in pixels
    // How far the mouse moved this frame, in pixels. Keeps working while the
    // mouse is locked (lock_mouse()), when mouse() itself stops changing —
    // that's what mouse-look in a first-person game reads.
    vec2 mouse_delta() const;
    bool mouse_down(Mouse b) const;
    bool mouse_pressed(Mouse b) const;
    // Vertical scroll wheel/trackpad delta accumulated this frame (positive =
    // scroll up/away from you on most platforms/mice — sign varies by OS and
    // device, so treat it as "some scroll happened this frame," not a
    // guaranteed direction, the same caveat any cross-platform scroll API has).
    float mouse_scroll() const;
    // Files dragged onto the window and let go this frame, as full paths.
    // Desktop only (macOS, Windows, Linux): always empty on phones and the web.
    const std::vector<std::string>& dropped_files() const;

    // Touch. On phones a tap is also delivered as a left-mouse click, so mouse_*
    // code works as-is; these are just clearer names for the primary finger.
    bool touching() const;   // a finger is currently down
    vec2 touch_pos() const;  // its position in pixels

    // Gamepad (first connected controller). pad_pressed is edge-triggered.
    bool pad_connected() const;
    bool pad_down(Pad b) const;
    bool pad_pressed(Pad b) const;
    vec2 pad_left_stick() const;   // each axis -1..1
    vec2 pad_right_stick() const;

    PadKind pad_kind() const;         // Xbox / PlayStation / Nintendo / ...
    std::string pad_name() const;     // vendor name, e.g. "Xbox Wireless Controller"
    const char* pad_label(Pad b) const; // brand-correct label, e.g. A -> "Cross" on PS
};

// --- audio --------------------------------------------------------------

// Fire-and-forget playback of a sound effect (wav/mp3/flac). Non-blocking.
void play_sound(const std::string& path);

// Background music: one looping, streamed track at a time. Calling play_music
// again replaces the current track.
void play_music(const std::string& path, float volume = 1.0f);
void stop_music();

// Volumes are 0..1. music/sfx are independent; master scales everything.
void set_music_volume(float volume);
void set_sfx_volume(float volume);
void set_master_volume(float volume);

// --- debug UI (optional: Dear ImGui) ------------------------------------
// Windows for tweaking and inspecting a game while you make it: sliders,
// checkboxes, stats, a list of what's alive. Opt-in, like 3D physics:
// `thistle enable debug_ui` (or -DTHISTLE_DEBUG_UI=ON). Then
// #include <imgui.h> and call Dear ImGui anywhere in your update callback;
// it's drawn last, on top of everything (post-effects too), and clicks or
// typing that land in a debug window don't reach the game. Wrap that code
// in #if THISTLE_DEBUG_UI so it still builds with the module off. Not for
// the game's own UI (it looks like a tool, on purpose).
bool debug_ui_available();
bool debug_ui_wants_mouse();    // the mouse is over a debug window
bool debug_ui_wants_keyboard(); // a debug field has the keyboard
// A ready-made window: fps, a frame-time graph, 3D draw calls/triangles,
// sounds playing. Call it every frame you want it shown.
void debug_stats_window();

// --- post-processing ----------------------------------------------------

// Full-screen shader effects applied after the scene is drawn. The frame is
// rendered to an offscreen texture, then composited through the chosen effect.
// (Currently implemented on Metal — macOS/iOS; a no-op on other backends.)
enum class PostEffect { None, Grayscale, Vignette, Chromatic, Flash, Fade };

// Set the active effect and its 0..1 strength. Flash/Fade are handy for damage
// hits and scene transitions; drive intensity yourself over time.
void set_post_effect(PostEffect effect, float intensity = 1.0f);

// --- platform -----------------------------------------------------------

enum class Platform { Windows, MacOS, Linux, iOS, Android, Web, Unknown };

Platform platform();          // what the build is running on
bool is_mobile();             // iOS or Android
const char* platform_name();  // "iOS", "macOS", ...
std::string device_name();    // device model on mobile, else the platform name

// --- save ---------------------------------------------------------------

// A tiny persistent key/value store, written to the correct writable location
// for each platform (iOS Documents, macOS App Support, XDG dir, %APPDATA%).
// Requires an App to exist (it uses the app title for the folder name).
namespace save {
    void set(const std::string& key, const std::string& value);
    void set_int(const std::string& key, int value);
    void set_float(const std::string& key, float value);
    std::string get(const std::string& key, const std::string& fallback = "");
    int get_int(const std::string& key, int fallback = 0);
    float get_float(const std::string& key, float fallback = 0.0f);
    bool has(const std::string& key);
    void remove(const std::string& key);
    void clear();
    std::string path(); // absolute path of the save file
}

// --- logging & clipboard ------------------------------------------------

// Logs print to the console and are captured for the debug overlay (see below).
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// Copy text to the system clipboard (where the platform supports it).
void set_clipboard(const std::string& text);

// Hides the cursor and keeps it inside the window, so the mouse can turn a
// first-person camera forever without hitting the screen edge; read
// Frame::mouse_delta() for the movement. The OS may unlock it on its own
// (e.g. when the window loses focus) — check mouse_locked().
void lock_mouse(bool locked);
bool mouse_locked();
void show_mouse(bool visible);

void set_fullscreen(bool fullscreen);
bool is_fullscreen();

// Closes the window at the end of this frame (App::run() then returns).
void quit();

// Trigger device haptic feedback (iOS only; a no-op elsewhere).
enum class Haptic { Light, Medium, Heavy, Success };
void haptic(Haptic style = Haptic::Light);

// --- crash reporting (fully local — nothing is ever sent anywhere) ------
// Installed automatically when you construct an App. On a crash (segfault,
// abort/assert, floating-point exception, an uncaught C++ exception, ...)
// writes a plain text file — when, what kind of crash, a best-effort stack
// trace, any context you attached with set_crash_context(), and the last
// ~50 log lines leading up to it — into crash_log_dir(), then lets the
// crash continue as normal (the OS still sees a real crash; this doesn't
// swallow or "fix" anything, it just leaves a note explaining what
// happened before the process actually goes down). This is not Unreal's
// crash reporter — there's no server, no upload, no telemetry of any
// kind; the file just sits on disk for you (or a player who hits a crash)
// to find and read.
//
// Read this before you lean on it: signal/exception handlers run in a
// severely restricted context where most of the C++ standard library is
// technically unsafe to call (the crash could have interrupted a malloc
// call, and calling malloc again from the handler can deadlock). This is
// the same pragmatic "best effort" every real-world game crash handler
// makes — it writes a report the overwhelming majority of the time, but a
// sufficiently unlucky crash can occasionally make the handler itself fail
// silently rather than produce a file. It fails open: worst case is no
// report, never a hang or a second crash that erases the first one's
// information.

// Where crash_*.txt files get written (next to your save data).
std::string crash_log_dir();

// Whether a crash also shows a native "the game crashed" dialog, on top of
// always writing the report file. On by default. Turn it off if you'd
// rather build your own crash UI, or for automated/CI test runs where a
// blocking system dialog would just hang the run.
//
// How this is actually shown, per platform, and why: rendering a NEW UI
// from inside the crashed process's own (possibly the actual cause of the
// crash) graphics context is exactly the kind of risky work a crash
// handler should avoid — so this never touches Thistle's own renderer.
// Windows calls the OS's native MessageBox directly (a separate system
// surface, not dependent on your app's window/graphics state). macOS
// spawns a fresh `osascript` process to show a native alert — a genuinely
// separate, uncorrupted process, the same principle real crash reporters
// (Crashpad, Breakpad) use. Linux tries `zenity` if it's on the system;
// if it isn't, this silently does nothing — the report file still gets
// written either way, this only affects the popup. iOS never shows one:
// once an iOS app crashes, the OS has already killed it and returned to
// the home screen before any of your code could run — there's no hook
// for a crashed app to show anything. The file's still there for you to
// pull off the device later; there's just no in-the-moment popup possible.
void set_crash_popup(bool enabled);

// Attaches extra context that shows up in a crash report if one happens —
// e.g. set_crash_context("level", "3") so a crash report says what level
// the player was on. Call as often as you want; only the latest value per
// key is kept. Purely in-memory bookkeeping, no cost unless a crash
// actually occurs.
void set_crash_context(const std::string& key, const std::string& value);

// On-screen text input. begin_text_input() shows the soft keyboard on mobile and
// starts capturing typed characters (printable ASCII; Backspace edits; pasting
// with Ctrl+V / Cmd+V adds the clipboard's text) up to max_length characters;
// read the buffer with text_input() each frame; end_text_input() hides it. Use
// for naming a level, a login field, a file path, etc.
void begin_text_input(const std::string& initial = "", size_t max_length = 40);
void end_text_input();
const std::string& text_input();

// --- networking ---------------------------------------------------------

// Minimal async HTTP for small JSON payloads (e.g. community levels).
// Implemented on Apple (NSURLSession) and Windows (WinHTTP); on other
// platforms it completes immediately as a failure (status 0). Start a
// request, poll done() each frame, then read status()/body().
//   Http req = Http::get("http://host/levels");
//   ... each frame: if (req.done() && req.ok()) parse(req.body());
class Http {
public:
    static Http get(const std::string& url);
    static Http post(const std::string& url, const std::string& json_body);
    Http() = default;
    bool done();                              // poll each frame; true once finished
    bool ok() { return done() && status_ >= 200 && status_ < 300; }  // any 2xx
    int  status() const { return status_; }   // HTTP status, 0 = network error
    const std::string& body() const { return body_; }
    void reset();                             // cancel / free
private:
    std::shared_ptr<void> h_;
    bool done_ = false;
    int  status_ = 0;
    std::string body_;
};

// --- realtime networking (a small Mirror-flavored layer over TCP) -------
// A server-authoritative client/server model, deliberately scoped: TCP only
// (no UDP), JSON messages (not a packed binary format), no NAT traversal or
// relay (you still need port-forwarding or a host both sides can reach —
// same as raw Mirror without its relay service), no client-side prediction,
// no interest management, no "host mode" (a process is a server OR a
// client, not both at once). It gets real client/server play working on a
// LAN or through a dedicated server; it is not a shooter's netcode. See
// docs/networking.md before building anything serious on this.

namespace net {
    bool is_server();   // true from NetServer::listen() succeeding until stop()
    bool is_client();   // true from NetClient::connect() succeeding until disconnect()
    // Inside an on_command handler (server): which connection sent it, the
    // same id NetServer::on_connect got. 0 anywhere else. This is how a
    // server checks "is this player allowed to move this object?"
    int command_sender();
}

// Arguments/payload for Commands and ClientRpcs: NetArgs{{"x", 1.0f}} to
// build, args.at("x").get<float>() to read. It's just nlohmann::json.
using NetArgs = nlohmann::json;

// A server-authoritative synced field (Mirror's [SyncVar]). Assign it from
// server-side code; every connected (and later-connecting) client's copy
// updates automatically on the next NetServer::update(). Read it anywhere
// via the implicit conversion. T must be JSON-convertible — every built-in
// numeric type, std::string, and vec2/vec3/rgba already are; give your own
// structs ADL to_json/from_json overloads the same way vec2 does, above.
template <typename T>
class NetVar {
public:
    NetVar() = default;
    NetVar(const T& v) : value_(v) {}
    operator const T&() const { return value_; }
    NetVar& operator=(const T& v) { value_ = v; dirty_ = true; return *this; }
    const T& get() const { return value_; }

private:
    T value_{};
    bool dirty_ = false;
    friend class NetObject;
};

// Base class for a networked entity. Register synced fields and RPC handlers
// in your constructor; NetServer::update() / NetClient::update() do the rest
// every tick. Don't construct these directly — use net_spawn() server-side;
// clients get theirs automatically when the server spawns one.
class NetObject {
public:
    NetObject() = default;
    virtual ~NetObject() = default;
    NetObject(const NetObject&) = delete;
    NetObject& operator=(const NetObject&) = delete;

    uint32_t net_id() const { return id_; }
    const std::string& class_name() const { return class_name_; }

protected:
    // Registers a field for auto-sync. Call once per field, in your
    // constructor, on a NetVar<T> member of the derived class.
    template <typename T>
    void net_sync(const std::string& name, NetVar<T>& var) {
        NetSyncField field;
        field.get        = [&var]() -> nlohmann::json { return var.value_; };
        field.is_dirty    = [&var]() { return var.dirty_; };
        field.clear_dirty = [&var]() { var.dirty_ = false; };
        field.set         = [&var](const nlohmann::json& j) { var.value_ = j.get<T>(); var.dirty_ = false; };
        add_sync_field(name, std::move(field));
    }

    // Runs on the SERVER when a client calls call_command(name, ...) on its
    // local copy of this same object (matched by net_id).
    void on_command(const std::string& name, std::function<void(const NetArgs&)> fn);
    // Runs on EVERY CLIENT when the server calls call_client_rpc(name, ...).
    void on_client_rpc(const std::string& name, std::function<void(const NetArgs&)> fn);

    // Client-side: sends a Command to the server for this object. Called
    // server-side instead: logs a warning and does nothing (Commands only
    // flow client -> server, same restriction Mirror enforces).
    void call_command(const std::string& name, NetArgs args = NetArgs::object());
    // Server-side: sends a ClientRpc to every connected client for this
    // object. Called client-side instead: logs a warning and does nothing.
    void call_client_rpc(const std::string& name, NetArgs args = NetArgs::object());
    // Server-side: the same, to one connection only (Mirror's TargetRpc):
    // a newcomer's catch-up data, one player's private info. False if that
    // connection is gone.
    bool call_target_rpc(int conn_id, const std::string& name, NetArgs args = NetArgs::object());

private:
    struct NetSyncField {
        std::function<nlohmann::json()> get;
        std::function<bool()> is_dirty;
        std::function<void()> clear_dirty;
        std::function<void(const nlohmann::json&)> set;
    };
    void add_sync_field(const std::string& name, NetSyncField field);

    uint32_t id_ = 0;
    std::string class_name_;
    std::vector<std::pair<std::string, NetSyncField>> fields_;
    std::vector<std::pair<std::string, std::function<void(const NetArgs&)>>> commands_;
    std::vector<std::pair<std::string, std::function<void(const NetArgs&)>>> client_rpcs_;

    friend struct NetObjectAccess;   // lets the .cpp's send/dispatch code reach these
};

// Registers a spawnable class by name. Every machine that will ever spawn or
// receive this type — the server AND every client — must call this with the
// same name and an equivalent constructor before it can be spawned or
// replicated. Do it once at startup, not per-spawn.
void net_register_class(const std::string& class_name, std::function<std::unique_ptr<NetObject>()> make);

// Server-only: spawns a NetObject and replicates it (class + current field
// values) to every connected client, and to anyone who connects later.
// Returns nullptr if called on a client — the server is always authoritative
// over what exists, there is no client-requested spawning in this version.
NetObject* net_spawn(const std::string& class_name);

// Server-only: despawns an object and tells every client to remove theirs.
void net_despawn(NetObject* obj);

// Looks up an object this process currently knows about, by id — the
// server's authoritative instance, or a client's local replica of it.
// nullptr if this process has never heard of that id. This is how
// client-side code gets from "the server told me about object 7" to a
// Player* it can actually render or read. If this process has BOTH an
// active NetServer and an active NetClient (uncommon — "host mode" isn't
// really supported, see the top of this section — mainly a same-process
// test harness would do this), this prefers the server's copy; call
// NetServer::find() / NetClient::find() directly when you need to
// disambiguate.
NetObject* net_find(uint32_t id);

// Calls fn once for every object this process currently knows about (every
// spawned object on the server, every replicated object on a client) —
// the usual way a render loop draws "all the players." Same server-over-
// client tiebreak as net_find() if both are active in one process.
void net_each_object(const std::function<void(NetObject&)>& fn);

class NetServer {
public:
    NetServer();
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool listen(int port);   // starts accepting connections; false on bind/listen failure
    void update();            // call every tick: accept, read+dispatch Commands, flush dirty NetVars
    void stop();

    int connection_count() const;
    NetObject* find(uint32_t id) const;   // unambiguous even if a NetClient is also active in this process

    std::function<void(int conn_id)> on_connect;
    std::function<void(int conn_id)> on_disconnect;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct NetObjectAccess;
    friend NetObject* net_spawn(const std::string&);
    friend void net_despawn(NetObject*);
};

class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool connect(const std::string& host, int port);   // resolves + connects; blocks briefly
    void update();                                       // call every tick: read+dispatch incoming messages
    void disconnect();
    bool connected() const;
    NetObject* find(uint32_t id) const;   // unambiguous even if a NetServer is also active in this process
    // This client's connection id as the server knows it (what
    // net::command_sender() returns there for our commands), e.g. to find
    // which player object is ours. 0 until the server's welcome arrives
    // (on the first update() after connecting).
    int connection_id() const;

    std::function<void()> on_connect;
    std::function<void()> on_disconnect;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct NetObjectAccess;
};

// --- in-app purchases -----------------------------------------------------
// Poll-based, same shape as Http: kick a request off, poll for the result
// each frame. iOS/macOS via StoreKit — specifically the classic Objective-C
// StoreKit 1 API, not StoreKit 2, which is Swift-only and has no C/C++-
// callable surface at all. A no-op everywhere else: Android (Google Play
// Billing) would need its own JNI bridge the same way Windows got its own
// WinHTTP bridge, and there's no working Android build target yet to bridge
// to (see docs/building.md) — nothing to add this to until that exists.

struct IAPProduct {
    std::string id;
    std::string title;
    std::string description;
    std::string price_string;    // localized ("$4.99") — display this, don't format price_value yourself
    double      price_value = 0.0;
    std::string currency_code;   // e.g. "USD"
};

enum class IAPEventKind { Purchased, Failed, Restored, Deferred };

struct IAPEvent {
    IAPEventKind kind = IAPEventKind::Failed;
    std::string product_id;
    std::string transaction_id;  // pass to iap_finish_transaction() once you've delivered the content
    std::string error_message;   // set when kind == Failed
};

// Whether this device/account can make payments at all (parental controls,
// managed devices, restricted regions) — check before showing a buy button.
bool iap_can_make_payments();

// Kicks off an async product-info request for the given App Store Connect
// product identifiers. Poll iap_products_ready() each frame; once true,
// iap_products() has whatever Apple returned — misconfigured/unknown ids
// are just silently absent, so check the count against what you asked for.
void iap_fetch_products(const std::vector<std::string>& product_ids);
bool iap_products_ready();
const std::vector<IAPProduct>& iap_products();

// Starts a purchase (must be one of the ids from a completed
// iap_fetch_products() first — you can't purchase by id blind). This does
// NOT tell you whether it succeeded; that arrives later as an IAPEvent.
void iap_purchase(const std::string& product_id);

// Re-delivers past non-consumable/subscription purchases as IAPEvents
// (kind == Restored) — required by App Review for anything non-consumable.
void iap_restore_purchases();

// Call once you've durably delivered whatever the transaction paid for
// (unlocked content, credited currency, etc.) — StoreKit keeps re-delivering
// a transaction on every launch until you finish it.
void iap_finish_transaction(const std::string& transaction_id);

// Pops one pending purchase/restore/failure event, if there is one:
//   IAPEvent e;
//   while (iap_poll_event(e)) { ... }
bool iap_poll_event(IAPEvent& out);

// --- menu ---------------------------------------------------------------

// A vertical, horizontally-centered stack of menu items with an entrance
// animation. Build one every frame, passing seconds since the menu appeared:
//   Menu m(f, f.time - menu_start, MenuAnim::FromTop);
//   m.title("My Game");
//   if (m.button("Play")) set_scene("play");
class Menu {
public:
    Menu(Frame& f, float elapsed, MenuAnim anim = MenuAnim::None, float duration = 0.4f);
    void title(const std::string& text, float size = 60.0f, rgba color = white);
    void label(const std::string& text, float size = 24.0f, rgba color = white);
    bool button(const std::string& text, ButtonStyle style = {});
    void gap(float pixels = 18.0f);

private:
    Frame& f_;
    MenuAnim anim_;
    float cx_;
    float y_;
    vec2 offset_{0.0f, 0.0f};
    float alpha_ = 1.0f;
    float scale_ = 1.0f;
    int index_ = 0; // running item index, for gamepad focus
};

// --- particles ----------------------------------------------------------

struct ParticleOpts {
    vec2 velocity{0.0f, 0.0f}; // base velocity added to every particle
    float spread = 200.0f;     // random velocity magnitude in all directions
    float life = 0.6f;         // seconds
    float size = 6.0f;         // pixels (square)
    rgba color = white;
    float gravity = 800.0f;    // downward accel
};

// A simple particle pool the game owns: emit bursts, update, draw.
class Particles {
public:
    void emit(vec2 pos, int count, ParticleOpts opts = {});
    void update(float dt);
    void draw(Frame& f) const;
    void clear() { items_.clear(); }
    std::size_t count() const { return items_.size(); }

private:
    struct P { vec2 pos, vel; float life, max_life, size, gravity; rgba color; };
    std::vector<P> items_;
};

// --- scene graph (transform hierarchy + actions) ------------------------

// A queued tween applied to a Node's transform. Build via Node::move_to etc.
struct Action {
    enum class Kind { MoveTo, MoveBy, ScaleTo, RotateTo, FadeTo, Delay, Call } kind = Kind::Delay;
    vec2 v_to{0, 0}, v_from{0, 0};
    float f_to = 0.0f, f_from = 0.0f;
    float dur = 0.0f, elapsed = 0.0f;
    Ease ease = Ease::OutCubic;
    std::function<void()> call;
    bool started = false;
};

// A Node's mesh_pos/mesh_rotation/mesh_scale composed down through its whole
// parent chain — the actual world-space placement Node::draw_meshes() ends
// up drawing at, since those fields are each parent-relative (see
// Node::draw_meshes()'s doc comment). Get one via Node::world_mesh_transform().
// The obvious use: build a Box3D/Sphere3D from a Prim::Trigger node's world
// pos/scale to actually test whether something is inside it — see "3D
// collision" below.
struct WorldMeshTransform {
    vec3 pos{0, 0, 0};
    vec3 rotation{0, 0, 0}; // Euler radians, same order as Frame::mesh3d
    vec3 scale{1, 1, 1};
};

// A node in a transform hierarchy. Origin is the node's center; children inherit
// position, rotation, scale, and alpha. Optionally draws one sprite.
class Node {
public:
    vec2 pos{0, 0};
    vec2 scale{1, 1};
    float rotation = 0.0f; // radians
    float alpha = 1.0f;    // multiplied down the tree
    bool visible = true;

    // Purely for your own identification (an editor's Outliner, a lookup by
    // name in your own game code, a trigger volume's label) — Node itself
    // never reads this. Empty by default; nothing requires setting it.
    std::string name;

    // Optional sprite drawn centered on the node's origin.
    Texture sprite{};
    vec2 sprite_size{0, 0};   // {0,0} = texture's native size
    rgba sprite_tint = white;
    Rect sprite_src{{0, 0}, {0, 0}};
    // The path sprite was loaded from, if any — save_scene()'s only reason to
    // exist. A Texture is just a runtime id; the id from this run means
    // nothing after a restart, so this is what actually survives a save.
    // Set this yourself if you assign `sprite` directly instead of through
    // load_scene(); save_scene() has no other way to know where it came from.
    std::string sprite_path;

    // Optional mesh placed independently of this node's own 2D pos/rotation/
    // scale above (those drive `sprite` through draw()'s 2D affine
    // transform — a 2D transform has no meaningful way to drive a 3D
    // placement, so the mesh gets its own vec3 fields instead of trying to
    // reuse the 2D ones). draw()/draw_rec() never touches these; call
    // draw_meshes() yourself, after f.camera3d(...), the same manual
    // sequencing any other minor-3D drawing needs. mesh_pos/rotation/scale
    // are relative to the PARENT's own composed mesh transform, not world
    // space — see draw_meshes()'s doc comment for exactly what that means.
    Mesh mesh{};
    vec3 mesh_pos{0, 0, 0};
    vec3 mesh_rotation{0, 0, 0}; // Euler radians, same order as Frame::mesh3d
    vec3 mesh_scale{1, 1, 1};
    rgba mesh_tint = white;
    Texture mesh_texture{};      // invalid = flat mesh_tint, no texture
    // Set alongside `mesh` if it came from make_cube_mesh() etc. instead of
    // load_mesh() — load_scene() needs this to know how to rebuild `mesh`.
    Prim mesh_prim = Prim::None;
    // Paths mesh/mesh_texture were loaded from, if any — save_scene()'s only
    // reason to exist, same as sprite_path above. Set these yourself if you
    // assign mesh/mesh_texture directly instead of through load_scene().
    // Unused (leave empty) when mesh_prim is set instead.
    std::string mesh_path;
    std::string mesh_texture_path;

    Node* add_child(std::unique_ptr<Node> c) {
        c->parent_ = this;
        children_.push_back(std::move(c));
        return children_.back().get();
    }
    Node* add_child() { return add_child(std::make_unique<Node>()); }
    std::size_t child_count() const { return children_.size(); }
    Node* child(std::size_t i) const { return children_[i].get(); }
    Node* parent() const { return parent_; }

    // Removes and destroys one direct child, found by pointer identity.
    // Returns false if `child` isn't actually a direct child of this node
    // (nothing removed). `child` itself is destroyed and must not be used
    // afterward; pointers to *other* children stay valid, since each Node is
    // its own heap allocation — erasing one from `children_` only moves
    // `unique_ptr` handles around, not the Node objects they point to.
    bool remove_child(Node* child) {
        for (auto it = children_.begin(); it != children_.end(); ++it) {
            if (it->get() == child) { children_.erase(it); return true; }
        }
        return false;
    }

    // Like remove_child(), but hands the child back instead of destroying
    // it — for reparenting (add_child(node.detach_child(x))'ing it onto a
    // different node) rather than deleting. Returns nullptr if `child` isn't
    // actually a direct child of this node. The returned Node's parent() is
    // reset to nullptr until you add_child() it somewhere.
    std::unique_ptr<Node> detach_child(Node* child) {
        for (auto it = children_.begin(); it != children_.end(); ++it) {
            if (it->get() == child) {
                std::unique_ptr<Node> detached = std::move(*it);
                children_.erase(it);
                detached->parent_ = nullptr;
                return detached;
            }
        }
        return nullptr;
    }

    // Actions queue and run in order (chainable): a.move_to(...).scale_to(...).
    Node& move_to(vec2 to, float dur, Ease e = Ease::OutCubic) { return push({Action::Kind::MoveTo, to, {}, 0, 0, dur, 0, e, {}, false}); }
    Node& move_by(vec2 delta, float dur, Ease e = Ease::OutCubic) { return push({Action::Kind::MoveBy, delta, {}, 0, 0, dur, 0, e, {}, false}); }
    Node& scale_to(vec2 to, float dur, Ease e = Ease::OutCubic) { return push({Action::Kind::ScaleTo, to, {}, 0, 0, dur, 0, e, {}, false}); }
    Node& rotate_to(float rad, float dur, Ease e = Ease::OutCubic) { Action a; a.kind = Action::Kind::RotateTo; a.f_to = rad; a.dur = dur; a.ease = e; return push(a); }
    Node& fade_to(float a01, float dur, Ease e = Ease::Linear) { Action a; a.kind = Action::Kind::FadeTo; a.f_to = a01; a.dur = dur; a.ease = e; return push(a); }
    Node& delay(float secs) { Action a; a.kind = Action::Kind::Delay; a.dur = secs; return push(a); }
    Node& call(std::function<void()> fn) { Action a; a.kind = Action::Kind::Call; a.call = std::move(fn); return push(a); }
    bool actions_done() const { return actions_.empty(); }
    void clear_actions() { actions_.clear(); }

    void update(float dt);       // advances actions, recurses into children
    void draw(Frame& f);         // draws this subtree with composed transforms
    vec2 world_pos() const;      // this node's origin in world space

    // Draws every mesh in this subtree via Frame::mesh3d(). Separate from
    // draw() on purpose: draw() is purely 2D (orthographic camera, sprites),
    // so drawing a mesh has to happen in its own pass, after f.camera3d(...)
    // and before switching back with f.camera({0, 0}).
    //
    // mesh_pos/mesh_rotation/mesh_scale are relative to the PARENT's already-
    // composed mesh transform, not absolute world space — a child inherits
    // its parent's position, rotation, and scale the same way sprite's 2D
    // pos/rotation/scale inherit down the tree in draw()/draw_rec(). A node
    // with no mesh of its own still composes its transform down to its
    // children, so it works as a pure grouping/anchor node. Rotation composes
    // by simple addition per axis, not a real rotation-matrix multiply — this
    // is only exactly right for rotation around one shared axis (the common
    // case: everything here uses Y-axis "turntable" rotation), and becomes
    // an approximation once nested nodes rotate around different axes. Good
    // enough for grouping a handful of props together, not a general rig.
    void draw_meshes(Frame& f) const;

    // This node's mesh_pos/mesh_rotation/mesh_scale composed through its
    // whole parent chain — the same composition draw_meshes() does
    // internally, exposed so your own game code can get a node's actual
    // world-space placement (to test against a Prim::Trigger volume, say)
    // instead of only ever seeing the parent-relative raw fields.
    WorldMeshTransform world_mesh_transform() const;

private:
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    std::vector<Action> actions_;
    Node& push(const Action& a) { actions_.push_back(a); return *this; }
    void draw_rec(Frame& f, float inherited_alpha);
    void draw_meshes_rec(Frame& f, vec3 parent_pos, vec3 parent_rot, vec3 parent_scale) const;
};

// Snapshots (or restores) a Node subtree's pose/sprite/mesh/hierarchy as
// JSON — a save-game or checkpoint, not a level-authoring format; nobody is
// meant to hand-edit the file. Captures whatever pos/rotation/alpha/mesh_pos/
// etc. actually are at the moment you call it, live gameplay values
// included, not just however the tree was originally built. Queued actions
// (move_to, call, ...) are NOT saved — only the static pose survives a round
// trip, same as a snapshot of a struct wouldn't include "and it's 60% of the
// way through animating." load_scene() calls load_texture()/load_mesh() for
// every sprite_path/mesh_path/mesh_texture_path it finds (once per unique
// path, even if many nodes share one) and returns the new root, or nullptr
// if the file couldn't be read/parsed.
bool save_scene(const Node& root, const std::string& path);
std::unique_ptr<Node> load_scene(const std::string& path);

// --- 3D collision (not physics) ------------------------------------------
// Overlap/intersection tests, and nothing else: no velocities, no forces, no
// resolution, no broad-phase, no continuous collision, no rigid bodies. This
// is the "minor 3D" equivalent of what `Physics` below gives you in 2D via a
// real engine (Box2D) — except there's no 3D physics engine underneath this
// at all, just the raw geometry math, because a real one is a genuinely
// different, much bigger project (see docs/drawing.md's "3D physics" note).
// What this actually exists for: making a Prim::Trigger volume (or any other
// node) usable — build a Box3D/Sphere3D from Node::world_mesh_transform(),
// call one of these every frame, and you have a working trigger check.
//
// Box3D is always axis-aligned — it ignores any rotation a
// WorldMeshTransform might carry, even though the editor draws a Trigger's
// wireframe rotated (see tools/thistle-editor). A rotated Box3D-vs-Box3D
// test (oriented bounding boxes, via the separating axis theorem) is real
// extra math for a case most trigger volumes don't actually need — placing
// an axis-aligned volume is the common case in practice, including in
// Source's own trigger brushes. If you rotate a trigger, these tests won't
// account for that; keep triggers unrotated if you rely on them.
struct Box3D {
    vec3 center{0, 0, 0};
    vec3 half_extent{0.5f, 0.5f, 0.5f};
};

struct Sphere3D {
    vec3 center{0, 0, 0};
    float radius = 0.5f;
};

bool box3d_overlap(const Box3D& a, const Box3D& b);
bool box3d_contains_point(const Box3D& box, vec3 point);
bool sphere3d_overlap(const Sphere3D& a, const Sphere3D& b);
bool box3d_sphere3d_overlap(const Box3D& box, const Sphere3D& sphere);

// Ray-box intersection — the actual math behind real 3D picking (as opposed
// to the screen-space-nearest-point picking tools/thistle-editor uses, which
// doesn't need this since it never tests against real bounds). `out_t` is
// how far along the ray (in `ray_dir` units, so pass a normalized direction
// if you want `out_t` in world units) the hit is, only meaningful when this
// returns true.
bool ray_box3d(vec3 ray_origin, vec3 ray_dir, const Box3D& box, float& out_t);

// --- physics (Box2D) ----------------------------------------------------

struct Body {
    int id = -1;
    bool valid() const { return id >= 0; }
    bool operator==(Body o) const { return id == o.id; }
};

// Result of a physics raycast.
struct RayHit {
    bool hit = false;
    Body body{};
    vec2 point{0, 0};
    vec2 normal{0, 0};
    float fraction = 1.0f; // 0..1 along the ray
};

// A 2D physics world in pixel units (+y is down, matching the screen). Wraps
// Box2D. Step it each frame, then read body positions to place your sprites.
class Physics {
public:
    explicit Physics(vec2 gravity = {0.0f, 980.0f}); // pixels/s^2
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    Body add_static_box(vec2 center, vec2 size);
    // A kinematic box you move with set_velocity(); unaffected by collisions but
    // carries dynamic bodies resting on it. Use it for moving platforms.
    Body add_kinematic_box(vec2 center, vec2 size, float friction = 0.9f);
    Body add_dynamic_box(vec2 center, vec2 size, float density = 1.0f, float friction = 0.3f, float restitution = 0.0f);
    Body add_dynamic_circle(vec2 center, float radius, float density = 1.0f, float friction = 0.3f, float restitution = 0.2f);
    // A static overlap trigger (no physical response); use on_collision to react.
    Body add_sensor_box(vec2 center, vec2 size);

    void step(float dt);

    // Called during step() for each pair that starts touching (order unspecified).
    void on_collision(std::function<void(Body, Body)> begin);
    bool touching(Body a, Body b) const;
    RayHit raycast(vec2 from, vec2 to) const;

    vec2 position(Body b) const; // center in pixels
    float angle(Body b) const;   // radians
    vec2 velocity(Body b) const;
    void set_velocity(Body b, vec2 v);
    void apply_impulse(Body b, vec2 impulse);
    void set_position(Body b, vec2 center);
    void set_fixed_rotation(Body b, bool fixed);       // stop the body from tipping over
    void set_angular_velocity(Body b, float rad_per_s);
    void reset_body(Body b, vec2 center);              // teleport to center, angle 0, zero velocity

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- tilemap ------------------------------------------------------------

// A grid of tiles drawn from a tileset texture. Loads Tiled's CSV export
// (Map -> Export As -> .csv): comma-separated tile ids, 0 = empty, ids are
// 1-based into the tileset in row-major order.
class Tilemap {
public:
    // Single-layer CSV (Tiled Map -> Export As -> .csv).
    bool load_csv(const std::string& csv_path, Texture tileset, int tile_w, int tile_h, int tileset_cols = 0);
    // Full Tiled JSON (.tmj / .json). Loads the first tileset's image and all
    // tile layers; the image path is resolved relative to the JSON file.
    bool load_tiled_json(const std::string& path);

    void draw(Frame& f, vec2 offset = {0, 0}) const; // draws all layers back-to-front
    int at(int col, int row, int layer = 0) const;   // tile id (0 = empty)
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int layer_count() const { return static_cast<int>(layers_.size()); }
    int tile_w = 0, tile_h = 0;

private:
    Texture tileset_{};
    int cols_ = 0, rows_ = 0, tileset_cols_ = 0, firstgid_ = 1;
    std::vector<std::vector<int>> layers_;
};

// --- app ----------------------------------------------------------------

// --- scenes -------------------------------------------------------------

// A named chunk of game state (menu, gameplay, game-over, ...). Register scenes
// with App::scene() and switch between them with set_scene().
struct Scene {
    std::function<void()> enter;         // called when this scene becomes active
    std::function<void(Frame)> update;   // called every frame while active
    std::function<void()> exit;          // called when leaving this scene
};

struct AppConfig {
    std::string title = "Thistle App";
    int width = 1280;
    int height = 720;
    std::string icon; // optional path to a PNG for the window/dock icon
    // The save-data folder's name (save::, and anything next to save::path()).
    // Empty: the title. Set it when the title changes between versions
    // ("My Game v1.2"), or every update would start players from nothing.
    std::string save_name;
};

class App {
public:
    App(AppConfig config = {});
    App(std::string title, int width, int height);

    App& start(std::function<void()> fn);
    App& update(std::function<void(Frame)> fn);
    App& stop(std::function<void()> fn);

    // Register a scene. The first one registered becomes the initial scene.
    App& scene(const std::string& name, Scene s);

    // Opens the window and runs the loop. Returns after the window closes.
    int run();
};

// Switch to a registered scene; the change is applied at the start of the next frame.
void set_scene(const std::string& name);

} // namespace thistle

// =========================================================================
//  THISTLE 3D  —  namespace thistle::three
// =========================================================================
// The real 3D engine: GPU-resident meshes, lit shaders, models, shadows,
// and the rest. Everything 3D lives in `thistle::three` so it never tangles
// with the 2D API above — `using namespace thistle::three;` next to
// `using namespace thistle;` is safe, because nothing in here reuses a name
// from up there (the old sokol_gl "minor 3D" calls — Frame::cube(),
// load_mesh(), Camera3D — are a separate, older thing and keep working).
//
// Conventions, same as glTF and OpenGL: right-handed, +Y is up, a camera
// with no rotation looks down -Z, angles are radians, 1 unit = 1 meter.
namespace thistle::detail {
struct VoxelWorldAccess; // engine-internal (src/thistle_internal.h)
}

namespace thistle::three {

// --- math ----------------------------------------------------------------

inline constexpr float pi = 3.14159265358979323846f;
inline constexpr float radians(float degrees) { return degrees * (pi / 180.0f); }
inline constexpr float degrees(float radians) { return radians * (180.0f / pi); }

struct vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// A rotation. Build one with the static helpers, never by filling x/y/z/w
// by hand; the default is "no rotation".
struct quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    // Not an aggregate on purpose: with both `using namespace` lines in
    // effect, dot({1, 2, 3}, {4, 5, 6}) would otherwise be ambiguous between
    // the vec3 and quat overloads, since {1, 2, 3} could also build a quat.
    constexpr quat() = default;
    constexpr quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static quat axis_angle(vec3 axis, float radians);
    // Yaw around world +Y, then pitch around the (yawed) +X, then roll around
    // the resulting +Z: exactly an FPS camera's mouse-look. Positive pitch
    // looks up, positive yaw turns left. euler(0, 0, 0) is no rotation.
    static quat euler(float pitch, float yaw, float roll = 0.0f);
    // Rotation that points -Z (the "forward" axis) along `forward`.
    static quat look_rotation(vec3 forward, vec3 up = {0.0f, 1.0f, 0.0f});
    // Shortest rotation taking direction `from` onto direction `to`.
    static quat from_to(vec3 from, vec3 to);

    vec3 forward() const; // -Z rotated
    vec3 right() const;   // +X rotated
    vec3 up() const;      // +Y rotated
    quat inverse() const { return {-x, -y, -z, w}; } // valid for unit quats, which is all this API makes
};

quat operator*(quat a, quat b); // a * b applies b first, then a
vec3 operator*(quat q, vec3 v); // rotate v
float dot(quat a, quat b);
quat normalize(quat q);
quat slerp(quat a, quat b, float t); // takes the short way around
// Back to angles: {pitch, yaw, roll} (radians) such that quat::euler(pitch,
// yaw, roll) gives the same rotation. For showing and editing rotations as
// numbers; keep doing the math with quats.
vec3 to_euler(quat q);

// 4x4 matrix, column-major (m[column * 4 + row]) — the same memory layout
// the GPU shaders expect, so it goes straight into a uniform.
struct mat4 {
    float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};

    static mat4 identity() { return {}; }
    static mat4 translate(vec3 t);
    static mat4 scale(vec3 s);
    static mat4 rotate(quat q);
    static mat4 trs(vec3 translation, quat rotation, vec3 scale);
    // OpenGL-style clip space (z in -1..1). The renderer converts for
    // Metal/D3D11 itself, so you never pick a convention per platform.
    static mat4 perspective(float fovy_radians, float aspect, float near_z, float far_z);
    static mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z);
    static mat4 look_at(vec3 eye, vec3 target, vec3 up = {0.0f, 1.0f, 0.0f});

    float& operator()(int row, int col) { return m[col * 4 + row]; }
    float operator()(int row, int col) const { return m[col * 4 + row]; }

    vec3 transform_point(vec3 p) const;     // includes the perspective divide
    vec3 transform_direction(vec3 d) const; // ignores translation
};

mat4 operator*(const mat4& a, const mat4& b); // a * b applies b first, then a
vec4 operator*(const mat4& a, vec4 v);
mat4 transpose(const mat4& a);
mat4 inverse(const mat4& a); // returns identity for a singular matrix

// Position + rotation + scale — what every object in a 3D scene has.
struct Transform {
    vec3 position{0.0f, 0.0f, 0.0f};
    quat rotation{};
    vec3 scale{1.0f, 1.0f, 1.0f};

    Transform() = default;
    // Implicit on purpose: anywhere a Transform is wanted, a plain position
    // works too — world.draw(crate, player_pos).
    Transform(vec3 position_, quat rotation_ = {}, vec3 scale_ = {1.0f, 1.0f, 1.0f})
        : position(position_), rotation(rotation_), scale(scale_) {}
    // ...and so does a bare {x, y, z}: world.draw(crate, {0, 1, 0}).
    Transform(float x, float y, float z) : position{x, y, z} {}

    mat4 matrix() const { return mat4::trs(position, rotation, scale); }
    vec3 forward() const { return rotation.forward(); }
    vec3 right() const { return rotation.right(); }
    vec3 up() const { return rotation.up(); }
    // Converts a point/direction from this object's local space to the world.
    vec3 apply(vec3 local_point) const { return position + rotation * (scale * local_point); }
    void look_at(vec3 target, vec3 up_dir = {0.0f, 1.0f, 0.0f}) {
        rotation = quat::look_rotation(target - position, up_dir);
    }
};

// parent * child = the child's transform in the parent's space, flattened.
// Exact for uniform scale; with non-uniform parent scale plus a rotated
// child it's the usual TRS approximation (no shear), same as most engines.
Transform operator*(const Transform& parent, const Transform& child);

inline void to_json(nlohmann::json& j, const quat& q) { j = {q.x, q.y, q.z, q.w}; }
inline void from_json(const nlohmann::json& j, quat& q) {
    q.x = j.at(0).get<float>(); q.y = j.at(1).get<float>(); q.z = j.at(2).get<float>(); q.w = j.at(3).get<float>();
}
inline void to_json(nlohmann::json& j, const Transform& t) {
    j = {{"position", t.position}, {"rotation", t.rotation}, {"scale", t.scale}};
}
inline void from_json(const nlohmann::json& j, Transform& t) {
    j.at("position").get_to(t.position); j.at("rotation").get_to(t.rotation); j.at("scale").get_to(t.scale);
}

// Axis-aligned box, min/max corners. Default-constructed = empty (invalid).
struct Bounds {
    vec3 min{1e30f, 1e30f, 1e30f};
    vec3 max{-1e30f, -1e30f, -1e30f};

    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    vec3 center() const { return (min + max) * 0.5f; }
    vec3 size() const { return max - min; }
    void add(vec3 p);
    Bounds transformed(const mat4& m) const; // bounds of the 8 transformed corners
    bool overlaps(const Bounds& o) const {   // touching edges don't count
        return min.x < o.max.x && max.x > o.min.x && min.y < o.max.y && max.y > o.min.y && min.z < o.max.z && max.z > o.min.z;
    }
    bool contains(vec3 p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
};

// --- geometry --------------------------------------------------------------

struct Vertex {
    vec3 position;
    vec3 normal{0.0f, 1.0f, 0.0f};
    vec2 uv;
    rgba color = white; // multiplied into the material color: this is how vertex-colored low-poly models get their colors
    // Skinning (from glTF): up to 4 skeleton joints (indices, as floats) and
    // how much each one moves this vertex. All-zero weights: not skinned.
    vec4 joints;
    vec4 weights;
};

// Geometry on the CPU side: build or edit it however you like, then turn it
// into something drawable with make_model(). Triangles are counter-clockwise
// when seen from the front (the glTF/OpenGL convention).
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // 3 per triangle

    void add_triangle(const Vertex& a, const Vertex& b, const Vertex& c);
    void add_quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d); // a-b-c-d counter-clockwise
    void append(const MeshData& other, const Transform& t = {});
    void recalculate_normals(); // smooth, averaged per shared vertex
    // Splits every triangle into its own 3 vertices with the face normal —
    // the faceted low-poly look. A low-res sphere_mesh() plus this is a rock.
    void make_flat();
    void set_color(rgba color); // paints every vertex
    Bounds bounds() const;
};

// Ready-made shapes, centered on the origin, 1 unit big by default.
MeshData box_mesh(vec3 size = {1.0f, 1.0f, 1.0f});
MeshData sphere_mesh(float radius = 0.5f, int rings = 16, int segments = 24);
MeshData cylinder_mesh(float radius = 0.5f, float height = 1.0f, int segments = 24);
MeshData cone_mesh(float radius = 0.5f, float height = 1.0f, int segments = 24);
MeshData capsule_mesh(float radius = 0.5f, float height = 2.0f, int segments = 16); // height includes the caps
// Flat on the ground (facing +Y). uv_repeat tiles the texture that many times.
MeshData plane_mesh(float width = 1.0f, float depth = 1.0f, float uv_repeat = 1.0f);

// --- materials -------------------------------------------------------------

enum class AlphaMode {
    Opaque, // alpha ignored
    Cutout, // pixels below alpha_cutoff are thrown away: leaves, fences, grass cards
    Blend,  // real see-through: glass, water, ghosts (drawn last, sorted back to front)
};

enum class TextureFilter {
    Linear,  // smooth — photos, painted textures
    Nearest, // crisp pixels — pixel art, Minecraft-style blocks
};

struct Material {
    rgba color = white;         // multiplied with the texture and the vertex colors
    Texture texture;            // any thistle::load_texture() result; none = plain color
    rgba emissive = black;      // light the surface gives off itself; black = none
    Texture emissive_texture;   // optional glow map, multiplied by `emissive` (screens, windows, lava)
    float specular = 0.2f;      // highlight strength, 0 = completely matte
    float shininess = 24.0f;    // highlight tightness: ~8 rough plastic, ~64 polished
    bool unlit = false;         // ignore lighting entirely (signs, UI in the world, stylized looks)
    bool double_sided = false;  // draw the back faces too (leaves, flags, paper)
    AlphaMode alpha = AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;  // only for AlphaMode::Cutout
    vec2 uv_scale{1.0f, 1.0f};  // texture tiling
    TextureFilter filter = TextureFilter::Linear;
    bool casts_shadow = true;   // AlphaMode::Blend surfaces never cast shadows, whatever this says
};

// --- models ------------------------------------------------------------------

// A drawable 3D thing, stored on the GPU: one or more parts, each a mesh with
// its own material. A lightweight handle — copy it freely, draw it as many
// times per frame as you like, at different transforms.
struct Model {
    int id = -1;
    bool valid() const { return id >= 0; }
};

Model make_model(const MeshData& mesh, const Material& material = {});

// What a model file contains, parsed but not yet on the GPU: edit it (recolor
// a part, merge parts, bake it into something else) and then make_model() it.
// The bones of an animated model (glTF skins). Joints are listed with
// their parents; a vertex's Vertex::joints index into this list.
struct Skeleton {
    struct Joint {
        std::string name;
        int parent = -1;   // index into joints; -1 for a root
        Transform rest;    // relative to the parent, when nothing is animating it
        mat4 inverse_bind; // from the model's space to the joint's, as the mesh was modeled
        mat4 root_offset;  // roots only: where the root's parent (an armature node, say) puts it
    };
    std::vector<Joint> joints;
    bool empty() const { return joints.empty(); }
    int find(const std::string& name) const; // -1 if there's none by that name
};

// One animation (walk, run, wave): keyframes for joints' position,
// rotation and scale over time.
struct AnimationClip {
    struct Channel {
        enum class Path { Translation, Rotation, Scale };
        int joint = -1;
        Path path = Path::Rotation;
        bool step = false;         // jump between keyframes instead of blending
        std::vector<float> times;  // seconds, increasing
        std::vector<vec4> values;  // xyz (rotations: xyzw)
    };
    std::string name;
    float duration = 0.0f;
    std::vector<Channel> channels;
};

struct ModelData {
    struct Part {
        std::string name;
        MeshData mesh;
        Material material;
        mat4 transform; // where the part sits in the model (from the file's node hierarchy)
    };
    std::vector<Part> parts;
    Skeleton skeleton;                    // empty unless the model is skinned
    std::vector<AnimationClip> animations;
    bool empty() const { return parts.empty(); }
    Bounds bounds() const;
};

// Reads .obj (+ its .mtl: colors, textures, transparency, glow), .gltf and
// .glb (the format to export from Blender: node hierarchy, materials,
// vertex colors, embedded or external textures). Textures load through
// load_texture(), relative to the model file. Logs a warning and returns
// an empty/invalid result if the file can't be read.
ModelData load_model_data(const std::string& path);
Model make_model(const ModelData& data);
Model load_model(const std::string& path); // load_model_data() + make_model()
void unload_model(Model& model);
Bounds model_bounds(Model model);       // in the model's own space
int model_part_count(Model model);
Material model_material(Model model, int part = 0);
void set_model_material(Model model, const Material& material, int part = -1); // -1 = every part

// --- skeletal animation ------------------------------------------------------------
// Animated glTF characters: a model with a skeleton and animation clips
// (walk, run, idle, wave...), and an Animator per character that plays
// them. Drawn without an Animator, a skinned model stands in its rest pose.
//
//   Model fox = load_model("assets/Fox.glb");
//   Animator anim(fox);
//   anim.play("Walk");
//   // every frame:
//   anim.update(f.dt);
//   world.draw(fox, fox_transform, anim);

const Skeleton* model_skeleton(Model model); // nullptr if it has none
int model_animation_count(Model model);
const AnimationClip* model_animation(Model model, int index);
int find_animation(Model model, const std::string& name); // -1 if there's none by that name

// One character's playback: which clip, how far into it, and the pose that
// results. Cheap to make; one per character, even when they share a Model.
class Animator {
public:
    float speed = 1.0f; // playback rate: 2 = double speed, negative = backwards

    Animator() = default;
    explicit Animator(Model model);

    // Switch to a clip, blending from what was playing over `fade` seconds
    // (0 = cut). Playing the clip that's already playing does nothing, so
    // it's fine to call every frame: anim.play(moving ? "Run" : "Idle").
    void play(const std::string& clip, float fade = 0.2f, bool loop = true);
    void play(int clip, float fade = 0.2f, bool loop = true);
    void stop(float fade = 0.2f); // back to the rest pose
    void update(float dt);

    int clip() const { return clip_; }      // -1: none
    std::string clip_name() const;
    float time() const { return time_; }    // seconds into the clip
    void set_time(float seconds);
    bool finished() const;                  // a non-looping clip reached its end

    // Where a joint is right now, in the model's own space (multiply by the
    // character's transform for the world): hold a sword, attach a hat.
    int find_joint(const std::string& name) const;
    mat4 joint_matrix(int joint) const;
    Transform joint_transform(int joint) const;

    Model model() const { return model_; }
    // Per joint: from the rest mesh to the current pose (what skinning uses).
    const std::vector<mat4>& skin_matrices() const { return skin_; }

private:
    struct Local {
        vec3 t;
        quat r;
        vec3 s{1.0f, 1.0f, 1.0f};
    };
    void evaluate();
    void sample(int clip, float time, std::vector<Local>& out) const;

    Model model_;
    int clip_ = -1, from_clip_ = -1;
    float time_ = 0.0f, from_time_ = 0.0f;
    bool loop_ = true, from_loop_ = true;
    float fade_ = 0.0f, fade_length_ = 0.0f; // blending from from_clip_ while fade_ < fade_length_
    std::vector<int> order_;                  // joints, parents before children
    std::vector<Local> pose_, from_pose_;
    std::vector<mat4> global_, skin_;
};

// --- rays & picking -------------------------------------------------------------

struct Ray {
    vec3 origin;
    vec3 direction{0.0f, 0.0f, -1.0f}; // keep it normalized: hit distances are in these units
    vec3 at(float distance) const { return origin + direction * distance; }
};

struct RaycastHit {
    bool hit = false;
    float distance = 0.0f; // along the ray
    vec3 point;
    vec3 normal;           // surface normal at the hit, facing back toward the ray
    explicit operator bool() const { return hit; }
};

inline constexpr float no_limit = 1e30f;

RaycastHit raycast(const Ray& ray, const Bounds& box, float max_distance = no_limit);
// Exact: tests every triangle of every part (after a cheap bounds check).
RaycastHit raycast(const Ray& ray, Model model, const Transform& transform, float max_distance = no_limit);
RaycastHit raycast_sphere(const Ray& ray, vec3 center, float radius, float max_distance = no_limit);
RaycastHit raycast_plane(const Ray& ray, vec3 point_on_plane, vec3 plane_normal, float max_distance = no_limit);
RaycastHit raycast_triangle(const Ray& ray, vec3 a, vec3 b, vec3 c, float max_distance = no_limit); // both sides

// The 6 planes of what a camera can see. The renderer uses it to skip
// objects that are off-screen; you can use it for the same thing (e.g. to
// not bother updating the animation of something nobody can see).
struct Frustum {
    vec4 planes[6]; // xyz = inward normal, w = distance
    static Frustum from_matrix(const mat4& view_proj); // OpenGL-style clip space, like mat4::perspective
    bool contains(vec3 point) const;
    bool intersects(const Bounds& box) const;
    bool intersects_sphere(vec3 center, float radius) const;
};

// --- camera --------------------------------------------------------------------

struct Camera {
    vec3 position{0.0f, 2.0f, 6.0f};
    quat rotation{};                // identity looks down -Z
    float fov = radians(60.0f);     // vertical field of view
    float near_z = 0.1f;
    float far_z = 1000.0f;
    bool orthographic = false;
    float ortho_height = 10.0f;     // world units visible top-to-bottom when orthographic

    void look_at(vec3 target, vec3 up = {0.0f, 1.0f, 0.0f}) { rotation = quat::look_rotation(target - position, up); }
    vec3 forward() const { return rotation.forward(); }
    vec3 right() const { return rotation.right(); }
    vec3 up() const { return rotation.up(); }
    mat4 view() const;
    mat4 projection(float aspect) const;
    Frustum frustum(float aspect) const;

    // The ray from the camera through a pixel (top-left origin, y down, same
    // as Frame::mouse()) — this is mouse picking: raycast it at your objects.
    Ray screen_ray(vec2 pixel, const Frame& f) const;
    Ray screen_ray(vec2 pixel, Rect viewport) const;
    // Where a world point lands on screen, in pixels. False (and `out` left
    // alone) if the point is behind the camera. Health bars, nameplates.
    bool world_to_screen(vec3 point, const Frame& f, vec2& out) const;
    bool world_to_screen(vec3 point, Rect viewport, vec2& out) const;
};

// --- camera controllers -----------------------------------------------------------
// Each one reads input from the Frame and moves a Camera; call update() once
// per frame before rendering. They're plain structs: read or set any field
// (e.g. point MouseLook's yaw at a spawn direction) whenever you like.

// First-person look: mouse (while captured) and the gamepad's right stick
// turn the camera. With capture_mouse, clicking the window captures the
// mouse and Escape releases it; otherwise it turns while the right button is held.
struct MouseLook {
    float yaw = 0.0f;   // radians, positive = turned left
    float pitch = 0.0f; // radians, positive = looking up
    float sensitivity = 0.0025f; // radians per pixel of mouse movement
    float stick_speed = 2.5f;    // radians per second at full right-stick
    float max_pitch = radians(89.0f);
    bool invert_y = false;
    bool capture_mouse = true;

    void update(Camera& camera, const Frame& f);
    // WASD / arrow keys / left stick as a flat (y = 0) world-space direction
    // relative to where you're facing, length 0..1 — feed it to your
    // character's movement.
    vec3 move_input(const Frame& f) const;
    vec3 forward_flat() const; // facing direction with y = 0
};

// A free-flying noclip camera: MouseLook plus WASD to move, E/Space up,
// Q/Ctrl down, Shift for speed. The debug/editor camera.
struct FlyCamera {
    MouseLook look;
    float speed = 8.0f; // units per second
    float boost = 4.0f; // speed multiplier while Shift is held

    void update(Camera& camera, const Frame& f);
};

// Circles around a target point. Right-drag orbits, scroll zooms, middle-drag
// (or Shift+right-drag) pans. For a third-person game, set `target` to the
// player every frame and turn on capture_mouse to orbit without a button.
struct OrbitCamera {
    vec3 target{0.0f, 0.0f, 0.0f};
    float distance = 8.0f;
    float yaw = radians(25.0f);   // radians around the target, 0 = camera on the +Z side
    float pitch = radians(25.0f); // radians above the target (negative = below)
    float min_distance = 0.5f;
    float max_distance = 200.0f;
    float min_pitch = radians(-85.0f);
    float max_pitch = radians(85.0f);
    float sensitivity = 0.006f;   // radians per pixel
    float zoom_speed = 0.12f;     // fraction of the distance per scroll step
    bool allow_pan = true;
    bool capture_mouse = false;

    void update(Camera& camera, const Frame& f);
    void apply(Camera& camera) const; // just position the camera, no input
};

// --- world -----------------------------------------------------------------------

// The main light: parallel rays from far away. `direction` is the way the
// light travels, so the default (mostly -Y) is a sun high in the sky.
struct Sun {
    vec3 direction{-0.35f, -1.0f, -0.45f};
    rgba color = rgb(1.0f, 0.96f, 0.88f);
    float intensity = 1.0f;

    bool shadows = true;
    // Shadows are drawn out to this far from the camera. One shadow map is
    // stretched over that range, so smaller = sharper shadows up close.
    float shadow_distance = 60.0f;
    float shadow_strength = 1.0f;  // 1 = shadowed areas get only ambient light, 0.5 = half as dark
    float shadow_softness = 1.0f;  // edge blur, in shadow-map texels
    int shadow_resolution = 2048;  // shadow map size in pixels (memory: 4 bytes each)
};

// A sky made of 6 square images, one per direction (a cube map). A handle,
// like Model. Named the way skybox packs name their files: "front" is what a
// default camera (looking down -Z) sees, "right" is +X. Packs that use axis
// names map as px = right, nx = left, py = top, ny = bottom, pz = front,
// nz = back — the usual cube-map convention; the renderer handles the
// left-handed/right-handed difference so nothing comes out mirrored.
struct Skybox {
    int id = -1;
    bool valid() const { return id >= 0; }
};
Skybox load_skybox(const std::string& right, const std::string& left, const std::string& top,
                   const std::string& bottom, const std::string& front, const std::string& back);
void unload_skybox(Skybox& skybox);

// The background. Its colors also light the scene: surfaces facing up pick
// up `top`, facing down pick up `ground` (scaled by World::ambient), so
// shadowed sides are tinted by the sky instead of going flat black. With a
// skybox set, the skybox is drawn instead of the gradient and its own
// average up/down colors do the lighting.
struct Sky {
    rgba top = rgb(0.30f, 0.52f, 0.85f);
    rgba horizon = rgb(0.72f, 0.82f, 0.92f);
    rgba ground = rgb(0.33f, 0.31f, 0.29f);
    bool visible = true;  // false: no background, whatever 2D was drawn earlier shows through
    bool sun_disc = true; // draw the sun (from World::sun) as a bright disc with a soft glow
    Skybox skybox;
};

// A light bulb: shines in all directions, fading to nothing at `range`.
struct PointLight {
    vec3 position;
    rgba color = white;
    float range = 10.0f;
    float intensity = 1.0f;
};

// A flashlight / stage light: a cone pointing along `direction`.
struct SpotLight {
    vec3 position;
    vec3 direction{0.0f, -1.0f, 0.0f};
    rgba color = white;
    float range = 15.0f;
    float intensity = 1.0f;
    float angle = radians(30.0f); // half-angle of the cone
    float softness = 0.25f;       // fraction of the cone's edge that fades out, 0 = hard edge
};

// Distance fog: things fade into `color` between `start` and `end` meters
// from the camera. With match_sky the fog takes the sky's horizon color, so
// distant geometry melts into the horizon instead of ending at a hard edge.
struct Fog {
    bool enabled = false;
    float start = 30.0f;
    float end = 150.0f;
    rgba color = rgb(0.72f, 0.82f, 0.92f);
    bool match_sky = true;
};

struct RenderStats {
    int draw_calls = 0;
    int triangles = 0;  // copies drawn by draw_many() count once each
    int culled = 0; // model parts skipped because they were off-screen
};

class VoxelWorld;
class Terrain;

// A flat picture in the 3D world that always turns to face the camera:
// sprites, far-away trees, a Doom-style enemy, a marker over an objective.
struct Billboard {
    vec3 position;             // center
    vec2 size{1.0f, 1.0f};     // world units
    rgba color = white;        // tint (and alpha)
    Texture texture;           // none = a plain colored square
    Rect frame;                // part of the texture in pixels, for sprite sheets; empty = all of it
    float rotation = 0.0f;     // radians, spinning in the camera's view
    bool upright = false;      // only turn around the vertical axis (trees, characters), don't tip back
    bool additive = false;     // glow: adds light instead of covering (fire, sparks, magic)
};

// How a ParticleSystem's particles look and move.
struct ParticleSettings {
    Texture texture;                    // none = a soft round dot
    rgba start_color = white;
    rgba end_color = rgba{1.0f, 1.0f, 1.0f, 0.0f}; // fades out by default
    float start_size = 0.25f;           // world units
    float end_size = 0.05f;
    float lifetime = 1.0f;              // seconds
    float lifetime_jitter = 0.3f;       // +- this fraction, so they don't all vanish at once
    vec3 velocity{0.0f, 1.5f, 0.0f};    // starting velocity...
    float spread = 1.0f;                // ...plus up to this much in a random direction (m/s)
    vec3 gravity{0.0f, -4.0f, 0.0f};
    float drag = 0.5f;                  // fraction of velocity lost per second
    float spin = 0.0f;                  // max random rotation speed, radians/second
    bool additive = false;              // glowing particles (fire, sparks) vs. solid ones (dust, smoke)
};

// Smoke, sparks, dust, explosions: emit() bursts, update() moves them, and
// World::draw(particles) draws them — per frame, like everything else.
class ParticleSystem {
public:
    ParticleSettings settings;
    size_t max_particles = 20000; // emits past this are dropped

    void emit(vec3 position, int count) { emit(position, count, settings); }
    void emit(vec3 position, int count, const ParticleSettings& look); // a one-off burst in a different style
    void update(float dt);
    void clear() { particles_.clear(); }
    size_t count() const { return particles_.size(); }

    struct Particle {
        vec3 position, velocity;
        float age, lifetime, rotation, spin;
        const ParticleSettings* look; // the style it was emitted with
    };
    // Read-only, e.g. to let sparks set things on fire. Order is arbitrary.
    const std::vector<Particle>& particles() const { return particles_; }

private:
    std::vector<Particle> particles_;
    std::vector<std::unique_ptr<ParticleSettings>> looks_; // one per distinct style emitted
    uint32_t seed_ = 12345;
};

// A 3D scene. Settings (sun, sky, ambient) stay until you change them; draw
// calls are per frame, exactly like the 2D API — call draw() for everything
// visible every frame, then render() once:
//
//   world.draw(crate, {0, 0.5f, 0});
//   world.box({3, 0.5f, 0}, {1, 1, 1}, coral);
//   world.render(f, camera);
//   f.text("score", ...);   // 2D after render() lands on top of the 3D
//
// 2D drawn before render() ends up behind the 3D (hidden by the sky unless
// sky.visible is false). render() can be called more than once per frame,
// e.g. once per viewport for split-screen.
class World {
public:
    Sun sun;
    Sky sky;
    Fog fog;
    float ambient = 0.55f; // how strongly the sky/ground colors light everything

    // Up to 16 point/spot lights light each render(); past that, the ones
    // nearest the camera win (off-screen ones are dropped first). Per frame,
    // like draw() — call it every frame for every light that's on.
    static constexpr int max_lights = 16;

    World();
    ~World();
    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void light(const PointLight& light);
    void light(const SpotLight& light);

    void draw(Model model, const Transform& transform = {}, rgba tint = white);
    // Many copies of one model in one draw call per part — a forest, grass,
    // rubble, a crowd. Each copy has its own transform and (optionally) tint.
    // Culled as one group; see-through copies aren't sorted among themselves.
    void draw_many(Model model, const Transform* transforms, size_t count, const rgba* tints = nullptr);
    void draw_many(Model model, const std::vector<Transform>& transforms) { draw_many(model, transforms.data(), transforms.size()); }
    void draw_many(Model model, const std::vector<Transform>& transforms, const std::vector<rgba>& tints) {
        draw_many(model, transforms.data(), transforms.size(), tints.size() >= transforms.size() ? tints.data() : nullptr);
    }
    void billboard(const Billboard& b);
    void draw(const ParticleSystem& particles);
    // A block world: re-meshes whatever chunks were edited, then draws them.
    void draw(VoxelWorld& voxels);
    void draw(Terrain& terrain);
    // Same, but every part of the model uses `material` instead of its own.
    void draw(Model model, const Transform& transform, const Material& material);
    // An animated model in the Animator's current pose.
    void draw(Model model, const Transform& transform, const Animator& pose, rgba tint = white);

    // Quick shapes, no Model needed (they share built-in unit meshes).
    void box(vec3 center, vec3 size, rgba color = white);
    void sphere(vec3 center, float radius, rgba color = white);
    void cylinder(vec3 center, float radius, float height, rgba color = white);
    void cone(vec3 center, float radius, float height, rgba color = white);
    void plane(vec3 center, vec2 size, rgba color = white);
    // Rotated, too: a 1 m cube / 1 m wide sphere, cylinder and cone, scaled
    // by transform.scale — so world.box(physics.transform(crate, size)).
    // (Templates only so that box({x, y, z}, {w, h, d}) still means the
    // center-and-size version above instead of being ambiguous.)
    template <class T> requires std::is_same_v<T, Transform>
    void box(const T& transform, rgba color = white) { shape(0, transform, color); }
    template <class T> requires std::is_same_v<T, Transform>
    void sphere(const T& transform, rgba color = white) { shape(1, transform, color); }
    template <class T> requires std::is_same_v<T, Transform>
    void cylinder(const T& transform, rgba color = white) { shape(2, transform, color); }
    template <class T> requires std::is_same_v<T, Transform>
    void cone(const T& transform, rgba color = white) { shape(3, transform, color); }
    template <class T> requires std::is_same_v<T, Transform>
    void plane(const T& transform, rgba color = white) { shape(4, transform, color); } // 1x1 m, facing +Y

    // Debug lines: 1 pixel wide, unlit. on_top draws them through everything
    // (gizmos, selection outlines); otherwise they're hidden behind solid objects.
    void line(vec3 a, vec3 b, rgba color = white, bool on_top = false);
    void wire_box(const Bounds& box, rgba color = white, bool on_top = false);
    void wire_box(const Transform& transform, rgba color = white, bool on_top = false); // a unit cube, transformed
    void wire_sphere(vec3 center, float radius, rgba color = white, bool on_top = false);
    void grid(vec3 center, float size, float spacing = 1.0f, rgba color = rgba{1.0f, 1.0f, 1.0f, 0.25f});

    void render(const Frame& f, const Camera& camera);
    void render(const Frame& f, const Camera& camera, Rect viewport); // viewport in pixels, top-left origin

private:
    void shape(int kind, const Transform& transform, rgba color); // 0 box, 1 sphere, 2 cylinder, 3 cone, 4 plane
    std::unique_ptr<struct WorldImpl> impl_;
};

// Totals over every World::render() of the previous frame.
RenderStats render_stats();

// --- 3D sound ----------------------------------------------------------------------------
// Sounds with a place in the world: quieter the farther they are from the
// listener, and panned toward the side they're on. Sounds without a place
// (UI clicks, music) stay on play_sound() / play_music().
//
//   play_sound_at("assets/boom.wav", crate_position);
//   Sound engine = play_sound_at("assets/engine.ogg", car, {.loop = true});
//   set_sound_position(engine, car);   // every frame, to follow the car
//
// The listener is the camera of the last World::render() automatically.
// Call set_listener() to put it somewhere else (a third-person game might
// prefer the character's head); from then on it stays where you put it.
struct SoundSettings {
    float volume = 1.0f;
    float pitch = 1.0f;          // 2 = an octave up (and twice as fast)
    bool loop = false;           // until stop_sound()
    float min_distance = 1.0f;   // full volume this close
    float max_distance = 60.0f;  // no quieter past this
    float rolloff = 1.0f;        // how fast it fades in between (1: about like real life)
    bool stream = false;         // decode while playing: long ambience loops, not effects played often
};

// A playing 3D sound. Goes invalid by itself when the sound finishes (every
// call on it then does nothing), so it's safe to keep around.
struct Sound {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
    explicit operator bool() const { return id != 0; }
    bool operator==(Sound o) const { return id == o.id; }
};

// An invalid Sound when the file can't be played, or there's no audio
// device yet (before App::run(), or on a machine without one). Up to 128
// play at once; past that, the oldest non-looping one is cut off.
Sound play_sound_at(const std::string& path, vec3 position, const SoundSettings& settings = {});
void set_sound_position(Sound sound, vec3 position);
void set_sound_volume(Sound sound, float volume);
void set_sound_pitch(Sound sound, float pitch);
void stop_sound(Sound sound);
bool sound_playing(Sound sound);
void stop_all_sounds(); // every 3D sound (not play_sound()/music)
int playing_sound_count();
// Decode a file now, so its first play_sound_at() doesn't stall a frame.
// Files are decoded once and shared by every play either way.
void preload_sound(const std::string& path);
void set_listener(vec3 position, quat rotation);
void set_listener(const Camera& camera);
void set_listener_automatic(); // back to following World::render()'s camera

// --- voxels ------------------------------------------------------------------------
// A block world, Minecraft- or Teardown-style: a grid of block ids, stored
// in 32x32x32 chunks that only exist where something was placed, turned into
// ordinary meshes (and so lit, shadowed and culled like everything else).
// Optional — nothing else in the engine depends on it.

struct ivec3 {
    int x = 0, y = 0, z = 0;
};
inline ivec3 operator+(ivec3 a, ivec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline ivec3 operator-(ivec3 a, ivec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline ivec3 operator*(ivec3 a, int s) { return {a.x * s, a.y * s, a.z * s}; }
inline bool operator==(ivec3 a, ivec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline bool operator!=(ivec3 a, ivec3 b) { return !(a == b); }

using BlockId = uint16_t; // 0 is always air

// What a block looks like. Either a flat color (the MagicaVoxel/Teardown
// look) or textured from the VoxelWorld's atlas (the Minecraft look) — set
// the tiles and the color becomes a tint on top of them.
struct BlockType {
    std::string name;
    rgba color = white;
    // Tile indices into the atlas (see VoxelWorld::set_atlas), counted left
    // to right, top to bottom. -1 = untextured. set_tiles() fills all three.
    int tile_top = -1;
    int tile_side = -1;
    int tile_bottom = -1;
    // Opaque: a normal block. Cutout: see-through where the texture is
    // (leaves). Blend: glass, water — color.a says how see-through.
    AlphaMode alpha = AlphaMode::Opaque;
    rgba emissive = black; // lava, glowstone
    bool solid = true;     // false: things walk through it (water, tall grass)

    BlockType& set_tiles(int all) { tile_top = tile_side = tile_bottom = all; return *this; }
    BlockType& set_tiles(int top, int side, int bottom) { tile_top = top; tile_side = side; tile_bottom = bottom; return *this; }
    bool textured() const { return tile_top >= 0 || tile_side >= 0 || tile_bottom >= 0; }
};

class VoxelWorld {
public:
    static constexpr int chunk_size = 32;

    float voxel_size = 1.0f;     // world units per block: 1 for Minecraft, ~0.1 for Teardown
    vec3 origin{0.0f, 0.0f, 0.0f}; // world position of block (0,0,0)'s minimum corner
    // Turns the whole grid around `origin` — a voxel prop or a chunk of
    // debris. Drawing, raycast() and to_block() follow it; the built-in
    // CharacterController/CollisionWorld assume an unrotated grid.
    quat rotation{};
    bool ambient_occlusion = true; // darken the inside corners where blocks meet (applies on the next re-mesh)

    VoxelWorld();
    ~VoxelWorld();
    VoxelWorld(VoxelWorld&&) noexcept;
    VoxelWorld& operator=(VoxelWorld&&) noexcept;
    VoxelWorld(const VoxelWorld&) = delete;
    VoxelWorld& operator=(const VoxelWorld&) = delete;

    // Block types. Ids are handed out in order starting at 1.
    BlockId add_block(const BlockType& type);
    const BlockType& block_type(BlockId id) const; // id 0 or unknown: an "air" type
    // Changes what an existing type looks like (its blocks re-mesh). Unknown ids and 0 are ignored.
    void set_block_type(BlockId id, const BlockType& type);
    BlockId find_block(const std::string& name) const; // 0 if there's none by that name
    int block_type_count() const;
    // One texture holding every block texture in a grid of tile_size-pixel
    // squares. Filter defaults to Nearest: crisp pixels, the Minecraft look.
    void set_atlas(Texture atlas, int tile_size, TextureFilter filter = TextureFilter::Nearest);

    BlockId get(int x, int y, int z) const;
    BlockId get(ivec3 p) const { return get(p.x, p.y, p.z); }
    void set(int x, int y, int z, BlockId id);
    void set(ivec3 p, BlockId id) { set(p.x, p.y, p.z, id); }
    void fill(ivec3 min, ivec3 max, BlockId id); // inclusive box
    void fill_sphere(vec3 center, float radius, BlockId id); // in block coordinates; id 0 carves
    void clear();

    ivec3 to_block(vec3 world_point) const;    // which block a world point is inside
    vec3 block_center(ivec3 block) const;      // world position of a block's center
    Bounds block_bounds(ivec3 block) const;    // world-space box of one block
    Bounds bounds() const;                     // world-space box around every non-air block
    // The same box in the grid's own space: block coordinates times
    // voxel_size, before origin and rotation. Both scan the blocks only after
    // a change; otherwise they're remembered.
    Bounds grid_bounds() const;

    int chunk_count() const;
    int block_count() const; // blocks that aren't air
    // Every block that isn't air, in no particular order.
    void each_block(const std::function<void(ivec3 block, BlockId id)>& fn) const;
    // Takes another world's block types and atlas (replacing this one's), so
    // ids mean the same in both: for pieces broken off it, copies, previews.
    void copy_block_types(const VoxelWorld& from);
    // A separate world with the same blocks, types, atlas and placement
    // (not the generator). Worlds can't be copied with `=`: they own GPU meshes.
    VoxelWorld copy() const;
    // The mesh the renderer draws for one chunk (chunk coordinates, i.e.
    // block / 32), in the chunk's own block units — useful for exporting,
    // or for tests. Parts are split by material (flat vs textured, alpha).
    ModelData mesh_chunk(ivec3 chunk) const;
    // Chunks are re-meshed lazily, when drawn after an edit. Call this to
    // do it up front (e.g. behind a loading screen) instead.
    void remesh_all();

    // Walks the grid along the ray, block by block (exact, no stepping
    // artifacts), and stops at the first block that isn't air — or, with
    // solid_only, the first solid one (so a ray passes through water).
    // hit.block + hit.normal is the empty cell in front of the face that was
    // hit: exactly where "place a block" should put it.
    struct Hit {
        bool hit = false;
        ivec3 block;
        ivec3 normal;   // which face: {0,1,0} = top, {-1,0,0} = the -X side, ...
        BlockId id = 0;
        float distance = 0.0f;
        vec3 point;
        explicit operator bool() const { return hit; }
    };
    Hit raycast(const Ray& ray, float max_distance = 100.0f, bool solid_only = false) const;

    // Is any solid block inside this world-space box?
    bool overlaps_solid(const Bounds& box) const;

    // --- endless worlds ---
    // A generator fills one chunk (chunk coordinates; its blocks span
    // chunk * 32 .. chunk * 32 + 31) by calling set(). stream_around() then
    // generates chunks near a point as it moves and drops far ones — except
    // chunks the player changed, which stay loaded so edits aren't lost
    // (save() them if you want them to outlive the program). The generator
    // must be deterministic (same chunk -> same blocks): noise, not rand().
    using Generator = std::function<void(VoxelWorld& world, ivec3 chunk)>;
    void set_generator(Generator generator);
    // Generates at most `budget` new chunks per call (nearest first), so
    // walking into new land costs a few chunks per frame, not a hitch.
    void stream_around(vec3 world_position, float radius, int budget = 4);
    // Re-meshing is capped per draw too (nearest the streaming point first);
    // 0 = unlimited.
    int max_remesh_per_frame = 24;

    // Everything — block types, blocks, voxel_size — as compact bytes
    // (run-length compressed chunks), and back. save()/load() write/read a
    // file; serialize()/deserialize() are for sending a world over the
    // network or embedding it in your own save format. load/deserialize
    // replace the whole world and return false (leaving it empty) on bad data.
    // changed_only: just the chunks changed after the generator made them
    // (and all of a world without one) — for endless worlds, where the
    // generator remakes the rest. Loading marks every chunk it brings back
    // as changed, so the generator never overwrites them.
    std::vector<uint8_t> serialize(bool changed_only = false) const;
    bool deserialize(const uint8_t* data, size_t size);
    bool save(const std::string& path, bool changed_only = false) const;
    bool load(const std::string& path);
    // One chunk's blocks (chunk coordinates, i.e. block / 32) as compact
    // bytes, and back: deserialize_chunk() replaces that chunk's blocks
    // (empty bytes clear it). Ids only, so both worlds need the same block
    // types. What VoxelSync sends over the network; handy for your own
    // region-based saves too.
    std::vector<uint8_t> serialize_chunk(ivec3 chunk) const;
    bool deserialize_chunk(ivec3 chunk, const uint8_t* data, size_t size);
    std::vector<ivec3> chunks() const; // every chunk that holds at least one block

    // Imports a MagicaVoxel .vox file's blocks with their minimum corner at
    // `at` (MagicaVoxel is Z-up; it's turned to stand upright here). Each
    // palette color becomes a flat-colored block type (shared between
    // imports), glass materials become see-through and emissive ones glow.
    // Multi-model files are placed where MagicaVoxel's scene puts them.
    // Returns false if the file can't be read.
    bool load_vox(const std::string& path, ivec3 at = {0, 0, 0});

private:
    friend class World;
    friend struct detail::VoxelWorldAccess;
    std::unique_ptr<struct VoxelWorldImpl> impl_;
};

// --- noise & terrain -----------------------------------------------------------------

// Smooth pseudo-random values, for terrain, clouds, wobble, anything organic.
// Deterministic: the same inputs and seed give the same value on every
// machine, so every player of a multiplayer game generates the same world.
// (The 3D versions have their own names: with overloads, perlin(x, y, 42)
// couldn't tell a seed from a z coordinate.)
float perlin(float x, float y, uint32_t seed = 0);           // roughly -1..1, 0 at whole numbers
float perlin3(float x, float y, float z, uint32_t seed = 0); // caves, clouds, anything volumetric
// Octaves of perlin layered: big shapes plus finer and finer detail.
// Roughly -1..1. More octaves = more detail (and more cost).
float fbm(float x, float y, int octaves = 5, uint32_t seed = 0, float lacunarity = 2.0f, float gain = 0.5f);
float fbm3(float x, float y, float z, int octaves = 5, uint32_t seed = 0, float lacunarity = 2.0f, float gain = 0.5f);
// Sharp crests instead of rounded hills — mountain ranges. 0..1.
float ridged(float x, float y, int octaves = 5, uint32_t seed = 0);

// A heightmap landscape: a grid of heights, drawn as low-poly triangles
// colored by height and steepness (sand, grass, rock, snow by default),
// split into tiles so off-screen parts are culled. Walkable: add it to a
// CollisionWorld.
class Terrain {
public:
    // cells_x * cells_z squares of cell_size meters, so (cells + 1) heights per side.
    Terrain(int cells_x = 128, int cells_z = 128, float cell_size = 1.0f);
    ~Terrain();
    Terrain(Terrain&&) noexcept;
    Terrain& operator=(Terrain&&) noexcept;
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;

    vec3 origin{0.0f, 0.0f, 0.0f}; // world position of height (0, 0)
    bool flat_shaded = true;        // faceted low-poly look; false = smooth hills
    Material material;              // colors come from colorize (as vertex colors); a texture tiles in meters via uv_scale
    // Vertex color for a height (meters) and slope (0 = flat, 1 = vertical).
    // The default bands sand / grass / rock / snow between the lowest and
    // highest points. Changes apply on the next rebuild.
    std::function<rgba(float height, float slope)> colorize;

    int cells_x() const;
    int cells_z() const;
    float cell_size() const;
    float height(int x, int z) const;         // grid point; clamped at the edges
    void set_height(int x, int z, float h);
    // Fill every height from a function of world x/z (e.g. fbm noise).
    void generate(const std::function<float(float x, float z)>& height_fn);
    float height_at(float x, float z) const;  // world x/z, exactly on the drawn triangles
    vec3 normal_at(float x, float z) const;
    Bounds bounds() const;

    void rebuild(); // done automatically when drawn after an edit

private:
    friend class World;
    friend class CollisionWorld;
    std::unique_ptr<struct TerrainImpl> impl_;
};

// --- collision & characters -------------------------------------------------------
// Built in, no physics library needed: static colliders plus a character
// controller that walks on them. It handles what a first- or third-person
// game needs (walls, floors, stairs, jumping). Things that tumble and bounce
// off each other are rigid-body physics — see the optional Physics3D.

// The static stuff characters collide with: voxel worlds (read live, so
// broken blocks stop blocking immediately), triangle meshes from models,
// and plain boxes. Fill it once, or add/remove as the level changes.
class CollisionWorld {
public:
    CollisionWorld();
    ~CollisionWorld();
    CollisionWorld(CollisionWorld&&) noexcept;
    CollisionWorld& operator=(CollisionWorld&&) noexcept;
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;

    // Each add returns an id for remove(). The voxel world must outlive this.
    int add(const VoxelWorld& voxels);
    int add(Model model, const Transform& transform = {}); // its triangles, as they are now
    int add(const Terrain& terrain);                        // its triangles, as they are now
    int add_box(const Bounds& box);
    void remove(int id);
    void clear();

    bool overlaps(const Bounds& box) const;
    RaycastHit raycast(const Ray& ray, float max_distance = no_limit) const;

private:
    friend struct CharacterController;
    std::unique_ptr<struct CollisionWorldImpl> impl_;
};

// A walking character: an upright box that slides along walls, lands on
// floors, climbs small steps and jumps. `position` is the middle of its
// feet. Feed it input every frame:
//
//   player.update(level, look.move_input(f), f.key_pressed(Key::Space), f.dt);
//   camera.position = player.eye();
struct CharacterController {
    vec3 position;
    vec3 velocity;
    float radius = 0.3f;        // half the box's width (a 0.6 m wide person)
    float height = 1.8f;
    float eye_height = 1.62f;   // where eye() puts the camera
    float move_speed = 5.0f;    // m/s at full input
    float acceleration = 40.0f; // how quickly it reaches move_speed on the ground
    float air_control = 0.25f;  // fraction of that acceleration while airborne
    float gravity = 25.0f;      // m/s^2, a bit stronger than real life: jumps feel less floaty
    float jump_speed = 8.0f;    // takeoff speed; 8 with gravity 25 clears ~1.25 m
    float step_height = 0.55f;  // walk up ledges this tall without jumping (1.05 = Minecraft auto-jump)
    float max_slope = radians(50.0f); // mesh slopes steeper than this act as walls instead of ramps
    float coyote_time = 0.1f;   // can still jump this long after walking off a ledge
    float jump_buffer = 0.1f;   // a jump pressed this long before landing still happens

    // `wish`: desired horizontal direction, length 0..1 (y ignored).
    void update(const CollisionWorld& world, vec3 wish, bool jump, float dt);
    // Lower level: moves by `delta` against the world, sliding along whatever
    // it hits, and returns how far it actually got. update() is built on this.
    vec3 move(const CollisionWorld& world, vec3 delta);

    bool on_ground() const { return grounded_; }
    Bounds bounds() const { return Bounds{position - vec3{radius, 0.0f, radius}, position + vec3{radius, height, radius}}; }
    vec3 eye() const { return position + vec3{0.0f, eye_height, 0.0f}; }

private:
    bool grounded_ = false;
    float since_ground_ = 1e9f;
    float since_jump_press_ = 1e9f;
};

// --- rigid-body physics (optional, Jolt Physics) ------------------------------------
// Things that tumble, stack, bounce and get knocked over: crates, barrels,
// debris, a wrecking ball. Built on Jolt Physics, and opt-in because it's a
// big library: while it's off the engine doesn't download or compile it,
// and this API still compiles but does nothing (it logs once that it's
// off). Turn it on with `thistle enable physics3d` in a CLI project, or
// -DTHISTLE_PHYSICS3D=ON. Meters, kilograms, seconds.
//
//   Physics3D physics;
//   physics.add_box({0, -0.5f, 0}, {40, 1, 40}, BodyType::Static); // the ground
//   RigidBody crate = physics.add_box({0, 5, 0}, {1, 1, 1});      // falls onto it
//   // every frame:
//   physics.step(f.dt);
//   world.box(physics.transform(crate, {1, 1, 1}), brown);
//
// Walking characters don't need any of this (see CharacterController).

// Whether the engine was built with Physics3D. Code that must compile
// either way can also test the THISTLE_PHYSICS3D macro.
bool physics3d_available();

enum class BodyType {
    Static,    // never moves: floors, walls, the level
    Dynamic,   // moved by the simulation: falls, gets pushed, spins
    Kinematic, // moved only by you (move_kinematic), and pushes dynamic bodies out of its way: platforms, doors
};

// A body's shape. Boxes, spheres, capsules and cylinders are the cheap
// ones. convex() shrink-wraps a point cloud (a rock, a barrel, any model)
// and is still fast. mesh() is the exact triangles, for static and
// kinematic bodies only: a dynamic body given one uses its convex hull
// instead. compound() glues several together, each at its own offset.
// voxels() is a block world's solid blocks, merged into as few boxes as it
// can — debris, a voxel vehicle, a crate built from blocks.
struct Collider {
    enum class Kind { Box, Sphere, Capsule, Cylinder, Convex, Mesh, Compound, Voxels };
    Kind kind = Kind::Box;
    vec3 size{1.0f, 1.0f, 1.0f}; // Box: full size. Sphere: x = radius. Capsule, Cylinder: x = radius, y = full height. Voxels: x = voxel size
    // Convex: the cloud. Mesh: 3 per triangle, counter-clockwise from
    // outside. Voxels: each box's min and max corner.
    std::vector<vec3> points;
    std::vector<Collider> parts; // Compound
    vec3 offset;                 // where it sits within its body (or compound)
    quat rotation;

    static Collider box(vec3 size);
    static Collider sphere(float radius);
    static Collider capsule(float radius, float height); // upright; height includes the caps, like capsule_mesh()
    static Collider cylinder(float radius, float height);
    static Collider convex(std::vector<vec3> points);
    static Collider convex(const MeshData& mesh);
    static Collider convex(Model model, vec3 scale = {1.0f, 1.0f, 1.0f}); // every part, placed as in the model
    static Collider mesh(const MeshData& mesh);
    static Collider mesh(Model model, vec3 scale = {1.0f, 1.0f, 1.0f});
    static Collider compound(std::vector<Collider> parts);
    // In the grid's own space (block (0,0,0)'s corner at the body's origin),
    // so a body at voxels.origin with voxels.rotation lines up with the
    // drawn blocks. Non-solid blocks (water) are left out.
    static Collider voxels(const VoxelWorld& voxels);
    // This collider moved within its body: box({1, 2, 1}).at({0, 1, 0})
    // puts a box's bottom at the body's origin.
    Collider at(vec3 offset, quat rotation = {}) const;
};

struct BodySettings {
    Collider collider;
    BodyType type = BodyType::Dynamic;
    vec3 position;
    quat rotation;
    vec3 velocity;
    vec3 angular_velocity;         // radians per second around each axis
    float mass = 0.0f;             // kg; 0 = worked out from the collider's volume and `density`
    float density = 500.0f;        // kg per m^3: 500 is wood, water is 1000, stone about 2500
    float friction = 0.5f;         // 0 = ice
    float bounciness = 0.0f;       // 0 = lands with a thud, 1 = a superball
    float linear_damping = 0.05f;  // like air resistance
    float angular_damping = 0.05f;
    float gravity_scale = 1.0f;    // 0 = floats
    // A trigger reports what enters and leaves it (see on_trigger) but
    // doesn't block anything. It never sleeps, so it notices resting bodies.
    bool trigger = false;
    // Extra checks so small fast bodies (bullets, thrown knives) can't pass
    // through thin walls between two steps. Costs a little, so opt-in.
    bool fast = false;
    // Bodies at rest stop simulating until something touches them: that's
    // what makes thousands of settled crates cheap. Turn off for bodies you
    // steer every frame with forces.
    bool can_sleep = true;
    int layer = 0;      // 0..31; see Physics3D::set_layers_collide
    uint64_t user = 0;  // yours: an entity id, an index, a pointer...
};

// A handle to a body in a Physics3D. Stays safe to use after the body is
// removed: every call then just does nothing (or returns zeroes).
struct RigidBody {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
    explicit operator bool() const { return id != 0; }
    bool operator==(RigidBody o) const { return id == o.id; }
    bool operator!=(RigidBody o) const { return id != o.id; }
};

struct PhysicsHit : RaycastHit {
    RigidBody body;
};

// Two bodies starting to touch.
struct Contact {
    RigidBody a, b;
    vec3 point;         // where they touch
    vec3 normal;        // pointing from a toward b
    float speed = 0.0f; // how fast they were closing in, m/s: loudness for an impact sound, or damage
};

struct TriggerEvent {
    RigidBody trigger, other;
    bool entered = true; // false = left it (or was removed)
};

class Physics3D {
public:
    vec3 gravity{0.0f, -9.81f, 0.0f};

    // max_bodies is a hard limit (bodies past it aren't added); the
    // default is plenty for almost anything.
    explicit Physics3D(int max_bodies = 65536);
    ~Physics3D();
    Physics3D(Physics3D&&) noexcept;
    Physics3D& operator=(Physics3D&&) noexcept;
    Physics3D(const Physics3D&) = delete;
    Physics3D& operator=(const Physics3D&) = delete;

    RigidBody add(const BodySettings& settings); // an invalid handle if the collider couldn't be built
    RigidBody add_box(vec3 center, vec3 size, BodyType type = BodyType::Dynamic);
    RigidBody add_sphere(vec3 center, float radius, BodyType type = BodyType::Dynamic);
    // Level geometry as exact triangles (placed by `transform`, scale
    // included), as it is now. Static.
    RigidBody add_static(Model model, const Transform& transform = {});
    RigidBody add_static(const Terrain& terrain);
    // A block world, one static body per chunk, kept up to date: chunks that
    // changed are rebuilt at the start of the next step(), and bodies resting
    // there are woken (so a crate on a block you break falls). The world
    // must outlive this, or be removed first.
    void add_static(const VoxelWorld& voxels);
    void remove_static(const VoxelWorld& voxels);
    void remove(RigidBody body);
    void clear(); // every body, including block worlds added with add_static()
    bool contains(RigidBody body) const;
    int body_count() const;

    // Advances the simulation: call once per frame with f.dt. Long frames
    // are split into steps of at most 1/60 s; past 4 of those the rest is
    // dropped (the game briefly runs slow instead of freezing up).
    void step(float dt);

    // --- one body ---
    // Where to draw it: world.draw(crate_model, physics.transform(crate)).
    // `scale` goes into the result as is, for scaled shapes like world.box().
    Transform transform(RigidBody body, vec3 scale = {1.0f, 1.0f, 1.0f}) const;
    vec3 position(RigidBody body) const;
    quat rotation(RigidBody body) const;
    void set_position(RigidBody body, vec3 position); // teleports (and wakes it)
    void set_rotation(RigidBody body, quat rotation);
    // Kinematic bodies: be at this position and rotation `dt` seconds from
    // now, pushing whatever is in the way. Call before step(f.dt), with f.dt.
    void move_kinematic(RigidBody body, vec3 position, quat rotation, float dt);
    vec3 velocity(RigidBody body) const;
    void set_velocity(RigidBody body, vec3 velocity);
    vec3 angular_velocity(RigidBody body) const;
    void set_angular_velocity(RigidBody body, vec3 velocity);
    // Forces and torques last one step: call them every frame for a steady
    // push (thrusters, wind). Impulses are instant kicks: mass * change in
    // velocity. at_point off the center of mass also sets it spinning.
    void apply_force(RigidBody body, vec3 force);
    void apply_torque(RigidBody body, vec3 torque);
    void apply_impulse(RigidBody body, vec3 impulse);
    void apply_impulse(RigidBody body, vec3 impulse, vec3 at_point);
    float mass(RigidBody body) const; // 0 for static bodies
    BodyType type(RigidBody body) const;
    // Dynamic <-> Kinematic (pick something up, then drop it), or to
    // Static. A body added as Static stays static.
    void set_type(RigidBody body, BodyType type);
    bool sleeping(RigidBody body) const;
    void wake(RigidBody body);
    Bounds bounds(RigidBody body) const;
    uint64_t user(RigidBody body) const;
    // A new shape for an existing body (a piece of debris that got carved).
    // Mass follows the new volume at the body's current density.
    void set_collider(RigidBody body, const Collider& collider);

    // --- queries ---
    // Nearest body along the ray. Triggers are ignored, and so is `ignore`
    // (e.g. whoever fired).
    PhysicsHit raycast(const Ray& ray, float max_distance = no_limit, RigidBody ignore = {}) const;
    // Every body touching the sphere or box (exact shapes, not just bounds).
    std::vector<RigidBody> overlap_sphere(vec3 center, float radius) const;
    std::vector<RigidBody> overlap_box(const Bounds& box) const;
    // Every dynamic body within `radius` flies away from `center` at up to
    // `speed` m/s: full at the center, fading to nothing at the edge. The
    // same speed for a pebble and a car, so it's easy to tune. Returns how
    // many bodies it moved.
    int explode(vec3 center, float radius, float speed);

    // --- events ---
    // What happened during the last step(). A resting pair that falls
    // asleep and is woken again (by a push, a nearby explosion) reports
    // touching again, with speed near 0: filter on speed for sounds.
    const std::vector<Contact>& contacts() const;
    const std::vector<TriggerEvent>& trigger_events() const;
    // Or get called for each, at the end of every step() (on your thread,
    // so it's fine to add and remove bodies from inside).
    void on_contact(std::function<void(const Contact&)> fn);
    void on_trigger(std::function<void(const TriggerEvent&)> fn);

    // Layers 0..31. Every pair collides until told otherwise, e.g.
    // set_layers_collide(DEBRIS, PLAYER, false). Triggers go by layers
    // too: one only notices bodies on layers it collides with.
    void set_layers_collide(int layer_a, int layer_b, bool collide);
    bool layers_collide(int layer_a, int layer_b) const;

    // Wireframes of every collider: green awake, gray asleep, blue static,
    // yellow triggers. Hidden behind solid geometry unless on_top.
    void draw_debug(World& world, bool on_top = false) const;

private:
    std::unique_ptr<struct Physics3DImpl> impl_;
};

// --- destruction (Teardown-style) ---------------------------------------------------------
// Blow holes in a block world, and whatever that leaves hanging breaks off
// and falls as debris made of the same blocks, which can itself be blown
// apart again. Needs Physics3D for anything to fall: built without it,
// carving still works and loose parts just stay where they are.
//
//   VoxelDestruction boom(level, physics);
//   // every frame:
//   if (f.mouse_pressed(Mouse::Left)) boom.explode(hit.point, 1.5f);
//   physics.step(f.dt);
//   boom.update(f.dt);
//   boom.draw(world); // the level, the debris, and the flying chips
class VoxelDestruction {
public:
    // What holds things up: blocks at or below ground_y (block
    // coordinates), and blocks of any type listed in `anchors` (bedrock, a
    // hook in the ceiling). A group of blocks not connected face to face,
    // through solid blocks, to either of those falls.
    int ground_y = 0;
    std::vector<BlockId> anchors;
    // A loose group bigger than this counts as held up anyway. It keeps the
    // search cheap in a big world, and a whole hillside sliding off is
    // rarely what a level wants.
    int max_piece = 20000;
    int min_piece = 4;      // smaller loose groups crumble into chips instead of becoming debris
    int max_debris = 300;   // past this many pieces, the oldest crumble away
    float density = 800.0f; // kg per m^3 of debris
    ParticleSystem chips;   // block-colored bits that fly off; set chips.max_particles = 0 for none

    // Also makes `voxels` solid to `physics` (Physics3D::add_static), so
    // debris lands on it. Both must outlive this.
    VoxelDestruction(VoxelWorld& voxels, Physics3D& physics);
    ~VoxelDestruction();
    VoxelDestruction(const VoxelDestruction&) = delete;
    VoxelDestruction& operator=(const VoxelDestruction&) = delete;

    // Removes every block within `radius` of `center` (world units), from
    // the world and from debris, then lets go of whatever that left
    // unsupported. Returns how many blocks were removed.
    int carve(vec3 center, float radius);
    // carve(), plus bodies within twice the radius (debris included) flying
    // outward at up to `speed` m/s, and more chips.
    int explode(vec3 center, float radius, float speed = 12.0f);

    // Once per frame, after physics.step(): moves the chips, and removes
    // debris that fell out of the world.
    void update(float dt);
    void draw(World& world); // the world, every piece of debris, and the chips

    int debris_count() const;
    RigidBody debris_body(int index) const; // e.g. to check what just hit the player
    const VoxelWorld& debris_blocks(int index) const;
    bool is_debris(RigidBody body) const;

private:
    std::unique_ptr<struct VoxelDestructionImpl> impl_;
};

// --- multiplayer -----------------------------------------------------------------------------
// On top of the realtime networking layer (NetServer / NetClient / NetObject,
// see docs/networking.md), two things a multiplayer 3D game needs.

// Keeps a VoxelWorld the same on the server and on every client. A client
// that joins gets the whole world, streamed a few chunks per update so the
// server never stalls. After that, any change to the server's world (set,
// fill, carving, a VoxelDestruction blast) reaches every client as the
// changed chunks. Clients ask for edits with request_set(); the server has
// the last word.
//
//   // server and every client, before listen()/connect():
//   VoxelSync sync(world);
//   // server, after listen():
//   sync.host();
//   // every frame, after server.update() / client.update():
//   sync.update();
//   // client, when the player clicks:
//   sync.request_set(target, stone);
class VoxelSync {
public:
    // One per synced world. `name` tells worlds apart when there are several;
    // it must match on the server and the clients.
    explicit VoxelSync(VoxelWorld& world, const std::string& name = "world");
    ~VoxelSync();
    VoxelSync(const VoxelSync&) = delete;
    VoxelSync& operator=(const VoxelSync&) = delete;

    void host();   // server: start sharing the world (after NetServer::listen())
    void update(); // every frame, both sides

    // Client: change a block. Shown here right away; the server applies it
    // for everyone, or refuses (allow_edit) and this client's chunk is put back.
    void request_set(ivec3 block, BlockId id);
    // Server: return false to refuse a client's edit (out of reach, a
    // protected area, not their turn). Everything is allowed by default.
    std::function<bool(int conn_id, ivec3 block, BlockId id)> allow_edit;

    bool ready() const;     // client: the whole world has arrived (the server: always)
    float progress() const; // client: 0..1 while it streams in
    int stream_bytes_per_update = 64 * 1024; // server: per joining client

private:
    std::unique_ptr<struct VoxelSyncImpl> impl_;
    friend struct VoxelSyncAccess;
};

// Smooths movement that arrives from the network in steps (other players,
// synced objects): add() each new position as it arrives, and sample() a
// smoothly moving one a little in the past. The delay trades lag for
// smoothness: at least one network update's worth, plus a bit for jitter.
class TransformInterpolator {
public:
    float delay = 0.1f; // seconds

    void add(const Transform& transform, double time); // time: when it arrived (Frame::time)
    Transform sample(double time) const;               // at time - delay; holds the last value past the newest
    bool empty() const { return samples_.empty(); }
    void clear() { samples_.clear(); }

private:
    struct Sample {
        double time;
        Transform transform;
    };
    std::vector<Sample> samples_;
};

// --- scenes (what the Thistle Editor saves) ------------------------------------------------------
// A level: named things, each with a transform relative to its parent, that
// are a model, a built-in shape, a block object (voxels), a light, a trigger
// volume, a spawn point, or an empty used as a group. Plus the environment
// (sun, sky, fog). The
// Thistle Editor saves these as .scene.json; a game loads one and either
// draws it as is, or reads it to place its own things: that's what spawn
// points, triggers and each entity's free-form properties are for.
//
//   Scene3D level;
//   level.load("assets/scenes/level1.scene.json");
//   int start = level.find("player_start");
//   player.position = level.world_transform(start).position;
//   // every frame:
//   level.draw(world);
struct SceneEntity {
    enum class Kind { Empty, Model, Box, Sphere, Cylinder, Cone, Plane, PointLight, SpotLight, Trigger, Spawn, Voxels };
    std::string name;
    Kind kind = Kind::Empty;
    int parent = -1;       // index into Scene3D::entities, -1 = at the top
    Transform transform;   // relative to the parent
    std::string model;     // Kind::Model: its file, as load_model() takes it
    rgba color = white;    // shapes: their color; models: a tint; lights: the light's color
    float intensity = 1.0f; // lights
    float range = 10.0f;    // lights
    float spot_angle = radians(30.0f); // spot lights: half the cone's opening
    // Anything the game needs to know: {"health", "100"}, {"door", "exit"}.
    std::vector<std::pair<std::string, std::string>> properties;
    // Kind::Voxels: its blocks. The grid's origin (block 0,0,0's corner) is
    // the entity's position and it turns with its rotation; scale doesn't
    // apply (the block size is voxels->voxel_size). Copies of the entity
    // share this world: copy() it for a separate one. Hand it to Physics3D,
    // VoxelDestruction or VoxelSync like any other VoxelWorld.
    std::shared_ptr<VoxelWorld> voxels;
    // Kind::Voxels with textured blocks: the atlas image, and its tile size in pixels.
    std::string atlas;
    int atlas_tile = 16;

    std::string property(const std::string& key, const std::string& fallback = {}) const;
    void set_property(const std::string& key, const std::string& value);
};

class Scene3D {
public:
    std::vector<SceneEntity> entities;
    Sun sun;
    Sky sky;
    Fog fog;
    float ambient = 0.55f;

    // The file's text, and back. Paths inside stay as they were written.
    // Voxel entities' blocks go in the file too (compressed, base64).
    bool save(const std::string& path) const;
    bool load(const std::string& path); // false (and an empty scene) if it can't be read
    // voxel_blocks = false leaves the blocks out: quick, for telling whether
    // anything else changed (the editor's undo). Such text doesn't load back
    // with its blocks.
    std::string to_json(bool voxel_blocks = true) const;
    bool from_json(const std::string& text);

    int add(const SceneEntity& entity);  // returns its index
    void remove(int index);              // and everything under it; later indices shift down
    void remove(const std::vector<int>& indices); // several at once (removing them one by one would shift the later ones)
    // Moves `child` under `parent` (-1: the top), keeping where it is in the
    // world. Refused (false) if that would put it under itself.
    bool set_parent(int child, int parent);
    std::vector<int> children(int index) const; // -1: the top-level ones
    int find(const std::string& name) const;    // -1 if none

    Transform world_transform(int index) const;
    void set_world_transform(int index, const Transform& world);
    // Its own box, before its transform: the model's, a unit shape's, the
    // blocks' (voxels), or a small one for lights, spawns and empties.
    Bounds local_bounds(int index) const;
    // Whether a point in the world is inside its box (local_bounds() placed by
    // world_transform(), rotation included): "is the player in this trigger".
    bool inside(int index, vec3 point) const;
    Model model(int index) const; // the loaded model of a Kind::Model entity (loaded once per file, shared)
    // Puts every voxel entity's world where its transform says (origin and
    // rotation, through its parents). load() and draw() do this; call it
    // yourself after moving one, before raycasting it or adding it to physics.
    void place_voxels();
    // Makes the level solid for CharacterController: shapes and models by
    // their triangles (turned and scaled as placed), block objects as their
    // live voxel worlds. Lights, triggers, spawns and empties aren't solid,
    // nor is anything with the property solid = "false" (decoration). Block
    // objects collide as if unturned (the built-in collision takes grids as
    // axis-aligned; a turned one logs a warning). Returns the ids added.
    // The scene's voxel worlds must outlive `world`.
    std::vector<int> add_colliders(CollisionWorld& world) const;

    // Draws every model, shape and block object and adds every light to
    // `world`; with `environment`, also sets its sun, sky, fog and ambient.
    // Triggers, spawns and empties don't draw (they're for the game to read).
    void draw(World& world, bool environment = true) const;
};

} // namespace thistle::three

// --- entry point ---------------------------------------------------------
// Everywhere except Android, a Thistle game is a plain `int main()` that ends
// with `return app.run();` — write it however you like, these macros are
// optional there.
//
// Android has no process main(): the OS loads your game as a shared library
// and jumps straight into sokol_app.h's own ANativeActivity_onCreate, which
// expects a sokol_main() it can call to get set up — not a function that
// blocks running a game loop. THISTLE_MAIN/THISTLE_RUN hide that difference:
// wrap your entry point in THISTLE_MAIN { ... THISTLE_RUN(app); } and the
// exact same source compiles unchanged on every platform, Android included.
#if defined(__ANDROID__)
    #define THISTLE_MAIN extern "C" void thistle_user_main()
    #define THISTLE_RUN(app) do { (app).run(); return; } while (0)
#else
    #define THISTLE_MAIN int main()
    #define THISTLE_RUN(app) return (app).run()
#endif
