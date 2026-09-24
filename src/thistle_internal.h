// Engine-internal glue between thistle.cpp (the 2D engine, which owns the
// sokol setup and the frame loop) and the src/three_*.cpp files. Not part of
// the public API — games only ever include thistle.hpp.
#pragma once

#include <thistle.hpp>

#include "sokol_gfx.h"

struct ma_engine;
struct ma_sound;
struct sapp_event;

namespace thistle::detail {

// --- implemented in thistle.cpp ---
sg_view texture_view(Texture tex); // uploads lazily; id == SG_INVALID_ID if unusable
// Whole-file reads that also work on Android, where assets live inside the
// APK rather than on the filesystem.
bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out);
bool read_file_text(const std::string& path, std::string& out);
bool load_image_rgba(const std::string& path, std::vector<unsigned char>& out, int& w, int& h);
uint64_t frame_index();            // increments once per rendered frame
// The miniaudio engine and the sound-effects group (set_sfx_volume) — null
// before App::run() has set audio up, or if there's no audio device.
ma_engine* audio_engine();
ma_sound* sfx_group();
int frame_width();
int frame_height();

// --- implemented in three_math.cpp ---
// Cheap pre-filter: can the ray touch this box within max_distance? True when
// the ray starts inside it — unlike raycast(ray, Bounds), which reports where
// such a ray *exits* and so would wrongly skip a box you're standing in.
bool ray_reaches_box(const three::Ray& ray, const three::Bounds& box, float max_distance);

// --- implemented in three_voxel.cpp ---
struct VoxelWorldAccess {
    // Bumped by every change to any of the world's chunks.
    static uint64_t revision(const three::VoxelWorld& world);
    // Every chunk holding at least one block, with the revision of its last change.
    static void chunks(const three::VoxelWorld& world, std::vector<std::pair<three::ivec3, uint64_t>>& out);
};

// --- implemented in three_anim.cpp ---
// A matrix back into translation, rotation and scale (no shear).
void decompose(const three::mat4& m, vec3& translation, three::quat& rotation, vec3& scale);
// The skinning matrices of a skeleton standing in its rest pose.
void rest_skin_matrices(const three::Skeleton& skeleton, std::vector<three::mat4>& out);

// --- implemented in three_collider.cpp ---
// One chunk's solid blocks merged into boxes: block min (inclusive) and max
// (exclusive), in the world's block coordinates. Appends to `out`.
void voxel_chunk_boxes(const three::VoxelWorld& world, three::ivec3 chunk, std::vector<std::pair<three::ivec3, three::ivec3>>& out);

// --- implemented in three_terrain.cpp ---
void terrain_triangles(const three::Terrain& terrain, std::vector<vec3>& out); // 3 vec3 per triangle, world space

// --- implemented in three_render.cpp ---
// Swaps a model's parts for new ones, keeping its id — so a mesh that gets
// rebuilt over and over (a voxel chunk) doesn't grow the model registry.
// An invalid `model` gets a fresh one; empty `data` unloads it.
void replace_model(three::Model& model, const three::ModelData& data);
// Every triangle of every part, in world space (3 vec3 per triangle).
void model_world_triangles(three::Model model, const three::mat4& transform, std::vector<vec3>& out);

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
