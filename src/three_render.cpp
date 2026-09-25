#include "thistle_internal.h"
#include "three_models.h"

#include "sokol_gfx.h"
#include "sokol_gl.h"

#include "shaders/background.glsl.h"
#include "shaders/billboard.glsl.h"
#include "shaders/lines.glsl.h"
#include "shaders/lit.glsl.h"
#include "shaders/shadow.glsl.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace thistle::three {

struct WorldImpl {
    struct DrawCmd {
        int model = -1;
        mat4 world;
        rgba tint = white;
        bool override_material = false;
        Material material;
        // Posed by an Animator: its skinned parts' vertices were skinned on
        // the CPU into `skinned` (in part order), starting here. -1: not posed.
        int64_t skin_first = -1;
        Bounds skin_bounds; // world space, of the posed vertices
    };
    // Same layout as GpuVertex (the lit shader's vertex format).
    struct SkinVertex {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
        uint32_t color;
    };
    struct LineVertex {
        float x, y, z;
        uint32_t color;
    };
    struct Light {
        vec3 position;
        float range = 10.0f;
        rgba color = white;
        float intensity = 1.0f;
        bool spot = false;
        vec3 direction{0.0f, -1.0f, 0.0f};
        float cos_outer = -1.0f;
        float cos_inner = -1.0f;
    };
    struct Instance {
        mat4 matrix;
        float color[4];
    };
    struct ManyCmd {
        int model = -1;
        uint32_t first = 0; // into `instances`
        uint32_t count = 0;
        Bounds bounds;      // world space, all copies
    };
    // One camera-facing quad, laid out exactly as the billboard shader's
    // per-instance buffer wants it.
    struct Quad {
        float pos[4];    // center xyz, width
        float params[4]; // height, rotation, upright, unused
        float color[4];
        float uv[4];     // u0 v0 u1 v1
    };
    struct BillboardCmd {
        Quad quad;
        Texture texture;
        bool dot = false; // no texture: soft round dot (particles) instead of a square
        bool additive = false;
    };
    std::vector<DrawCmd> draws;
    std::vector<BillboardCmd> billboards;
    std::vector<ManyCmd> many;
    std::vector<Instance> instances;
    std::vector<Light> lights;
    std::vector<LineVertex> lines;
    std::vector<LineVertex> lines_on_top;
    std::vector<SkinVertex> skinned;
    uint64_t frame = ~0ull;
};

namespace {

struct GpuVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    uint32_t color; // RGBA8, normalized in the shader
};

uint32_t pack_color(rgba c) {
    auto b = [](float x) { return static_cast<uint32_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(c.a) << 24);
}

struct SkyboxRecord {
    std::vector<unsigned char> faces; // 6 tightly packed RGBA faces, dropped after upload
    int size = 0;
    sg_image image = {};
    sg_view view = {};
    bool uploaded = false;
    bool alive = true;
    rgba average_up, average_down, average_side;
};

struct PassRecord {
    Camera camera;
    Rect viewport;
    Sun sun;
    Sky sky;
    Fog fog;
    float ambient = 0.0f;
    std::vector<WorldImpl::Light> lights;
    int shadow_target = -1;   // index into RenderState::shadow_targets, -1 = no shadows this pass
    mat4 shadow_matrix;       // world -> (u, v, depth) in that shadow map
    float shadow_texel = 0.0f; // world size of one shadow-map texel
    std::vector<WorldImpl::DrawCmd> draws;
    std::vector<WorldImpl::BillboardCmd> billboards;
    struct BillboardBatch {
        uint32_t first, count; // into the frame's shared quad buffer
        Texture texture;
        bool dot, additive;
    };
    std::vector<BillboardBatch> billboard_batches; // filled in three_before_passes, in draw order
    std::vector<WorldImpl::ManyCmd> many;
    std::vector<WorldImpl::Instance> instances;
    uint32_t instance_base = 0; // this pass's first instance in the frame's shared instance buffer
    std::vector<WorldImpl::SkinVertex> skinned;
    uint32_t skin_base = 0;     // this pass's first vertex in the frame's shared skinned-vertex buffer
    std::vector<WorldImpl::LineVertex> lines;
    std::vector<WorldImpl::LineVertex> lines_on_top;
    int line_first = 0; // vertex offsets into this frame's shared line buffer
    int on_top_first = 0;
};

enum PipelineKind { PipOpaque, PipOpaqueDouble, PipBlend, PipBlendDouble, PipCount };

struct RenderState {
    bool ready = false;
    sg_shader lit_shader = {};
    sg_shader lit_instanced_shader = {};
    sg_shader background_shader = {};
    sg_pipeline pipelines[PipCount] = {};
    sg_pipeline instanced_pipelines[PipCount] = {};
    sg_buffer instance_buffer = {};
    size_t instance_capacity = 0; // in instances
    sg_buffer skin_buffer = {};   // posed vertices of animated models, this frame
    size_t skin_capacity = 0;
    sg_shader billboard_shader = {};
    sg_pipeline billboard_pipeline = {};
    sg_pipeline billboard_additive_pipeline = {};
    sg_buffer quad_buffer = {};
    size_t quad_capacity = 0;
    sg_image dot_image = {};
    sg_view dot_view = {};
    sg_sampler sprite_sampler = {};
    sg_pipeline sky_pipeline = {};
    sg_pipeline depth_reset_pipeline = {};
    sg_sampler sampler_linear = {};
    sg_sampler sampler_nearest = {};
    sg_image white_image = {};
    sg_view white_view = {};
    sg_image black_cube = {};
    sg_view black_cube_view = {};
    sg_sampler sky_sampler = {};
    std::vector<SkyboxRecord> skyboxes;

    struct ShadowTarget {
        int size = 0;
        sg_image image = {};
        sg_view attachment = {};
        sg_view texture = {};
    };
    sg_shader shadow_shader = {};
    sg_pipeline shadow_pipeline = {};
    sg_shader shadow_instanced_shader = {};
    sg_pipeline shadow_instanced_pipeline = {};
    sg_sampler shadow_sampler = {};
    std::vector<ShadowTarget> shadow_targets; // one per render() that has shadows this frame
    ShadowTarget no_shadow;                   // 1x1, cleared to "far": what passes without shadows bind
    bool no_shadow_cleared = false;
    sg_shader lines_shader = {};
    sg_pipeline line_pipeline = {};
    sg_pipeline line_on_top_pipeline = {};
    sg_buffer line_buffer = {};
    size_t line_capacity = 0; // in vertices

    std::vector<PassRecord> passes;
    int current_layer = 0;
    RenderStats stats_this_frame;
    RenderStats stats_last_frame;

    Model unit_box, unit_sphere, unit_cylinder, unit_cone, unit_plane;
};

RenderState g_three;

bool clip_space_is_gl() {
    const sg_backend b = sg_query_backend();
    return b == SG_BACKEND_GLCORE || b == SG_BACKEND_GLES3;
}

// Metal/D3D11 clip space has z in 0..1 instead of OpenGL's -1..1. The whole
// public API speaks OpenGL (mat4::perspective etc.), so the conversion
// happens exactly once, here, right before the matrix reaches the GPU.
mat4 clip_fix(const mat4& proj_gl) {
    if (clip_space_is_gl()) return proj_gl;
    mat4 fix;
    fix(2, 2) = 0.5f;
    fix(2, 3) = 0.5f;
    return fix * proj_gl;
}

void upload_mesh(MeshRecord& mesh) {
    if (mesh.uploaded || !g_three.ready) return;
    mesh.uploaded = true;
    if (mesh.cpu.indices.empty() || mesh.cpu.vertices.empty()) return;
    std::vector<GpuVertex> verts;
    verts.reserve(mesh.cpu.vertices.size());
    for (const Vertex& v : mesh.cpu.vertices) {
        verts.push_back({v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                         v.uv.x, v.uv.y, pack_color(v.color)});
    }
    sg_buffer_desc vd = {};
    vd.data = {verts.data(), verts.size() * sizeof(GpuVertex)};
    vd.label = "three-mesh-vertices";
    mesh.vbuf = sg_make_buffer(&vd).id;

    sg_buffer_desc id = {};
    id.usage.vertex_buffer = false;
    id.usage.index_buffer = true;
    id.data = {mesh.cpu.indices.data(), mesh.cpu.indices.size() * sizeof(uint32_t)};
    id.label = "three-mesh-indices";
    mesh.ibuf = sg_make_buffer(&id).id;
    mesh.cpu = MeshData{};
    if (sg_query_buffer_state(sg_buffer{mesh.vbuf}) != SG_RESOURCESTATE_VALID || sg_query_buffer_state(sg_buffer{mesh.ibuf}) != SG_RESOURCESTATE_VALID) {
        // Out of GPU buffer slots (or memory). Skip the mesh rather than
        // hand sokol an invalid handle, which is a hard error in debug builds.
        static bool warned = false;
        if (!warned) log_warn("3D: couldn't create a GPU buffer (too many meshes loaded at once?) - some meshes won't draw");
        warned = true;
        return;
    }
    mesh.index_count = static_cast<int>(id.data.size / sizeof(uint32_t));
}

