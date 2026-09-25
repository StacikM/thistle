// The model registry behind three::Model handles: every mesh's triangles on
// the CPU, for raycasts, collision and skinning. Shared by the full engine,
// whose renderer uploads the meshes to the GPU, and the headless server
// library, where a model is only ever geometry. Engine-internal.
#pragma once

#include "thistle_core.h"

namespace thistle::three {

struct MeshRecord {
    MeshData cpu; // dropped once uploaded
    // Kept for the life of the mesh (unlike `cpu`): raycasts and collision
    // need the triangles, not the normals/UVs/colors.
    std::vector<vec3> positions;
    std::vector<uint32_t> triangles;
    // The renderer's GPU buffers (sokol ids; 0 = none).
    uint32_t vbuf = 0;
    uint32_t ibuf = 0;
    int index_count = 0;
    bool uploaded = false;
    Bounds bounds;
    // Skinned meshes: the vertices as modeled (with joints/weights), which
    // an Animator's pose is applied to. vbuf holds the rest pose.
    std::vector<Vertex> bind;
};

struct PartRecord {
    int mesh = 0;
    Material material;
    mat4 local;
};

struct ModelRecord {
    std::vector<MeshRecord> meshes;
    std::vector<PartRecord> parts;
    Bounds bounds;
    Skeleton skeleton;
    std::vector<AnimationClip> animations;
    bool alive = true;
};

std::vector<ModelRecord>& model_records();
ModelRecord* model_record(Model m); // null for an invalid or unloaded model

// Frees a mesh's GPU buffers, when a model is unloaded or rebuilt. Set by the
// renderer while it runs; none in the server library.
void set_mesh_release(void (*release)(MeshRecord& mesh));
// Unloads every model (the renderer shutting down).
void unload_all_models();

// Linear blend skinning: each vertex moved by the weighted sum of its
// joints' skin matrices.
void skin_vertices(const std::vector<Vertex>& bind, const std::vector<mat4>& skin, std::vector<Vertex>& out);

} // namespace thistle::three
