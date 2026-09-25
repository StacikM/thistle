// Engine-internal glue between thistle.cpp (the 2D engine, which owns the
// sokol setup and the frame loop) and the src/three_*.cpp files: the parts
// that need the GPU, the window or audio. What the headless server library
// shares is in thistle_core.h, included here. Not part of the public API —
// games only ever include thistle.hpp.
#pragma once

#include "thistle_core.h"

#include "sokol_gfx.h"

struct ma_engine;
struct ma_sound;
struct sapp_event;

namespace thistle::detail {

// --- implemented in thistle.cpp ---
sg_view texture_view(Texture tex); // uploads lazily; id == SG_INVALID_ID if unusable
uint64_t frame_index();            // increments once per rendered frame
// The miniaudio engine and the sound-effects group (set_sfx_volume) — null
// before App::run() has set audio up, or if there's no audio device.
ma_engine* audio_engine();
ma_sound* sfx_group();
int frame_width();
int frame_height();

// --- implemented in debug_ui.cpp (or debug_ui_off.cpp), called from thistle.cpp ---
void debug_ui_setup();
void debug_ui_new_frame(int width, int height, double dt);
bool debug_ui_event(const sapp_event* e); // true: the debug UI took it, the game doesn't see it
void debug_ui_render();                   // inside the last pass of the frame
void debug_ui_shutdown();

// --- implemented in three_audio.cpp ---
void three_audio_update();     // once per frame: frees finished sounds
void three_audio_shutdown();   // before the audio engine goes
void three_audio_camera(vec3 position, three::quat rotation); // World::render(): the listener follows unless set manually

// --- implemented in three_render.cpp, called from thistle.cpp's frame loop ---
void three_setup();
void three_shutdown();
void three_before_passes(); // offscreen passes (shadows) — before the main pass begins
void three_draw_layers();   // replaces sgl_draw() inside the main pass
void three_end_frame();

} // namespace thistle::detail