void release_mesh(MeshRecord& mesh) {
    if (mesh.vbuf != SG_INVALID_ID) sg_destroy_buffer(sg_buffer{mesh.vbuf});
    if (mesh.ibuf != SG_INVALID_ID) sg_destroy_buffer(sg_buffer{mesh.ibuf});
    mesh.vbuf = mesh.ibuf = SG_INVALID_ID;
    mesh.index_count = 0;
}

Model unit_model(Model& slot, MeshData (*make)()) {
    if (!slot.valid()) slot = make_model(make());
    return slot;
}

// Per-instance attributes for the *_instanced shaders: four matrix columns
// and a tint, from vertex buffer 1, advancing once per copy.
void add_instance_layout(sg_pipeline_desc& pd, int first_attr) {
    pd.layout.buffers[1].stride = sizeof(WorldImpl::Instance);
    pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
    for (int i = 0; i < 5; ++i) {
        pd.layout.attrs[first_attr + i].buffer_index = 1;
        pd.layout.attrs[first_attr + i].offset = static_cast<int>(i * sizeof(vec4));
        pd.layout.attrs[first_attr + i].format = SG_VERTEXFORMAT_FLOAT4;
    }
}

sg_pipeline make_lit_pipeline(bool blend, bool double_sided, bool instanced) {
    sg_pipeline_desc pd = {};
    pd.shader = instanced ? g_three.lit_instanced_shader : g_three.lit_shader;
    pd.layout.buffers[0].stride = sizeof(GpuVertex);
    pd.layout.attrs[ATTR_lit_position] = {0, offsetof(GpuVertex, px), SG_VERTEXFORMAT_FLOAT3};
    pd.layout.attrs[ATTR_lit_normal] = {0, offsetof(GpuVertex, nx), SG_VERTEXFORMAT_FLOAT3};
    pd.layout.attrs[ATTR_lit_texcoord0] = {0, offsetof(GpuVertex, u), SG_VERTEXFORMAT_FLOAT2};
    pd.layout.attrs[ATTR_lit_color0] = {0, offsetof(GpuVertex, color), SG_VERTEXFORMAT_UBYTE4N};
    if (instanced) add_instance_layout(pd, ATTR_lit_instanced_inst_m0);
    pd.index_type = SG_INDEXTYPE_UINT32;
    pd.face_winding = SG_FACEWINDING_CCW;
    pd.cull_mode = double_sided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
    pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = !blend;
    if (blend) {
        // Same alpha-channel factors as the 2D pipeline: the swapchain's
        // alpha only ever goes up, so a see-through 3D surface never makes
        // the window itself see-through under a compositor.
        pd.colors[0].blend.enabled = true;
        pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }
    pd.label = instanced ? "three-lit-instanced" : "three-lit";
    return sg_make_pipeline(&pd);
}

sg_pipeline make_line_pipeline(bool on_top) {
    sg_pipeline_desc pd = {};
    pd.shader = g_three.lines_shader;
    pd.layout.attrs[ATTR_lines_position].format = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_lines_color0].format = SG_VERTEXFORMAT_UBYTE4N;
    pd.primitive_type = SG_PRIMITIVETYPE_LINES;
    pd.depth.compare = on_top ? SG_COMPAREFUNC_ALWAYS : SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.label = on_top ? "three-lines-on-top" : "three-lines";
    return sg_make_pipeline(&pd);
}

void draw_lines(sg_pipeline pip, int first, int count, const mat4& view_proj) {
    if (count <= 0) return;
    sg_apply_pipeline(pip);
    sg_bindings bind = {};
    bind.vertex_buffers[0] = g_three.line_buffer;
    sg_apply_bindings(&bind);
    lines_vs_params_t vs = {};
    vs.view_proj = view_proj;
    sg_apply_uniforms(UB_lines_vs_params, SG_RANGE(vs));
    sg_draw(first, count, 1);
    ++g_three.stats_this_frame.draw_calls;
}

sg_pipeline make_billboard_pipeline(bool additive) {
    sg_pipeline_desc pd = {};
    pd.shader = g_three.billboard_shader;
    pd.layout.buffers[0].stride = sizeof(WorldImpl::Quad);
    pd.layout.buffers[0].step_func = SG_VERTEXSTEP_PER_INSTANCE;
    pd.layout.attrs[ATTR_billboard_inst_pos] = {0, offsetof(WorldImpl::Quad, pos), SG_VERTEXFORMAT_FLOAT4};
    pd.layout.attrs[ATTR_billboard_inst_params] = {0, offsetof(WorldImpl::Quad, params), SG_VERTEXFORMAT_FLOAT4};
    pd.layout.attrs[ATTR_billboard_inst_color] = {0, offsetof(WorldImpl::Quad, color), SG_VERTEXFORMAT_FLOAT4};
    pd.layout.attrs[ATTR_billboard_inst_uv] = {0, offsetof(WorldImpl::Quad, uv), SG_VERTEXFORMAT_FLOAT4};
    pd.cull_mode = SG_CULLMODE_NONE;
    pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = additive ? SG_BLENDFACTOR_ONE : SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = additive ? SG_BLENDFACTOR_ZERO : SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = additive ? SG_BLENDFACTOR_ONE : SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.label = additive ? "three-billboard-additive" : "three-billboard";
    return sg_make_pipeline(&pd);
}

sg_pipeline make_background_pipeline(bool write_color) {
    sg_pipeline_desc pd = {};
    pd.shader = g_three.background_shader;
    pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pd.depth.write_enabled = true;
    pd.cull_mode = SG_CULLMODE_NONE;
    if (!write_color) pd.colors[0].write_mask = SG_COLORMASK_NONE;
    pd.label = write_color ? "three-sky" : "three-depth-reset";
    return sg_make_pipeline(&pd);
}

vec4 to_vec4(rgba c) { return {c.r, c.g, c.b, c.a}; }

// sRGB -> linear, matching the shader's pow(2.2): the scene uniforms are
// pre-converted on the CPU so the shader does it once per color, not per pixel.
vec4 to_linear4(rgba c, float scale) {
    auto lin = [](float x) { return std::pow(std::max(x, 0.0f), 2.2f); };
    return {lin(c.r) * scale, lin(c.g) * scale, lin(c.b) * scale, 1.0f};
}

SkyboxRecord* skybox_record(Skybox s) {
    if (s.id < 0 || s.id >= static_cast<int>(g_three.skyboxes.size())) return nullptr;
    SkyboxRecord& rec = g_three.skyboxes[s.id];
    return rec.alive ? &rec : nullptr;
}

void upload_skybox(SkyboxRecord& rec) {
    if (rec.uploaded || !g_three.ready) return;
    rec.uploaded = true;
    sg_image_desc d = {};
    d.type = SG_IMAGETYPE_CUBE;
    d.width = rec.size;
    d.height = rec.size;
    d.num_slices = 6;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.data.mip_levels[0] = {rec.faces.data(), rec.faces.size()};
    d.label = "three-skybox";
    rec.image = sg_make_image(&d);
    sg_view_desc v = {};
    v.texture.image = rec.image;
    rec.view = sg_make_view(&v);
    rec.faces.clear();
    rec.faces.shrink_to_fit();
}

// The lights that fit in the shader's 16 slots: off-screen ones first go,
// then the ones whose reach starts furthest from the camera.
std::vector<WorldImpl::Light> pick_lights(const std::vector<WorldImpl::Light>& all, const Frustum& frustum, vec3 eye) {
    std::vector<WorldImpl::Light> visible;
    for (const WorldImpl::Light& l : all) {
        if (l.range > 0.0f && l.intensity > 0.0f && frustum.intersects_sphere(l.position, l.range)) visible.push_back(l);
    }
    if (static_cast<int>(visible.size()) > World::max_lights) {
        std::partial_sort(visible.begin(), visible.begin() + World::max_lights, visible.end(),
                          [&](const WorldImpl::Light& a, const WorldImpl::Light& b) {
                              return distance(a.position, eye) - a.range < distance(b.position, eye) - b.range;
                          });
        visible.resize(World::max_lights);
    }
    return visible;
}

RenderState::ShadowTarget make_shadow_target(int size) {
    RenderState::ShadowTarget t;
    t.size = size;
    sg_image_desc d = {};
    d.usage.depth_stencil_attachment = true;
    d.width = size;
    d.height = size;
    d.pixel_format = SG_PIXELFORMAT_DEPTH;
    d.sample_count = 1;
    d.label = "three-shadow-map";
    t.image = sg_make_image(&d);
    sg_view_desc a = {};
    a.depth_stencil_attachment.image = t.image;
    t.attachment = sg_make_view(&a);
    sg_view_desc v = {};
    v.texture.image = t.image;
    t.texture = sg_make_view(&v);
    return t;
}

