// Engine-internal glue that both libraries share: the full engine (thistle)
// and the headless one for dedicated servers (thistle_server), which has no
// window, graphics or audio. Nothing declared here may need sokol, so the
// server library builds on a machine without X11/OpenGL. GPU-side glue is in
// thistle_internal.h. Not part of the public API.
#pragma once

#include <thistle.hpp>

namespace thistle::detail {

// --- implemented in log.cpp ---
// The last ~1000 log lines, for the debug overlay and crash reports.
std::vector<std::string>& log_lines();
// Where each log line ("[info] ...") goes as well as log_lines(). nullptr,
// the default, prints it to stderr. The dedicated server's console sets one,
// so logs print above its input line and into its log file.
void set_log_sink(void (*sink)(const std::string& line));

// --- implemented in save.cpp ---
// The save folder's name under the platform's data folder: the app's
// save_name or title. Until one is set, "Thistle".
void set_save_name_source(std::string (*source)());
// A folder to keep save data in as it is, instead of the platform's data
// folder (a dedicated server keeps its own next to it). "" = the default.
void set_save_dir(const std::string& dir);

// --- implemented in thistle.cpp, or in headless.cpp for the server library ---
// Whole-file reads that also work on Android, where assets live inside the
// APK rather than on the filesystem.
bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out);
bool read_file_text(const std::string& path, std::string& out);
bool load_image_rgba(const std::string& path, std::vector<unsigned char>& out, int& w, int& h);

// --- implemented in three_math.cpp ---
// Cheap pre-filter: can the ray touch this box within max_distance? True when
// the ray starts inside it — unlike raycast(ray, Bounds), which reports where
// such a ray *exits* and so would wrongly skip a box you're standing in.
bool ray_reaches_box(const three::Ray& ray, const three::Bounds& box, float max_distance);

// --- implemented in three_net.cpp ---
// Bytes as text for JSON (VoxelSync's chunks, scenes' voxel entities), and back.
// Decoding skips anything that isn't base64.
std::string b64_encode(const std::vector<uint8_t>& in);
std::vector<uint8_t> b64_decode(const std::string& in);

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

// --- implemented in three_models.cpp ---
// Swaps a model's parts for new ones, keeping its id — so a mesh that gets
// rebuilt over and over (a voxel chunk) doesn't grow the model registry.
// An invalid `model` gets a fresh one; empty `data` unloads it.
void replace_model(three::Model& model, const three::ModelData& data);
// Every triangle of every part, in world space (3 vec3 per triangle).
void model_world_triangles(three::Model model, const three::mat4& transform, std::vector<vec3>& out);

} // namespace thistle::detail
