#include <thistle.hpp>

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_gl.h"

#include "stb_image.h"
#include "miniaudio.h"
#include "fontstash.h"
#include "sokol_fontstash.h"

#include "thistle_gamepad.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <box2d/box2d.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
extern "C" void thistle_ios_set_working_dir(void);
extern "C" void thistle_ios_init_audio_session(void);
extern "C" const char* thistle_apple_writable_dir(void);
extern "C" const char* thistle_apple_device_name(void);
extern "C" void thistle_apple_set_clipboard(const char* text);
extern "C" void thistle_apple_haptic(int style);
extern "C" void* thistle_http_start(const char* method, const char* url, const char* body);
extern "C" int   thistle_http_poll(void* h, int* status, const char** body, int* len);
extern "C" void  thistle_http_free(void* h);
#elif defined(_WIN32)
// Real async HTTP on Windows too (see src/win32_support.cpp) — same poll-based
// interface as the Apple side, just backed by WinHTTP instead of NSURLSession.
extern "C" void* thistle_http_start(const char* method, const char* url, const char* body);
extern "C" int   thistle_http_poll(void* h, int* status, const char** body, int* len);
extern "C" void  thistle_http_free(void* h);
#endif

namespace thistle {
namespace {

struct TextureRecord {
    unsigned char* pixels = nullptr; // CPU copy, freed after GPU upload
    int w = 0;
    int h = 0;
    sg_image img = {};
    sg_view view = {};
    bool uploaded = false;
    std::string path; // original file, for reload_texture
};

struct EngineState {
    AppConfig config;
    std::function<void()> on_start;
    std::function<void(Frame)> on_update;
    std::function<void()> on_stop;

    std::unordered_map<std::string, Scene> scenes;
    std::string current_scene;
    std::string pending_scene;
    bool has_pending = false;
    rgba clear_color = midnight;
    double elapsed = 0.0;

    std::vector<TextureRecord> textures;
    sg_sampler sampler = {};
    sgl_pipeline pip = {};          // alpha-blended pipeline for 2D
    sgl_pipeline pip_additive = {}; // additive-blend pipeline (Blend::Additive)
    sgl_pipeline pip_3d = {};       // depth-tested pipeline for camera3d()/cube()/etc.
    sg_image white_img = {};        // 1x1 white texel so rects and sprites share one path
    sg_view white_view = {};

    ma_engine audio = {};
    bool audio_ready = false;
    ma_sound_group sfx_group = {};
    bool sfx_group_ready = false;
    ma_sound music = {};
    bool music_ready = false;

    FONScontext* fons = nullptr;
    std::vector<std::string> font_paths;
    std::vector<int> font_ids;        // fontstash ids, parallel to font_paths
    int default_font = FONS_INVALID;

    // Post-processing / render-to-texture.
    PostEffect post_effect = PostEffect::None;
    float post_intensity = 1.0f;
    sg_image off_color = {};
    sg_image off_depth = {};
    sg_view off_color_att = {};
    sg_view off_depth_att = {};
    sg_view off_tex = {};
    sg_sampler post_sampler = {};
    sg_shader post_shader = {};
    sg_pipeline post_pip = {};
    int off_w = 0, off_h = 0;
    bool post_pipeline_ready = false;

    bool key_held[512] = {};
    bool key_pressed[512] = {};
    bool mouse_held[3] = {};
    bool mouse_pressed[3] = {};
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;

    bool text_capturing = false;   // on-screen keyboard active, accumulating chars
    std::string text_buffer;
    std::size_t text_max = 40;

    ThistleGamepad pad_cur{};
    ThistleGamepad pad_prev{};

    int menu_focus = 0;      // gamepad-focused menu item
    int menu_last_count = 0; // items in the last menu built

