// Optional Dear ImGui debug UI (THISTLE_DEBUG_UI; debug_ui_off.cpp stands in
// otherwise), on sokol_imgui. Games call ImGui directly anywhere in their
// update callback; the frame loop starts an ImGui frame before it and draws
// the result last, on top of everything (post-effects included).
#include "thistle_internal.h"

#include "imgui.h"

// Same backend choice as sokol_impl.c: sokol_imgui only compiles the shader
// for the backend it's told about.
#if defined(__APPLE__)
    #define SOKOL_METAL
#elif defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    #define SOKOL_GLES3
#elif defined(_WIN32)
    #define SOKOL_D3D11
#else
    #define SOKOL_GLCORE
#endif
#include "sokol_app.h"
#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"

#include <algorithm>
#include <cstdio>

namespace thistle {

namespace {
bool g_ready = false;
float g_frame_ms[120] = {};
int g_frame_at = 0;
} // namespace

bool debug_ui_available() { return true; }

bool debug_ui_wants_mouse() { return g_ready && ImGui::GetIO().WantCaptureMouse; }
bool debug_ui_wants_keyboard() { return g_ready && ImGui::GetIO().WantCaptureKeyboard; }

void debug_stats_window() {
    if (!g_ready) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (!ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    float worst = 0.0f, sum = 0.0f;
    for (float ms : g_frame_ms) {
        worst = std::max(worst, ms);
        sum += ms;
    }
    ImGui::Text("%.0f fps  (%.2f ms, worst %.1f ms)", io.Framerate, sum / 120.0f, worst);
    char label[32];
    std::snprintf(label, sizeof label, "%.1f ms", g_frame_ms[(g_frame_at + 119) % 120]);
    ImGui::PlotLines("##frames", g_frame_ms, 120, g_frame_at, label, 0.0f, std::max(33.4f, worst), ImVec2(240, 50));
    const three::RenderStats st = three::render_stats();
    ImGui::Text("3D: %d draw calls, %d triangles, %d culled", st.draw_calls, st.triangles, st.culled);
    ImGui::Text("3D sounds playing: %d", three::playing_sound_count());
    ImGui::End();
}

namespace detail {

void debug_ui_setup() {
    simgui_desc_t desc = {};
    desc.no_default_font = false;
    desc.ini_filename = nullptr; // don't drop an imgui.ini next to the game
    simgui_setup(&desc);
    g_ready = true;
}

void debug_ui_new_frame(int width, int height, double dt) {
    if (!g_ready) return;
    g_frame_ms[g_frame_at] = static_cast<float>(dt * 1000.0);
    g_frame_at = (g_frame_at + 1) % 120;
    simgui_frame_desc_t fd = {};
    fd.width = width;
    fd.height = height;
    fd.delta_time = dt > 0.0 ? dt : 1.0 / 60.0;
    fd.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&fd);
}

bool debug_ui_event(const sapp_event* e) {
    if (!g_ready) return false;
    simgui_handle_event(e);
    // Releases always reach the game (a key held when the mouse moved onto a
    // window must not stay stuck down); presses, typing and scrolling don't,
    // while ImGui is using the mouse or keyboard.
    const ImGuiIO& io = ImGui::GetIO();
    switch (e->type) {
        case SAPP_EVENTTYPE_MOUSE_DOWN:
        case SAPP_EVENTTYPE_MOUSE_SCROLL: return io.WantCaptureMouse;
        case SAPP_EVENTTYPE_KEY_DOWN:
        case SAPP_EVENTTYPE_CHAR: return io.WantCaptureKeyboard;
        default: return false;
    }
}

void debug_ui_render() {
    if (g_ready) simgui_render();
}

void debug_ui_shutdown() {
    if (!g_ready) return;
    simgui_shutdown();
    g_ready = false;
}

} // namespace detail

} // namespace thistle