void destroy_shadow_target(RenderState::ShadowTarget& t) {
    if (t.texture.id != SG_INVALID_ID) sg_destroy_view(t.texture);
    if (t.attachment.id != SG_INVALID_ID) sg_destroy_view(t.attachment);
    if (t.image.id != SG_INVALID_ID) sg_destroy_image(t.image);
    t = RenderState::ShadowTarget{};
}

sg_pipeline make_shadow_pipeline(bool instanced) {
    sg_pipeline_desc pd = {};
    pd.shader = instanced ? g_three.shadow_instanced_shader : g_three.shadow_shader;
    // Same vertex buffers as the lit pass, minus the normal, so the offsets
    // and stride are spelled out instead of inferred from the attributes.
    pd.layout.buffers[0].stride = sizeof(GpuVertex);
    pd.layout.attrs[ATTR_shadow_position] = {0, offsetof(GpuVertex, px), SG_VERTEXFORMAT_FLOAT3};
    pd.layout.attrs[ATTR_shadow_texcoord0] = {0, offsetof(GpuVertex, u), SG_VERTEXFORMAT_FLOAT2};
    pd.layout.attrs[ATTR_shadow_color0] = {0, offsetof(GpuVertex, color), SG_VERTEXFORMAT_UBYTE4N};
    if (instanced) add_instance_layout(pd, ATTR_shadow_instanced_inst_m0);
    pd.index_type = SG_INDEXTYPE_UINT32;
    // Both faces: a single-sided plane (a roof, a billboard) still blocks the sun.
    pd.cull_mode = SG_CULLMODE_NONE;
    pd.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
    pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = true;
    pd.depth.bias = 1.0f;
    pd.depth.bias_slope_scale = 1.5f;
    pd.colors[0].pixel_format = SG_PIXELFORMAT_NONE; // sokol's way of saying "depth only"
    pd.sample_count = 1;
    pd.label = "three-shadow";
    return sg_make_pipeline(&pd);
}

// Maps OpenGL-style clip space onto shadow-map (u, v, depth). GL render
// targets have their origin at the bottom and depth in -1..1; D3D11/Metal
// at the top, with depth already 0..1 after clip_fix().
mat4 shadow_bias_matrix() {
    mat4 b;
    b(0, 0) = 0.5f;
    b(0, 3) = 0.5f;
    if (clip_space_is_gl()) {
        b(1, 1) = 0.5f;
        b(1, 3) = 0.5f;
        b(2, 2) = 0.5f;
        b(2, 3) = 0.5f;
    } else {
        b(1, 1) = -0.5f;
        b(1, 3) = 0.5f;
    }
    return b;
}

struct LightFit {
    mat4 view_proj_gl; // for culling casters
    mat4 view_proj;    // what the shadow pass renders with (backend-fixed)
    float texel = 0.0f;
};