    // Debug overlay state (only used when THISTLE_DEBUG is defined).
    vec2 dbg_btn{20.0f, 120.0f};
    bool dbg_open = false;
    bool dbg_drag = false;
    vec2 dbg_drag_off{0.0f, 0.0f};
    float dbg_moved = 0.0f;
};

// Captured log lines (also echoed to the console). Shared by the debug overlay.
std::vector<std::string> g_logs;

// Single app per process; sokol's callbacks are plain C function pointers so we
// route them through this pointer.
EngineState* g_state = nullptr;

void ensure_uploaded(TextureRecord& rec) {
    if (rec.uploaded || rec.pixels == nullptr) return;
    sg_image_desc desc = {};
    desc.width = rec.w;
    desc.height = rec.h;
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    desc.data.mip_levels[0].ptr = rec.pixels;
    desc.data.mip_levels[0].size = static_cast<size_t>(rec.w) * rec.h * 4;
    rec.img = sg_make_image(&desc);

    sg_view_desc vdesc = {};
    vdesc.texture.image = rec.img;
    rec.view = sg_make_view(&vdesc);

    stbi_image_free(rec.pixels);
    rec.pixels = nullptr;
    rec.uploaded = true;
}

void init_cb() {
#if defined(__APPLE__)
    thistle_ios_set_working_dir(); // no-op on macOS
#endif

    sg_desc gfx = {};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    sgl_desc_t gl = {};
    gl.logger.func = slog_func;
    sgl_setup(&gl);

    sg_sampler_desc smp = {};
    smp.min_filter = SG_FILTER_LINEAR;
    smp.mag_filter = SG_FILTER_LINEAR;
    smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    g_state->sampler = sg_make_sampler(&smp);

    sg_pipeline_desc pip = {};
    pip.colors[0].blend.enabled = true;
    pip.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pip.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pip.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pip.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    g_state->pip = sgl_make_pipeline(&pip);

    sg_pipeline_desc apip = {};
    apip.colors[0].blend.enabled = true;
    apip.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    apip.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE; // additive
    apip.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    apip.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    g_state->pip_additive = sgl_make_pipeline(&apip);

    // Depth-tested pipeline for the minor-3D drawing calls (camera3d/cube/
    // plane3d/line3d). Uses the swapchain's own depth buffer, which sokol_app
    // provisions by default — no offscreen pass needed.
    sg_pipeline_desc pip3 = {};
    pip3.colors[0].blend.enabled = true;
    pip3.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pip3.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pip3.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pip3.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pip3.depth.pixel_format = sglue_environment().defaults.depth_format;
    pip3.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    pip3.depth.write_enabled = true;
    g_state->pip_3d = sgl_make_pipeline(&pip3);

    // 1x1 white texture: rects draw it tinted, so rects and sprites share one
    // texture-enabled path and sokol_gl can batch consecutive draws.
    unsigned char white_px[4] = {255, 255, 255, 255};
    sg_image_desc wdesc = {};
    wdesc.width = 1;
    wdesc.height = 1;
    wdesc.pixel_format = SG_PIXELFORMAT_RGBA8;
    wdesc.data.mip_levels[0].ptr = white_px;
    wdesc.data.mip_levels[0].size = sizeof(white_px);
    g_state->white_img = sg_make_image(&wdesc);
    sg_view_desc wv = {};
    wv.texture.image = g_state->white_img;
    g_state->white_view = sg_make_view(&wv);

#if defined(__APPLE__)
    thistle_ios_init_audio_session(); // no-op on macOS
#endif
    if (ma_engine_init(nullptr, &g_state->audio) == MA_SUCCESS) {
        g_state->audio_ready = true;
        if (ma_sound_group_init(&g_state->audio, 0, nullptr, &g_state->sfx_group) == MA_SUCCESS) {
            g_state->sfx_group_ready = true;
        }
    }

    sfons_desc_t fdesc = {};
    fdesc.width = 1024;
    fdesc.height = 1024;
    g_state->fons = sfons_create(&fdesc);
    // Self-heal when the glyph atlas fills up (e.g., a game that draws text at
    // many different pixel sizes). Without a handler fontstash silently drops
    // every new glyph *forever*; resetting the atlas re-caches on demand so text
    // recovers instead of breaking permanently.
    fonsSetErrorCallback(g_state->fons, [](void* uptr, int error, int) {
        if (error == FONS_ATLAS_FULL) {
            FONScontext* ctx = static_cast<FONScontext*>(uptr);
            int w = 1024, h = 1024;
            fonsGetAtlasSize(ctx, &w, &h);
            fonsResetAtlas(ctx, w, h);
        }
    }, g_state->fons);

    // Load any fonts requested before the window existed.
    for (size_t i = 0; i < g_state->font_paths.size(); ++i) {
        if (g_state->font_ids[i] == FONS_INVALID) {
            const int fid = fonsAddFont(g_state->fons, "font", g_state->font_paths[i].c_str());
            g_state->font_ids[i] = fid;
            if (fid != FONS_INVALID && g_state->default_font == FONS_INVALID) {
                g_state->default_font = fid;
            }
        }
    }

    if (g_state->on_start) g_state->on_start();
}

// --- post-processing helpers -------------------------------------------

#if defined(__APPLE__)
const char* POST_MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct vs_out { float4 pos [[position]]; float2 uv; };
vertex vs_out vs_main(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    vs_out o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
struct Params { float mode; float intensity; float time; float pad; };
fragment float4 fs_main(vs_out in [[stage_in]],
                        texture2d<float> tex [[texture(0)]],
                        sampler smp [[sampler(0)]],
                        constant Params& p [[buffer(0)]]) {
    float4 c = tex.sample(smp, in.uv);
    int mode = int(p.mode);
    float k = p.intensity;
    if (mode == 1) { float g = dot(c.rgb, float3(0.299, 0.587, 0.114)); c.rgb = mix(c.rgb, float3(g), k); }
    else if (mode == 2) { float2 d = in.uv - 0.5; float v = 1.0 - dot(d, d) * k * 3.0; c.rgb *= clamp(v, 0.0, 1.0); }
    else if (mode == 3) { float o = 0.004 * k; c.r = tex.sample(smp, in.uv + float2(o, 0)).r; c.b = tex.sample(smp, in.uv - float2(o, 0)).b; }
    else if (mode == 4) { c.rgb = mix(c.rgb, float3(1.0), k); }
    else if (mode == 5) { c.rgb *= (1.0 - k); }
    return c;
}
)MSL";
#endif

void destroy_offscreen(EngineState& s) {
    if (s.off_tex.id != SG_INVALID_ID) { sg_destroy_view(s.off_tex); s.off_tex = {}; }
    if (s.off_color_att.id != SG_INVALID_ID) { sg_destroy_view(s.off_color_att); s.off_color_att = {}; }
    if (s.off_depth_att.id != SG_INVALID_ID) { sg_destroy_view(s.off_depth_att); s.off_depth_att = {}; }
    if (s.off_color.id != SG_INVALID_ID) { sg_destroy_image(s.off_color); s.off_color = {}; }
    if (s.off_depth.id != SG_INVALID_ID) { sg_destroy_image(s.off_depth); s.off_depth = {}; }
    s.off_w = 0;
    s.off_h = 0;
}

// Lazily creates the post pipeline + (re)sizes the offscreen target. Returns
// true when everything is ready to render through. Metal only for now.
bool ensure_post(EngineState& s, int w, int h) {
#if defined(__APPLE__)
    const sg_environment env = sglue_environment();
    if (!s.post_pipeline_ready) {
        sg_sampler_desc smp = {};
        smp.min_filter = SG_FILTER_LINEAR;
        smp.mag_filter = SG_FILTER_LINEAR;
        smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        s.post_sampler = sg_make_sampler(&smp);

        sg_shader_desc sd = {};
        sd.vertex_func.source = POST_MSL;   sd.vertex_func.entry = "vs_main";
        sd.fragment_func.source = POST_MSL; sd.fragment_func.entry = "fs_main";
        sd.uniform_blocks[0].stage = SG_SHADERSTAGE_FRAGMENT;
        sd.uniform_blocks[0].size = 16;
        sd.uniform_blocks[0].msl_buffer_n = 0;
        sd.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
        sd.views[0].texture.image_type = SG_IMAGETYPE_2D;
        sd.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
        sd.views[0].texture.msl_texture_n = 0;
        sd.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
        sd.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
        sd.samplers[0].msl_sampler_n = 0;
        sd.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
        sd.texture_sampler_pairs[0].view_slot = 0;
        sd.texture_sampler_pairs[0].sampler_slot = 0;
        s.post_shader = sg_make_shader(&sd);

        sg_pipeline_desc pd = {};
        pd.shader = s.post_shader;
        pd.colors[0].pixel_format = env.defaults.color_format;
        pd.depth.pixel_format = env.defaults.depth_format;
        pd.depth.write_enabled = false;
        pd.sample_count = env.defaults.sample_count;
        pd.cull_mode = SG_CULLMODE_NONE;
        s.post_pip = sg_make_pipeline(&pd);
        s.post_pipeline_ready = true;
    }
    if (s.off_w != w || s.off_h != h) {
        destroy_offscreen(s);
        sg_image_desc cd = {};
        cd.usage.color_attachment = true;
        cd.width = w; cd.height = h;
        cd.pixel_format = env.defaults.color_format;
        cd.sample_count = env.defaults.sample_count;
        s.off_color = sg_make_image(&cd);

        sg_image_desc dd = {};
        dd.usage.depth_stencil_attachment = true;
        dd.width = w; dd.height = h;
        dd.pixel_format = env.defaults.depth_format;
        dd.sample_count = env.defaults.sample_count;
        s.off_depth = sg_make_image(&dd);

        sg_view_desc cv = {}; cv.color_attachment.image = s.off_color;
        s.off_color_att = sg_make_view(&cv);
        sg_view_desc dv = {}; dv.depth_stencil_attachment.image = s.off_depth;
        s.off_depth_att = sg_make_view(&dv);
        sg_view_desc tv = {}; tv.texture.image = s.off_color;
        s.off_tex = sg_make_view(&tv);
        s.off_w = w;
        s.off_h = h;
    }
    return true;
#else
    (void)s; (void)w; (void)h;
    return false;
#endif
}

#ifdef THISTLE_DEBUG
void draw_debug(Frame& f) {
    EngineState& s = *g_state;
    f.camera({0.0f, 0.0f}); // the debug UI ignores any game camera/shake

    const vec2 m = f.mouse();
    const Rect fab{s.dbg_btn, {64.0f, 44.0f}};
    const bool down = f.mouse_down(Mouse::Left);
    const bool pressed = f.mouse_pressed(Mouse::Left);

    if (pressed && fab.contains(m)) {
        s.dbg_drag = true;
        s.dbg_drag_off = {m.x - s.dbg_btn.x, m.y - s.dbg_btn.y};
        s.dbg_moved = 0.0f;
    }
    if (s.dbg_drag) {
        if (down) {
            const vec2 np{m.x - s.dbg_drag_off.x, m.y - s.dbg_drag_off.y};
            s.dbg_moved += std::abs(np.x - s.dbg_btn.x) + std::abs(np.y - s.dbg_btn.y);
            s.dbg_btn = np;
        } else {
            if (s.dbg_moved < 8.0f) s.dbg_open = !s.dbg_open; // a tap (not a drag) toggles
            s.dbg_drag = false;
        }
    }

    f.rect(s.dbg_btn, {64.0f, 44.0f}, rgba{0.0f, 0.0f, 0.0f, 0.65f});
    f.text("LOG", {s.dbg_btn.x + 12.0f, s.dbg_btn.y + 12.0f}, {.size = 20.0f, .color = coral});

    if (!s.dbg_open) return;

    const float W = static_cast<float>(f.width);
    const float H = static_cast<float>(f.height);
    f.rect({0.0f, 0.0f}, {W, H}, rgba{0.02f, 0.02f, 0.05f, 0.93f});
    f.text("LOGS (" + std::to_string(g_logs.size()) + ")", {24.0f, 24.0f}, {.size = 26.0f, .color = white});

    const float lh = 20.0f;
    const int fit = std::max(1, static_cast<int>((H - 150.0f) / lh));
    int start = static_cast<int>(g_logs.size()) - fit;
    if (start < 0) start = 0;
    float y = 66.0f;
    for (int i = start; i < static_cast<int>(g_logs.size()); ++i) {
        f.text(g_logs[static_cast<size_t>(i)], {24.0f, y}, {.size = 15.0f, .color = rgb(0.80f, 0.82f, 0.92f)});
        y += lh;
    }

    if (f.button("COPY ALL", {{W - 360.0f, H - 62.0f}, {170.0f, 46.0f}})) {
        std::string all;
        for (const std::string& line : g_logs) { all += line; all += '\n'; }
        set_clipboard(all);
    }
    if (f.button("CLOSE", {{W - 176.0f, H - 62.0f}, {150.0f, 46.0f}})) {
        s.dbg_open = false;
    }
}
#endif

void frame_cb() {
    const int w = sapp_width();
    const int h = sapp_height();
    const double dt = sapp_frame_duration();
    g_state->elapsed += dt;

    // Poll the gamepad (previous frame kept for edge detection).
    g_state->pad_prev = g_state->pad_cur;
#if defined(__APPLE__)
    thistle_apple_poll_gamepad(&g_state->pad_cur);
#else
    g_state->pad_cur = ThistleGamepad{};
#endif

    // Pixel-space, top-left origin, y down; alpha blending on.
    sgl_defaults();
    sgl_load_pipeline(g_state->pip);
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    sgl_enable_texture(); // stays on all frame; rects use the 1x1 white texture

    Frame f;
    f.dt = static_cast<float>(dt);
    f.time = g_state->elapsed;
    f.width = w;
    f.height = h;

    // Apply a pending scene switch (exit old, enter new) before updating.
    if (g_state->has_pending) {
        auto next = g_state->scenes.find(g_state->pending_scene);
        if (next != g_state->scenes.end()) {
            if (!g_state->current_scene.empty()) {
                auto cur = g_state->scenes.find(g_state->current_scene);
                if (cur != g_state->scenes.end() && cur->second.exit) cur->second.exit();
            }
            g_state->current_scene = g_state->pending_scene;
            if (next->second.enter) next->second.enter();
        }
        g_state->has_pending = false;
    }

    if (!g_state->current_scene.empty()) {
        auto cur = g_state->scenes.find(g_state->current_scene);
        if (cur != g_state->scenes.end() && cur->second.update) cur->second.update(f);
    } else if (g_state->on_update) {
        g_state->on_update(f);
    }

#ifdef THISTLE_DEBUG
    draw_debug(f); // draws on top of whatever the scene rendered
#endif

    const sg_color clear = {
        g_state->clear_color.r, g_state->clear_color.g,
        g_state->clear_color.b, g_state->clear_color.a};

#if defined(__APPLE__)
    const bool use_post = g_state->post_effect != PostEffect::None && ensure_post(*g_state, w, h);
#else
    const bool use_post = false;
#endif

    if (g_state->fons) sfons_flush(g_state->fons); // upload any newly rasterized glyphs

    if (use_post) {
        // Pass 1: render the scene into the offscreen texture.
        sg_pass off = {};
        off.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        off.action.colors[0].clear_value = clear;
        off.attachments.colors[0] = g_state->off_color_att;
        off.attachments.depth_stencil = g_state->off_depth_att;
        sg_begin_pass(&off);
        sgl_draw();
        sg_end_pass();

        // Pass 2: composite it to the screen through the post-effect shader.
        sg_pass sc = {};
        sc.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
        sc.swapchain = sglue_swapchain();
        sg_begin_pass(&sc);
        sg_apply_pipeline(g_state->post_pip);
        sg_bindings bind = {};
        bind.views[0] = g_state->off_tex;
        bind.samplers[0] = g_state->post_sampler;
        sg_apply_bindings(&bind);
        struct { float mode, intensity, time, pad; } params = {
            static_cast<float>(static_cast<int>(g_state->post_effect)),
            g_state->post_intensity,
            static_cast<float>(g_state->elapsed),
            0.0f};
        sg_range ur = {&params, sizeof(params)};
        sg_apply_uniforms(0, &ur);
        sg_draw(0, 3, 1);
        sg_end_pass();
        sg_commit();
    } else {
        sg_pass pass = {};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].clear_value = clear;
        pass.swapchain = sglue_swapchain();
        sg_begin_pass(&pass);
        sgl_draw();
        sg_end_pass();
        sg_commit();
    }

    // Edge-triggered input is only true for the frame it happened.
    for (bool& p : g_state->key_pressed) p = false;
    for (bool& p : g_state->mouse_pressed) p = false;
}

void cleanup_cb() {
    if (g_state->on_stop) g_state->on_stop();
    for (TextureRecord& rec : g_state->textures) {
        if (rec.pixels) stbi_image_free(rec.pixels);
        if (rec.view.id != SG_INVALID_ID) sg_destroy_view(rec.view);
        if (rec.img.id != SG_INVALID_ID) sg_destroy_image(rec.img);
    }
    if (g_state->white_view.id != SG_INVALID_ID) sg_destroy_view(g_state->white_view);
    if (g_state->white_img.id != SG_INVALID_ID) sg_destroy_image(g_state->white_img);
    destroy_offscreen(*g_state);
    if (g_state->post_pip.id != SG_INVALID_ID) sg_destroy_pipeline(g_state->post_pip);
    if (g_state->post_shader.id != SG_INVALID_ID) sg_destroy_shader(g_state->post_shader);
    if (g_state->post_sampler.id != SG_INVALID_ID) sg_destroy_sampler(g_state->post_sampler);
    if (g_state->music_ready) ma_sound_uninit(&g_state->music);
    if (g_state->sfx_group_ready) ma_sound_group_uninit(&g_state->sfx_group);
    if (g_state->audio_ready) ma_engine_uninit(&g_state->audio);
    if (g_state->fons) sfons_destroy(g_state->fons);
    sgl_shutdown();
    sg_shutdown();
}

void event_cb(const sapp_event* e) {
    switch (e->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
            if (e->key_code >= 0 && e->key_code < 512) {
                g_state->key_held[e->key_code] = true;
                if (!e->key_repeat) g_state->key_pressed[e->key_code] = true;
            }
            if (g_state->text_capturing && e->key_code == SAPP_KEYCODE_BACKSPACE && !g_state->text_buffer.empty())
                g_state->text_buffer.pop_back();
            break;
        case SAPP_EVENTTYPE_CHAR:
            // Accumulate typed text while a field is capturing (printable ASCII).
            if (g_state->text_capturing && e->char_code >= 32 && e->char_code < 127 &&
                g_state->text_buffer.size() < g_state->text_max)
                g_state->text_buffer.push_back(static_cast<char>(e->char_code));
            break;
        case SAPP_EVENTTYPE_KEY_UP:
            if (e->key_code >= 0 && e->key_code < 512) {
                g_state->key_held[e->key_code] = false;
            }
            break;
        case SAPP_EVENTTYPE_MOUSE_DOWN:
            if (e->mouse_button >= 0 && e->mouse_button < 3) {
                g_state->mouse_held[e->mouse_button] = true;
                g_state->mouse_pressed[e->mouse_button] = true;
            }
            break;
        case SAPP_EVENTTYPE_MOUSE_UP:
            if (e->mouse_button >= 0 && e->mouse_button < 3) {
                g_state->mouse_held[e->mouse_button] = false;
            }
            break;
        case SAPP_EVENTTYPE_MOUSE_MOVE:
            g_state->mouse_x = e->mouse_x;
            g_state->mouse_y = e->mouse_y;
            break;

        // Touch maps onto the primary pointer (mouse button 0) so games written
        // with mouse_pressed/mouse() work unchanged on phones: a tap is a click.
        case SAPP_EVENTTYPE_TOUCHES_BEGAN:
            if (e->num_touches > 0) {
                g_state->mouse_x = e->touches[0].pos_x;
                g_state->mouse_y = e->touches[0].pos_y;
                g_state->mouse_held[0] = true;
                g_state->mouse_pressed[0] = true;
            }
            break;
        case SAPP_EVENTTYPE_TOUCHES_MOVED:
            if (e->num_touches > 0) {
                g_state->mouse_x = e->touches[0].pos_x;
                g_state->mouse_y = e->touches[0].pos_y;
            }
            break;
        case SAPP_EVENTTYPE_TOUCHES_ENDED:
        case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
            g_state->mouse_held[0] = false;
            break;

        default:
            break;
    }
}

} // namespace

