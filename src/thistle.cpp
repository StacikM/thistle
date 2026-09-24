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
#include "thistle_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <box2d/box2d.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
// Must come before any windows.h-family include: windows.h defines min/max
// as macros unless told not to, which mangles every std::min/std::max call
// in this file (MSVC error C2589, found by CI actually building on real
// Windows — see docs/building.md's running theme).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbghelp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")   // MessageBoxA, for the crash popup — MSVC only, see CMakeLists.txt for MinGW
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
#include <signal.h>
#if !defined(__ANDROID__)
// Bionic (Android's libc) doesn't provide execinfo.h/backtrace() — see
// crash_backtrace()'s Android branch, which skips stack traces entirely.
#include <execinfo.h>
#endif
#if defined(__ANDROID__)
#include <android/native_activity.h>
#include <android/asset_manager.h>
#endif
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
// StoreKit bridge (see ios_support.mm). Products/events cross the C boundary
// as JSON strings — thistle.cpp parses them — rather than a pile of
// individually-fragile const char* out-params.
extern "C" int         thistle_iap_can_make_payments(void);
extern "C" void        thistle_iap_fetch_products(const char* ids_json);
extern "C" int         thistle_iap_products_ready(void);
extern "C" const char* thistle_iap_products_json(void);
extern "C" void        thistle_iap_purchase(const char* product_id);
extern "C" void        thistle_iap_restore_purchases(void);
extern "C" void        thistle_iap_finish_transaction(const char* transaction_id);
extern "C" const char* thistle_iap_poll_event_json(void);
#elif defined(_WIN32)
// Real async HTTP on Windows too (see src/win32_support.cpp) — same poll-based
// interface as the Apple side, just backed by WinHTTP instead of NSURLSession.
extern "C" void* thistle_http_start(const char* method, const char* url, const char* body);
extern "C" int   thistle_http_poll(void* h, int* status, const char** body, int* len);
extern "C" void  thistle_http_free(void* h);
#endif

#if defined(__ANDROID__)
// App::run() has no sapp_run() call to block in on Android — there's no
// process main() for it to block from. Instead it stashes the desc here and
// sokol_main() (defined at the bottom of this file, called by sokol_app.h's
// own ANativeActivity_onCreate) hands it back. See THISTLE_MAIN in thistle.hpp.
static sapp_desc g_thistle_android_desc;
#endif

namespace thistle {
namespace {

#if defined(__ANDROID__)
// --- Android asset loading ----------------------------------------------
// Everywhere else, game assets are plain files next to the executable
// ("assets/foo.png"). On Android they're packed inside the APK's zip and
// only reachable through AAssetManager — there's no fopen()-able path at
// all. examples/android/build_apk.sh packages the game's assets/ directory
// contents directly at the APK assets root (see its `aapt add`), so a path
// like "assets/foo.png" needs that literal prefix stripped before it means
// anything to AAssetManager.
std::string android_asset_path(const std::string& path) {
    const std::string prefix = "assets/";
    return path.compare(0, prefix.size(), prefix) == 0 ? path.substr(prefix.size()) : path;
}

bool android_read_asset(const std::string& path, std::vector<unsigned char>& out) {
    const auto* activity = static_cast<const ANativeActivity*>(sapp_android_get_native_activity());
    if (!activity || !activity->assetManager) return false;
    AAsset* asset = AAssetManager_open(activity->assetManager, android_asset_path(path).c_str(), AASSET_MODE_BUFFER);
    if (!asset) return false;
    const off_t len = AAsset_getLength(asset);
    out.resize(static_cast<size_t>(len));
    const int n = len > 0 ? AAsset_read(asset, out.data(), out.size()) : 0;
    AAsset_close(asset);
    return n == static_cast<int>(out.size());
}

// Lets fontstash own the buffer (matches fonsAddFont's own file-loading path,
// which also hands fonsAddFontMem a malloc'd buffer with freeData=1) instead
// of us tracking its lifetime separately.
int android_fons_add_font(FONScontext* fons, const std::string& path) {
    std::vector<unsigned char> bytes;
    if (!android_read_asset(path, bytes)) return FONS_INVALID;
    unsigned char* data = static_cast<unsigned char*>(std::malloc(bytes.size()));
    if (!data) return FONS_INVALID;
    std::memcpy(data, bytes.data(), bytes.size());
    return fonsAddFontMem(fons, "font", data, static_cast<int>(bytes.size()), 1);
}

// miniaudio VFS backed by AAssetManager, so play_sound()/play_music() (and
// anything else that goes through the engine's ma_resource_manager) work
// unchanged on Android — see where this gets installed in init_cb() below.
// Read-only: games don't write audio assets, so onWrite/onOpenW are unused.
ma_result android_ma_vfs_open(ma_vfs*, const char* filePath, ma_uint32 openMode, ma_vfs_file* pFile) {
    if ((openMode & MA_OPEN_MODE_WRITE) != 0) return MA_NOT_IMPLEMENTED;
    const auto* activity = static_cast<const ANativeActivity*>(sapp_android_get_native_activity());
    if (!activity || !activity->assetManager) return MA_ERROR;
    AAsset* asset = AAssetManager_open(activity->assetManager, android_asset_path(filePath).c_str(), AASSET_MODE_RANDOM);
    if (!asset) return MA_DOES_NOT_EXIST;
    *pFile = reinterpret_cast<ma_vfs_file>(asset);
    return MA_SUCCESS;
}
ma_result android_ma_vfs_close(ma_vfs*, ma_vfs_file file) {
    AAsset_close(reinterpret_cast<AAsset*>(file));
    return MA_SUCCESS;
}
ma_result android_ma_vfs_read(ma_vfs*, ma_vfs_file file, void* dst, size_t sizeInBytes, size_t* pBytesRead) {
    const int n = AAsset_read(reinterpret_cast<AAsset*>(file), dst, sizeInBytes);
    if (n < 0) return MA_ERROR;
    if (pBytesRead) *pBytesRead = static_cast<size_t>(n);
    return (n == 0 && sizeInBytes > 0) ? MA_AT_END : MA_SUCCESS;
}
ma_result android_ma_vfs_seek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    const int whence = origin == ma_seek_origin_start ? SEEK_SET : origin == ma_seek_origin_end ? SEEK_END : SEEK_CUR;
    return AAsset_seek(reinterpret_cast<AAsset*>(file), static_cast<off_t>(offset), whence) < 0 ? MA_ERROR : MA_SUCCESS;
}
ma_result android_ma_vfs_tell(ma_vfs*, ma_vfs_file file, ma_int64* pCursor) {
    AAsset* asset = reinterpret_cast<AAsset*>(file);
    if (pCursor) *pCursor = static_cast<ma_int64>(AAsset_getLength(asset) - AAsset_getRemainingLength(asset));
    return MA_SUCCESS;
}
ma_result android_ma_vfs_info(ma_vfs*, ma_vfs_file file, ma_file_info* pInfo) {
    if (pInfo) pInfo->sizeInBytes = static_cast<ma_uint64>(AAsset_getLength(reinterpret_cast<AAsset*>(file)));
    return MA_SUCCESS;
}
ma_vfs_callbacks g_android_ma_vfs = {
    android_ma_vfs_open, nullptr /* onOpenW: never used off Windows */, android_ma_vfs_close,
    android_ma_vfs_read, nullptr /* onWrite: assets are read-only */, android_ma_vfs_seek,
    android_ma_vfs_tell, android_ma_vfs_info,
};
ma_resource_manager g_android_ma_resource_manager;
#endif

// stb_image only knows fopen(); on Android that means reading the asset into
// memory ourselves first. Everywhere else this is just stbi_load().
unsigned char* thistle_stbi_load(const std::string& path, int* w, int* h, int* channels, int req_comp) {
#if defined(__ANDROID__)
    std::vector<unsigned char> bytes;
    if (!android_read_asset(path, bytes)) return nullptr;
    return stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), w, h, channels, req_comp);
#else
    return stbi_load(path.c_str(), w, h, channels, req_comp);
#endif
}