LightFit fit_sun(const PassRecord& pass, float aspect, int resolution) {
    const Camera& cam = pass.camera;
    Camera slice = cam;
    slice.far_z = std::max(std::min(cam.far_z, pass.sun.shadow_distance), cam.near_z + 0.01f);
    const mat4 inv = inverse(slice.projection(aspect) * slice.view());
    vec3 corners[8];
    vec3 center{0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        corners[i] = inv.transform_point({(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f});
        center += corners[i] * 0.125f;
    }
    float radius = 0.0f;
    for (const vec3& c : corners) radius = std::max(radius, distance(c, center));
    // A bounding sphere (not a tight box) keeps the shadow map's size fixed
    // while the camera turns, and rounding it keeps it fixed as the
    // camera's near/far math jitters — both prevent shimmering edges.
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const vec3 dir = normalize(pass.sun.direction);
    const vec3 up = std::fabs(dir.y) > 0.99f ? vec3{0, 0, 1} : vec3{0, 1, 0};
    const mat4 light_rot = mat4::look_at({0, 0, 0}, dir, up);
    vec3 c = light_rot.transform_point(center);
    const float texel = 2.0f * radius / static_cast<float>(resolution);
    // Snap to whole texels, so moving the camera slides the shadow map in
    // texel steps and existing shadow edges don't crawl.
    c.x = std::floor(c.x / texel) * texel;
    c.y = std::floor(c.y / texel) * texel;
    // Casters between the sun and the visible area can be far outside the
    // view (a mountain behind you), so the depth range reaches well back.
    const float caster_reach = std::max(150.0f, radius * 4.0f);
    const mat4 proj = mat4::ortho(c.x - radius, c.x + radius, c.y - radius, c.y + radius,
                                  -c.z - radius - caster_reach, -c.z + radius);
    LightFit fit;
    fit.view_proj_gl = proj * light_rot;
    fit.view_proj = clip_fix(proj) * light_rot;
    fit.texel = texel;
    return fit;
}

void render_shadow_map(PassRecord& pass, int index, int fb_w, int fb_h) {
    RenderState& s = g_three;
    const int res = std::clamp(pass.sun.shadow_resolution, 256, 8192);
    if (static_cast<int>(s.shadow_targets.size()) <= index) s.shadow_targets.resize(index + 1);
    RenderState::ShadowTarget& target = s.shadow_targets[index];
    if (target.size != res) {
        destroy_shadow_target(target);
        target = make_shadow_target(res);
    }
    const bool full = pass.viewport.size.x <= 0.0f || pass.viewport.size.y <= 0.0f;
    // (Shadow casters: pass.draws with the regular pipeline, then pass.many instanced.)
    const float vw = full ? static_cast<float>(fb_w) : pass.viewport.size.x;
    const float vh = full ? static_cast<float>(fb_h) : pass.viewport.size.y;
    const LightFit fit = fit_sun(pass, vh > 0.0f ? vw / vh : 1.0f, res);
    const Frustum light_frustum = Frustum::from_matrix(fit.view_proj_gl);

    sg_pass sp = {};
    sp.action.depth.load_action = SG_LOADACTION_CLEAR;
    sp.action.depth.clear_value = 1.0f;
    sp.attachments.depth_stencil = target.attachment;
    sp.label = "three-shadow-pass";
    sg_begin_pass(&sp);
    sg_apply_pipeline(s.shadow_pipeline);
    for (const WorldImpl::DrawCmd& cmd : pass.draws) {
        ModelRecord* rec = model_record(Model{cmd.model});
        if (!rec) continue;
        int64_t skin_at = cmd.skin_first;
        for (const PartRecord& part : rec->parts) {
            MeshRecord& mesh = rec->meshes[part.mesh];
            const int64_t skin_first = cmd.skin_first >= 0 && !mesh.bind.empty() ? skin_at : -1;
            if (skin_first >= 0) skin_at += static_cast<int64_t>(mesh.bind.size());
            const Material& mat = cmd.override_material ? cmd.material : part.material;
            if (!mat.casts_shadow || mat.alpha == AlphaMode::Blend) continue;
            upload_mesh(mesh);
            if (mesh.index_count == 0) continue;
            const mat4 world = skin_first >= 0 ? cmd.world : cmd.world * part.local;
            // (instanced casters are drawn after these, with their own pipeline)
            if (!light_frustum.intersects(skin_first >= 0 ? cmd.skin_bounds : mesh.bounds.transformed(world))) continue;
            sg_bindings bind = {};
            bind.vertex_buffers[0] = sg_buffer{mesh.vbuf};
            if (skin_first >= 0) {
                bind.vertex_buffers[0] = s.skin_buffer;
                bind.vertex_buffer_offsets[0] = static_cast<int>((pass.skin_base + skin_first) * sizeof(WorldImpl::SkinVertex));
            }
            bind.index_buffer = sg_buffer{mesh.ibuf};
            const bool cutout = mat.alpha == AlphaMode::Cutout;
            sg_view tex = cutout && mat.texture.valid() ? detail::texture_view(mat.texture) : sg_view{};
            bind.views[VIEW_base_tex] = tex.id != SG_INVALID_ID ? tex : s.white_view;
            bind.samplers[SMP_base_smp] = mat.filter == TextureFilter::Nearest ? s.sampler_nearest : s.sampler_linear;
            sg_apply_bindings(&bind);
            shadow_vs_params_t vs = {};
            vs.light_mvp = fit.view_proj * world;
            vs.uv_transform = {mat.uv_scale.x, mat.uv_scale.y, 0.0f, 0.0f};
            sg_apply_uniforms(UB_shadow_vs_params, SG_RANGE(vs));
            shadow_fs_params_t fs = {};
            fs.cutout = {cutout ? mat.alpha_cutoff : -1.0f, mat.color.a * cmd.tint.a, 0.0f, 0.0f};
            sg_apply_uniforms(UB_shadow_fs_params, SG_RANGE(fs));
            sg_draw(0, mesh.index_count, 1);
            ++s.stats_this_frame.draw_calls;
        }
    }
    bool instanced_bound = false;
    for (const WorldImpl::ManyCmd& cmd : pass.many) {
        ModelRecord* rec = model_record(Model{cmd.model});
        if (!rec || cmd.count == 0 || !light_frustum.intersects(cmd.bounds)) continue;
        for (const PartRecord& part : rec->parts) {
            const Material& mat = part.material;
            if (!mat.casts_shadow || mat.alpha == AlphaMode::Blend) continue;
            MeshRecord& mesh = rec->meshes[part.mesh];
            upload_mesh(mesh);
            if (mesh.index_count == 0) continue;
            if (!instanced_bound) { sg_apply_pipeline(s.shadow_instanced_pipeline); instanced_bound = true; }
            sg_bindings bind = {};
            bind.vertex_buffers[0] = sg_buffer{mesh.vbuf};
            bind.vertex_buffers[1] = s.instance_buffer;
            bind.vertex_buffer_offsets[1] = static_cast<int>((pass.instance_base + cmd.first) * sizeof(WorldImpl::Instance));
            bind.index_buffer = sg_buffer{mesh.ibuf};
            const bool cutout = mat.alpha == AlphaMode::Cutout;
            sg_view tex = cutout && mat.texture.valid() ? detail::texture_view(mat.texture) : sg_view{};
            bind.views[VIEW_base_tex] = tex.id != SG_INVALID_ID ? tex : s.white_view;
            bind.samplers[SMP_base_smp] = mat.filter == TextureFilter::Nearest ? s.sampler_nearest : s.sampler_linear;
            sg_apply_bindings(&bind);
            shadow_vs_params_t vs = {};
            vs.light_mvp = fit.view_proj * part.local;
            vs.uv_transform = {mat.uv_scale.x, mat.uv_scale.y, 0.0f, 0.0f};
            sg_apply_uniforms(UB_shadow_vs_params, SG_RANGE(vs));
            shadow_fs_params_t fs = {};
            fs.cutout = {cutout ? mat.alpha_cutoff : -1.0f, mat.color.a, 0.0f, 0.0f};
            sg_apply_uniforms(UB_shadow_fs_params, SG_RANGE(fs));
            sg_draw(0, mesh.index_count, static_cast<int>(cmd.count));
            ++s.stats_this_frame.draw_calls;
        }
    }
    sg_end_pass();
    pass.shadow_target = index;
    pass.shadow_matrix = shadow_bias_matrix() * fit.view_proj;
    pass.shadow_texel = fit.texel;
}

struct DrawItem {
    const MeshRecord* mesh;
    const Material* material;
    mat4 world;          // for instanced items: the part's placement inside the model
    rgba tint;
    float sort_depth;
    PipelineKind pipeline;
    uint32_t inst_first = 0; // instanced items: offset into the frame's instance buffer
    uint32_t inst_count = 0; // 0 = a plain single draw
    int64_t skin_first = -1; // posed items: first vertex in the frame's skinned-vertex buffer
};

void render_pass(const PassRecord& pass, int fb_w, int fb_h) {
    const bool full = pass.viewport.size.x <= 0.0f || pass.viewport.size.y <= 0.0f;
    const int vx = full ? 0 : static_cast<int>(pass.viewport.pos.x);
    const int vy = full ? 0 : static_cast<int>(pass.viewport.pos.y);
    const int vw = full ? fb_w : static_cast<int>(pass.viewport.size.x);
    const int vh = full ? fb_h : static_cast<int>(pass.viewport.size.y);
    if (vw <= 0 || vh <= 0) return;
    sg_apply_viewport(vx, vy, vw, vh, true);
    sg_apply_scissor_rect(vx, vy, vw, vh, true);

    const mat4 view = pass.camera.view();
    const mat4 proj_gl = pass.camera.projection(static_cast<float>(vw) / static_cast<float>(vh));
    const mat4 view_proj_gl = proj_gl * view;
    const mat4 view_proj = clip_fix(proj_gl) * view;
    RenderStats& stats = g_three.stats_this_frame;

    SkyboxRecord* skybox = skybox_record(pass.sky.skybox);
    if (skybox) upload_skybox(*skybox);
    const bool use_skybox = skybox && skybox->image.id != SG_INVALID_ID;
    // With a skybox, its own average colors drive ambient and fog, so a
    // sunset skybox actually tints the scene like a sunset.
    const rgba amb_top = use_skybox ? skybox->average_up : pass.sky.top;
    const rgba amb_ground = use_skybox ? skybox->average_down : pass.sky.ground;
    const rgba horizon = use_skybox ? skybox->average_side : pass.sky.horizon;
    const vec3 sun_dir = normalize(pass.sun.direction);

    // Background: sky (or nothing) plus a depth reset for this pass.
    sg_apply_pipeline(pass.sky.visible ? g_three.sky_pipeline : g_three.depth_reset_pipeline);
    sg_bindings bg_bind = {};
    bg_bind.views[VIEW_sky_tex] = use_skybox ? skybox->view : g_three.black_cube_view;
    bg_bind.samplers[SMP_sky_smp] = g_three.sky_sampler;
    sg_apply_bindings(&bg_bind);
    background_vs_params_t bvs = {};
    bvs.inv_view_proj = inverse(view_proj_gl);
    sg_apply_uniforms(UB_background_vs_params, SG_RANGE(bvs));
    background_fs_params_t bfs = {};
    bfs.top = to_vec4(pass.sky.top);
    bfs.horizon = to_vec4(pass.sky.horizon);
    bfs.ground = to_vec4(pass.sky.ground);
    bfs.to_sun = {-sun_dir.x, -sun_dir.y, -sun_dir.z, pass.sky.sun_disc ? 1.0f : 0.0f};
    bfs.sun_color = to_vec4(pass.sun.color);
    bfs.mode = {use_skybox ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    sg_apply_uniforms(UB_background_fs_params, SG_RANGE(bfs));
    sg_draw(0, 3, 1);
    ++stats.draw_calls;

    const Frustum frustum = Frustum::from_matrix(view_proj_gl);
    std::vector<DrawItem> items;
    for (const WorldImpl::DrawCmd& cmd : pass.draws) {
        ModelRecord* rec = model_record(Model{cmd.model});
        if (!rec) continue;
        int64_t skin_at = cmd.skin_first;
        for (const PartRecord& part : rec->parts) {
            MeshRecord& mesh = rec->meshes[part.mesh];
            // Posed parts: their vertices are already in the model's space
            // (the pose placed them), so only the draw's transform applies.
            const int64_t skin_first = cmd.skin_first >= 0 && !mesh.bind.empty() ? skin_at : -1;
            if (skin_first >= 0) skin_at += static_cast<int64_t>(mesh.bind.size());
            upload_mesh(mesh);
            if (mesh.index_count == 0) continue;
            const Material* mat = cmd.override_material ? &cmd.material : &part.material;
            const bool blend = mat->alpha == AlphaMode::Blend;
            const PipelineKind kind = blend ? (mat->double_sided ? PipBlendDouble : PipBlend)
                                            : (mat->double_sided ? PipOpaqueDouble : PipOpaque);
            const mat4 world = skin_first >= 0 ? cmd.world : cmd.world * part.local;
            const Bounds box = skin_first >= 0 ? cmd.skin_bounds : mesh.bounds.transformed(world);
            if (!frustum.intersects(box)) {
                ++stats.culled;
                continue;
            }
            DrawItem item{&mesh, mat, world, cmd.tint, dot(box.center() - pass.camera.position, pass.camera.forward()), kind};
            if (skin_first >= 0) item.skin_first = pass.skin_base + skin_first;
            items.push_back(item);
        }
    }
    for (const WorldImpl::ManyCmd& cmd : pass.many) {
        ModelRecord* rec = model_record(Model{cmd.model});
        if (!rec || cmd.count == 0) continue;
        if (!frustum.intersects(cmd.bounds)) {
            stats.culled += static_cast<int>(rec->parts.size());
            continue;
        }
        for (const PartRecord& part : rec->parts) {
            MeshRecord& mesh = rec->meshes[part.mesh];
            upload_mesh(mesh);
            if (mesh.index_count == 0) continue;
            const Material* mat = &part.material;
            const bool blend = mat->alpha == AlphaMode::Blend;
            const PipelineKind kind = blend ? (mat->double_sided ? PipBlendDouble : PipBlend)
                                            : (mat->double_sided ? PipOpaqueDouble : PipOpaque);
            DrawItem item{&mesh, mat, part.local, white, dot(cmd.bounds.center() - pass.camera.position, pass.camera.forward()), kind};
            item.inst_first = pass.instance_base + cmd.first;
            item.inst_count = cmd.count;
            items.push_back(item);
        }
    }
    // Opaque first (grouped by pipeline to cut state changes), then
    // see-through surfaces far-to-near so they blend over what's behind them.
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        const bool ab = a.pipeline >= PipBlend, bb = b.pipeline >= PipBlend;
        if (ab != bb) return !ab;
        if (ab) return a.sort_depth > b.sort_depth;
        if (a.pipeline != b.pipeline) return a.pipeline < b.pipeline;
        return (a.inst_count > 0) < (b.inst_count > 0);
    });

    lit_scene_params_t scene = {};
    scene.camera_pos = {pass.camera.position.x, pass.camera.position.y, pass.camera.position.z, 1.0f};
    scene.sun_dir = {sun_dir.x, sun_dir.y, sun_dir.z, 0.0f};
    scene.sun_color = to_linear4(pass.sun.color, pass.sun.intensity);
    scene.sky_ambient = to_linear4(amb_top, pass.ambient);
    scene.ground_ambient = to_linear4(amb_ground, pass.ambient);
    if (pass.fog.enabled) {
        scene.fog_color = to_linear4(pass.fog.match_sky ? horizon : pass.fog.color, 1.0f);
        const float span = std::max(pass.fog.end - pass.fog.start, 1e-3f);
        scene.fog_params = {pass.fog.start, 1.0f / span, 1.0f, 0.0f};
    }
    const bool shadowed = pass.shadow_target >= 0;
    if (shadowed) {
        const int res = g_three.shadow_targets[pass.shadow_target].size;
        scene.shadow_matrix = pass.shadow_matrix;
        scene.shadow_params = {1.0f, std::max(pass.sun.shadow_softness, 0.0f) / static_cast<float>(res),
                               pass.shadow_texel * 1.5f, std::clamp(pass.sun.shadow_strength, 0.0f, 1.0f)};
        const float fade_len = pass.sun.shadow_distance * 0.15f;
        scene.shadow_fade = {pass.sun.shadow_distance - fade_len, 1.0f / std::max(fade_len, 1e-3f), 0.0f, 0.0f};
    }
    const sg_view shadow_view = shadowed ? g_three.shadow_targets[pass.shadow_target].texture : g_three.no_shadow.texture;
    const std::vector<WorldImpl::Light> lights = pick_lights(pass.lights, frustum, pass.camera.position);
    scene.light_count = {static_cast<float>(lights.size()), 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < lights.size(); ++i) {
        const WorldImpl::Light& l = lights[i];
        scene.light_pos[i] = {l.position.x, l.position.y, l.position.z, std::max(l.range, 1e-3f)};
        const vec4 c = to_linear4(l.color, l.intensity);
        scene.light_color[i] = {c.x, c.y, c.z, l.spot ? l.cos_outer : -2.0f};
        scene.light_dir[i] = {l.direction.x, l.direction.y, l.direction.z, l.cos_inner};
    }

    int bound_pipeline = -1;
    bool lines_drawn = false;
    for (const DrawItem& item : items) {
        if (item.pipeline >= PipBlend && !lines_drawn) {
            draw_lines(g_three.line_pipeline, pass.line_first, static_cast<int>(pass.lines.size()), view_proj);
            lines_drawn = true;
            bound_pipeline = -1;
        }
        const bool instanced = item.inst_count > 0;
        const int want_pipeline = item.pipeline + (instanced ? PipCount : 0);
        if (want_pipeline != bound_pipeline) {
            sg_apply_pipeline(instanced ? g_three.instanced_pipelines[item.pipeline] : g_three.pipelines[item.pipeline]);
            sg_apply_uniforms(UB_lit_scene_params, SG_RANGE(scene));
            bound_pipeline = want_pipeline;
        }
        const Material& mat = *item.material;
        sg_bindings bind = {};
        bind.vertex_buffers[0] = sg_buffer{item.mesh->vbuf};
        if (item.skin_first >= 0) {
            bind.vertex_buffers[0] = g_three.skin_buffer;
            bind.vertex_buffer_offsets[0] = static_cast<int>(item.skin_first * sizeof(WorldImpl::SkinVertex));
        }
        if (instanced) {
            bind.vertex_buffers[1] = g_three.instance_buffer;
            bind.vertex_buffer_offsets[1] = static_cast<int>(item.inst_first * sizeof(WorldImpl::Instance));
        }
        bind.index_buffer = sg_buffer{item.mesh->ibuf};
        sg_view tex_view = mat.texture.valid() ? detail::texture_view(mat.texture) : sg_view{};
        bind.views[VIEW_base_tex] = tex_view.id != SG_INVALID_ID ? tex_view : g_three.white_view;
        const sg_view glow_view = mat.emissive_texture.valid() ? detail::texture_view(mat.emissive_texture) : sg_view{};
        bind.views[VIEW_emissive_tex] = glow_view.id != SG_INVALID_ID ? glow_view : g_three.white_view;
        bind.views[VIEW_shadow_map] = shadow_view;
        bind.samplers[SMP_shadow_smp] = g_three.shadow_sampler;
        bind.samplers[SMP_base_smp] = mat.filter == TextureFilter::Nearest ? g_three.sampler_nearest : g_three.sampler_linear;
        sg_apply_bindings(&bind);

        lit_vs_params_t vs = {};
        vs.model = item.world;
        vs.view_proj = view_proj;
        vs.normal_matrix = instanced ? mat4{} : transpose(inverse(item.world)); // instanced: computed per copy in the shader
        vs.uv_transform = {mat.uv_scale.x, mat.uv_scale.y, 0.0f, 0.0f};
        sg_apply_uniforms(UB_lit_vs_params, SG_RANGE(vs));

        lit_material_params_t mp = {};
        mp.base_color = {mat.color.r * item.tint.r, mat.color.g * item.tint.g,
                         mat.color.b * item.tint.b, mat.color.a * item.tint.a};
        const bool opaque = mat.alpha == AlphaMode::Opaque;
        mp.emissive = {mat.emissive.r, mat.emissive.g, mat.emissive.b, opaque ? 1.0f : 0.0f};
        mp.surface = {mat.specular, std::max(mat.shininess, 1.0f), mat.unlit ? 1.0f : 0.0f,
                      mat.alpha == AlphaMode::Cutout ? mat.alpha_cutoff : -1.0f};
        sg_apply_uniforms(UB_lit_material_params, SG_RANGE(mp));

        const int copies = instanced ? static_cast<int>(item.inst_count) : 1;
        sg_draw(0, item.mesh->index_count, copies);
        ++stats.draw_calls;
        stats.triangles += item.mesh->index_count / 3 * copies;
    }

    if (!lines_drawn) draw_lines(g_three.line_pipeline, pass.line_first, static_cast<int>(pass.lines.size()), view_proj);

    if (!pass.billboard_batches.empty()) {
        billboard_vs_params_t bvs2 = {};
        bvs2.view_proj = view_proj;
        const vec3 r = pass.camera.right(), u = pass.camera.up();
        bvs2.cam_right = {r.x, r.y, r.z, 0.0f};
        bvs2.cam_up = {u.x, u.y, u.z, 0.0f};
        bvs2.cam_pos = {pass.camera.position.x, pass.camera.position.y, pass.camera.position.z, 1.0f};
        int bound = -1;
        for (const PassRecord::BillboardBatch& batch : pass.billboard_batches) {
            if (static_cast<int>(batch.additive) != bound) {
                sg_apply_pipeline(batch.additive ? g_three.billboard_additive_pipeline : g_three.billboard_pipeline);
                sg_apply_uniforms(UB_billboard_vs_params, SG_RANGE(bvs2));
                billboard_fs_params_t bfs2 = {};
                bfs2.fog_color = scene.fog_color;
                bfs2.fog_params = {scene.fog_params.x, scene.fog_params.y, scene.fog_params.z, batch.additive ? 1.0f : 0.0f};
                bfs2.fs_cam_pos = bvs2.cam_pos;
                sg_apply_uniforms(UB_billboard_fs_params, SG_RANGE(bfs2));
                bound = batch.additive;
            }
            sg_bindings bind = {};
            bind.vertex_buffers[0] = g_three.quad_buffer;
            bind.vertex_buffer_offsets[0] = static_cast<int>(batch.first * sizeof(WorldImpl::Quad));
            sg_view tex = batch.texture.valid() ? detail::texture_view(batch.texture) : sg_view{};
            bind.views[VIEW_sprite_tex] = tex.id != SG_INVALID_ID ? tex : (batch.dot ? g_three.dot_view : g_three.white_view);
            bind.samplers[SMP_sprite_smp] = g_three.sprite_sampler;
            sg_apply_bindings(&bind);
            sg_draw(0, 6, static_cast<int>(batch.count));
            ++stats.draw_calls;
            stats.triangles += static_cast<int>(batch.count) * 2;
        }
    }
    draw_lines(g_three.line_on_top_pipeline, pass.on_top_first, static_cast<int>(pass.lines_on_top.size()), view_proj);

    // Back to the full framebuffer for the 2D layers drawn after this pass.
    sg_apply_viewport(0, 0, fb_w, fb_h, true);
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
}

} // namespace