namespace {
unsigned char float_to_byte(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<unsigned char>(c * 255.0f + 0.5f);
}

struct TextLayout { std::vector<std::string> lines; float width = 0.0f; float line_h = 0.0f; };

float measure_line(FONScontext* fons, const std::string& s) {
    float b[4] = {0, 0, 0, 0};
    fonsTextBounds(fons, 0.0f, 0.0f, s.c_str(), nullptr, b);
    return b[2] - b[0];
}

// Splits on '\n' and (when max_width > 0) word-wraps each paragraph.
TextLayout layout_text(FONScontext* fons, int fid, const std::string& str, const TextOpts& opts) {
    TextLayout out;
    out.line_h = opts.size * opts.line_spacing;
    fonsClearState(fons);
    fonsSetFont(fons, fid);
    fonsSetSize(fons, opts.size);
    fonsSetAlign(fons, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);

    std::vector<std::string> paras;
    {
        std::string p;
        for (char c : str) { if (c == '\n') { paras.push_back(p); p.clear(); } else p += c; }
        paras.push_back(p);
    }
    for (const std::string& p : paras) {
        if (opts.max_width <= 0.0f) { out.lines.push_back(p); continue; }
        std::stringstream ws(p);
        std::string word, line;
        while (ws >> word) {
            const std::string cand = line.empty() ? word : line + " " + word;
            if (measure_line(fons, cand) > opts.max_width && !line.empty()) {
                out.lines.push_back(line);
                line = word;
            } else {
                line = cand;
            }
        }
        out.lines.push_back(line);
    }
    for (const std::string& l : out.lines) out.width = std::max(out.width, measure_line(fons, l));
    return out;
}
} // namespace

void Frame::clear(rgba color) {
    g_state->clear_color = color;
}

void Frame::rect(vec2 pos, vec2 size, rgba color) {
    sgl_texture(g_state->white_view, g_state->sampler);
    sgl_c4f(color.r, color.g, color.b, color.a);
    sgl_begin_quads();
    sgl_v2f_t2f(pos.x,          pos.y,          0.0f, 0.0f);
    sgl_v2f_t2f(pos.x + size.x, pos.y,          0.0f, 0.0f);
    sgl_v2f_t2f(pos.x + size.x, pos.y + size.y, 0.0f, 0.0f);
    sgl_v2f_t2f(pos.x,          pos.y + size.y, 0.0f, 0.0f);
    sgl_end();
}

void Frame::line(vec2 a, vec2 b, rgba color, float thickness) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return;
    const float nx = -dy / len * thickness * 0.5f;
    const float ny = dx / len * thickness * 0.5f;
    sgl_texture(g_state->white_view, g_state->sampler);
    sgl_c4f(color.r, color.g, color.b, color.a);
    sgl_begin_quads();
    sgl_v2f_t2f(a.x + nx, a.y + ny, 0.0f, 0.0f);
    sgl_v2f_t2f(b.x + nx, b.y + ny, 0.0f, 0.0f);
    sgl_v2f_t2f(b.x - nx, b.y - ny, 0.0f, 0.0f);
    sgl_v2f_t2f(a.x - nx, a.y - ny, 0.0f, 0.0f);
    sgl_end();
}

void Frame::triangle(vec2 a, vec2 b, vec2 c, rgba color) {
    sgl_texture(g_state->white_view, g_state->sampler);
    sgl_c4f(color.r, color.g, color.b, color.a);
    sgl_begin_triangles();
    sgl_v2f_t2f(a.x, a.y, 0.0f, 0.0f);
    sgl_v2f_t2f(b.x, b.y, 0.0f, 0.0f);
    sgl_v2f_t2f(c.x, c.y, 0.0f, 0.0f);
    sgl_end();
}

void Frame::circle(vec2 center, float radius, rgba color, int segments) {
    if (segments < 3) segments = 3;
    const float step = 6.2831853f / static_cast<float>(segments);
    sgl_texture(g_state->white_view, g_state->sampler);
    sgl_c4f(color.r, color.g, color.b, color.a);
    sgl_begin_triangles();
    for (int i = 0; i < segments; ++i) {
        const float a0 = i * step, a1 = (i + 1) * step;
        sgl_v2f_t2f(center.x, center.y, 0.0f, 0.0f);
        sgl_v2f_t2f(center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius, 0.0f, 0.0f);
        sgl_v2f_t2f(center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius, 0.0f, 0.0f);
    }
    sgl_end();
}

void Frame::circle_outline(vec2 center, float radius, rgba color, float thickness, int segments) {
    if (segments < 3) segments = 3;
    const float step = 6.2831853f / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = i * step, a1 = (i + 1) * step;
        line({center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius},
             {center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius}, color, thickness);
    }
}

void Frame::sprite(Texture tex, vec2 pos, SpriteOpts opts) {
    if (!tex.valid()) return;
    TextureRecord& rec = g_state->textures[tex.id];
    ensure_uploaded(rec);
    if (rec.img.id == SG_INVALID_ID) return;

    // Source region -> UVs. Zero-size src means the whole texture.
    const float texW = static_cast<float>(tex.width);
    const float texH = static_cast<float>(tex.height);
    float sx = opts.src.pos.x, sy = opts.src.pos.y;
    float sw = opts.src.size.x, sh = opts.src.size.y;
    if (sw <= 0.0f || sh <= 0.0f) { sx = 0.0f; sy = 0.0f; sw = texW; sh = texH; }
    float u0 = texW > 0.0f ? sx / texW : 0.0f;
    float v0 = texH > 0.0f ? sy / texH : 0.0f;
    float u1 = texW > 0.0f ? (sx + sw) / texW : 1.0f;
    float v1 = texH > 0.0f ? (sy + sh) / texH : 1.0f;
    if (opts.flip_x) { const float t = u0; u0 = u1; u1 = t; }
    if (opts.flip_y) { const float t = v0; v0 = v1; v1 = t; }

    const float w = opts.size.x > 0.0f ? opts.size.x : sw;
    const float h = opts.size.y > 0.0f ? opts.size.y : sh;
    const float cx = pos.x + w * 0.5f;
    const float cy = pos.y + h * 0.5f;
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    const float cs = std::cos(opts.rotation);
    const float sn = std::sin(opts.rotation);

    // Rotate corners on the CPU so the modelview matrix never changes — that
    // keeps all sprites of one texture in a single batched draw call.
    auto corner = [&](float ox, float oy) {
        return vec2{cx + ox * cs - oy * sn, cy + ox * sn + oy * cs};
    };
    const vec2 p0 = corner(-hw, -hh);
    const vec2 p1 = corner(hw, -hh);
    const vec2 p2 = corner(hw, hh);
    const vec2 p3 = corner(-hw, hh);

    sgl_texture(rec.view, g_state->sampler);
    sgl_c4f(opts.tint.r, opts.tint.g, opts.tint.b, opts.tint.a);
    sgl_begin_quads();
    sgl_v2f_t2f(p0.x, p0.y, u0, v0);
    sgl_v2f_t2f(p1.x, p1.y, u1, v0);
    sgl_v2f_t2f(p2.x, p2.y, u1, v1);
    sgl_v2f_t2f(p3.x, p3.y, u0, v1);
    sgl_end();
}

void Frame::sprite(Texture tex, vec2 pos) {
    sprite(tex, pos, SpriteOpts{});
}

bool Frame::key_down(Key k) const {
    const int i = static_cast<int>(k);
    return i >= 0 && i < 512 && g_state->key_held[i];
}

bool Frame::key_pressed(Key k) const {
    const int i = static_cast<int>(k);
    return i >= 0 && i < 512 && g_state->key_pressed[i];
}

vec2 Frame::mouse() const {
    return vec2{g_state->mouse_x, g_state->mouse_y};
}

bool Frame::mouse_down(Mouse b) const {
    const int i = static_cast<int>(b);
    return i >= 0 && i < 3 && g_state->mouse_held[i];
}

bool Frame::mouse_pressed(Mouse b) const {
    const int i = static_cast<int>(b);
    return i >= 0 && i < 3 && g_state->mouse_pressed[i];
}

bool Frame::touching() const {
    return g_state->mouse_held[0];
}

vec2 Frame::touch_pos() const {
    return vec2{g_state->mouse_x, g_state->mouse_y};
}

// --- gamepad -----------------------------------------------------------

namespace {
int pad_field(const ThistleGamepad& g, Pad b) {
    switch (b) {
        case Pad::A:     return g.a;
        case Pad::B:     return g.b;
        case Pad::X:     return g.x;
        case Pad::Y:     return g.y;
        case Pad::Up:    return g.up;
        case Pad::Down:  return g.down;
        case Pad::Left:  return g.left;
        case Pad::Right: return g.right;
        case Pad::L1:    return g.l1;
        case Pad::R1:    return g.r1;
        case Pad::Start: return g.start;
        case Pad::Back:  return g.back;
    }
    return 0;
}
} // namespace

bool Frame::pad_connected() const { return g_state->pad_cur.connected != 0; }
bool Frame::pad_down(Pad b) const { return pad_field(g_state->pad_cur, b) != 0; }
bool Frame::pad_pressed(Pad b) const {
    return pad_field(g_state->pad_cur, b) != 0 && pad_field(g_state->pad_prev, b) == 0;
}
vec2 Frame::pad_left_stick() const { return {g_state->pad_cur.lx, g_state->pad_cur.ly}; }
vec2 Frame::pad_right_stick() const { return {g_state->pad_cur.rx, g_state->pad_cur.ry}; }

PadKind Frame::pad_kind() const {
    switch (g_state->pad_cur.kind) {
        case 1: return PadKind::Xbox;
        case 2: return PadKind::PlayStation;
        case 3: return PadKind::Nintendo;
        case 4: return PadKind::Generic;
        default: return PadKind::None;
    }
}

std::string Frame::pad_name() const { return std::string(g_state->pad_cur.name); }

const char* Frame::pad_label(Pad b) const {
    switch (pad_kind()) {
        case PadKind::PlayStation:
            switch (b) {
                case Pad::A: return "Cross";  case Pad::B: return "Circle";
                case Pad::X: return "Square"; case Pad::Y: return "Triangle";
                case Pad::L1: return "L1";    case Pad::R1: return "R1";
                case Pad::Start: return "Options"; case Pad::Back: return "Share";
                case Pad::Up: return "Up"; case Pad::Down: return "Down";
                case Pad::Left: return "Left"; case Pad::Right: return "Right";
            }
            break;
        case PadKind::Nintendo: // Nintendo swaps A/B and X/Y positions
            switch (b) {
                case Pad::A: return "B";  case Pad::B: return "A";
                case Pad::X: return "Y";  case Pad::Y: return "X";
                case Pad::L1: return "L"; case Pad::R1: return "R";
                case Pad::Start: return "+"; case Pad::Back: return "-";
                case Pad::Up: return "Up"; case Pad::Down: return "Down";
                case Pad::Left: return "Left"; case Pad::Right: return "Right";
            }
            break;
        default: // Xbox / Generic
            switch (b) {
                case Pad::A: return "A"; case Pad::B: return "B";
                case Pad::X: return "X"; case Pad::Y: return "Y";
                case Pad::L1: return "LB"; case Pad::R1: return "RB";
                case Pad::Start: return "Start"; case Pad::Back: return "Back";
                case Pad::Up: return "Up"; case Pad::Down: return "Down";
                case Pad::Left: return "Left"; case Pad::Right: return "Right";
            }
            break;
    }
    return "?";
}

