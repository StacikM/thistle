// The debug UI when the engine is built without Dear ImGui (THISTLE_DEBUG_UI
// off, the default): nothing to draw, nothing to capture input.
#include "thistle_internal.h"

namespace thistle {

bool debug_ui_available() { return false; }
bool debug_ui_wants_mouse() { return false; }
bool debug_ui_wants_keyboard() { return false; }
void debug_stats_window() {}

namespace detail {
void debug_ui_setup() {}
void debug_ui_new_frame(int, int, double) {}
bool debug_ui_event(const sapp_event*) { return false; }
void debug_ui_render() {}
void debug_ui_shutdown() {}
} // namespace detail

} // namespace thistle