// --- world ---------------------------------------------------------------------

namespace {
WorldImpl& touch(std::unique_ptr<WorldImpl>& impl) {
    if (!impl) impl = std::make_unique<WorldImpl>();
    const uint64_t frame = detail::frame_index();
    if (impl->frame != frame) {
        impl->draws.clear();
        impl->billboards.clear();
        impl->many.clear();
        impl->instances.clear();
        impl->lights.clear();
        impl->lines.clear();
        impl->lines_on_top.clear();
        impl->skinned.clear();
        impl->frame = frame;
    }
    return *impl;
}
} // namespace

World::World() : impl_(std::make_unique<WorldImpl>()) {}
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

void World::light(const PointLight& light) {
    WorldImpl::Light l;
    l.position = light.position;
    l.range = light.range;
    l.color = light.color;
    l.intensity = light.intensity;
    touch(impl_).lights.push_back(l);
}

void World::light(const SpotLight& light) {
    WorldImpl::Light l;
    l.position = light.position;
    l.range = light.range;
    l.color = light.color;
    l.intensity = light.intensity;
    l.spot = true;
    l.direction = normalize(light.direction);
    const float outer = std::clamp(light.angle, 0.001f, pi * 0.5f);
    const float inner = outer * (1.0f - std::clamp(light.softness, 0.0f, 1.0f));
    l.cos_outer = std::cos(outer);
    l.cos_inner = std::max(std::cos(inner), l.cos_outer + 1e-4f); // smoothstep needs edge0 < edge1
    touch(impl_).lights.push_back(l);
}