// --- particles ---------------------------------------------------------

namespace {
float rand01() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }
} // namespace

void Particles::emit(vec2 pos, int count, ParticleOpts opts) {
    for (int i = 0; i < count; ++i) {
        const float ang = rand01() * 6.2831853f;
        const float mag = rand01() * opts.spread;
        P p;
        p.pos = pos;
        p.vel = {opts.velocity.x + std::cos(ang) * mag, opts.velocity.y + std::sin(ang) * mag};
        p.max_life = opts.life * (0.6f + 0.4f * rand01());
        p.life = p.max_life;
        p.size = opts.size * (0.6f + 0.6f * rand01());
        p.gravity = opts.gravity;
        p.color = opts.color;
        items_.push_back(p);
    }
}

void Particles::update(float dt) {
    for (auto& p : items_) {
        p.vel.y += p.gravity * dt;
        p.pos += p.vel * dt;
        p.life -= dt;
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const P& p) { return p.life <= 0.0f; }),
                 items_.end());
}

void Particles::draw(Frame& f) const {
    for (const auto& p : items_) {
        rgba c = p.color;
        c.a *= (p.max_life > 0.0f ? p.life / p.max_life : 0.0f);
        f.rect({p.pos.x - p.size * 0.5f, p.pos.y - p.size * 0.5f}, {p.size, p.size}, c);
    }
}

// --- easing ------------------------------------------------------------

float ease(Ease e, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (e) {
        case Ease::Linear:    return t;
        case Ease::InQuad:    return t * t;
        case Ease::OutQuad:   return 1.0f - (1.0f - t) * (1.0f - t);
        case Ease::InOutQuad: return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
        case Ease::OutCubic:  { const float u = 1.0f - t; return 1.0f - u * u * u; }
        case Ease::OutBack:   {
            const float c1 = 1.70158f, c3 = c1 + 1.0f, u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case Ease::OutElastic: {
            if (t == 0.0f || t == 1.0f) return t;
            const float c4 = (2.0f * 3.14159265f) / 3.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        }
        case Ease::OutBounce: {
            const float n1 = 7.5625f, d1 = 2.75f;
            if (t < 1.0f / d1) return n1 * t * t;
            else if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
            else if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
            else { t -= 2.625f / d1; return n1 * t * t + 0.984375f; }
        }
    }
    return t;
}

// --- transform stack ---------------------------------------------------

void Frame::push_transform(vec2 pos, float rotation, vec2 scale) {
    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, 0.0f);
    sgl_rotate(rotation, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale.x, scale.y, 1.0f);
}

void Frame::pop_transform() { sgl_pop_matrix(); }

// --- scene graph -------------------------------------------------------

void Node::update(float dt) {
    while (!actions_.empty()) {
        Action& a = actions_.front();
        if (!a.started) {
            a.started = true;
            a.elapsed = 0.0f;
            switch (a.kind) {
                case Action::Kind::MoveTo:   a.v_from = pos; break;
                case Action::Kind::MoveBy:   a.v_from = pos; a.v_to = pos + a.v_to; break;
                case Action::Kind::ScaleTo:  a.v_from = scale; break;
                case Action::Kind::RotateTo: a.f_from = rotation; break;
                case Action::Kind::FadeTo:   a.f_from = alpha; break;
                case Action::Kind::Delay:    break;
                case Action::Kind::Call:     if (a.call) a.call(); break;
            }
            if (a.kind == Action::Kind::Call) { actions_.erase(actions_.begin()); continue; }
        }
        a.elapsed += dt;
        float lin = a.dur > 0.0f ? a.elapsed / a.dur : 1.0f;
        if (lin > 1.0f) lin = 1.0f;
        const float e = ease(a.ease, lin);
        switch (a.kind) {
            case Action::Kind::MoveTo:
            case Action::Kind::MoveBy:   pos = lerp(a.v_from, a.v_to, e); break;
            case Action::Kind::ScaleTo:  scale = lerp(a.v_from, a.v_to, e); break;
            case Action::Kind::RotateTo: rotation = lerp(a.f_from, a.f_to, e); break;
            case Action::Kind::FadeTo:   alpha = lerp(a.f_from, a.f_to, e); break;
            case Action::Kind::Delay:
            case Action::Kind::Call:     break;
        }
        if (lin >= 1.0f) actions_.erase(actions_.begin());
        break; // one time-based action per frame
    }
    for (auto& c : children_) c->update(dt);
}

void Node::draw(Frame& f) { draw_rec(f, 1.0f); }

void Node::draw_rec(Frame& f, float inherited_alpha) {
    if (!visible) return;
    const float a = inherited_alpha * alpha;
    f.push_transform(pos, rotation, scale);
    if (sprite.valid()) {
        const vec2 sz = sprite_size.x > 0.0f
            ? sprite_size
            : vec2{static_cast<float>(sprite.width), static_cast<float>(sprite.height)};
        rgba tint = sprite_tint;
        tint.a *= a;
        f.sprite(sprite, {-sz.x * 0.5f, -sz.y * 0.5f}, {.size = sz, .tint = tint, .src = sprite_src});
    }
    for (auto& c : children_) c->draw_rec(f, a);
    f.pop_transform();
}

vec2 Node::world_pos() const {
    vec2 p = pos; // this node's origin in its parent's space
    for (const Node* n = parent_; n; n = n->parent_) {
        p.x *= n->scale.x;
        p.y *= n->scale.y;
        const float c = std::cos(n->rotation), s = std::sin(n->rotation);
        p = {p.x * c - p.y * s, p.x * s + p.y * c};
        p.x += n->pos.x;
        p.y += n->pos.y;
    }
    return p;
}

// --- tilemap -----------------------------------------------------------

bool Tilemap::load_csv(const std::string& csv_path, Texture tileset, int tw, int th, int tileset_cols) {
    std::ifstream in(csv_path);
    if (!in) { log_warn("tilemap: could not open " + csv_path); return false; }
    tileset_ = tileset;
    tile_w = tw;
    tile_h = th;
    tileset_cols_ = tileset_cols > 0 ? tileset_cols
                                     : (tw > 0 ? std::max(1, tileset.width / tw) : 1);
    firstgid_ = 1;
    std::vector<int> data;
    cols_ = 0;
    rows_ = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        int cols_this = 0;
        while (std::getline(ss, cell, ',')) {
            if (cell.find_first_not_of(" \t\r") == std::string::npos) continue;
            data.push_back(std::atoi(cell.c_str()));
            ++cols_this;
        }
        if (cols_this > 0) { cols_ = std::max(cols_, cols_this); ++rows_; }
    }
    layers_.clear();
    layers_.push_back(std::move(data));
    return !layers_[0].empty();
}

bool Tilemap::load_tiled_json(const std::string& path) {
    std::ifstream in(path);
    if (!in) { log_warn("tilemap: could not open " + path); return false; }
    nlohmann::json j;
    try { in >> j; } catch (...) { log_warn("tilemap: invalid JSON " + path); return false; }

    cols_ = j.value("width", 0);
    rows_ = j.value("height", 0);
    tile_w = j.value("tilewidth", 0);
    tile_h = j.value("tileheight", 0);

    if (!j.contains("tilesets") || j["tilesets"].empty()) { log_warn("tilemap: no tilesets in " + path); return false; }
    const auto& ts = j["tilesets"][0];
    firstgid_ = ts.value("firstgid", 1);
    const std::string image = ts.value("image", std::string());
    const int imgw = ts.value("imagewidth", 0);
    tileset_cols_ = ts.value("columns", (tile_w > 0 && imgw > 0) ? imgw / tile_w : 1);

    // Resolve the tileset image relative to the JSON file's directory.
    std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    std::string img_path = (!image.empty() && image.front() != '/') ? dir + image : image;
    tileset_ = load_texture(img_path);
    if (tileset_cols_ <= 0 && tile_w > 0) tileset_cols_ = std::max(1, tileset_.width / tile_w);

    layers_.clear();
    for (const auto& layer : j.value("layers", nlohmann::json::array())) {
        if (layer.value("type", std::string()) != "tilelayer") continue;
        std::vector<int> data;
        for (const auto& v : layer.value("data", nlohmann::json::array())) data.push_back(v.get<int>());
        if (!data.empty()) layers_.push_back(std::move(data));
    }
    return !layers_.empty();
}

int Tilemap::at(int col, int row, int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return 0;
    if (col < 0 || row < 0 || col >= cols_ || row >= rows_) return 0;
    const std::size_t i = static_cast<std::size_t>(row) * cols_ + col;
    return i < layers_[layer].size() ? layers_[layer][i] : 0;
}

void Tilemap::draw(Frame& f, vec2 offset) const {
    if (!tileset_.valid() || tileset_cols_ <= 0) return;
    for (int L = 0; L < static_cast<int>(layers_.size()); ++L) {
        for (int row = 0; row < rows_; ++row) {
            for (int col = 0; col < cols_; ++col) {
                const int gid = at(col, row, L);
                if (gid <= 0) continue;
                const int idx = gid - firstgid_;
                if (idx < 0) continue;
                const float sx = static_cast<float>((idx % tileset_cols_) * tile_w);
                const float sy = static_cast<float>((idx / tileset_cols_) * tile_h);
                f.sprite(tileset_,
                         {offset.x + col * tile_w, offset.y + row * tile_h},
                         {.size = {static_cast<float>(tile_w), static_cast<float>(tile_h)},
                          .src = {{sx, sy}, {static_cast<float>(tile_w), static_cast<float>(tile_h)}}});
            }
        }
    }
}

// --- physics (Box2D) ---------------------------------------------------

struct Physics::Impl : public b2ContactListener {
    static constexpr float PPM = 100.0f; // pixels per meter
    b2World world;
    std::vector<b2Body*> bodies;
    std::function<void(Body, Body)> on_begin;
    explicit Impl(b2Vec2 g) : world(g) { world.SetContactListener(this); }
    b2Body* get(Body b) const {
        return (b.id >= 0 && b.id < static_cast<int>(bodies.size())) ? bodies[b.id] : nullptr;
    }
    Body register_body(b2Body* body) {
        const int id = static_cast<int>(bodies.size());
        body->GetUserData().pointer = static_cast<uintptr_t>(id + 1);
        bodies.push_back(body);
        return Body{id};
    }
    static int id_of(b2Body* b) { return static_cast<int>(b->GetUserData().pointer) - 1; }
    void BeginContact(b2Contact* c) override {
        if (!on_begin) return;
        const int a = id_of(c->GetFixtureA()->GetBody());
        const int b = id_of(c->GetFixtureB()->GetBody());
        if (a >= 0 && b >= 0) on_begin(Body{a}, Body{b});
    }
};

Physics::Physics(vec2 gravity) {
    const float ppm = Impl::PPM;
    impl_ = std::make_unique<Impl>(b2Vec2(gravity.x / ppm, gravity.y / ppm));
}
Physics::~Physics() = default;