// Same story as thistle_stbi_load, for the text-based tilemap formats
// (Tiled CSV/JSON) that otherwise read via std::ifstream.
bool thistle_read_text_asset(const std::string& path, std::string& out) {
#if defined(__ANDROID__)
    std::vector<unsigned char> bytes;
    if (!android_read_asset(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
#else
    std::ifstream file(path);
    if (!file) return false;
    std::stringstream buf;
    buf << file.rdbuf();
    out = buf.str();
    return true;
#endif
}

struct TextureRecord {
    unsigned char* pixels = nullptr; // CPU copy, freed after GPU upload
    int w = 0;
    int h = 0;
    sg_image img = {};
    sg_view view = {};
    bool uploaded = false;
    std::string path; // original file, for reload_texture
};

// A single loaded-mesh vertex — position, normal (for the fixed key light),
// and a UV for the optional textured mesh3d() overload.
struct MeshVertex {
    vec3 pos;
    vec3 normal;
    float u = 0.0f;
    float v = 0.0f;
};

// A flat triangle list (3 MeshVertex per triangle) — everything load_mesh()
// parses out of a .obj, with no further structure (no per-object/material
// grouping) since Frame::mesh3d() draws the whole thing in one pass anyway.
struct MeshRecord {
    std::vector<MeshVertex> tris;
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
    std::vector<MeshRecord> meshes;
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
    float scroll_y = 0.0f; // accumulated this frame, reset after each frame like key_pressed

    bool text_capturing = false;   // on-screen keyboard active, accumulating chars
    std::string text_buffer;
    std::size_t text_max = 40;

    ThistleGamepad pad_cur{};
    ThistleGamepad pad_prev{};

    uint64_t frame_index = 0;

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

// Box-filtered mip chain, so a texture on a far-away 3D surface averages
// out instead of shimmering. The 2D sampler clamps to level 0 (max_lod), so
// sprites draw exactly as they did before mips existed.
std::vector<std::vector<unsigned char>> build_mips(const unsigned char* px, int w, int h) {
    std::vector<std::vector<unsigned char>> levels;
    const unsigned char* src = px;
    while ((w > 1 || h > 1) && static_cast<int>(levels.size()) + 1 < SG_MAX_MIPMAPS) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<unsigned char> dst(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                for (int c = 0; c < 4; ++c) {
                    const int sum = src[(y0 * w + x0) * 4 + c] + src[(y0 * w + x1) * 4 + c] +
                                    src[(y1 * w + x0) * 4 + c] + src[(y1 * w + x1) * 4 + c];
                    dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<unsigned char>((sum + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
        src = levels.back().data();
        w = nw;
        h = nh;
    }
    return levels;
}

void ensure_uploaded(TextureRecord& rec) {
    if (rec.uploaded || rec.pixels == nullptr) return;
    const std::vector<std::vector<unsigned char>> mips = build_mips(rec.pixels, rec.w, rec.h);
    sg_image_desc desc = {};
    desc.width = rec.w;
    desc.height = rec.h;
    desc.num_mipmaps = 1 + static_cast<int>(mips.size());
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    desc.data.mip_levels[0].ptr = rec.pixels;
    desc.data.mip_levels[0].size = static_cast<size_t>(rec.w) * rec.h * 4;
    for (size_t i = 0; i < mips.size(); ++i) {
        desc.data.mip_levels[i + 1].ptr = mips[i].data();
        desc.data.mip_levels[i + 1].size = mips[i].size();
    }
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
    smp.max_lod = 0.0f;
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

    detail::three_setup();

#if defined(__APPLE__)
    thistle_ios_init_audio_session(); // no-op on macOS
#endif
#if defined(__ANDROID__)
    // Route every audio file load through AAssetManager instead of fopen —
    // see android_ma_vfs_open() above. play_sound()/play_music() need no
    // changes themselves; this is transparent to everything above them.
    ma_resource_manager_config rm_config = ma_resource_manager_config_init();
    rm_config.pVFS = reinterpret_cast<ma_vfs*>(&g_android_ma_vfs);
    bool audio_ok = ma_resource_manager_init(&rm_config, &g_android_ma_resource_manager) == MA_SUCCESS;
    if (audio_ok) {
        ma_engine_config engine_config = ma_engine_config_init();
        engine_config.pResourceManager = &g_android_ma_resource_manager;
        audio_ok = ma_engine_init(&engine_config, &g_state->audio) == MA_SUCCESS;
    }
    if (audio_ok) {
#else
    if (ma_engine_init(nullptr, &g_state->audio) == MA_SUCCESS) {
#endif
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
#if defined(__ANDROID__)
            const int fid = android_fons_add_font(g_state->fons, g_state->font_paths[i]);
#else
            const int fid = fonsAddFont(g_state->fons, "font", g_state->font_paths[i].c_str());
#endif
            g_state->font_ids[i] = fid;
            if (fid != FONS_INVALID && g_state->default_font == FONS_INVALID) {
                g_state->default_font = fid;
            }
        }
    }

    if (g_state->on_start) g_state->on_start();
}

// --- post-processing helpers -------------------------------------------

// One post-effect shader per backend, all doing the exact same thing: sample
// a fullscreen triangle (no vertex buffer — the classic (vid<<1)&2 / vid&2
// trick) and apply whichever effect `params.x` (the PostEffect enum value)
// selects, at strength `params.y`. Keeping four near-identical shaders
// instead of one is the price of not depending on a build-time cross-compiler
// like sokol-shdc — see ensure_post() for how each one gets wired up.
#if defined(__APPLE__)
const char* POST_SHADER = R"MSL(
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
#elif defined(_WIN32)
const char* POST_SHADER = R"HLSL(
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
vs_out vs_main(uint vid : SV_VertexID) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    vs_out o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
cbuffer params : register(b0) { float4 params; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 fs_main(vs_out inp) : SV_Target {
    float4 c = tex.Sample(smp, inp.uv);
    int mode = int(params.x);
    float k = params.y;
    if (mode == 1) { float g = dot(c.rgb, float3(0.299, 0.587, 0.114)); c.rgb = lerp(c.rgb, float3(g, g, g), k); }
    else if (mode == 2) { float2 d = inp.uv - 0.5; float v = 1.0 - dot(d, d) * k * 3.0; c.rgb *= saturate(v); }
    else if (mode == 3) { float o = 0.004 * k; c.r = tex.Sample(smp, inp.uv + float2(o, 0.0)).r; c.b = tex.Sample(smp, inp.uv - float2(o, 0.0)).b; }
    else if (mode == 4) { c.rgb = lerp(c.rgb, float3(1.0, 1.0, 1.0), k); }
    else if (mode == 5) { c.rgb *= (1.0 - k); }
    return c;
}
)HLSL";
#else
// GLCore (Linux) and GLES3 (Android/Web) share the same GLSL body — only the
// #version/precision preamble differs, so it's spliced on in ensure_post().
const char* POST_GLSL_VS_BODY = R"GLSL(
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    // No V flip here, unlike the Metal/HLSL versions: a GL render target's
    // row 0 is the bottom of the image, so clip-space up already matches
    // texture-space up. Copying the flip from the other backends rendered
    // every post effect upside down on Linux — caught by actually running
    // it (Xvfb + Mesa llvmpipe), after it had only ever been compile-checked.
    uv = p;
}
)GLSL";
const char* POST_GLSL_FS_BODY = R"GLSL(
in vec2 uv;
out vec4 frag_color;
uniform sampler2D tex_smp;
uniform vec4 params; // x=mode y=intensity z=time w=unused
void main() {
    vec4 c = texture(tex_smp, uv);
    int mode = int(params.x);
    float k = params.y;
    if (mode == 1) { float g = dot(c.rgb, vec3(0.299, 0.587, 0.114)); c.rgb = mix(c.rgb, vec3(g), k); }
    else if (mode == 2) { vec2 d = uv - 0.5; float v = 1.0 - dot(d, d) * k * 3.0; c.rgb *= clamp(v, 0.0, 1.0); }
    else if (mode == 3) { float o = 0.004 * k; c.r = texture(tex_smp, uv + vec2(o, 0.0)).r; c.b = texture(tex_smp, uv - vec2(o, 0.0)).b; }
    else if (mode == 4) { c.rgb = mix(c.rgb, vec3(1.0), k); }
    else if (mode == 5) { c.rgb *= (1.0 - k); }
    frag_color = c;
}
)GLSL";
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
// true when everything is ready to render through.
bool ensure_post(EngineState& s, int w, int h) {
    const sg_environment env = sglue_environment();
    if (!s.post_pipeline_ready) {
        sg_sampler_desc smp = {};
        smp.min_filter = SG_FILTER_LINEAR;
        smp.mag_filter = SG_FILTER_LINEAR;
        smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        s.post_sampler = sg_make_sampler(&smp);

        sg_shader_desc sd = {};
        sd.uniform_blocks[0].stage = SG_SHADERSTAGE_FRAGMENT;
        sd.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
        sd.views[0].texture.image_type = SG_IMAGETYPE_2D;
        sd.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
        sd.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
        sd.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
        sd.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
        sd.texture_sampler_pairs[0].view_slot = 0;
        sd.texture_sampler_pairs[0].sampler_slot = 0;

#if defined(__APPLE__)
        sd.vertex_func.source = POST_SHADER;   sd.vertex_func.entry = "vs_main";
        sd.fragment_func.source = POST_SHADER; sd.fragment_func.entry = "fs_main";
        sd.uniform_blocks[0].size = 16;
        sd.uniform_blocks[0].msl_buffer_n = 0;
        sd.views[0].texture.msl_texture_n = 0;
        sd.samplers[0].msl_sampler_n = 0;
#elif defined(_WIN32)
        sd.vertex_func.source = POST_SHADER;   sd.vertex_func.entry = "vs_main";
        sd.fragment_func.source = POST_SHADER; sd.fragment_func.entry = "fs_main";
        sd.uniform_blocks[0].size = 16;
        sd.uniform_blocks[0].hlsl_register_b_n = 0;
        sd.views[0].texture.hlsl_register_t_n = 0;
        sd.samplers[0].hlsl_register_s_n = 0;
#else
        // GLCore (Linux) vs GLES3 (Android/Web) only differ in the
        // #version/precision preamble — see the shader-source comment above.
        #if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
        static const std::string vs_src = "#version 300 es\n" + std::string(POST_GLSL_VS_BODY);
        static const std::string fs_src = "#version 300 es\nprecision mediump float;\n" + std::string(POST_GLSL_FS_BODY);
        #else
        static const std::string vs_src = "#version 410 core\n" + std::string(POST_GLSL_VS_BODY);
        static const std::string fs_src = "#version 410 core\n" + std::string(POST_GLSL_FS_BODY);
        #endif
        sd.vertex_func.source = vs_src.c_str();   sd.vertex_func.entry = "main";
        sd.fragment_func.source = fs_src.c_str(); sd.fragment_func.entry = "main";
        sd.uniform_blocks[0].size = 16;
        sd.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
        sd.uniform_blocks[0].glsl_uniforms[0].glsl_name = "params";
        sd.texture_sampler_pairs[0].glsl_name = "tex_smp";
#endif
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
#elif defined(_WIN32)
    thistle_win32_poll_gamepad(&g_state->pad_cur);
#elif defined(__linux__) && !defined(__ANDROID__)
    thistle_linux_poll_gamepad(&g_state->pad_cur);
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

    const bool use_post = g_state->post_effect != PostEffect::None && ensure_post(*g_state, w, h);

    if (g_state->fons) sfons_flush(g_state->fons); // upload any newly rasterized glyphs
    detail::three_before_passes();

    if (use_post) {
        // Pass 1: render the scene into the offscreen texture.
        sg_pass off = {};
        off.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        off.action.colors[0].clear_value = clear;
        off.attachments.colors[0] = g_state->off_color_att;
        off.attachments.depth_stencil = g_state->off_depth_att;
        sg_begin_pass(&off);
        detail::three_draw_layers();
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
        detail::three_draw_layers();
        sg_end_pass();
        sg_commit();
    }
    detail::three_end_frame();
    ++g_state->frame_index;

    // Edge-triggered input is only true for the frame it happened.
    for (bool& p : g_state->key_pressed) p = false;
    for (bool& p : g_state->mouse_pressed) p = false;
    g_state->scroll_y = 0.0f;
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
    detail::three_shutdown();
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
        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            g_state->scroll_y += e->scroll_y;
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

void Frame::rounded_rect(vec2 pos, vec2 size, float radius, rgba color, int segments_per_corner) {
    radius = std::min({radius, size.x * 0.5f, size.y * 0.5f});
    if (radius <= 0.0f) { rect(pos, size, color); return; }
    if (segments_per_corner < 1) segments_per_corner = 1;

    const float x0 = pos.x, y0 = pos.y, x1 = pos.x + size.x, y1 = pos.y + size.y;

    sgl_texture(g_state->white_view, g_state->sampler);
    sgl_c4f(color.r, color.g, color.b, color.a);
    sgl_begin_triangles();

    // Body, decomposed into 3 rects (avoiding the 4 rounded corners) plus 4
    // quarter-circle fans — the standard gap-free tiling for a filled
    // rounded rect with no shader/SDF involved, just triangles.
    auto quad = [](float ax, float ay, float bx, float by) {
        sgl_v2f_t2f(ax, ay, 0.0f, 0.0f); sgl_v2f_t2f(bx, ay, 0.0f, 0.0f); sgl_v2f_t2f(bx, by, 0.0f, 0.0f);
        sgl_v2f_t2f(ax, ay, 0.0f, 0.0f); sgl_v2f_t2f(bx, by, 0.0f, 0.0f); sgl_v2f_t2f(ax, by, 0.0f, 0.0f);
    };
    quad(x0 + radius, y0, x1 - radius, y1);              // center band, full height
    quad(x0, y0 + radius, x0 + radius, y1 - radius);      // left band
    quad(x1 - radius, y0 + radius, x1, y1 - radius);      // right band

    auto corner_fan = [&](float cx, float cy, float start_angle) {
        const float step = 1.5707963f / static_cast<float>(segments_per_corner); // 90 degrees
        for (int i = 0; i < segments_per_corner; ++i) {
            const float a0 = start_angle + static_cast<float>(i) * step;
            const float a1 = start_angle + static_cast<float>(i + 1) * step;
            sgl_v2f_t2f(cx, cy, 0.0f, 0.0f);
            sgl_v2f_t2f(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius, 0.0f, 0.0f);
            sgl_v2f_t2f(cx + std::cos(a1) * radius, cy + std::sin(a1) * radius, 0.0f, 0.0f);
        }
    };
    corner_fan(x0 + radius, y0 + radius, 3.14159265f);  // top-left:     180 -> 270 deg
    corner_fan(x1 - radius, y0 + radius, 4.71238898f);  // top-right:    270 -> 360 deg
    corner_fan(x1 - radius, y1 - radius, 0.0f);         // bottom-right:   0 ->  90 deg
    corner_fan(x0 + radius, y1 - radius, 1.57079633f);  // bottom-left:   90 -> 180 deg

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

void Frame::sprite9(Texture tex, vec2 pos, vec2 size, float border, rgba tint) {
    if (!tex.valid()) return;
    TextureRecord& rec = g_state->textures[tex.id];
    ensure_uploaded(rec);
    if (rec.img.id == SG_INVALID_ID) return;

    const float texW = static_cast<float>(tex.width);
    const float texH = static_cast<float>(tex.height);
    if (texW <= 0.0f || texH <= 0.0f) return;

    // Clamp so the border can't exceed half the target box or half the
    // source texture — either would make the 9 slices overlap/invert.
    const float b = std::min({border, size.x * 0.5f, size.y * 0.5f, texW * 0.5f, texH * 0.5f});
    const float bu = b / texW;
    const float bv = b / texH;

    const float x[4] = {pos.x, pos.x + b, pos.x + size.x - b, pos.x + size.x};
    const float y[4] = {pos.y, pos.y + b, pos.y + size.y - b, pos.y + size.y};
    const float u[4] = {0.0f, bu, 1.0f - bu, 1.0f};
    const float v[4] = {0.0f, bv, 1.0f - bv, 1.0f};

    sgl_texture(rec.view, g_state->sampler);
    sgl_c4f(tint.r, tint.g, tint.b, tint.a);
    sgl_begin_quads();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            sgl_v2f_t2f(x[col],     y[row],     u[col],     v[row]);
            sgl_v2f_t2f(x[col + 1], y[row],     u[col + 1], v[row]);
            sgl_v2f_t2f(x[col + 1], y[row + 1], u[col + 1], v[row + 1]);
            sgl_v2f_t2f(x[col],     y[row + 1], u[col],     v[row + 1]);
        }
    }
    sgl_end();
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

float Frame::mouse_scroll() const {
    return g_state->scroll_y;
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

namespace {
// Defined later, next to the other minor-3D helpers (shade_face, cube_corners,
// etc.) — forward-declared here since Node::draw_meshes_rec() needs it before
// that point in the file. Despite the name (it exists for rotating a face
// normal by the mesh3d() Euler-angle convention), it's just a generic vec3
// rotation and Node::draw_meshes_rec() reuses it for position offsets too.
vec3 rotate_normal(vec3 n, vec3 rot);
} // namespace

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

void Node::draw_meshes(Frame& f) const {
    draw_meshes_rec(f, vec3{0.0f, 0.0f, 0.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 1.0f, 1.0f});
}

void Node::draw_meshes_rec(Frame& f, vec3 parent_pos, vec3 parent_rot, vec3 parent_scale) const {
    if (!visible) return;
    // rotate_normal() is a generic Euler-angle vector rotation despite its
    // name (defined further down next to the other minor-3D helpers) —
    // reused here to rotate a position offset, not just a normal.
    const vec3 scaled_local{mesh_pos.x * parent_scale.x, mesh_pos.y * parent_scale.y, mesh_pos.z * parent_scale.z};
    const vec3 world_pos = parent_pos + rotate_normal(scaled_local, parent_rot);
    const vec3 world_rot = parent_rot + mesh_rotation;
    const vec3 world_scale{parent_scale.x * mesh_scale.x, parent_scale.y * mesh_scale.y, parent_scale.z * mesh_scale.z};

    if (mesh.valid()) {
        if (mesh_texture.valid()) f.mesh3d(mesh, world_pos, world_rot, world_scale, mesh_texture, mesh_tint);
        else f.mesh3d(mesh, world_pos, world_rot, world_scale, mesh_tint);
    }
    for (const auto& c : children_) c->draw_meshes_rec(f, world_pos, world_rot, world_scale);
}

WorldMeshTransform Node::world_mesh_transform() const {
    // Walk from the root down to `this`, composing each level the same way
    // draw_meshes_rec() does — same math, just building up a result to
    // return instead of feeding it straight into f.mesh3d().
    std::vector<const Node*> chain;
    for (const Node* cur = this; cur; cur = cur->parent()) chain.push_back(cur);
    WorldMeshTransform t;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Node* cur = *it;
        const vec3 scaled_local{cur->mesh_pos.x * t.scale.x, cur->mesh_pos.y * t.scale.y, cur->mesh_pos.z * t.scale.z};
        t.pos = t.pos + rotate_normal(scaled_local, t.rotation);
        t.rotation = t.rotation + cur->mesh_rotation;
        t.scale = {t.scale.x * cur->mesh_scale.x, t.scale.y * cur->mesh_scale.y, t.scale.z * cur->mesh_scale.z};
    }
    return t;
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

// --- scene serialization -------------------------------------------------

namespace {
nlohmann::json node_to_json(const Node& n) {
    nlohmann::json j;
    j["pos"] = n.pos;
    j["scale"] = n.scale;
    j["rotation"] = n.rotation;
    j["alpha"] = n.alpha;
    j["visible"] = n.visible;
    j["name"] = n.name;
    j["sprite_path"] = n.sprite_path;
    j["sprite_size"] = n.sprite_size;
    j["sprite_tint"] = n.sprite_tint;
    j["sprite_src"] = n.sprite_src;
    j["mesh_path"] = n.mesh_path;
    j["mesh_pos"] = n.mesh_pos;
    j["mesh_rotation"] = n.mesh_rotation;
    j["mesh_scale"] = n.mesh_scale;
    j["mesh_tint"] = n.mesh_tint;
    j["mesh_texture_path"] = n.mesh_texture_path;
    j["mesh_prim"] = static_cast<int>(n.mesh_prim);
    auto children = nlohmann::json::array();
    for (std::size_t i = 0; i < n.child_count(); ++i) children.push_back(node_to_json(*n.child(i)));
    j["children"] = std::move(children);
    return j;
}

// tex_cache/mesh_cache avoid reloading (and re-uploading to the GPU) the same
// file once per node when many nodes share a sprite_path/mesh_path/
// mesh_texture_path — tex_cache is shared between sprite and mesh textures
// since both just key off the same path -> Texture mapping.
std::unique_ptr<Node> node_from_json(const nlohmann::json& j, std::unordered_map<std::string, Texture>& tex_cache,
                                      std::unordered_map<std::string, Mesh>& mesh_cache) {
    auto n = std::make_unique<Node>();
    if (j.contains("pos")) j.at("pos").get_to(n->pos);
    if (j.contains("scale")) j.at("scale").get_to(n->scale);
    n->rotation = j.value("rotation", 0.0f);
    n->alpha = j.value("alpha", 1.0f);
    n->visible = j.value("visible", true);
    n->name = j.value("name", std::string());
    n->sprite_path = j.value("sprite_path", std::string());
    if (j.contains("sprite_size")) j.at("sprite_size").get_to(n->sprite_size);
    if (j.contains("sprite_tint")) j.at("sprite_tint").get_to(n->sprite_tint);
    if (j.contains("sprite_src")) j.at("sprite_src").get_to(n->sprite_src);
    n->mesh_path = j.value("mesh_path", std::string());
    if (j.contains("mesh_pos")) j.at("mesh_pos").get_to(n->mesh_pos);
    if (j.contains("mesh_rotation")) j.at("mesh_rotation").get_to(n->mesh_rotation);
    if (j.contains("mesh_scale")) j.at("mesh_scale").get_to(n->mesh_scale);
    if (j.contains("mesh_tint")) j.at("mesh_tint").get_to(n->mesh_tint);
    n->mesh_texture_path = j.value("mesh_texture_path", std::string());
    n->mesh_prim = static_cast<Prim>(j.value("mesh_prim", 0));

    if (!n->sprite_path.empty()) {
        auto it = tex_cache.find(n->sprite_path);
        if (it == tex_cache.end()) it = tex_cache.emplace(n->sprite_path, load_texture(n->sprite_path)).first;
        n->sprite = it->second;
    }
    if (n->mesh_prim != Prim::None) {
        // Not cached (unlike mesh_path/mesh_texture_path below) — these are
        // generated, not loaded from a file, so there's no path to key a
        // cache on; each primitive node gets its own fresh Mesh, same as
        // calling make_cube_mesh() directly would.
        switch (n->mesh_prim) {
            case Prim::Cube:     n->mesh = make_cube_mesh(); break;
            case Prim::Sphere:   n->mesh = make_sphere_mesh(); break;
            case Prim::Cylinder: n->mesh = make_cylinder_mesh(); break;
            case Prim::Cone:     n->mesh = make_cone_mesh(); break;
            case Prim::Plane:    n->mesh = make_plane_mesh(); break;
            case Prim::Trigger:  break; // pure data — no geometry, ever
            case Prim::None:     break;
        }
    } else if (!n->mesh_path.empty()) {
        auto it = mesh_cache.find(n->mesh_path);
        if (it == mesh_cache.end()) it = mesh_cache.emplace(n->mesh_path, load_mesh(n->mesh_path)).first;
        n->mesh = it->second;
    }
    if (!n->mesh_texture_path.empty()) {
        auto it = tex_cache.find(n->mesh_texture_path);
        if (it == tex_cache.end()) it = tex_cache.emplace(n->mesh_texture_path, load_texture(n->mesh_texture_path)).first;
        n->mesh_texture = it->second;
    }
    if (j.contains("children")) {
        for (const auto& cj : j.at("children")) n->add_child(node_from_json(cj, tex_cache, mesh_cache));
    }
    return n;
}
} // namespace

bool save_scene(const Node& root, const std::string& path) {
    std::ofstream out(path);
    if (!out) { log_warn("save_scene: could not open " + path); return false; }
    out << node_to_json(root).dump(2);
    return true;
}

std::unique_ptr<Node> load_scene(const std::string& path) {
    std::ifstream in(path);
    if (!in) { log_warn("load_scene: could not open " + path); return nullptr; }
    nlohmann::json j;
    try { j = nlohmann::json::parse(in); } catch (...) { log_warn("load_scene: invalid JSON " + path); return nullptr; }
    std::unordered_map<std::string, Texture> tex_cache;
    std::unordered_map<std::string, Mesh> mesh_cache;
    return node_from_json(j, tex_cache, mesh_cache);
}

// --- 3D collision ---------------------------------------------------------

bool box3d_overlap(const Box3D& a, const Box3D& b) {
    return std::fabs(a.center.x - b.center.x) <= (a.half_extent.x + b.half_extent.x) &&
           std::fabs(a.center.y - b.center.y) <= (a.half_extent.y + b.half_extent.y) &&
           std::fabs(a.center.z - b.center.z) <= (a.half_extent.z + b.half_extent.z);
}

bool box3d_contains_point(const Box3D& box, vec3 point) {
    return std::fabs(point.x - box.center.x) <= box.half_extent.x &&
           std::fabs(point.y - box.center.y) <= box.half_extent.y &&
           std::fabs(point.z - box.center.z) <= box.half_extent.z;
}

bool sphere3d_overlap(const Sphere3D& a, const Sphere3D& b) {
    const float dx = a.center.x - b.center.x, dy = a.center.y - b.center.y, dz = a.center.z - b.center.z;
    const float r = a.radius + b.radius;
    return (dx * dx + dy * dy + dz * dz) <= r * r;
}

bool box3d_sphere3d_overlap(const Box3D& box, const Sphere3D& sphere) {
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const float cx = clampf(sphere.center.x, box.center.x - box.half_extent.x, box.center.x + box.half_extent.x);
    const float cy = clampf(sphere.center.y, box.center.y - box.half_extent.y, box.center.y + box.half_extent.y);
    const float cz = clampf(sphere.center.z, box.center.z - box.half_extent.z, box.center.z + box.half_extent.z);
    const float dx = sphere.center.x - cx, dy = sphere.center.y - cy, dz = sphere.center.z - cz;
    return (dx * dx + dy * dy + dz * dz) <= sphere.radius * sphere.radius;
}

bool ray_box3d(vec3 ray_origin, vec3 ray_dir, const Box3D& box, float& out_t) {
    const float box_min[3] = {box.center.x - box.half_extent.x, box.center.y - box.half_extent.y, box.center.z - box.half_extent.z};
    const float box_max[3] = {box.center.x + box.half_extent.x, box.center.y + box.half_extent.y, box.center.z + box.half_extent.z};
    const float origin[3] = {ray_origin.x, ray_origin.y, ray_origin.z};
    const float dir[3] = {ray_dir.x, ray_dir.y, ray_dir.z};
    float t_min = -std::numeric_limits<float>::infinity();
    float t_max = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-8f) {
            if (origin[i] < box_min[i] || origin[i] > box_max[i]) return false;
            continue;
        }
        float t1 = (box_min[i] - origin[i]) / dir[i];
        float t2 = (box_max[i] - origin[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) return false;
    }
    if (t_max < 0.0f) return false; // box is entirely behind the ray
    out_t = t_min >= 0.0f ? t_min : t_max; // ray origin starts inside the box
    return true;
}

// --- tilemap -----------------------------------------------------------

bool Tilemap::load_csv(const std::string& csv_path, Texture tileset, int tw, int th, int tileset_cols) {
    std::string content;
    if (!thistle_read_text_asset(csv_path, content)) { log_warn("tilemap: could not open " + csv_path); return false; }
    std::istringstream in(content);
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
    std::string content;
    if (!thistle_read_text_asset(path, content)) { log_warn("tilemap: could not open " + path); return false; }
    nlohmann::json j;
    try { j = nlohmann::json::parse(content); } catch (...) { log_warn("tilemap: invalid JSON " + path); return false; }

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
    const vec2 tm = measure_text(label, {.size = style.text_size, .font = style.font});
    text(label,
         {area.pos.x + (area.size.x - tm.x) * 0.5f,
          area.pos.y + (area.size.y - style.text_size) * 0.5f},
         {.size = style.text_size, .color = style.text, .font = style.font});
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
         {.size = style.text_size, .color = style.text, .font = style.font});
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

// --- in-app purchases -----------------------------------------------------

bool iap_can_make_payments() {
#if defined(__APPLE__)
    return thistle_iap_can_make_payments() != 0;
#else
    return false;
#endif
}

void iap_fetch_products(const std::vector<std::string>& product_ids) {
#if defined(__APPLE__)
    nlohmann::json arr = product_ids;
    thistle_iap_fetch_products(arr.dump().c_str());
#else
    (void)product_ids;
#endif
}

bool iap_products_ready() {
#if defined(__APPLE__)
    return thistle_iap_products_ready() != 0;
#else
    return true;   // nothing was ever requested, so "ready" with an empty list
#endif
}

const std::vector<IAPProduct>& iap_products() {
    static std::vector<IAPProduct> cache;
#if defined(__APPLE__)
    cache.clear();
    try {
        auto j = nlohmann::json::parse(thistle_iap_products_json());
        for (auto& p : j) {
            IAPProduct ip;
            ip.id            = p.value("id", std::string());
            ip.title         = p.value("title", std::string());
            ip.description   = p.value("description", std::string());
            ip.price_string  = p.value("price_string", std::string());
            ip.price_value   = p.value("price_value", 0.0);
            ip.currency_code = p.value("currency_code", std::string());
            cache.push_back(std::move(ip));
        }
    } catch (...) { log_error("iap_products: bad JSON from the platform bridge"); }
#endif
    return cache;
}

void iap_purchase(const std::string& product_id) {
#if defined(__APPLE__)
    thistle_iap_purchase(product_id.c_str());
#else
    log_warn("iap_purchase(\"" + product_id + "\"): not supported on this platform");
#endif
}

void iap_restore_purchases() {
#if defined(__APPLE__)
    thistle_iap_restore_purchases();
#endif
}

void iap_finish_transaction(const std::string& transaction_id) {
#if defined(__APPLE__)
    thistle_iap_finish_transaction(transaction_id.c_str());
#else
    (void)transaction_id;
#endif
}

bool iap_poll_event(IAPEvent& out) {
#if defined(__APPLE__)
    const char* raw = thistle_iap_poll_event_json();
    if (!raw || !*raw) return false;
    try {
        auto j = nlohmann::json::parse(raw);
        std::string kind = j.value("kind", std::string());
        out.kind = kind == "purchased" ? IAPEventKind::Purchased
                 : kind == "restored"  ? IAPEventKind::Restored
                 : kind == "deferred"  ? IAPEventKind::Deferred
                                       : IAPEventKind::Failed;
        out.product_id     = j.value("product_id", std::string());
        out.transaction_id = j.value("transaction_id", std::string());
        out.error_message  = j.value("error_message", std::string());
        return true;
    } catch (...) { return false; }
#else
    (void)out;
    return false;
#endif
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
#elif defined(__ANDROID__)
    // The only path an Android app can actually write to; there's no HOME or
    // XDG env var here. Set on the activity before sokol_main() ever runs
    // (see ANativeActivity_onCreate in sokol_app.h), so this is safe anywhere.
    const auto* activity = static_cast<const ANativeActivity*>(sapp_android_get_native_activity());
    base = (activity && activity->internalDataPath) ? activity->internalDataPath : "/data/local/tmp";
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

// --- crash reporting -----------------------------------------------------
// See the big comment on this API in thistle.hpp before touching this —
// the "best effort, fails open" framing is deliberate, not an oversight.

namespace {
std::string g_crash_dir;              // computed + created once, at install time (safe context)
std::string g_crash_platform_cached;  // platform_name()/device_name() cached here at install time —
std::string g_crash_device_cached;    //   device_name() goes through Objective-C on Apple, unsafe to call from a handler
std::map<std::string, std::string> g_crash_context;
bool g_crash_handler_installed = false;
bool g_crash_popup_enabled = true;
std::atomic<bool> g_in_crash_handler{false};   // re-entrancy guard: a crash while already handling one gets out of the way instead of looping

void crash_make_dirs(const std::string& path) {
    std::string cur;
    for (char c : path) {
        cur += c;
#if defined(_WIN32)
        if ((c == '/' || c == '\\') && cur.size() > 1) _mkdir(cur.c_str());
#else
        if (c == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
#endif
    }
#if defined(_WIN32)
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

std::vector<std::string> crash_backtrace() {
    std::vector<std::string> out;
#if defined(_WIN32)
    void* frames[64];
    USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buf);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        char line[320];
        if (SymFromAddr(process, addr, nullptr, symbol))
            std::snprintf(line, sizeof(line), "0x%016llx %s", static_cast<unsigned long long>(addr), symbol->Name);
        else
            std::snprintf(line, sizeof(line), "0x%016llx", static_cast<unsigned long long>(addr));
        out.emplace_back(line);
    }
#elif defined(__ANDROID__)
    // Bionic doesn't provide execinfo.h/backtrace(); a real unwinder would
    // need <unwind.h>'s _Unwind_Backtrace plus a symbolizer. Not done — the
    // report still gets the reason, context, and log tail, just no frames.
    out.emplace_back("(backtrace not available on Android)");
#else
    void* frames[64];
    int n = backtrace(frames, 64);
    char** syms = backtrace_symbols(frames, n);
    if (syms) {
        for (int i = 0; i < n; ++i) out.emplace_back(syms[i]);
        free(syms);   // backtrace_symbols' own malloc'd array — not our std::vector, plain free() is correct here
    }
#endif
    return out;
}

// Writes the actual report. Called from a signal handler / SEH filter /
// std::terminate handler — deliberately does as little as possible with the
// least-risky subset of the standard library available (plain C file I/O,
// no iostreams; the only heap allocation beyond what's already unavoidable
// is crash_backtrace()'s own, which real crash handlers universally accept).
// Platform-native "the game crashed" dialog — deliberately never touches
// Thistle's own renderer (see the doc comment on set_crash_popup() in
// thistle.hpp for why). Called after the report file is already written,
// so even if this itself misbehaves, the report exists.
#if defined(__APPLE__)
void crash_show_popup(const std::string& title, const std::string& message) {
#if TARGET_OS_IPHONE
    (void)title; (void)message;   // no hook exists — iOS has already killed the app by the time this would run
#else
    pid_t pid = fork();
    if (pid == 0) {
        // setsid() moves the child into its own new session AND process
        // group, detached from the crashing parent's — without this, the
        // parent re-raising its signal a few lines below can trigger the
        // shell/terminal's process-group cleanup before the child finishes
        // execl(), killing the popup before it ever shows. Verified this
        // race for real: without setsid() the popup silently failed to
        // appear essentially every time; with it, every time.
        setsid();
        std::string script = "display dialog \"" + message + "\" buttons {\"OK\"} with icon caution with title \"" + title + "\"";
        execl("/usr/bin/osascript", "osascript", "-e", script.c_str(), static_cast<char*>(nullptr));
        _exit(1);
    }
    // Parent doesn't wait: the popup process is fully independent and keeps
    // running (and showing) even after this (crashing) process exits.
#endif
}
#elif defined(_WIN32)
void crash_show_popup(const std::string& title, const std::string& message) {
    MessageBoxA(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}
#else
void crash_show_popup(const std::string& title, const std::string& message) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();   // detach from the crashing parent's process group — see the macOS branch's comment for why this matters
        execlp("zenity", "zenity", "--error", "--title", title.c_str(), "--text", message.c_str(), static_cast<char*>(nullptr));
        _exit(1);   // zenity not installed — silently give up; the report file still exists
    }
}
#endif

void crash_write_report(const char* reason, const std::vector<std::string>& bt) {
    bool expected = false;
    if (!g_in_crash_handler.compare_exchange_strong(expected, true)) return;   // already handling a crash — don't recurse
    if (g_crash_dir.empty()) return;
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    // The timestamp alone collides when two crashes land in the same second
    // (verified: a fast automated test doing exactly that silently
    // overwrote an earlier report before this was added) — the process id
    // makes the filename unique across processes; the re-entrancy guard
    // above already makes sure a single process only ever writes once.
#if defined(_WIN32)
    unsigned long pid = GetCurrentProcessId();
#else
    long pid = static_cast<long>(getpid());
#endif
    std::string path = g_crash_dir + "/crash_" + stamp + "-" + std::to_string(pid) + ".txt";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;

    std::fprintf(f, "Thistle crash report\n");
    std::fprintf(f, "time: %s\n", stamp);
    std::fprintf(f, "platform: %s\n", g_crash_platform_cached.c_str());
    std::fprintf(f, "device: %s\n", g_crash_device_cached.c_str());
    std::fprintf(f, "reason: %s\n", reason);

    if (!g_crash_context.empty()) {
        std::fprintf(f, "\ncontext:\n");
        for (auto& kv : g_crash_context) std::fprintf(f, "  %s: %s\n", kv.first.c_str(), kv.second.c_str());
    }

    std::fprintf(f, "\nbacktrace:\n");
    for (auto& line : bt) std::fprintf(f, "  %s\n", line.c_str());

    std::fprintf(f, "\nlast log lines:\n");
    size_t start = g_logs.size() > 50 ? g_logs.size() - 50 : 0;
    for (size_t i = start; i < g_logs.size(); ++i) std::fprintf(f, "  %s\n", g_logs[i].c_str());

    std::fclose(f);

    if (g_crash_popup_enabled) {
        std::string title = g_state ? g_state->config.title : std::string("Thistle");
        std::string message = title + " ran into a problem and had to close.\n\n"
                               "A report was saved to:\n" + path + "\n\nReason: " + reason;
        crash_show_popup(title, message);
    }
}

#if defined(_WIN32)
LONG WINAPI crash_seh_filter(EXCEPTION_POINTERS* info) {
    char reason[64];
    std::snprintf(reason, sizeof(reason), "unhandled exception 0x%08lX",
                  static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
    crash_write_report(reason, crash_backtrace());
    return EXCEPTION_CONTINUE_SEARCH;   // let Windows' own crash handling (WER, a debugger) still happen
}
#else
void crash_signal_handler(int sig) {
    crash_write_report(strsignal(sig) ? strsignal(sig) : "unknown signal", crash_backtrace());
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);   // re-raise so the OS's own crash behavior (core dump, debugger) still happens
}
#endif

void crash_terminate_handler() {
    crash_write_report("uncaught C++ exception (std::terminate)", crash_backtrace());
    std::abort();
}

void install_crash_handler() {
    if (g_crash_handler_installed) return;
    g_crash_handler_installed = true;

    g_crash_platform_cached = platform_name();
    g_crash_device_cached = device_name();

    std::string save_file = save::path();
    size_t slash = save_file.find_last_of("/\\");
    std::string base = slash == std::string::npos ? "." : save_file.substr(0, slash);
    g_crash_dir = base + "/crashes";
    crash_make_dirs(g_crash_dir);

    crash_backtrace();   // prime backtrace()'s lazy first-call init before a real crash needs it

    std::set_terminate(crash_terminate_handler);

#if defined(_WIN32)
    SetUnhandledExceptionFilter(crash_seh_filter);
#else
    struct sigaction sa{};
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) sigaction(sig, &sa, nullptr);
#endif
}
} // namespace

std::string crash_log_dir() { return g_crash_dir; }

void set_crash_context(const std::string& key, const std::string& value) {
    g_crash_context[key] = value;
}

void set_crash_popup(bool enabled) { g_crash_popup_enabled = enabled; }

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

// Binds `tex` for a textured 3D draw, uploading it first if needed. Returns
// false (nothing bound) if the texture is invalid or failed to upload, so
// callers can fall back to a flat-colored draw instead of drawing garbage.
bool bind_texture3d(Texture tex) {
    if (!tex.valid()) return false;
    TextureRecord& rec = g_state->textures[tex.id];
    ensure_uploaded(rec);
    if (rec.img.id == SG_INVALID_ID) return false;
    sgl_texture(rec.view, g_state->sampler);
    return true;
}

struct CubeFace { int a, b, c, d; float nx, ny, nz; };
constexpr CubeFace kCubeFaces[6] = {
    {0, 1, 2, 3,  0.0f,  0.0f, -1.0f}, // back
    {5, 4, 7, 6,  0.0f,  0.0f,  1.0f}, // front
    {4, 0, 3, 7, -1.0f,  0.0f,  0.0f}, // left
    {1, 5, 6, 2,  1.0f,  0.0f,  0.0f}, // right
    {3, 2, 6, 7,  0.0f,  1.0f,  0.0f}, // top
    {4, 5, 1, 0,  0.0f, -1.0f,  0.0f}, // bottom
};

void cube_corners(vec3 center, vec3 size, vec3 out[8]) {
    const float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
    out[0] = {center.x - hx, center.y - hy, center.z - hz};
    out[1] = {center.x + hx, center.y - hy, center.z - hz};
    out[2] = {center.x + hx, center.y + hy, center.z - hz};
    out[3] = {center.x - hx, center.y + hy, center.z - hz};
    out[4] = {center.x - hx, center.y - hy, center.z + hz};
    out[5] = {center.x + hx, center.y - hy, center.z + hz};
    out[6] = {center.x + hx, center.y + hy, center.z + hz};
    out[7] = {center.x - hx, center.y + hy, center.z + hz};
}

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

// Walks a UV-sphere's triangles (two per lat/lon quad, degenerate slivers
// at the poles — harmless zero-area triangles) calling emit(pos, normal, u, v)
// for every vertex. Shared by the flat-color and textured sphere3d() overloads
// so the trig only lives in one place.
template <typename Emit>
void gen_sphere(vec3 center, float radius, int rings, int segments, Emit&& emit) {
    if (rings < 2) rings = 2;
    if (segments < 3) segments = 3;
    for (int r = 0; r < rings; ++r) {
        const float v0 = static_cast<float>(r) / rings, v1 = static_cast<float>(r + 1) / rings;
        const float phi0 = v0 * kPi, phi1 = v1 * kPi;
        for (int s = 0; s < segments; ++s) {
            const float u0 = static_cast<float>(s) / segments, u1 = static_cast<float>(s + 1) / segments;
            const float th0 = u0 * kTwoPi, th1 = u1 * kTwoPi;
            auto normal_at = [&](float phi, float theta) {
                const float sp = std::sin(phi), cp = std::cos(phi);
                return vec3{sp * std::cos(theta), cp, sp * std::sin(theta)};
            };
            auto point_at = [&](vec3 n) {
                return vec3{center.x + radius * n.x, center.y + radius * n.y, center.z + radius * n.z};
            };
            const vec3 n00 = normal_at(phi0, th0), n01 = normal_at(phi0, th1);
            const vec3 n10 = normal_at(phi1, th0), n11 = normal_at(phi1, th1);
            const vec3 p00 = point_at(n00), p01 = point_at(n01);
            const vec3 p10 = point_at(n10), p11 = point_at(n11);
            emit(p00, n00, u0, v0); emit(p10, n10, u0, v1); emit(p11, n11, u1, v1);
            emit(p00, n00, u0, v0); emit(p11, n11, u1, v1); emit(p01, n01, u1, v0);
        }
    }
}

// Walks a capped cylinder's triangles (smooth normal around the side,
// flat +-Y normal on the caps). Shared by both cylinder3d() overloads.
template <typename Emit>
void gen_cylinder(vec3 center, float radius, float height, int segments, Emit&& emit) {
    if (segments < 3) segments = 3;
    const float halfH = height * 0.5f;
    const float yTop = center.y + halfH, yBot = center.y - halfH;
    for (int s = 0; s < segments; ++s) {
        const float u0 = static_cast<float>(s) / segments, u1 = static_cast<float>(s + 1) / segments;
        const float th0 = u0 * kTwoPi, th1 = u1 * kTwoPi;
        const float c0 = std::cos(th0), s0 = std::sin(th0);
        const float c1 = std::cos(th1), s1 = std::sin(th1);
        const vec3 n0{c0, 0.0f, s0}, n1{c1, 0.0f, s1};
        const vec3 pTop0{center.x + radius * c0, yTop, center.z + radius * s0};
        const vec3 pTop1{center.x + radius * c1, yTop, center.z + radius * s1};
        const vec3 pBot0{center.x + radius * c0, yBot, center.z + radius * s0};
        const vec3 pBot1{center.x + radius * c1, yBot, center.z + radius * s1};
        emit(pBot0, n0, u0, 0.0f); emit(pBot1, n1, u1, 0.0f); emit(pTop1, n1, u1, 1.0f);
        emit(pBot0, n0, u0, 0.0f); emit(pTop1, n1, u1, 1.0f); emit(pTop0, n0, u0, 1.0f);

        const vec3 up{0.0f, 1.0f, 0.0f}, down{0.0f, -1.0f, 0.0f};
        const vec3 capCenterTop{center.x, yTop, center.z}, capCenterBot{center.x, yBot, center.z};
        emit(capCenterTop, up, 0.5f, 0.5f);
        emit(pTop1, up, 0.5f + c1 * 0.5f, 0.5f + s1 * 0.5f);
        emit(pTop0, up, 0.5f + c0 * 0.5f, 0.5f + s0 * 0.5f);
        emit(capCenterBot, down, 0.5f, 0.5f);
        emit(pBot0, down, 0.5f + c0 * 0.5f, 0.5f + s0 * 0.5f);
        emit(pBot1, down, 0.5f + c1 * 0.5f, 0.5f + s1 * 0.5f);
    }
}

// Walks a capped cone's triangles (apex up). Side normals slope by
// radius/height (a wide, flat cone has near-vertical side normals, a tall
// thin one has near-horizontal ones); the base cap is a flat -Y fan.
template <typename Emit>
void gen_cone(vec3 center, float radius, float height, int segments, Emit&& emit) {
    if (segments < 3) segments = 3;
    const float halfH = height * 0.5f;
    const float yApex = center.y + halfH, yBase = center.y - halfH;
    const vec3 apex{center.x, yApex, center.z};
    const float slope = height > 0.0f ? radius / height : 0.0f;
    const float nlen = std::sqrt(1.0f + slope * slope);
    for (int s = 0; s < segments; ++s) {
        const float u0 = static_cast<float>(s) / segments, u1 = static_cast<float>(s + 1) / segments;
        const float th0 = u0 * kTwoPi, th1 = u1 * kTwoPi;
        const float c0 = std::cos(th0), s0 = std::sin(th0);
        const float c1 = std::cos(th1), s1 = std::sin(th1);
        const vec3 n0{c0 / nlen, slope / nlen, s0 / nlen};
        const vec3 n1{c1 / nlen, slope / nlen, s1 / nlen};
        const vec3 nApex{(n0.x + n1.x) * 0.5f, (n0.y + n1.y) * 0.5f, (n0.z + n1.z) * 0.5f};
        const vec3 pBase0{center.x + radius * c0, yBase, center.z + radius * s0};
        const vec3 pBase1{center.x + radius * c1, yBase, center.z + radius * s1};
        emit(pBase0, n0, u0, 0.0f);
        emit(pBase1, n1, u1, 0.0f);
        emit(apex, nApex, (u0 + u1) * 0.5f, 1.0f);

        const vec3 down{0.0f, -1.0f, 0.0f};
        emit(vec3{center.x, yBase, center.z}, down, 0.5f, 0.5f);
        emit(pBase1, down, 0.5f + c1 * 0.5f, 0.5f + s1 * 0.5f);
        emit(pBase0, down, 0.5f + c0 * 0.5f, 0.5f + s0 * 0.5f);
    }
}

// Rotates a normal by the same X-then-Y-then-Z Euler angles Frame::mesh3d()
// feeds to sgl_rotate() for the vertex positions, so the CPU-computed
// per-vertex shading (done in world space, since sgl's matrix stack only
// transforms the position it's given) lines up with where the mesh actually
// ends up. Translation/scale don't affect a direction, so they're not part
// of this — see mesh3d()'s doc comment for the non-uniform-scale caveat.
vec3 rotate_normal(vec3 n, vec3 rot) {
    const float cx = std::cos(rot.x), sx = std::sin(rot.x);
    const float cy = std::cos(rot.y), sy = std::sin(rot.y);
    const float cz = std::cos(rot.z), sz = std::sin(rot.z);
    const vec3 afterX{n.x, n.y * cx - n.z * sx, n.y * sx + n.z * cx};
    const vec3 afterY{afterX.x * cy + afterX.z * sy, afterX.y, -afterX.x * sy + afterX.z * cy};
    const vec3 afterZ{afterY.x * cz - afterY.y * sz, afterY.x * sz + afterY.y * cz, afterY.z};
    return afterZ;
}
} // namespace

void Frame::cube(vec3 center, vec3 size, rgba color) {
    vec3 c[8];
    cube_corners(center, size, c);
    sgl_disable_texture();
    sgl_begin_triangles();
    for (const CubeFace& fc : kCubeFaces) {
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

void Frame::cube(vec3 center, vec3 size, Texture tex, rgba tint) {
    if (!bind_texture3d(tex)) { cube(center, size, tint); return; }
    vec3 c[8];
    cube_corners(center, size, c);
    sgl_begin_triangles();
    for (const CubeFace& fc : kCubeFaces) {
        const rgba sc = shade_face(tint, fc.nx, fc.ny, fc.nz);
        const vec3& v0 = c[fc.a]; const vec3& v1 = c[fc.b]; const vec3& v2 = c[fc.c]; const vec3& v3 = c[fc.d];
        sgl_v3f_t2f_c4f(v0.x, v0.y, v0.z, 0.0f, 0.0f, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_t2f_c4f(v1.x, v1.y, v1.z, 1.0f, 0.0f, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_t2f_c4f(v2.x, v2.y, v2.z, 1.0f, 1.0f, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_t2f_c4f(v0.x, v0.y, v0.z, 0.0f, 0.0f, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_t2f_c4f(v2.x, v2.y, v2.z, 1.0f, 1.0f, sc.r, sc.g, sc.b, sc.a);
        sgl_v3f_t2f_c4f(v3.x, v3.y, v3.z, 0.0f, 1.0f, sc.r, sc.g, sc.b, sc.a);
    }
    sgl_end();
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

void Frame::plane3d(vec3 center, float width_, float depth_, Texture tex, rgba tint) {
    if (!bind_texture3d(tex)) { plane3d(center, width_, depth_, tint); return; }
    const float hw = width_ * 0.5f, hd = depth_ * 0.5f;
    const rgba sc = shade_face(tint, 0.0f, 1.0f, 0.0f);
    const vec3 v0{center.x - hw, center.y, center.z - hd}, v1{center.x + hw, center.y, center.z - hd},
               v2{center.x + hw, center.y, center.z + hd}, v3{center.x - hw, center.y, center.z + hd};
    sgl_begin_triangles();
    sgl_v3f_t2f_c4f(v0.x, v0.y, v0.z, 0.0f, 0.0f, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_t2f_c4f(v1.x, v1.y, v1.z, 1.0f, 0.0f, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_t2f_c4f(v2.x, v2.y, v2.z, 1.0f, 1.0f, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_t2f_c4f(v0.x, v0.y, v0.z, 0.0f, 0.0f, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_t2f_c4f(v2.x, v2.y, v2.z, 1.0f, 1.0f, sc.r, sc.g, sc.b, sc.a);
    sgl_v3f_t2f_c4f(v3.x, v3.y, v3.z, 0.0f, 1.0f, sc.r, sc.g, sc.b, sc.a);
    sgl_end();
}

void Frame::line3d(vec3 a, vec3 b, rgba color) {
    sgl_disable_texture();
    sgl_begin_lines();
    sgl_v3f_c4f(a.x, a.y, a.z, color.r, color.g, color.b, color.a);
    sgl_v3f_c4f(b.x, b.y, b.z, color.r, color.g, color.b, color.a);
    sgl_end();
    sgl_enable_texture();
}

void Frame::sphere3d(vec3 center, float radius, rgba color, int rings, int segments) {
    sgl_disable_texture();
    sgl_begin_triangles();
    gen_sphere(center, radius, rings, segments, [&](vec3 p, vec3 n, float, float) {
        const rgba sc = shade_face(color, n.x, n.y, n.z);
        sgl_v3f_c4f(p.x, p.y, p.z, sc.r, sc.g, sc.b, sc.a);
    });
    sgl_end();
    sgl_enable_texture();
}

void Frame::sphere3d(vec3 center, float radius, Texture tex, rgba tint, int rings, int segments) {
    if (!bind_texture3d(tex)) { sphere3d(center, radius, tint, rings, segments); return; }
    sgl_begin_triangles();
    gen_sphere(center, radius, rings, segments, [&](vec3 p, vec3 n, float u, float v) {
        const rgba sc = shade_face(tint, n.x, n.y, n.z);
        sgl_v3f_t2f_c4f(p.x, p.y, p.z, u, v, sc.r, sc.g, sc.b, sc.a);
    });
    sgl_end();
}

void Frame::cylinder3d(vec3 center, float radius, float height, rgba color, int segments) {
    sgl_disable_texture();
    sgl_begin_triangles();
    gen_cylinder(center, radius, height, segments, [&](vec3 p, vec3 n, float, float) {
        const rgba sc = shade_face(color, n.x, n.y, n.z);
        sgl_v3f_c4f(p.x, p.y, p.z, sc.r, sc.g, sc.b, sc.a);
    });
    sgl_end();
    sgl_enable_texture();
}

void Frame::cylinder3d(vec3 center, float radius, float height, Texture tex, rgba tint, int segments) {
    if (!bind_texture3d(tex)) { cylinder3d(center, radius, height, tint, segments); return; }
    sgl_begin_triangles();
    gen_cylinder(center, radius, height, segments, [&](vec3 p, vec3 n, float u, float v) {
        const rgba sc = shade_face(tint, n.x, n.y, n.z);
        sgl_v3f_t2f_c4f(p.x, p.y, p.z, u, v, sc.r, sc.g, sc.b, sc.a);
    });
    sgl_end();
}

void Frame::cone3d(vec3 center, float radius, float height, rgba color, int segments) {
    sgl_disable_texture();
    sgl_begin_triangles();
    gen_cone(center, radius, height, segments, [&](vec3 p, vec3 n, float, float) {
        const rgba sc = shade_face(color, n.x, n.y, n.z);
        sgl_v3f_c4f(p.x, p.y, p.z, sc.r, sc.g, sc.b, sc.a);
    });
    sgl_end();
    sgl_enable_texture();
}

void Frame::cone3d(vec3 center, float radius, float height, Texture tex, rgba tint, int segments) {
    if (!bind_texture3d(tex)) { cone3d(center, radius, height, tint, segments); return; }
    sgl_begin_triangles();
    gen_cone(center, radius, height, segments, [&](vec3 p, vec3 n, float u, float v) {
        const rgba sc = shade_face(tint, n.x, n.y, n.z);
        sgl_v3f_t2f_c4f(p.x, p.y, p.z, u, v, sc.r, sc.g, sc.b, sc.a);
    });
    sgl_end();
}

void Frame::mesh3d(Mesh mesh, vec3 pos, vec3 rotation_rad, vec3 scale, rgba tint) {
    if (mesh.id < 0 || mesh.id >= static_cast<int>(g_state->meshes.size())) return;
    const MeshRecord& rec = g_state->meshes[mesh.id];
    sgl_disable_texture();
    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, pos.z);
    sgl_rotate(rotation_rad.x, 1.0f, 0.0f, 0.0f);
    sgl_rotate(rotation_rad.y, 0.0f, 1.0f, 0.0f);
    sgl_rotate(rotation_rad.z, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale.x, scale.y, scale.z);
    sgl_begin_triangles();
    for (const MeshVertex& mv : rec.tris) {
        const vec3 wn = rotate_normal(mv.normal, rotation_rad);
        const rgba sc = shade_face(tint, wn.x, wn.y, wn.z);
        sgl_v3f_c4f(mv.pos.x, mv.pos.y, mv.pos.z, sc.r, sc.g, sc.b, sc.a);
    }
    sgl_end();
    sgl_pop_matrix();
    sgl_enable_texture();
}

void Frame::mesh3d(Mesh mesh, vec3 pos, vec3 rotation_rad, vec3 scale, Texture tex, rgba tint) {
    if (!bind_texture3d(tex)) { mesh3d(mesh, pos, rotation_rad, scale, tint); return; }
    if (mesh.id < 0 || mesh.id >= static_cast<int>(g_state->meshes.size())) return;
    const MeshRecord& rec = g_state->meshes[mesh.id];
    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, pos.z);
    sgl_rotate(rotation_rad.x, 1.0f, 0.0f, 0.0f);
    sgl_rotate(rotation_rad.y, 0.0f, 1.0f, 0.0f);
    sgl_rotate(rotation_rad.z, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale.x, scale.y, scale.z);
    sgl_begin_triangles();
    for (const MeshVertex& mv : rec.tris) {
        const vec3 wn = rotate_normal(mv.normal, rotation_rad);
        const rgba sc = shade_face(tint, wn.x, wn.y, wn.z);
        sgl_v3f_t2f_c4f(mv.pos.x, mv.pos.y, mv.pos.z, mv.u, mv.v, sc.r, sc.g, sc.b, sc.a);
    }
    sgl_end();
    sgl_pop_matrix();
}

Font load_font(const std::string& path) {
    const int idx = static_cast<int>(g_state->font_ids.size());
    int fid = FONS_INVALID;
    if (g_state->fons) { // window already up: load immediately
#if defined(__ANDROID__)
        fid = android_fons_add_font(g_state->fons, path);
#else
        fid = fonsAddFont(g_state->fons, "font", path.c_str());
#endif
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
    unsigned char* pixels = thistle_stbi_load(path, &w, &h, &channels, 4);
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

namespace {
// Parses one OBJ face-vertex token ("3", "3/4", "3//5", "3/4/5") into 0-based
// indices, resolving OBJ's 1-based (or negative-relative-to-end) indexing.
// Any component the token doesn't have comes back -1.
struct ObjFaceIndex { int p = -1, t = -1, n = -1; };

int resolve_obj_index(int raw, int count) {
    if (raw == 0) return -1;
    return raw > 0 ? raw - 1 : count + raw;
}

ObjFaceIndex parse_obj_face_token(const std::string& tok, int posCount, int uvCount, int nCount) {
    ObjFaceIndex out;
    const size_t p1 = tok.find('/');
    const std::string a = (p1 == std::string::npos) ? tok : tok.substr(0, p1);
    if (!a.empty()) out.p = resolve_obj_index(std::atoi(a.c_str()), posCount);
    if (p1 == std::string::npos) return out;
    const std::string rest = tok.substr(p1 + 1);
    const size_t p2 = rest.find('/');
    const std::string b = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
    if (!b.empty()) out.t = resolve_obj_index(std::atoi(b.c_str()), uvCount);
    if (p2 == std::string::npos) return out;
    const std::string c = rest.substr(p2 + 1);
    if (!c.empty()) out.n = resolve_obj_index(std::atoi(c.c_str()), nCount);
    return out;
}

vec3 normalize_or(vec3 v, vec3 fallback) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return len > 1e-8f ? vec3{v.x / len, v.y / len, v.z / len} : fallback;
}
} // namespace

Mesh load_mesh(const std::string& path) {
    std::string content;
    if (!thistle_read_text_asset(path, content)) { log_warn("load_mesh: could not open " + path); return Mesh{}; }

    std::vector<vec3> positions;
    std::vector<vec3> normals;
    std::vector<std::pair<float, float>> uvs;
    MeshRecord rec;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            vec3 p; ls >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "vn") {
            vec3 n; ls >> n.x >> n.y >> n.z;
            normals.push_back(normalize_or(n, vec3{0.0f, 1.0f, 0.0f}));
        } else if (tag == "vt") {
            // OBJ's v is bottom-up (0 = bottom of the image); every texture
            // sample elsewhere in this engine treats v=0 as the top row (the
            // first row stb_image decodes), so flip here once at load time
            // rather than making every caller remember to do it.
            float u = 0.0f, v = 0.0f; ls >> u >> v;
            uvs.emplace_back(u, 1.0f - v);
        } else if (tag == "f") {
            std::vector<ObjFaceIndex> face;
            std::string tok;
            while (ls >> tok) {
                face.push_back(parse_obj_face_token(tok, static_cast<int>(positions.size()),
                                                     static_cast<int>(uvs.size()), static_cast<int>(normals.size())));
            }
            if (face.size() < 3) continue;
            // Fan-triangulate anything beyond a triangle (n-gons, quads).
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                const ObjFaceIndex& fa = face[0];
                const ObjFaceIndex& fb = face[i];
                const ObjFaceIndex& fc = face[i + 1];
                auto pos_of = [&](const ObjFaceIndex& fi) {
                    return (fi.p >= 0 && fi.p < static_cast<int>(positions.size())) ? positions[fi.p] : vec3{};
                };
                const vec3 pa = pos_of(fa), pb = pos_of(fb), pc = pos_of(fc);
                const vec3 e1{pb.x - pa.x, pb.y - pa.y, pb.z - pa.z};
                const vec3 e2{pc.x - pa.x, pc.y - pa.y, pc.z - pa.z};
                const vec3 flatNormal = normalize_or(
                    vec3{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x},
                    vec3{0.0f, 1.0f, 0.0f});
                auto vertex_of = [&](const ObjFaceIndex& fi) {
                    MeshVertex mv;
                    mv.pos = pos_of(fi);
                    mv.normal = (fi.n >= 0 && fi.n < static_cast<int>(normals.size())) ? normals[fi.n] : flatNormal;
                    if (fi.t >= 0 && fi.t < static_cast<int>(uvs.size())) { mv.u = uvs[fi.t].first; mv.v = uvs[fi.t].second; }
                    return mv;
                };
                rec.tris.push_back(vertex_of(fa));
                rec.tris.push_back(vertex_of(fb));
                rec.tris.push_back(vertex_of(fc));
            }
        }
        // Everything else (o/g/s/usemtl/mtllib/comments) is deliberately
        // ignored — see load_mesh()'s doc comment for what that means.
    }

    if (rec.tris.empty()) { log_warn("load_mesh: no triangles parsed from " + path); return Mesh{}; }
    const int id = static_cast<int>(g_state->meshes.size());
    g_state->meshes.push_back(std::move(rec));
    return Mesh{id};
}

void unload_mesh(Mesh& mesh) {
    if (mesh.id >= 0 && mesh.id < static_cast<int>(g_state->meshes.size())) {
        g_state->meshes[mesh.id] = MeshRecord{};
    }
    mesh = Mesh{};
}

namespace {
Mesh register_mesh(MeshRecord rec) {
    const int id = static_cast<int>(g_state->meshes.size());
    g_state->meshes.push_back(std::move(rec));
    return Mesh{id};
}
} // namespace

Mesh make_cube_mesh() {
    vec3 c[8];
    cube_corners(vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 1.0f, 1.0f}, c);
    MeshRecord rec;
    for (const CubeFace& fc : kCubeFaces) {
        const vec3 n{fc.nx, fc.ny, fc.nz};
        const vec3& v0 = c[fc.a]; const vec3& v1 = c[fc.b]; const vec3& v2 = c[fc.c]; const vec3& v3 = c[fc.d];
        rec.tris.push_back({v0, n, 0.0f, 0.0f});
        rec.tris.push_back({v1, n, 1.0f, 0.0f});
        rec.tris.push_back({v2, n, 1.0f, 1.0f});
        rec.tris.push_back({v0, n, 0.0f, 0.0f});
        rec.tris.push_back({v2, n, 1.0f, 1.0f});
        rec.tris.push_back({v3, n, 0.0f, 1.0f});
    }
    return register_mesh(std::move(rec));
}

Mesh make_sphere_mesh() {
    MeshRecord rec;
    gen_sphere(vec3{0.0f, 0.0f, 0.0f}, 0.5f, 12, 16, [&](vec3 p, vec3 n, float u, float v) {
        rec.tris.push_back({p, n, u, v});
    });
    return register_mesh(std::move(rec));
}

Mesh make_cylinder_mesh() {
    MeshRecord rec;
    gen_cylinder(vec3{0.0f, 0.0f, 0.0f}, 0.5f, 1.0f, 16, [&](vec3 p, vec3 n, float u, float v) {
        rec.tris.push_back({p, n, u, v});
    });
    return register_mesh(std::move(rec));
}

Mesh make_cone_mesh() {
    MeshRecord rec;
    gen_cone(vec3{0.0f, 0.0f, 0.0f}, 0.5f, 1.0f, 16, [&](vec3 p, vec3 n, float u, float v) {
        rec.tris.push_back({p, n, u, v});
    });
    return register_mesh(std::move(rec));
}

Mesh make_plane_mesh() {
    const float hw = 0.5f, hd = 0.5f;
    const vec3 n{0.0f, 1.0f, 0.0f};
    const vec3 v0{-hw, 0.0f, -hd}, v1{hw, 0.0f, -hd}, v2{hw, 0.0f, hd}, v3{-hw, 0.0f, hd};
    MeshRecord rec;
    rec.tris.push_back({v0, n, 0.0f, 0.0f});
    rec.tris.push_back({v1, n, 1.0f, 0.0f});
    rec.tris.push_back({v2, n, 1.0f, 1.0f});
    rec.tris.push_back({v0, n, 0.0f, 0.0f});
    rec.tris.push_back({v2, n, 1.0f, 1.0f});
    rec.tris.push_back({v3, n, 0.0f, 1.0f});
    return register_mesh(std::move(rec));
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
    unsigned char* px = thistle_stbi_load(rec.path, &w, &h, &channels, 4);
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
    install_crash_handler();   // safe to call more than once; only the first call does anything
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
#if defined(__ANDROID__)
    // Window icons need AAssetManager-based asset loading (not done yet), and
    // there's no sapp_run() call here — see the comment on g_thistle_android_desc.
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
    d.icon.sokol_default = true;
    g_thistle_android_desc = d;
    return 0;
#else
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
#endif
}

namespace detail {

sg_view texture_view(Texture tex) {
    if (!g_state || tex.id < 0 || tex.id >= static_cast<int>(g_state->textures.size())) return {};
    TextureRecord& rec = g_state->textures[tex.id];
    ensure_uploaded(rec);
    return rec.img.id != SG_INVALID_ID ? rec.view : sg_view{};
}

uint64_t frame_index() { return g_state ? g_state->frame_index : 0; }
int frame_width() { return sapp_width(); }
int frame_height() { return sapp_height(); }

} // namespace detail

} // namespace thistle

#if defined(__ANDROID__)
// Defined for real by THISTLE_MAIN in the game's own .cpp, which overrides
// this weak no-op default — needed so headless Android executables that link
// against libthistle (e.g. thistle_net_smoketest, which never touches App at
// all) don't fail to link over a symbol they have no reason to define.
extern "C" __attribute__((weak)) void thistle_user_main() {}

// Declared by sokol_app.h; its own ANativeActivity_onCreate calls this
// directly (there is no process main() to call it for us) to get the desc it
// needs before it starts the real frame loop on a background thread.
sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    thistle_user_main();
    return g_thistle_android_desc;
}
#endif