void World::draw(Model model, const Transform& transform, rgba tint) {
    if (!model_record(model)) return;
    WorldImpl::DrawCmd cmd;
    cmd.model = model.id;
    cmd.world = transform.matrix();
    cmd.tint = tint;
    touch(impl_).draws.push_back(std::move(cmd));
}

void World::draw(Model model, const Transform& transform, const Animator& pose, rgba tint) {
    ModelRecord* rec = model_record(model);
    if (!rec) return;
    const std::vector<mat4>& skin = pose.skin_matrices();
    if (rec->skeleton.empty() || skin.size() != rec->skeleton.joints.size() || pose.model().id != model.id) {
        draw(model, transform, tint); // not animated (or the Animator is for another model): the rest pose
        return;
    }
    WorldImpl& impl = touch(impl_);
    WorldImpl::DrawCmd cmd;
    cmd.model = model.id;
    cmd.world = transform.matrix();
    cmd.tint = tint;
    cmd.skin_first = static_cast<int64_t>(impl.skinned.size());
    static thread_local std::vector<Vertex> posed;
    for (const PartRecord& part : rec->parts) {
        const MeshRecord& mesh = rec->meshes[part.mesh];
        if (mesh.bind.empty()) continue;
        skin_vertices(mesh.bind, skin, posed);
        for (const Vertex& v : posed) {
            impl.skinned.push_back({v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y,
                                    pack_color(v.color)});
            cmd.skin_bounds.add(cmd.world.transform_point(v.position));
        }
    }
    impl.draws.push_back(std::move(cmd));
}

void World::draw(Model model, const Transform& transform, const Material& material) {
    if (!model_record(model)) return;
    WorldImpl::DrawCmd cmd;
    cmd.model = model.id;
    cmd.world = transform.matrix();
    cmd.override_material = true;
    cmd.material = material;
    touch(impl_).draws.push_back(std::move(cmd));
}

void World::draw_many(Model model, const Transform* transforms, size_t count, const rgba* tints) {
    const ModelRecord* rec = model_record(model);
    if (!rec || count == 0 || !transforms) return;
    WorldImpl& impl = touch(impl_);
    WorldImpl::ManyCmd cmd;
    cmd.model = model.id;
    cmd.first = static_cast<uint32_t>(impl.instances.size());
    cmd.count = static_cast<uint32_t>(count);
    for (size_t i = 0; i < count; ++i) {
        WorldImpl::Instance inst;
        inst.matrix = transforms[i].matrix();
        const rgba t = tints ? tints[i] : white;
        inst.color[0] = t.r; inst.color[1] = t.g; inst.color[2] = t.b; inst.color[3] = t.a;
        impl.instances.push_back(inst);
        const Bounds b = rec->bounds.transformed(inst.matrix);
        if (b.valid()) { cmd.bounds.add(b.min); cmd.bounds.add(b.max); }
    }
    impl.many.push_back(cmd);
}

void World::billboard(const Billboard& b) {
    WorldImpl::BillboardCmd cmd;
    WorldImpl::Quad& q = cmd.quad;
    q.pos[0] = b.position.x; q.pos[1] = b.position.y; q.pos[2] = b.position.z; q.pos[3] = b.size.x;
    q.params[0] = b.size.y; q.params[1] = b.rotation; q.params[2] = b.upright ? 1.0f : 0.0f; q.params[3] = 0.0f;
    q.color[0] = b.color.r; q.color[1] = b.color.g; q.color[2] = b.color.b; q.color[3] = b.color.a;
    q.uv[0] = 0.0f; q.uv[1] = 0.0f; q.uv[2] = 1.0f; q.uv[3] = 1.0f;
    if (b.texture.valid() && b.frame.size.x > 0.0f && b.frame.size.y > 0.0f && b.texture.width > 0 && b.texture.height > 0) {
        const float w = static_cast<float>(b.texture.width), h = static_cast<float>(b.texture.height);
        q.uv[0] = b.frame.pos.x / w;
        q.uv[1] = b.frame.pos.y / h;
        q.uv[2] = (b.frame.pos.x + b.frame.size.x) / w;
        q.uv[3] = (b.frame.pos.y + b.frame.size.y) / h;
    }
    cmd.texture = b.texture;
    cmd.additive = b.additive;
    touch(impl_).billboards.push_back(cmd);
}

void World::draw(const ParticleSystem& particles) {
    WorldImpl& impl = touch(impl_);
    for (const ParticleSystem::Particle& p : particles.particles()) {
        const ParticleSettings& look = *p.look;
        const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
        const rgba c = lerp(look.start_color, look.end_color, t);
        const float size = look.start_size + (look.end_size - look.start_size) * t;
        WorldImpl::BillboardCmd cmd;
        WorldImpl::Quad& q = cmd.quad;
        q.pos[0] = p.position.x; q.pos[1] = p.position.y; q.pos[2] = p.position.z; q.pos[3] = size;
        q.params[0] = size; q.params[1] = p.rotation; q.params[2] = 0.0f; q.params[3] = 0.0f;
        q.color[0] = c.r; q.color[1] = c.g; q.color[2] = c.b; q.color[3] = c.a;
        q.uv[0] = 0.0f; q.uv[1] = 0.0f; q.uv[2] = 1.0f; q.uv[3] = 1.0f;
        cmd.texture = look.texture;
        cmd.dot = true;
        cmd.additive = look.additive;
        impl.billboards.push_back(cmd);
    }
}

void World::box(vec3 center, vec3 size, rgba color) {
    draw(unit_model(g_three.unit_box, [] { return box_mesh(); }), Transform{center, {}, size}, color);
}

void World::sphere(vec3 center, float radius, rgba color) {
    const float d = radius * 2.0f;
    draw(unit_model(g_three.unit_sphere, [] { return sphere_mesh(); }), Transform{center, {}, {d, d, d}}, color);
}

void World::cylinder(vec3 center, float radius, float height, rgba color) {
    const float d = radius * 2.0f;
    draw(unit_model(g_three.unit_cylinder, [] { return cylinder_mesh(); }), Transform{center, {}, {d, height, d}}, color);
}

void World::cone(vec3 center, float radius, float height, rgba color) {
    const float d = radius * 2.0f;
    draw(unit_model(g_three.unit_cone, [] { return cone_mesh(); }), Transform{center, {}, {d, height, d}}, color);
}

void World::plane(vec3 center, vec2 size, rgba color) {
    draw(unit_model(g_three.unit_plane, [] { return plane_mesh(); }), Transform{center, {}, {size.x, 1.0f, size.y}}, color);
}

void World::shape(int kind, const Transform& transform, rgba color) {
    switch (kind) {
        case 0: draw(unit_model(g_three.unit_box, [] { return box_mesh(); }), transform, color); break;
        case 1: draw(unit_model(g_three.unit_sphere, [] { return sphere_mesh(); }), transform, color); break;
        case 2: draw(unit_model(g_three.unit_cylinder, [] { return cylinder_mesh(); }), transform, color); break;
        case 3: draw(unit_model(g_three.unit_cone, [] { return cone_mesh(); }), transform, color); break;
        default: draw(unit_model(g_three.unit_plane, [] { return plane_mesh(); }), transform, color); break;
    }
}

namespace {
WorldImpl::LineVertex line_vertex(vec3 p, rgba c) { return {p.x, p.y, p.z, pack_color(c)}; }
} // namespace

void World::line(vec3 a, vec3 b, rgba color, bool on_top) {
    WorldImpl& impl = touch(impl_);
    auto& list = on_top ? impl.lines_on_top : impl.lines;
    list.push_back(line_vertex(a, color));
    list.push_back(line_vertex(b, color));
}

void World::wire_box(const Bounds& box, rgba color, bool on_top) {
    if (!box.valid()) return;
    vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        c[i] = {(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z};
    }
    static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                         {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) line(c[e[0]], c[e[1]], color, on_top);
}

void World::wire_box(const Transform& transform, rgba color, bool on_top) {
    const mat4 m = transform.matrix();
    vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        c[i] = m.transform_point({(i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f});
    }
    static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                         {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) line(c[e[0]], c[e[1]], color, on_top);
}