Body Physics::add_static_box(vec2 center, vec2 size) {
    const float ppm = Impl::PPM;
    b2BodyDef def;
    def.type = b2_staticBody;
    def.position.Set(center.x / ppm, center.y / ppm);
    b2Body* body = impl_->world.CreateBody(&def);
    b2PolygonShape box;
    box.SetAsBox(size.x * 0.5f / ppm, size.y * 0.5f / ppm);
    body->CreateFixture(&box, 0.0f);
    return impl_->register_body(body);
}

Body Physics::add_kinematic_box(vec2 center, vec2 size, float friction) {
    // A kinematic body: you drive it with set_velocity(); it isn't pushed by
    // collisions but carries dynamic bodies resting on it (moving platforms).
    const float ppm = Impl::PPM;
    b2BodyDef def;
    def.type = b2_kinematicBody;
    def.position.Set(center.x / ppm, center.y / ppm);
    b2Body* body = impl_->world.CreateBody(&def);
    b2PolygonShape box;
    box.SetAsBox(size.x * 0.5f / ppm, size.y * 0.5f / ppm);
    b2FixtureDef fd;
    fd.shape = &box;
    fd.density = 1.0f;      // ignored for kinematic mass, but Box2D wants it set
    fd.friction = friction;
    body->CreateFixture(&fd);
    return impl_->register_body(body);
}

Body Physics::add_dynamic_box(vec2 center, vec2 size, float density, float friction, float restitution) {
    const float ppm = Impl::PPM;
    b2BodyDef def;
    def.type = b2_dynamicBody;
    def.position.Set(center.x / ppm, center.y / ppm);
    b2Body* body = impl_->world.CreateBody(&def);
    b2PolygonShape box;
    box.SetAsBox(size.x * 0.5f / ppm, size.y * 0.5f / ppm);
    b2FixtureDef fd;
    fd.shape = &box;
    fd.density = density;
    fd.friction = friction;
    fd.restitution = restitution;
    body->CreateFixture(&fd);
    return impl_->register_body(body);
}

Body Physics::add_dynamic_circle(vec2 center, float radius, float density, float friction, float restitution) {
    const float ppm = Impl::PPM;
    b2BodyDef def;
    def.type = b2_dynamicBody;
    def.position.Set(center.x / ppm, center.y / ppm);
    b2Body* body = impl_->world.CreateBody(&def);
    b2CircleShape circle;
    circle.m_radius = radius / ppm;
    b2FixtureDef fd;
    fd.shape = &circle;
    fd.density = density;
    fd.friction = friction;
    fd.restitution = restitution;
    body->CreateFixture(&fd);
    return impl_->register_body(body);
}

Body Physics::add_sensor_box(vec2 center, vec2 size) {
    const float ppm = Impl::PPM;
    b2BodyDef def;
    def.type = b2_staticBody;
    def.position.Set(center.x / ppm, center.y / ppm);
    b2Body* body = impl_->world.CreateBody(&def);
    b2PolygonShape box;
    box.SetAsBox(size.x * 0.5f / ppm, size.y * 0.5f / ppm);
    b2FixtureDef fd;
    fd.shape = &box;
    fd.isSensor = true;
    fd.density = 0.0f;
    body->CreateFixture(&fd);
    return impl_->register_body(body);
}

void Physics::step(float dt) { impl_->world.Step(dt, 8, 3); }

void Physics::on_collision(std::function<void(Body, Body)> begin) {
    impl_->on_begin = std::move(begin);
}

bool Physics::touching(Body a, Body b) const {
    b2Body* ba = impl_->get(a);
    b2Body* bb = impl_->get(b);
    if (!ba || !bb) return false;
    for (b2ContactEdge* ce = ba->GetContactList(); ce; ce = ce->next)
        if (ce->other == bb && ce->contact->IsTouching()) return true;
    return false;
}

namespace {
struct RayCB : public b2RayCastCallback {
    RayHit result;
    float ppm = 100.0f;
    float ReportFixture(b2Fixture* fx, const b2Vec2& p, const b2Vec2& n, float fraction) override {
        result.hit = true;
        result.body = Body{static_cast<int>(fx->GetBody()->GetUserData().pointer) - 1};
        result.point = {p.x * ppm, p.y * ppm};
        result.normal = {n.x, n.y};
        result.fraction = fraction;
        return fraction; // clip the ray to the closest hit
    }
};
} // namespace

RayHit Physics::raycast(vec2 from, vec2 to) const {
    RayCB cb;
    cb.ppm = Impl::PPM;
    impl_->world.RayCast(&cb, {from.x / Impl::PPM, from.y / Impl::PPM},
                         {to.x / Impl::PPM, to.y / Impl::PPM});
    return cb.result;
}

vec2 Physics::position(Body b) const {
    b2Body* body = impl_->get(b);
    if (!body) return {};
    const b2Vec2 p = body->GetPosition();
    return {p.x * Impl::PPM, p.y * Impl::PPM};
}
float Physics::angle(Body b) const {
    b2Body* body = impl_->get(b);
    return body ? body->GetAngle() : 0.0f;
}
vec2 Physics::velocity(Body b) const {
    b2Body* body = impl_->get(b);
    if (!body) return {};
    const b2Vec2 v = body->GetLinearVelocity();
    return {v.x * Impl::PPM, v.y * Impl::PPM};
}
void Physics::set_velocity(Body b, vec2 v) {
    if (b2Body* body = impl_->get(b)) body->SetLinearVelocity({v.x / Impl::PPM, v.y / Impl::PPM});
}
void Physics::apply_impulse(Body b, vec2 impulse) {
    if (b2Body* body = impl_->get(b))
        body->ApplyLinearImpulseToCenter({impulse.x / Impl::PPM, impulse.y / Impl::PPM}, true);
}
void Physics::set_position(Body b, vec2 center) {
    if (b2Body* body = impl_->get(b))
        body->SetTransform({center.x / Impl::PPM, center.y / Impl::PPM}, body->GetAngle());
}
void Physics::set_fixed_rotation(Body b, bool fixed) {
    if (b2Body* body = impl_->get(b)) body->SetFixedRotation(fixed);
}
void Physics::set_angular_velocity(Body b, float rad_per_s) {
    if (b2Body* body = impl_->get(b)) body->SetAngularVelocity(rad_per_s);
}
void Physics::reset_body(Body b, vec2 center) {
    if (b2Body* body = impl_->get(b)) {
        body->SetTransform({center.x / Impl::PPM, center.y / Impl::PPM}, 0.0f);
        body->SetLinearVelocity({0.0f, 0.0f});
        body->SetAngularVelocity(0.0f);
        body->SetAwake(true);
    }
}

// --- ui: button --------------------------------------------------------

bool Frame::button(const std::string& label, Rect area, ButtonStyle style) {
    const vec2 m = mouse();
    const bool hover = area.contains(m);
    const bool held = hover && mouse_down(Mouse::Left);
    const bool clicked = hover && mouse_pressed(Mouse::Left);
    const rgba bg = held ? style.bg_press : (hover ? style.bg_hover : style.bg);
    rect(area.pos, area.size, bg);
    const vec2 tm = measure_text(label, {.size = style.text_size});
    text(label,
         {area.pos.x + (area.size.x - tm.x) * 0.5f,
          area.pos.y + (area.size.y - style.text_size) * 0.5f},
         {.size = style.text_size, .color = style.text});
    return clicked;
}

void Frame::blend(Blend mode) {
    sgl_load_pipeline(mode == Blend::Additive ? g_state->pip_additive : g_state->pip);
}

bool Frame::checkbox(const std::string& label, bool& value, Rect area, ButtonStyle style) {
    const bool hover = area.contains(mouse());
    const bool clicked = hover && mouse_pressed(Mouse::Left);
    if (clicked) value = !value;
    const float bs = area.size.y; // square box
    rect(area.pos, {bs, bs}, hover ? style.bg_hover : style.bg);
    if (value) rect({area.pos.x + bs * 0.25f, area.pos.y + bs * 0.25f}, {bs * 0.5f, bs * 0.5f}, style.bg_press);
    text(label, {area.pos.x + bs + 10.0f, area.pos.y + (bs - style.text_size) * 0.5f},
         {.size = style.text_size, .color = style.text});
    return clicked;
}

float Frame::slider(float value, float min_v, float max_v, Rect area, ButtonStyle style) {
    rect(area.pos, area.size, style.bg);
    if (mouse_down(Mouse::Left) && area.contains(mouse()) && area.size.x > 0.0f) {
        float t = (mouse().x - area.pos.x) / area.size.x;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        value = min_v + t * (max_v - min_v);
    }
    float t = (max_v > min_v) ? (value - min_v) / (max_v - min_v) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    rect(area.pos, {area.size.x * t, area.size.y}, style.bg_hover);
    const float kx = area.pos.x + area.size.x * t;
    rect({kx - 6.0f, area.pos.y - 3.0f}, {12.0f, area.size.y + 6.0f}, style.bg_press);
    return value;
}

void Frame::progress_bar(float t, Rect area, rgba fill, rgba bg) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    rect(area.pos, area.size, bg);
    rect(area.pos, {area.size.x * t, area.size.y}, fill);
}

// --- menu --------------------------------------------------------------

namespace {
float ease_out_cubic(float p) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const float inv = 1.0f - p;
    return 1.0f - inv * inv * inv;
}
} // namespace

Menu::Menu(Frame& f, float elapsed, MenuAnim anim, float duration) : f_(f), anim_(anim) {
    cx_ = f.width * 0.5f;
    y_ = f.height * 0.26f;
    const float e = ease_out_cubic(duration > 0.0f ? elapsed / duration : 1.0f);
    const float W = static_cast<float>(f.width);
    const float H = static_cast<float>(f.height);
    switch (anim) {
        case MenuAnim::FromTop:    offset_ = {0.0f, -(1.0f - e) * H * 0.6f}; break;
        case MenuAnim::FromBottom: offset_ = {0.0f,  (1.0f - e) * H * 0.6f}; break;
        case MenuAnim::FromLeft:   offset_ = {-(1.0f - e) * W * 0.6f, 0.0f}; break;
        case MenuAnim::FromRight:  offset_ = { (1.0f - e) * W * 0.6f, 0.0f}; break;
        case MenuAnim::FadeIn:     alpha_ = e; break;
        case MenuAnim::SmallToBig: scale_ = 0.2f + 0.8f * e; alpha_ = e; break;
        case MenuAnim::None:       break;
    }

    // Gamepad focus navigation (only when a controller is connected).
    if (f.pad_connected()) {
        if (f.pad_pressed(Pad::Down)) g_state->menu_focus++;
        if (f.pad_pressed(Pad::Up)) g_state->menu_focus--;
        const int n = g_state->menu_last_count;
        if (n > 0) g_state->menu_focus = ((g_state->menu_focus % n) + n) % n;
    }
    index_ = 0;
}

void Menu::title(const std::string& text, float size, rgba color) {
    const float ds = size * scale_;
    color.a *= alpha_;
    const vec2 m = f_.measure_text(text, {.size = ds});
    f_.text(text, {cx_ - m.x * 0.5f + offset_.x, y_ + offset_.y}, {.size = ds, .color = color});
    y_ += ds * 1.3f;
}

void Menu::label(const std::string& text, float size, rgba color) {
    title(text, size, color); // same centered layout, just a smaller default size
}

bool Menu::button(const std::string& text, ButtonStyle style) {
    const bool focused = f_.pad_connected() && (index_ == g_state->menu_focus);
    if (focused) style.bg = style.bg_hover; // highlight the gamepad-focused item
    const float bw = 340.0f * scale_;
    const float bh = 64.0f * scale_;
    style.bg.a *= alpha_;
    style.bg_hover.a *= alpha_;
    style.bg_press.a *= alpha_;
    style.text.a *= alpha_;
    style.text_size *= scale_;
    const Rect area{{cx_ - bw * 0.5f + offset_.x, y_ + offset_.y}, {bw, bh}};
    const bool clicked = f_.button(text, area, style) || (focused && f_.pad_pressed(Pad::A));
    y_ += bh + 16.0f * scale_;
    g_state->menu_last_count = ++index_;
    return clicked;
}

