#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

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

// --- ui -----------------------------------------------------------------

struct ButtonStyle {
    rgba bg       {0.16f, 0.17f, 0.24f, 1.0f};
    rgba bg_hover {0.24f, 0.26f, 0.36f, 1.0f};
    rgba bg_press = coral;
    rgba text     = white;
    float text_size = 28.0f;
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

// Frees a texture's GPU image + any pending pixels and invalidates the handle.
// Call when unloading a level so textures don't leak.
void unload_texture(Texture& tex);

// Re-reads a texture from its original file and re-uploads it (hot reload).
void reload_texture(Texture tex);

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

// --- input --------------------------------------------------------------

// Values match sokol/GLFW keycodes so no lookup table is needed internally.
enum class Key {
    Space = 32,
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259,
    Right = 262, Left = 263, Down = 264, Up = 265,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342,
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

    // Vector shapes (filled/outline), for debug draws, HUDs, effects.
    void line(vec2 a, vec2 b, rgba color, float thickness = 2.0f);
    void triangle(vec2 a, vec2 b, vec2 c, rgba color);
    void circle(vec2 center, float radius, rgba color, int segments = 28);
    void circle_outline(vec2 center, float radius, rgba color, float thickness = 2.0f, int segments = 28);

    // Draw a texture. The first overload uses the image's native size.
    void sprite(Texture tex, vec2 pos);
    void sprite(Texture tex, vec2 pos, SpriteOpts opts);

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

    // A flat, horizontal (XZ) ground/wall-sized quad centered at `center`.
    void plane3d(vec3 center, float width, float depth, rgba color);

    // A 3D line (1px, GPU line width — thickness is best-effort/platform-dependent).
    void line3d(vec3 a, vec3 b, rgba color);

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
    bool mouse_down(Mouse b) const;
    bool mouse_pressed(Mouse b) const;

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
// starts capturing typed characters (printable ASCII; Backspace edits); read the
// buffer with text_input() each frame; end_text_input() hides it. Use for naming
// a level, a login field, etc.
void begin_text_input(const std::string& initial = "");
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

// A node in a transform hierarchy. Origin is the node's center; children inherit
// position, rotation, scale, and alpha. Optionally draws one sprite.
class Node {
public:
    vec2 pos{0, 0};
    vec2 scale{1, 1};
    float rotation = 0.0f; // radians
    float alpha = 1.0f;    // multiplied down the tree
    bool visible = true;

    // Optional sprite drawn centered on the node's origin.
    Texture sprite{};
    vec2 sprite_size{0, 0};   // {0,0} = texture's native size
    rgba sprite_tint = white;
    Rect sprite_src{{0, 0}, {0, 0}};

    Node* add_child(std::unique_ptr<Node> c) {
        c->parent_ = this;
        children_.push_back(std::move(c));
        return children_.back().get();
    }
    Node* add_child() { return add_child(std::make_unique<Node>()); }
    std::size_t child_count() const { return children_.size(); }
    Node* child(std::size_t i) const { return children_[i].get(); }
    Node* parent() const { return parent_; }

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

private:
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    std::vector<Action> actions_;
    Node& push(const Action& a) { actions_.push_back(a); return *this; }
    void draw_rec(Frame& f, float inherited_alpha);
};

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