void World::wire_sphere(vec3 center, float radius, rgba color, bool on_top) {
    constexpr int segments = 32;
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < segments; ++i) {
            const float a0 = static_cast<float>(i) / segments * 2.0f * pi;
            const float a1 = static_cast<float>(i + 1) / segments * 2.0f * pi;
            auto point = [&](float a) {
                const float u = std::cos(a) * radius, v = std::sin(a) * radius;
                return center + (axis == 0 ? vec3{0, u, v} : axis == 1 ? vec3{u, 0, v} : vec3{u, v, 0});
            };
            line(point(a0), point(a1), color, on_top);
        }
    }
}

void World::grid(vec3 center, float size, float spacing, rgba color) {
    if (spacing <= 0.0f) return;
    const int half = static_cast<int>(size * 0.5f / spacing);
    const float extent = half * spacing;
    for (int i = -half; i <= half; ++i) {
        const float o = i * spacing;
        line(center + vec3{o, 0, -extent}, center + vec3{o, 0, extent}, color);
        line(center + vec3{-extent, 0, o}, center + vec3{extent, 0, o}, color);
    }
}

void World::render(const Frame& f, const Camera& camera) {
    render(f, camera, Rect{{0.0f, 0.0f}, {static_cast<float>(f.width), static_cast<float>(f.height)}});
}

void World::render(const Frame&, const Camera& camera, Rect viewport) {
    detail::three_audio_camera(camera.position, camera.rotation);
    WorldImpl& impl = touch(impl_);
    PassRecord pass;
    pass.camera = camera;
    pass.viewport = viewport;
    pass.sun = sun;
    pass.sky = sky;
    pass.fog = fog;
    pass.ambient = ambient;
    pass.lights = impl.lights;
    pass.draws = impl.draws;
    pass.billboards = impl.billboards;
    pass.many = impl.many;
    pass.instances = impl.instances;
    pass.skinned = impl.skinned;
    pass.lines = impl.lines;
    pass.lines_on_top = impl.lines_on_top;
    g_three.passes.push_back(std::move(pass));
    // Everything 2D drawn after this call lands in the next sokol_gl layer,
    // which three_draw_layers() draws after this pass — i.e. on top of it.
    sgl_layer(++g_three.current_layer);
}

RenderStats render_stats() { return g_three.stats_last_frame; }

Skybox load_skybox(const std::string& right, const std::string& left, const std::string& top,
                   const std::string& bottom, const std::string& front, const std::string& back) {
    // sokol's cube face order (+X, -X, +Y, -Y, +Z, -Z) in the cube map's own
    // left-handed space, where +Z is "front".
    const std::string* paths[6] = {&right, &left, &top, &bottom, &front, &back};
    SkyboxRecord rec;
    rgba averages[6];
    for (int i = 0; i < 6; ++i) {
        std::vector<unsigned char> px_data;
        int w = 0, h = 0;
        if (!detail::load_image_rgba(*paths[i], px_data, w, h)) {
            log_warn("load_skybox: could not load " + *paths[i]);
            return Skybox{};
        }
        if (w != h || (i > 0 && w != rec.size)) {
            log_warn("load_skybox: every face must be square and the same size (" + *paths[i] + " is " +
                     std::to_string(w) + "x" + std::to_string(h) + ")");
            return Skybox{};
        }
        rec.size = w;
        double sum[3] = {0, 0, 0};
        for (size_t p = 0; p < px_data.size(); p += 4) {
            for (int c = 0; c < 3; ++c) sum[c] += px_data[p + c];
        }
        const double n = static_cast<double>(w) * h * 255.0;
        averages[i] = rgba{static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n), 1.0f};
        rec.faces.insert(rec.faces.end(), px_data.begin(), px_data.end());
    }
    rec.average_up = averages[2];
    rec.average_down = averages[3];
    rec.average_side = rgba{(averages[0].r + averages[1].r + averages[4].r + averages[5].r) * 0.25f,
                            (averages[0].g + averages[1].g + averages[4].g + averages[5].g) * 0.25f,
                            (averages[0].b + averages[1].b + averages[4].b + averages[5].b) * 0.25f, 1.0f};
    g_three.skyboxes.push_back(std::move(rec));
    return Skybox{static_cast<int>(g_three.skyboxes.size()) - 1};
}

void unload_skybox(Skybox& skybox) {
    if (SkyboxRecord* rec = skybox_record(skybox)) {
        if (rec->view.id != SG_INVALID_ID) sg_destroy_view(rec->view);
        if (rec->image.id != SG_INVALID_ID) sg_destroy_image(rec->image);
        *rec = SkyboxRecord{};
        rec->alive = false;
    }
    skybox = Skybox{};
}

} // namespace thistle::three

namespace thistle::detail {

using namespace thistle::three;

void three_setup() {
    RenderState& s = g_three;
    set_mesh_release(release_mesh);
    s.lit_shader = sg_make_shader(lit_shader_desc(sg_query_backend()));
    s.lit_instanced_shader = sg_make_shader(lit_instanced_shader_desc(sg_query_backend()));
    s.background_shader = sg_make_shader(background_shader_desc(sg_query_backend()));
    s.ready = true;
    for (int instanced = 0; instanced < 2; ++instanced) {
        sg_pipeline* p = instanced ? s.instanced_pipelines : s.pipelines;
        p[PipOpaque] = make_lit_pipeline(false, false, instanced);
        p[PipOpaqueDouble] = make_lit_pipeline(false, true, instanced);
        p[PipBlend] = make_lit_pipeline(true, false, instanced);
        p[PipBlendDouble] = make_lit_pipeline(true, true, instanced);
    }
    s.sky_pipeline = make_background_pipeline(true);
    s.depth_reset_pipeline = make_background_pipeline(false);
    s.shadow_shader = sg_make_shader(shadow_shader_desc(sg_query_backend()));
    s.shadow_instanced_shader = sg_make_shader(shadow_instanced_shader_desc(sg_query_backend()));
    s.shadow_pipeline = make_shadow_pipeline(false);
    s.shadow_instanced_pipeline = make_shadow_pipeline(true);
    s.no_shadow = make_shadow_target(1);
    sg_sampler_desc cmp = {};
    cmp.min_filter = SG_FILTER_LINEAR;
    cmp.mag_filter = SG_FILTER_LINEAR;
    cmp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    cmp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    cmp.compare = SG_COMPAREFUNC_LESS_EQUAL;
    cmp.label = "three-shadow-compare";
    s.shadow_sampler = sg_make_sampler(&cmp);
    s.billboard_shader = sg_make_shader(billboard_shader_desc(sg_query_backend()));
    s.billboard_pipeline = make_billboard_pipeline(false);
    s.billboard_additive_pipeline = make_billboard_pipeline(true);
    {
        // The default particle: a soft round dot, white, alpha falling off
        // smoothly to the edge.
        constexpr int n = 32;
        std::vector<uint32_t> px(n * n);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const float dx = (x + 0.5f) / n * 2.0f - 1.0f, dy = (y + 0.5f) / n * 2.0f - 1.0f;
                const float a = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                px[y * n + x] = 0x00FFFFFFu | (static_cast<uint32_t>(a * a * (3.0f - 2.0f * a) * 255.0f) << 24);
            }
        }
        sg_image_desc dd = {};
        dd.width = n;
        dd.height = n;
        dd.data.mip_levels[0] = {px.data(), px.size() * sizeof(uint32_t)};
        dd.label = "three-particle-dot";
        s.dot_image = sg_make_image(&dd);
        sg_view_desc dv = {};
        dv.texture.image = s.dot_image;
        s.dot_view = sg_make_view(&dv);
        sg_sampler_desc sd = {};
        sd.min_filter = SG_FILTER_LINEAR;
        sd.mag_filter = SG_FILTER_LINEAR;
        sd.mipmap_filter = SG_FILTER_LINEAR;
        sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        sd.label = "three-sprite";
        s.sprite_sampler = sg_make_sampler(&sd);
    }
    s.lines_shader = sg_make_shader(lines_shader_desc(sg_query_backend()));
    s.line_pipeline = make_line_pipeline(false);
    s.line_on_top_pipeline = make_line_pipeline(true);

    sg_sampler_desc lin = {};
    lin.min_filter = SG_FILTER_LINEAR;
    lin.mag_filter = SG_FILTER_LINEAR;
    lin.mipmap_filter = SG_FILTER_LINEAR;
    lin.max_anisotropy = 4;
    lin.label = "three-linear";
    s.sampler_linear = sg_make_sampler(&lin);

    sg_sampler_desc nearest = {};
    nearest.min_filter = SG_FILTER_NEAREST;
    nearest.mag_filter = SG_FILTER_NEAREST;
    nearest.mipmap_filter = SG_FILTER_NEAREST;
    nearest.label = "three-nearest";
    s.sampler_nearest = sg_make_sampler(&nearest);

    const uint32_t white_px = 0xFFFFFFFFu;
    sg_image_desc wd = {};
    wd.width = 1;
    wd.height = 1;
    wd.data.mip_levels[0] = {&white_px, sizeof(white_px)};
    wd.label = "three-white";
    s.white_image = sg_make_image(&wd);
    sg_view_desc wv = {};
    wv.texture.image = s.white_image;
    s.white_view = sg_make_view(&wv);

    // The background shader always has a cube map bound (sokol validates
    // every declared binding), so there's a 1x1 black one for "no skybox".
    const uint32_t black_faces[6] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    sg_image_desc cd = {};
    cd.type = SG_IMAGETYPE_CUBE;
    cd.width = 1;
    cd.height = 1;
    cd.num_slices = 6;
    cd.data.mip_levels[0] = {black_faces, sizeof(black_faces)};
    cd.label = "three-no-skybox";
    s.black_cube = sg_make_image(&cd);
    sg_view_desc cv = {};
    cv.texture.image = s.black_cube;
    s.black_cube_view = sg_make_view(&cv);

    sg_sampler_desc sky = {};
    sky.min_filter = SG_FILTER_LINEAR;
    sky.mag_filter = SG_FILTER_LINEAR;
    sky.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sky.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sky.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sky.label = "three-sky";
    s.sky_sampler = sg_make_sampler(&sky);
}