void Menu::gap(float pixels) { y_ += pixels * scale_; }

// --- platform ----------------------------------------------------------

Platform platform() {
#if defined(__EMSCRIPTEN__)
    return Platform::Web;
#elif defined(__ANDROID__)
    return Platform::Android;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return Platform::iOS;
#else
    return Platform::MacOS;
#endif
#elif defined(_WIN32)
    return Platform::Windows;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

bool is_mobile() {
    const Platform p = platform();
    return p == Platform::iOS || p == Platform::Android;
}

const char* platform_name() {
    switch (platform()) {
        case Platform::Windows: return "Windows";
        case Platform::MacOS:   return "macOS";
        case Platform::Linux:   return "Linux";
        case Platform::iOS:     return "iOS";
        case Platform::Android: return "Android";
        case Platform::Web:     return "Web";
        default:                return "Unknown";
    }
}

std::string device_name() {
#if defined(__APPLE__)
    const char* n = thistle_apple_device_name();
    if (n && *n) return n;
#endif
    return platform_name();
}

// --- logging & clipboard ----------------------------------------------

namespace {
void push_log(const char* level, const std::string& msg) {
    std::string line = std::string(level) + msg;
    std::fprintf(stderr, "%s\n", line.c_str());
    g_logs.push_back(std::move(line));
    if (g_logs.size() > 1000) g_logs.erase(g_logs.begin(), g_logs.begin() + 200);
}
} // namespace

void log_info(const std::string& msg)  { push_log("[info] ", msg); }
void log_warn(const std::string& msg)  { push_log("[warn] ", msg); }
void log_error(const std::string& msg) { push_log("[error] ", msg); }

void set_clipboard(const std::string& text) {
#if defined(__APPLE__)
    thistle_apple_set_clipboard(text.c_str());
#endif
    sapp_set_clipboard_string(text.c_str());
}

void haptic(Haptic style) {
#if defined(__APPLE__)
    thistle_apple_haptic(static_cast<int>(style));
#else
    (void)style;
#endif
}

void begin_text_input(const std::string& initial) {
    g_state->text_buffer = initial.substr(0, g_state->text_max);
    g_state->text_capturing = true;
    sapp_show_keyboard(true);
}
void end_text_input() {
    g_state->text_capturing = false;
    sapp_show_keyboard(false);
}
const std::string& text_input() { return g_state->text_buffer; }

// --- networking ---------------------------------------------------------

Http Http::get(const std::string& url) {
    Http r;
#if defined(__APPLE__) || defined(_WIN32)
    void* raw = thistle_http_start("GET", url.c_str(), nullptr);
    r.h_ = std::shared_ptr<void>(raw, [](void* p) { if (p) thistle_http_free(p); });
#else
    (void)url;
#endif
    return r;
}

Http Http::post(const std::string& url, const std::string& json_body) {
    Http r;
#if defined(__APPLE__) || defined(_WIN32)
    void* raw = thistle_http_start("POST", url.c_str(), json_body.c_str());
    r.h_ = std::shared_ptr<void>(raw, [](void* p) { if (p) thistle_http_free(p); });
#else
    (void)url; (void)json_body;
#endif
    return r;
}

bool Http::done() {
    if (done_) return true;
    if (!h_) { done_ = true; return true; }   // unsupported / failed to start
#if defined(__APPLE__) || defined(_WIN32)
    int status = 0; const char* body = nullptr; int len = 0;
    if (thistle_http_poll(h_.get(), &status, &body, &len)) {
        status_ = status;
        if (body && len > 0) body_.assign(body, body + len);
        done_ = true;
    }
#else
    done_ = true;
#endif
    return done_;
}

void Http::reset() { h_.reset(); done_ = false; status_ = 0; body_.clear(); }

// --- realtime networking -------------------------------------------------
// Plain blocking-connect / non-blocking-poll TCP sockets, length-prefixed
// JSON messages. See the big comment on this API in thistle.hpp before
// touching this — the scope is deliberate, not an oversight.

#if defined(_WIN32)
    using SocketFd = SOCKET;
    constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
    using NetSockLen = int;
    static void close_socket(SocketFd s) { closesocket(s); }
    static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
    static void set_nonblocking(SocketFd s) { u_long mode = 1; ioctlsocket(s, FIONBIO, &mode); }
    static void ensure_sockets_ready() {
        static bool started = false;
        if (!started) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); started = true; }
    }
#else
    using SocketFd = int;
    constexpr SocketFd kInvalidSocket = -1;
    using NetSockLen = socklen_t;
    static void close_socket(SocketFd s) { ::close(s); }
    static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
    static void set_nonblocking(SocketFd s) { int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK); }
    static void ensure_sockets_ready() {}
#endif

static void set_nodelay(SocketFd s) {   // small JSON messages + Nagle's algorithm = needless latency
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
}

// Blocking send loop (busy-retries on EWOULDBLOCK) — fine for the tiny
// messages this protocol actually carries; not fine if you start sending
// megabytes over this, which you shouldn't.
static bool net_send_all(SocketFd s, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = ::send(s, data + sent, static_cast<int>(n - sent), 0);
        if (r > 0) { sent += static_cast<size_t>(r); continue; }
        if (r < 0 && would_block()) continue;
        return false;
    }
    return true;
}

static bool net_send_json(SocketFd s, const nlohmann::json& j) {
    std::string body = j.dump();
    uint32_t len = static_cast<uint32_t>(body.size());
    unsigned char hdr[4] = {
        static_cast<unsigned char>(len & 0xFF), static_cast<unsigned char>((len >> 8) & 0xFF),
        static_cast<unsigned char>((len >> 16) & 0xFF), static_cast<unsigned char>((len >> 24) & 0xFF)
    };
    return net_send_all(s, reinterpret_cast<const char*>(hdr), 4) && net_send_all(s, body.data(), body.size());
}

// Drains everything currently available into buf. Returns false if the
// connection should be closed (orderly EOF or a real error).
static bool net_recv_available(SocketFd s, std::string& buf) {
    char tmp[4096];
    for (;;) {
        int r = ::recv(s, tmp, sizeof(tmp), 0);
        if (r > 0) { buf.append(tmp, static_cast<size_t>(r)); continue; }
        if (r == 0) return false;
        if (would_block()) return true;
        return false;
    }
}

// Pulls one complete length-prefixed message out of buf, if there is one.
static bool net_extract_message(std::string& buf, nlohmann::json& out) {
    if (buf.size() < 4) return false;
    uint32_t len = static_cast<unsigned char>(buf[0]) | (static_cast<unsigned char>(buf[1]) << 8) |
                   (static_cast<unsigned char>(buf[2]) << 16) | (static_cast<unsigned char>(buf[3]) << 24);
    if (buf.size() < 4 + len) return false;
    bool ok = true;
    try { out = nlohmann::json::parse(buf.begin() + 4, buf.begin() + 4 + len); }
    catch (...) { ok = false; }
    buf.erase(0, 4 + len);
    return ok;
}

// A friend "access key": lets the send/dispatch code above reach NetObject's
// private registries without exposing them on the public API.
// struct NetServer::Impl / NetClient::Impl must be complete (defined) before
// anything below dereferences a NetServer*/NetClient*'s impl_ — hence up here,
// right after the access key that lets non-member code reach impl_ at all.
struct NetServer::Impl {
    SocketFd listen_fd = kInvalidSocket;
    struct Conn { SocketFd fd; std::string rx; int id; };
    std::vector<Conn> conns;
    int next_conn_id = 1;
    std::map<uint32_t, std::unique_ptr<NetObject>> objects;
    uint32_t next_net_id = 1;
};

struct NetClient::Impl {
    SocketFd fd = kInvalidSocket;
    std::string rx;
    std::map<uint32_t, std::unique_ptr<NetObject>> objects;
    bool connected = false;
};

struct NetObjectAccess {
    static void set_id(NetObject& o, uint32_t id, const std::string& cls) { o.id_ = id; o.class_name_ = cls; }
    static std::vector<std::pair<std::string, NetObject::NetSyncField>>& fields(NetObject& o) { return o.fields_; }
    static std::vector<std::pair<std::string, std::function<void(const NetArgs&)>>>& commands(NetObject& o) { return o.commands_; }
    static std::vector<std::pair<std::string, std::function<void(const NetArgs&)>>>& client_rpcs(NetObject& o) { return o.client_rpcs_; }
    static NetServer::Impl& server_impl(NetServer& s) { return *s.impl_; }
    static NetClient::Impl& client_impl(NetClient& c) { return *c.impl_; }
};

static std::unordered_map<std::string, std::function<std::unique_ptr<NetObject>()>>& net_class_registry() {
    static std::unordered_map<std::string, std::function<std::unique_ptr<NetObject>()>> m;
    return m;
}

static NetServer* g_active_server = nullptr;
static NetClient* g_active_client = nullptr;

static void net_send_spawn(SocketFd fd, NetObject& obj) {
    nlohmann::json vars = nlohmann::json::object();
    for (auto& [name, field] : NetObjectAccess::fields(obj)) vars[name] = field.get();
    net_send_json(fd, { {"t", "spawn"}, {"id", obj.net_id()}, {"class", obj.class_name()}, {"vars", vars} });
}

namespace net {
bool is_server() { return g_active_server != nullptr; }
bool is_client() { return g_active_client != nullptr; }
}

void net_register_class(const std::string& class_name, std::function<std::unique_ptr<NetObject>()> make) {
    net_class_registry()[class_name] = std::move(make);
}

// --- NetObject ------------------------------------------------------------

void NetObject::add_sync_field(const std::string& name, NetSyncField field) { fields_.emplace_back(name, std::move(field)); }
void NetObject::on_command(const std::string& name, std::function<void(const NetArgs&)> fn) { commands_.emplace_back(name, std::move(fn)); }
void NetObject::on_client_rpc(const std::string& name, std::function<void(const NetArgs&)> fn) { client_rpcs_.emplace_back(name, std::move(fn)); }

void NetObject::call_command(const std::string& name, NetArgs args) {
    if (!g_active_client) { log_warn("call_command(\"" + name + "\"): not connected as a client"); return; }
    net_send_json(NetObjectAccess::client_impl(*g_active_client).fd, { {"t", "cmd"}, {"id", id_}, {"name", name}, {"args", args} });
}

void NetObject::call_client_rpc(const std::string& name, NetArgs args) {
    if (!g_active_server) { log_warn("call_client_rpc(\"" + name + "\"): not running as a server"); return; }
    nlohmann::json msg = { {"t", "rpc"}, {"id", id_}, {"name", name}, {"args", args} };
    for (auto& c : NetObjectAccess::server_impl(*g_active_server).conns) net_send_json(c.fd, msg);
}

// --- spawn / despawn (server-only) -----------------------------------------

NetObject* net_spawn(const std::string& class_name) {
    if (!g_active_server) { log_warn("net_spawn(\"" + class_name + "\"): no active server"); return nullptr; }
    auto it = net_class_registry().find(class_name);
    if (it == net_class_registry().end()) { log_error("net_spawn: unregistered class \"" + class_name + "\""); return nullptr; }
    std::unique_ptr<NetObject> obj = it->second();
    uint32_t id = g_active_server->impl_->next_net_id++;
    NetObjectAccess::set_id(*obj, id, class_name);
    NetObject* raw = obj.get();
    g_active_server->impl_->objects[id] = std::move(obj);
    for (auto& c : g_active_server->impl_->conns) net_send_spawn(c.fd, *raw);
    return raw;
}

