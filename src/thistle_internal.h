// Engine-internal glue between thistle.cpp (the 2D engine, which owns the
// sokol setup and the frame loop) and the src/three_*.cpp files. Not part of
// the public API — games only ever include thistle.hpp.
#pragma once

#include <thistle.hpp>

#include "sokol_gfx.h"

namespace thistle::detail {

// --- implemented in thistle.cpp ---
sg_view texture_view(Texture tex); // uploads lazily; id == SG_INVALID_ID if unusable
uint64_t frame_index();            // increments once per rendered frame
int frame_width();
int frame_height();

// --- implemented in three_render.cpp, called from thistle.cpp's frame loop ---
void three_setup();
void three_shutdown();
void three_before_passes(); // offscreen passes (shadows) — before the main pass begins
void three_draw_layers();   // replaces sgl_draw() inside the main pass
void three_end_frame();

} // namespace thistle::detail