void three_shutdown() {
    RenderState& s = g_three;
    unload_all_models();
    set_mesh_release(nullptr);
    s.passes.clear();
    for (SkyboxRecord& rec : s.skyboxes) {
        if (rec.view.id != SG_INVALID_ID) sg_destroy_view(rec.view);
        if (rec.image.id != SG_INVALID_ID) sg_destroy_image(rec.image);
    }
    for (RenderState::ShadowTarget& t : s.shadow_targets) destroy_shadow_target(t);
    destroy_shadow_target(s.no_shadow);
    sg_destroy_pipeline(s.shadow_pipeline);
    sg_destroy_shader(s.shadow_shader);
    sg_destroy_sampler(s.shadow_sampler);
    sg_destroy_view(s.black_cube_view);
    sg_destroy_image(s.black_cube);
    sg_destroy_sampler(s.sky_sampler);
    for (sg_pipeline p : s.pipelines) sg_destroy_pipeline(p);
    for (sg_pipeline p : s.instanced_pipelines) sg_destroy_pipeline(p);
    sg_destroy_shader(s.lit_instanced_shader);
    sg_destroy_pipeline(s.billboard_pipeline);
    sg_destroy_pipeline(s.billboard_additive_pipeline);
    sg_destroy_shader(s.billboard_shader);
    if (s.quad_buffer.id != SG_INVALID_ID) sg_destroy_buffer(s.quad_buffer);
    sg_destroy_view(s.dot_view);
    sg_destroy_image(s.dot_image);
    sg_destroy_sampler(s.sprite_sampler);
    sg_destroy_pipeline(s.shadow_instanced_pipeline);
    sg_destroy_shader(s.shadow_instanced_shader);
    if (s.instance_buffer.id != SG_INVALID_ID) sg_destroy_buffer(s.instance_buffer);
    if (s.skin_buffer.id != SG_INVALID_ID) sg_destroy_buffer(s.skin_buffer);
    sg_destroy_pipeline(s.sky_pipeline);
    sg_destroy_pipeline(s.depth_reset_pipeline);
    sg_destroy_shader(s.lit_shader);
    sg_destroy_shader(s.background_shader);
    sg_destroy_pipeline(s.line_pipeline);
    sg_destroy_pipeline(s.line_on_top_pipeline);
    sg_destroy_shader(s.lines_shader);
    if (s.line_buffer.id != SG_INVALID_ID) sg_destroy_buffer(s.line_buffer);
    sg_destroy_sampler(s.sampler_linear);
    sg_destroy_sampler(s.sampler_nearest);
    sg_destroy_view(s.white_view);
    sg_destroy_image(s.white_image);
    s = RenderState{};
}

void three_before_passes() {
    RenderState& s = g_three;

    // Transient buffers must be written before anything binds them this
    // frame — and the shadow passes below already bind the instance buffer.
    std::vector<WorldImpl::Instance> all_instances;
    std::vector<WorldImpl::SkinVertex> all_skinned;
    std::vector<WorldImpl::LineVertex> all_lines;
    std::vector<WorldImpl::Quad> all_quads;
    for (PassRecord& pass : s.passes) {
        // See-through billboards far to near (so nearer ones blend over
        // farther ones), then glowing ones, which don't care about order.
        // Consecutive runs with the same texture become one instanced draw.
        std::vector<const WorldImpl::BillboardCmd*> order;
        order.reserve(pass.billboards.size());
        for (const auto& b : pass.billboards) order.push_back(&b);
        const vec3 eye = pass.camera.position;
        auto dist2 = [&](const WorldImpl::BillboardCmd* b) {
            const vec3 d = vec3{b->quad.pos[0], b->quad.pos[1], b->quad.pos[2]} - eye;
            return dot(d, d);
        };
        std::stable_sort(order.begin(), order.end(), [&](const WorldImpl::BillboardCmd* a, const WorldImpl::BillboardCmd* b) {
            if (a->additive != b->additive) return !a->additive;
            if (!a->additive) return dist2(a) > dist2(b);
            return a->texture.id < b->texture.id;
        });
        pass.billboard_batches.clear();
        for (const WorldImpl::BillboardCmd* b : order) {
            auto* last = pass.billboard_batches.empty() ? nullptr : &pass.billboard_batches.back();
            if (!last || last->texture.id != b->texture.id || last->dot != b->dot || last->additive != b->additive) {
                pass.billboard_batches.push_back({static_cast<uint32_t>(all_quads.size()), 0, b->texture, b->dot, b->additive});
                last = &pass.billboard_batches.back();
            }
            all_quads.push_back(b->quad);
            ++last->count;
        }

        pass.instance_base = static_cast<uint32_t>(all_instances.size());
        all_instances.insert(all_instances.end(), pass.instances.begin(), pass.instances.end());
        pass.skin_base = static_cast<uint32_t>(all_skinned.size());
        all_skinned.insert(all_skinned.end(), pass.skinned.begin(), pass.skinned.end());
        pass.line_first = static_cast<int>(all_lines.size());
        all_lines.insert(all_lines.end(), pass.lines.begin(), pass.lines.end());
        pass.on_top_first = static_cast<int>(all_lines.size());
        all_lines.insert(all_lines.end(), pass.lines_on_top.begin(), pass.lines_on_top.end());
    }
    auto write_transient = [](sg_buffer& buf, size_t& capacity, const void* data, size_t count, size_t stride, const char* label) {
        if (count == 0) return;
        if (count > capacity) {
            if (buf.id != SG_INVALID_ID) sg_destroy_buffer(buf);
            capacity = std::max<size_t>(count * 2, 1024);
            sg_buffer_desc bd = {};
            bd.size = capacity * stride;
            bd.usage.immutable = false;
            bd.usage.write_transient = true;
            bd.label = label;
            buf = sg_make_buffer(&bd);
        }
        sg_write_buffer_desc wd = {};
        wd.src.data = {data, count * stride};
        wd.dst.buffer = buf;
        sg_write_buffer_transient(&wd);
    };
    write_transient(s.instance_buffer, s.instance_capacity, all_instances.data(), all_instances.size(),
                    sizeof(WorldImpl::Instance), "three-instances");
    write_transient(s.skin_buffer, s.skin_capacity, all_skinned.data(), all_skinned.size(), sizeof(WorldImpl::SkinVertex), "three-skinned");
    write_transient(s.line_buffer, s.line_capacity, all_lines.data(), all_lines.size(), sizeof(WorldImpl::LineVertex), "three-lines");
    write_transient(s.quad_buffer, s.quad_capacity, all_quads.data(), all_quads.size(), sizeof(WorldImpl::Quad), "three-billboards");

    if (!s.no_shadow_cleared) {
        sg_pass clear = {};
        clear.action.depth.load_action = SG_LOADACTION_CLEAR;
        clear.action.depth.clear_value = 1.0f;
        clear.attachments.depth_stencil = s.no_shadow.attachment;
        sg_begin_pass(&clear);
        sg_end_pass();
        s.no_shadow_cleared = true;
    }
    int shadow_index = 0;
    for (PassRecord& pass : s.passes) {
        if (pass.sun.shadows && pass.sun.intensity > 0.0f && (!pass.draws.empty() || !pass.many.empty())) {
            render_shadow_map(pass, shadow_index++, frame_width(), frame_height());
        }
    }
}

void three_draw_layers() {
    const int pass_count = static_cast<int>(g_three.passes.size());
    const int fb_w = frame_width(), fb_h = frame_height();
    for (int layer = 0; layer <= pass_count; ++layer) {
        sgl_draw_layer(layer);
        if (layer < pass_count) render_pass(g_three.passes[layer], fb_w, fb_h);
    }
}

void three_end_frame() {
    three_audio_update();
    g_three.passes.clear();
    g_three.current_layer = 0;
    g_three.stats_last_frame = g_three.stats_this_frame;
    g_three.stats_this_frame = RenderStats{};
}

} // namespace thistle::detail