void net_despawn(NetObject* obj) {
    if (!g_active_server || !obj) return;
    uint32_t id = obj->net_id();
    nlohmann::json msg = { {"t", "despawn"}, {"id", id} };
    for (auto& c : g_active_server->impl_->conns) net_send_json(c.fd, msg);
    g_active_server->impl_->objects.erase(id);
}

NetObject* net_find(uint32_t id) {
    if (g_active_server) {
        auto& objs = NetObjectAccess::server_impl(*g_active_server).objects;
        auto it = objs.find(id);
        return it != objs.end() ? it->second.get() : nullptr;
    }
    if (g_active_client) {
        auto& objs = NetObjectAccess::client_impl(*g_active_client).objects;
        auto it = objs.find(id);
        return it != objs.end() ? it->second.get() : nullptr;
    }
    return nullptr;
}

void net_each_object(const std::function<void(NetObject&)>& fn) {
    if (g_active_server) { for (auto& [id, obj] : NetObjectAccess::server_impl(*g_active_server).objects) fn(*obj); return; }
    if (g_active_client) { for (auto& [id, obj] : NetObjectAccess::client_impl(*g_active_client).objects) fn(*obj); return; }
}

// --- NetServer --------------------------------------------------------------

NetServer::NetServer() : impl_(std::make_unique<Impl>()) {}
NetServer::~NetServer() { stop(); }

bool NetServer::listen(int port) {
    ensure_sockets_ready();
    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd == kInvalidSocket) return false;
    int yes = 1;
    setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(impl_->listen_fd, 16) != 0) {
        close_socket(impl_->listen_fd);
        impl_->listen_fd = kInvalidSocket;
        return false;
    }
    set_nonblocking(impl_->listen_fd);
    g_active_server = this;
    return true;
}

void NetServer::update() {
    if (impl_->listen_fd == kInvalidSocket) return;

    for (;;) {   // drain pending connections
        sockaddr_in cliaddr{};
        NetSockLen len = sizeof(cliaddr);
        SocketFd fd = ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&cliaddr), &len);
        if (fd == kInvalidSocket) break;
        set_nonblocking(fd);
        set_nodelay(fd);
        int cid = impl_->next_conn_id++;
        impl_->conns.push_back({fd, std::string(), cid});
        for (auto& [id, obj] : impl_->objects) net_send_spawn(fd, *obj);   // full snapshot for the newcomer
        if (on_connect) on_connect(cid);
    }

    for (size_t i = 0; i < impl_->conns.size(); ) {
        Impl::Conn& c = impl_->conns[i];
        if (!net_recv_available(c.fd, c.rx)) {
            close_socket(c.fd);
            int cid = c.id;
            impl_->conns.erase(impl_->conns.begin() + static_cast<long>(i));
            if (on_disconnect) on_disconnect(cid);
            continue;
        }
        nlohmann::json msg;
        while (net_extract_message(c.rx, msg)) {
            if (msg.value("t", std::string()) != "cmd") continue;   // the server only ever receives Commands
            uint32_t id = msg.value("id", 0u);
            std::string name = msg.value("name", std::string());
            auto it = impl_->objects.find(id);
            if (it == impl_->objects.end()) continue;
            for (auto& [n, fn] : NetObjectAccess::commands(*it->second))
                if (n == name) { fn(msg.value("args", NetArgs::object())); break; }
        }
        ++i;
    }

    for (auto& [id, obj] : impl_->objects) {   // flush dirty NetVars, one batched sync message per object
        nlohmann::json vars = nlohmann::json::object();
        for (auto& [name, field] : NetObjectAccess::fields(*obj))
            if (field.is_dirty()) { vars[name] = field.get(); field.clear_dirty(); }
        if (vars.empty()) continue;
        nlohmann::json msg = { {"t", "sync"}, {"id", id}, {"vars", vars} };
        for (auto& c : impl_->conns) net_send_json(c.fd, msg);
    }
}

void NetServer::stop() {
    if (impl_->listen_fd != kInvalidSocket) { close_socket(impl_->listen_fd); impl_->listen_fd = kInvalidSocket; }
    for (auto& c : impl_->conns) close_socket(c.fd);
    impl_->conns.clear();
    impl_->objects.clear();
    if (g_active_server == this) g_active_server = nullptr;
}

int NetServer::connection_count() const { return static_cast<int>(impl_->conns.size()); }

NetObject* NetServer::find(uint32_t id) const {
    auto it = impl_->objects.find(id);
    return it != impl_->objects.end() ? it->second.get() : nullptr;
}

// --- NetClient --------------------------------------------------------------

NetClient::NetClient() : impl_(std::make_unique<Impl>()) {}
NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const std::string& host, int port) {
    ensure_sockets_ready();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    SocketFd fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = fd != kInvalidSocket && ::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) { if (fd != kInvalidSocket) close_socket(fd); return false; }
    set_nonblocking(fd);
    set_nodelay(fd);
    impl_->fd = fd;
    impl_->connected = true;
    g_active_client = this;
    if (on_connect) on_connect();
    return true;
}

void NetClient::update() {
    if (!impl_->connected) return;
    if (!net_recv_available(impl_->fd, impl_->rx)) { disconnect(); return; }

    nlohmann::json msg;
    while (net_extract_message(impl_->rx, msg)) {
        std::string t = msg.value("t", std::string());
        uint32_t id = msg.value("id", 0u);
        if (t == "spawn") {
            std::string cls = msg.value("class", std::string());
            auto it = net_class_registry().find(cls);
            if (it == net_class_registry().end()) { log_error("net: unknown class \"" + cls + "\" (not registered on this machine)"); continue; }
            std::unique_ptr<NetObject> obj = it->second();
            NetObjectAccess::set_id(*obj, id, cls);
            nlohmann::json vars = msg.value("vars", NetArgs::object());
            for (auto& [name, field] : NetObjectAccess::fields(*obj))
                if (vars.contains(name)) field.set(vars[name]);
            impl_->objects[id] = std::move(obj);
        } else if (t == "sync") {
            auto it = impl_->objects.find(id);
            if (it == impl_->objects.end()) continue;
            // Bind to a named variable before .items() — nlohmann's items()
            // returns a proxy that references the json it was called on, and
            // calling it directly on a temporary (msg.value(...).items()) left
            // that proxy pointing at an already-destroyed temporary. Real bug,
            // found by actually running this, not by reading the code.
            nlohmann::json vars = msg.value("vars", NetArgs::object());
            for (auto& [name, val] : vars.items())
                for (auto& [n, field] : NetObjectAccess::fields(*it->second))
                    if (n == name) { field.set(val); break; }
        } else if (t == "rpc") {
            auto it = impl_->objects.find(id);
            if (it == impl_->objects.end()) continue;
            std::string name = msg.value("name", std::string());
            for (auto& [n, fn] : NetObjectAccess::client_rpcs(*it->second))
                if (n == name) { fn(msg.value("args", NetArgs::object())); break; }
        } else if (t == "despawn") {
            impl_->objects.erase(id);
        }
    }
}

void NetClient::disconnect() {
    bool was = impl_->connected;
    if (impl_->fd != kInvalidSocket) { close_socket(impl_->fd); impl_->fd = kInvalidSocket; }
    impl_->connected = false;
    impl_->objects.clear();
    if (g_active_client == this) g_active_client = nullptr;
    if (was && on_disconnect) on_disconnect();
}

bool NetClient::connected() const { return impl_->connected; }

NetObject* NetClient::find(uint32_t id) const {
    auto it = impl_->objects.find(id);
    return it != impl_->objects.end() ? it->second.get() : nullptr;
}

// --- save --------------------------------------------------------------

namespace save {
namespace {
std::map<std::string, std::string> g_data;
bool g_loaded = false;

void make_one(const std::string& p) {
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

// Create a directory and all missing parents (ignores "already exists").
void make_dirs(const std::string& path) {
    std::string cur;
    for (char c : path) {
        cur += c;
        if ((c == '/' || c == '\\') && cur.size() > 1) make_one(cur);
    }
    make_one(path);
}

std::string base_dir() {
    std::string base;
#if defined(__APPLE__)
    base = thistle_apple_writable_dir();
#elif defined(_WIN32)
    const char* ad = std::getenv("APPDATA");
    base = (ad && *ad) ? ad : ".";
    base += "\\Thistle";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = (home && *home) ? home : ".";
        base += "/.local/share";
    }
    base += "/thistle";
#endif
    const std::string title = g_state ? g_state->config.title : std::string("Thistle");
    std::string safe;
    for (char c : title) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    base += "/" + safe;
    make_dirs(base);
    return base;
}

std::string file_path() { return base_dir() + "/save.dat"; }

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o += (n == 'n') ? '\n' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(file_path());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_data[line.substr(0, eq)] = unescape(line.substr(eq + 1));
    }
}

void write() {
    std::ofstream out(file_path(), std::ios::trunc);
    for (const auto& [k, v] : g_data) out << k << '=' << escape(v) << '\n';
}
} // namespace

void set(const std::string& key, const std::string& value) {
    ensure_loaded();
    g_data[key] = value;
    write();
}
void set_int(const std::string& key, int value) { set(key, std::to_string(value)); }
void set_float(const std::string& key, float value) { set(key, std::to_string(value)); }

std::string get(const std::string& key, const std::string& fallback) {
    ensure_loaded();
    const auto it = g_data.find(key);
    return it == g_data.end() ? fallback : it->second;
}
int get_int(const std::string& key, int fallback) {
    ensure_loaded();
    const auto it = g_data.find(key);
    if (it == g_data.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}
float get_float(const std::string& key, float fallback) {
    ensure_loaded();
    const auto it = g_data.find(key);
    if (it == g_data.end()) return fallback;
    try { return std::stof(it->second); } catch (...) { return fallback; }
}
bool has(const std::string& key) { ensure_loaded(); return g_data.count(key) > 0; }
void remove(const std::string& key) { ensure_loaded(); g_data.erase(key); write(); }
void clear() { ensure_loaded(); g_data.clear(); write(); }
std::string path() { return file_path(); }
} // namespace save

void Frame::text(const std::string& str, vec2 pos, TextOpts opts) {
    FONScontext* fons = g_state->fons;
    if (!fons) return;
    int fid = g_state->default_font;
    if (opts.font.valid() && opts.font.id < static_cast<int>(g_state->font_ids.size())) {
        fid = g_state->font_ids[opts.font.id];
    }
    if (fid == FONS_INVALID) return;

    const TextLayout L = layout_text(fons, fid, str, opts);
    const float ref = opts.max_width > 0.0f ? opts.max_width : L.width;

    fonsClearState(fons);
    fonsSetFont(fons, fid);
    fonsSetSize(fons, opts.size);
    fonsSetColor(fons, sfons_rgba(float_to_byte(opts.color.r), float_to_byte(opts.color.g),
                                  float_to_byte(opts.color.b), float_to_byte(opts.color.a)));
    fonsSetAlign(fons, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);

    float y = pos.y;
    for (const std::string& line : L.lines) {
        float x = pos.x;
        if (opts.align != Align::Left) {
            const float lw = measure_line(fons, line);
            x += (opts.align == Align::Center) ? (ref - lw) * 0.5f : (ref - lw);
        }
        fonsDrawText(fons, x, y, line.c_str(), nullptr);
        y += L.line_h;
    }
}

vec2 Frame::measure_text(const std::string& str, TextOpts opts) const {
    FONScontext* fons = g_state->fons;
    if (!fons) return {};
    int fid = g_state->default_font;
    if (opts.font.valid() && opts.font.id < static_cast<int>(g_state->font_ids.size())) {
        fid = g_state->font_ids[opts.font.id];
    }
    if (fid == FONS_INVALID) return {};

    const TextLayout L = layout_text(fons, fid, str, opts);
    return vec2{L.width, static_cast<float>(L.lines.size()) * L.line_h};
}

void Frame::camera(vec2 offset) {
    // Also resets to 2D/orthographic mode in case camera3d() ran earlier this
    // frame — projection and pipeline are part of "the camera" here too.
    sgl_load_pipeline(g_state->pip);
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    sgl_translate(offset.x, offset.y, 0.0f);
    sgl_enable_texture();
}

void Frame::camera3d(const Camera3D& cam) {
    sgl_load_pipeline(g_state->pip_3d);
    sgl_matrix_mode_projection();
    sgl_load_identity();
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    sgl_perspective(sgl_rad(cam.fov_deg), aspect, cam.near_z, cam.far_z);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    sgl_lookat(cam.eye.x, cam.eye.y, cam.eye.z,
               cam.target.x, cam.target.y, cam.target.z,
               cam.up.x, cam.up.y, cam.up.z);
}

namespace {
// A single fixed key light (pre-normalized ~(0.4, 0.8, 0.5)) used to flat-
// shade the minor-3D primitives — cheap per-face brightness, not a real
// lighting model (there's no shader stage to compute one in).
constexpr float kLight3DX = 0.390f, kLight3DY = 0.781f, kLight3DZ = 0.488f;

rgba shade_face(rgba color, float nx, float ny, float nz) {
    const float ndotl = nx * kLight3DX + ny * kLight3DY + nz * kLight3DZ;
    const float shade = 0.45f + 0.55f * std::max(0.0f, ndotl); // ambient floor + diffuse
    return rgba{color.r * shade, color.g * shade, color.b * shade, color.a};
}
} // namespace

void Frame::cube(vec3 center, vec3 size, rgba color) {
    const float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
    const vec3 c[8] = {
        {center.x - hx, center.y - hy, center.z - hz}, {center.x + hx, center.y - hy, center.z - hz},
        {center.x + hx, center.y + hy, center.z - hz}, {center.x - hx, center.y + hy, center.z - hz},
        {center.x - hx, center.y - hy, center.z + hz}, {center.x + hx, center.y - hy, center.z + hz},
        {center.x + hx, center.y + hy, center.z + hz}, {center.x - hx, center.y + hy, center.z + hz},
    };
    struct Face { int a, b, c, d; float nx, ny, nz; };
    static const Face faces[6] = {
        {0, 1, 2, 3,  0.0f,  0.0f, -1.0f}, // back
        {5, 4, 7, 6,  0.0f,  0.0f,  1.0f}, // front
        {4, 0, 3, 7, -1.0f,  0.0f,  0.0f}, // left
        {1, 5, 6, 2,  1.0f,  0.0f,  0.0f}, // right
        {3, 2, 6, 7,  0.0f,  1.0f,  0.0f}, // top
        {4, 5, 1, 0,  0.0f, -1.0f,  0.0f}, // bottom
    };
    sgl_disable_texture();
    sgl_begin_triangles();
    for (const Face& fc : faces) {
        const rgba sc = shade_face(color, fc.nx, fc.ny, fc.nz);
        const vec3& v0 = c[fc.a]; const vec3& v1 = c[fc.b]; const vec3& v2 = c[fc.c]; const vec3& v3 = c[fc.d];
        sgl_v3f_c4f(v0.x, v0.y, v0.z, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_c4f(v1.x, v1.y, v1.z, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_c4f(v2.x, v2.y, v2.z, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_c4f(v0.x, v0.y, v0.z, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_c4f(v2.x, v2.y, v2.z, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_c4f(v3.x, v3.y, v3.z, sc.r, sc.g, sc.b, sc.a);
    }
    sgl_end();
    sgl_enable_texture();
}

void Frame::plane3d(vec3 center, float width_, float depth_, rgba color) {
    const float hw = width_ * 0.5f, hd = depth_ * 0.5f;
    const rgba sc = shade_face(color, 0.0f, 1.0f, 0.0f);
    const vec3 v0{center.x - hw, center.y, center.z - hd}, v1{center.x + hw, center.y, center.z - hd},
               v2{center.x + hw, center.y, center.z + hd}, v3{center.x - hw, center.y, center.z + hd};
    sgl_disable_texture();
    sgl_begin_triangles();
    sgl_v3f_c4f(v0.x, v0.y, v0.z, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_c4f(v1.x, v1.y, v1.z, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_c4f(v2.x, v2.y, v2.z, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_c4f(v0.x, v0.y, v0.z, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_c4f(v2.x, v2.y, v2.z, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_c4f(v3.x, v3.y, v3.z, sc.r, sc.g, sc.b, sc.a);
    sgl_end();
    sgl_enable_texture();
}

void Frame::line3d(vec3 a, vec3 b, rgba color) {
    sgl_disable_texture();
    sgl_begin_lines();
    sgl_v3f_c4f(a.x, a.y, a.z, color.r, color.g, color.b, color.a);
    sgl_v3f_c4f(b.x, b.y, b.z, color.r, color.g, color.b, color.a);
    sgl_end();
    sgl_enable_texture();
}

Font load_font(const std::string& path) {
    const int idx = static_cast<int>(g_state->font_ids.size());
    int fid = FONS_INVALID;
    if (g_state->fons) { // window already up: load immediately
        fid = fonsAddFont(g_state->fons, "font", path.c_str());
        if (fid != FONS_INVALID && g_state->default_font == FONS_INVALID) {
            g_state->default_font = fid;
        }
    }
    g_state->font_paths.push_back(path);
    g_state->font_ids.push_back(fid);
    return Font{idx};
}

Texture load_texture(const std::string& path) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        return Texture{}; // invalid handle; draws nothing
    }
    TextureRecord rec;
    rec.pixels = pixels;
    rec.w = w;
    rec.h = h;
    rec.path = path;
    const int id = static_cast<int>(g_state->textures.size());
    g_state->textures.push_back(rec);
    return Texture{id, w, h};
}

void unload_texture(Texture& tex) {
    if (tex.id >= 0 && tex.id < static_cast<int>(g_state->textures.size())) {
        TextureRecord& rec = g_state->textures[tex.id];
        if (rec.pixels) { stbi_image_free(rec.pixels); rec.pixels = nullptr; }
        if (rec.view.id != SG_INVALID_ID) { sg_destroy_view(rec.view); rec.view = {}; }
        if (rec.img.id != SG_INVALID_ID) { sg_destroy_image(rec.img); rec.img = {}; }
        rec.uploaded = false;
        rec.w = rec.h = 0;
    }
    tex = Texture{};
}

void reload_texture(Texture tex) {
    if (tex.id < 0 || tex.id >= static_cast<int>(g_state->textures.size())) return;
    TextureRecord& rec = g_state->textures[tex.id];
    if (rec.path.empty()) return;
    int w = 0, h = 0, channels = 0;
    unsigned char* px = stbi_load(rec.path.c_str(), &w, &h, &channels, 4);
    if (!px) { log_warn("reload_texture: could not read " + rec.path); return; }
    if (rec.pixels) stbi_image_free(rec.pixels);
    if (rec.view.id != SG_INVALID_ID) { sg_destroy_view(rec.view); rec.view = {}; }
    if (rec.img.id != SG_INVALID_ID) { sg_destroy_image(rec.img); rec.img = {}; }
    rec.pixels = px;
    rec.w = w;
    rec.h = h;
    rec.uploaded = false; // re-uploads lazily on next draw
}

void play_sound(const std::string& path) {
    if (!g_state->audio_ready) return;
    ma_sound_group* group = g_state->sfx_group_ready ? &g_state->sfx_group : nullptr;
    ma_engine_play_sound(&g_state->audio, path.c_str(), group);
}

void play_music(const std::string& path, float volume) {
    if (!g_state->audio_ready) return;
    if (g_state->music_ready) {
        ma_sound_stop(&g_state->music);
        ma_sound_uninit(&g_state->music);
        g_state->music_ready = false;
    }
    if (ma_sound_init_from_file(&g_state->audio, path.c_str(), MA_SOUND_FLAG_STREAM,
                                nullptr, nullptr, &g_state->music) == MA_SUCCESS) {
        ma_sound_set_looping(&g_state->music, MA_TRUE);
        ma_sound_set_volume(&g_state->music, volume);
        ma_sound_start(&g_state->music);
        g_state->music_ready = true;
    }
}

void stop_music() {
    if (g_state->music_ready) {
        ma_sound_stop(&g_state->music);
        ma_sound_uninit(&g_state->music);
        g_state->music_ready = false;
    }
}

void set_music_volume(float volume) {
    if (g_state->music_ready) ma_sound_set_volume(&g_state->music, volume);
}

void set_sfx_volume(float volume) {
    if (g_state->sfx_group_ready) ma_sound_group_set_volume(&g_state->sfx_group, volume);
}

void set_master_volume(float volume) {
    if (g_state->audio_ready) ma_engine_set_volume(&g_state->audio, volume);
}

void set_post_effect(PostEffect effect, float intensity) {
    g_state->post_effect = effect;
    g_state->post_intensity = intensity;
}

App::App(AppConfig config) {
    static EngineState state;
    state = EngineState{};
    state.config = std::move(config);
    g_state = &state;
#if defined(__APPLE__)
    // On iOS the app bundle is the asset root. Do this in the constructor so it
    // runs before the user's load_texture()/load_font() calls in main().
    thistle_ios_set_working_dir(); // no-op on macOS
#endif
}

App::App(std::string title, int width, int height)
    : App(AppConfig{std::move(title), width, height}) {}

App& App::start(std::function<void()> fn) {
    g_state->on_start = std::move(fn);
    return *this;
}

App& App::update(std::function<void(Frame)> fn) {
    g_state->on_update = std::move(fn);
    return *this;
}

App& App::stop(std::function<void()> fn) {
    g_state->on_stop = std::move(fn);
    return *this;
}

App& App::scene(const std::string& name, Scene s) {
    g_state->scenes[name] = std::move(s);
    if (g_state->current_scene.empty() && !g_state->has_pending) {
        g_state->pending_scene = name; // first registered scene is the initial one
        g_state->has_pending = true;
    }
    return *this;
}

void set_scene(const std::string& name) {
    g_state->pending_scene = name;
    g_state->has_pending = true;
}

int App::run() {
    // Load the window/dock icon (if any) up front; sokol reads the pixels while
    // sapp_run() starts up, so the buffer must outlive that call.
    unsigned char* icon_px = nullptr;
    int iw = 0, ih = 0, ic = 0;
    if (!g_state->config.icon.empty()) {
        icon_px = stbi_load(g_state->config.icon.c_str(), &iw, &ih, &ic, 4);
    }

    sapp_desc d = {};
    d.init_cb = init_cb;
    d.frame_cb = frame_cb;
    d.cleanup_cb = cleanup_cb;
    d.event_cb = event_cb;
    d.width = g_state->config.width;
    d.height = g_state->config.height;
    d.window_title = g_state->config.title.c_str();
    d.logger.func = slog_func;
    d.enable_clipboard = true;
    d.clipboard_size = 16384;
    if (icon_px) {
        d.icon.sokol_default = false;
        d.icon.images[0].width = iw;
        d.icon.images[0].height = ih;
        d.icon.images[0].pixels.ptr = icon_px;
        d.icon.images[0].pixels.size = static_cast<size_t>(iw) * ih * 4;
    } else {
        d.icon.sokol_default = true;
    }

    sapp_run(&d);

    if (icon_px) stbi_image_free(icon_px);
    return 0;
}

} // namespace thistle
