#include "thistle_internal.h"

#include "sokol_gfx.h"
#include "sokol_gl.h"

#include "shaders/background.glsl.h"
#include "shaders/lit.glsl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace thistle::three {

struct WorldImpl {
    struct DrawCmd {
        int model = -1;
        mat4 world;
        rgba tint = white;
        bool override_material = false;
        Material material;
    };
    std::vector<DrawCmd> draws;
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

struct MeshRecord {
    MeshData cpu; // dropped once uploaded
    sg_buffer vbuf = {};
    sg_buffer ibuf = {};
    int index_count = 0;
    bool uploaded = false;
    Bounds bounds;
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
    bool alive = true;
};

struct PassRecord {
    Camera camera;
    Rect viewport;
    Sun sun;
    Sky sky;
    float ambient = 0.0f;
    std::vector<WorldImpl::DrawCmd> draws;
};

enum PipelineKind { PipOpaque, PipOpaqueDouble, PipBlend, PipBlendDouble, PipCount };

struct RenderState {
    bool ready = false;
    sg_shader lit_shader = {};
    sg_shader background_shader = {};
    sg_pipeline pipelines[PipCount] = {};
    sg_pipeline sky_pipeline = {};
    sg_pipeline depth_reset_pipeline = {};
    sg_sampler sampler_linear = {};
    sg_sampler sampler_nearest = {};
    sg_image white_image = {};
    sg_view white_view = {};

    std::vector<ModelRecord> models;
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

ModelRecord* model_record(Model m) {
    if (m.id < 0 || m.id >= static_cast<int>(g_three.models.size())) return nullptr;
    ModelRecord& rec = g_three.models[m.id];
    return rec.alive ? &rec : nullptr;
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
    mesh.vbuf = sg_make_buffer(&vd);

    sg_buffer_desc id = {};
    id.usage.vertex_buffer = false;
    id.usage.index_buffer = true;
    id.data = {mesh.cpu.indices.data(), mesh.cpu.indices.size() * sizeof(uint32_t)};
    id.label = "three-mesh-indices";
    mesh.ibuf = sg_make_buffer(&id);
    mesh.index_count = static_cast<int>(mesh.cpu.indices.size());
    mesh.cpu = MeshData{};
}

void destroy_mesh(MeshRecord& mesh) {
    if (mesh.vbuf.id != SG_INVALID_ID) sg_destroy_buffer(mesh.vbuf);
    if (mesh.ibuf.id != SG_INVALID_ID) sg_destroy_buffer(mesh.ibuf);
    mesh = MeshRecord{};
}

Model unit_model(Model& slot, MeshData (*make)()) {
    if (!slot.valid()) slot = make_model(make());
    return slot;
}

sg_pipeline make_lit_pipeline(bool blend, bool double_sided) {
    sg_pipeline_desc pd = {};
    pd.shader = g_three.lit_shader;
    pd.layout.attrs[ATTR_lit_position].format = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_lit_normal].format = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_lit_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[ATTR_lit_color0].format = SG_VERTEXFORMAT_UBYTE4N;
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
    pd.label = "three-lit";
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

struct DrawItem {
    const MeshRecord* mesh;
    const Material* material;
    mat4 world;
    rgba tint;
    float sort_depth;
    PipelineKind pipeline;
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

    // Background: sky (or nothing) plus a depth reset for this pass.
    sg_apply_pipeline(pass.sky.visible ? g_three.sky_pipeline : g_three.depth_reset_pipeline);
    background_vs_params_t bvs = {};
    bvs.inv_view_proj = inverse(view_proj_gl);
    sg_apply_uniforms(UB_background_vs_params, SG_RANGE(bvs));
    background_fs_params_t bfs = {};
    bfs.top = to_vec4(pass.sky.top);
    bfs.horizon = to_vec4(pass.sky.horizon);
    bfs.ground = to_vec4(pass.sky.ground);
    sg_apply_uniforms(UB_background_fs_params, SG_RANGE(bfs));
    sg_draw(0, 3, 1);
    ++stats.draw_calls;

    std::vector<DrawItem> items;
    for (const WorldImpl::DrawCmd& cmd : pass.draws) {
        ModelRecord* rec = model_record(Model{cmd.model});
        if (!rec) continue;
        for (const PartRecord& part : rec->parts) {
            MeshRecord& mesh = rec->meshes[part.mesh];
            upload_mesh(mesh);
            if (mesh.index_count == 0) continue;
            const Material* mat = cmd.override_material ? &cmd.material : &part.material;
            const bool blend = mat->alpha == AlphaMode::Blend;
            const PipelineKind kind = blend ? (mat->double_sided ? PipBlendDouble : PipBlend)
                                            : (mat->double_sided ? PipOpaqueDouble : PipOpaque);
            const mat4 world = cmd.world * part.local;
            const vec3 center = world.transform_point(mesh.bounds.center());
            items.push_back({&mesh, mat, world, cmd.tint, dot(center - pass.camera.position, pass.camera.forward()), kind});
        }
    }
    // Opaque first (grouped by pipeline to cut state changes), then
    // see-through surfaces far-to-near so they blend over what's behind them.
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        const bool ab = a.pipeline >= PipBlend, bb = b.pipeline >= PipBlend;
        if (ab != bb) return !ab;
        if (ab) return a.sort_depth > b.sort_depth;
        return a.pipeline < b.pipeline;
    });

    lit_scene_params_t scene = {};
    scene.camera_pos = {pass.camera.position.x, pass.camera.position.y, pass.camera.position.z, 1.0f};
    const vec3 sun_dir = normalize(pass.sun.direction);
    scene.sun_dir = {sun_dir.x, sun_dir.y, sun_dir.z, 0.0f};
    scene.sun_color = to_linear4(pass.sun.color, pass.sun.intensity);
    scene.sky_ambient = to_linear4(pass.sky.top, pass.ambient);
    scene.ground_ambient = to_linear4(pass.sky.ground, pass.ambient);

    int bound_pipeline = -1;
    for (const DrawItem& item : items) {
        if (item.pipeline != bound_pipeline) {
            sg_apply_pipeline(g_three.pipelines[item.pipeline]);
            sg_apply_uniforms(UB_lit_scene_params, SG_RANGE(scene));
            bound_pipeline = item.pipeline;
        }
        const Material& mat = *item.material;
        sg_bindings bind = {};
        bind.vertex_buffers[0] = item.mesh->vbuf;
        bind.index_buffer = item.mesh->ibuf;
        sg_view tex_view = mat.texture.valid() ? detail::texture_view(mat.texture) : sg_view{};
        bind.views[VIEW_base_tex] = tex_view.id != SG_INVALID_ID ? tex_view : g_three.white_view;
        bind.samplers[SMP_base_smp] = mat.filter == TextureFilter::Nearest ? g_three.sampler_nearest : g_three.sampler_linear;
        sg_apply_bindings(&bind);

        lit_vs_params_t vs = {};
        vs.model = item.world;
        vs.view_proj = view_proj;
        vs.normal_matrix = transpose(inverse(item.world));
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

        sg_draw(0, item.mesh->index_count, 1);
        ++stats.draw_calls;
        stats.triangles += item.mesh->index_count / 3;
    }

    // Back to the full framebuffer for the 2D layers drawn after this pass.
    sg_apply_viewport(0, 0, fb_w, fb_h, true);
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
}

} // namespace

// --- models --------------------------------------------------------------------

Model make_model(const MeshData& mesh, const Material& material) {
    ModelRecord rec;
    MeshRecord m;
    m.cpu = mesh;
    m.bounds = mesh.bounds();
    rec.bounds = m.bounds;
    rec.meshes.push_back(std::move(m));
    rec.parts.push_back({0, material, mat4{}});
    g_three.models.push_back(std::move(rec));
    return Model{static_cast<int>(g_three.models.size()) - 1};
}

void unload_model(Model& model) {
    if (ModelRecord* rec = model_record(model)) {
        for (MeshRecord& mesh : rec->meshes) destroy_mesh(mesh);
        *rec = ModelRecord{};
        rec->alive = false;
    }
    model = Model{};
}

Bounds model_bounds(Model model) {
    const ModelRecord* rec = model_record(model);
    return rec ? rec->bounds : Bounds{};
}

int model_part_count(Model model) {
    const ModelRecord* rec = model_record(model);
    return rec ? static_cast<int>(rec->parts.size()) : 0;
}

Material model_material(Model model, int part) {
    const ModelRecord* rec = model_record(model);
    if (!rec || part < 0 || part >= static_cast<int>(rec->parts.size())) return {};
    return rec->parts[part].material;
}

void set_model_material(Model model, const Material& material, int part) {
    ModelRecord* rec = model_record(model);
    if (!rec) return;
    for (int i = 0; i < static_cast<int>(rec->parts.size()); ++i) {
        if (part < 0 || part == i) rec->parts[i].material = material;
    }
}

// --- camera --------------------------------------------------------------------

mat4 Camera::view() const {
    return mat4::rotate(rotation.inverse()) * mat4::translate(-position);
}

mat4 Camera::projection(float aspect) const {
    if (orthographic) {
        const float h = ortho_height * 0.5f;
        return mat4::ortho(-h * aspect, h * aspect, -h, h, near_z, far_z);
    }
    return mat4::perspective(fov, aspect, near_z, far_z);
}

// --- world ---------------------------------------------------------------------

namespace {
WorldImpl& touch(std::unique_ptr<WorldImpl>& impl) {
    if (!impl) impl = std::make_unique<WorldImpl>();
    const uint64_t frame = detail::frame_index();
    if (impl->frame != frame) {
        impl->draws.clear();
        impl->frame = frame;
    }
    return *impl;
}
} // namespace

World::World() : impl_(std::make_unique<WorldImpl>()) {}
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

void World::draw(Model model, const Transform& transform, rgba tint) {
    if (!model_record(model)) return;
    WorldImpl::DrawCmd cmd;
    cmd.model = model.id;
    cmd.world = transform.matrix();
    cmd.tint = tint;
    touch(impl_).draws.push_back(std::move(cmd));
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

void World::render(const Frame& f, const Camera& camera) {
    render(f, camera, Rect{{0.0f, 0.0f}, {static_cast<float>(f.width), static_cast<float>(f.height)}});
}

void World::render(const Frame&, const Camera& camera, Rect viewport) {
    WorldImpl& impl = touch(impl_);
    PassRecord pass;
    pass.camera = camera;
    pass.viewport = viewport;
    pass.sun = sun;
    pass.sky = sky;
    pass.ambient = ambient;
    pass.draws = impl.draws;
    g_three.passes.push_back(std::move(pass));
    // Everything 2D drawn after this call lands in the next sokol_gl layer,
    // which three_draw_layers() draws after this pass — i.e. on top of it.
    sgl_layer(++g_three.current_layer);
}

RenderStats render_stats() { return g_three.stats_last_frame; }

} // namespace thistle::three

namespace thistle::detail {

using namespace thistle::three;

void three_setup() {
    RenderState& s = g_three;
    s.lit_shader = sg_make_shader(lit_shader_desc(sg_query_backend()));
    s.background_shader = sg_make_shader(background_shader_desc(sg_query_backend()));
    s.ready = true;
    s.pipelines[PipOpaque] = make_lit_pipeline(false, false);
    s.pipelines[PipOpaqueDouble] = make_lit_pipeline(false, true);
    s.pipelines[PipBlend] = make_lit_pipeline(true, false);
    s.pipelines[PipBlendDouble] = make_lit_pipeline(true, true);
    s.sky_pipeline = make_background_pipeline(true);
    s.depth_reset_pipeline = make_background_pipeline(false);

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
}

void three_shutdown() {
    RenderState& s = g_three;
    for (ModelRecord& rec : s.models) {
        for (MeshRecord& mesh : rec.meshes) destroy_mesh(mesh);
    }
    s.models.clear();
    s.passes.clear();
    for (sg_pipeline p : s.pipelines) sg_destroy_pipeline(p);
    sg_destroy_pipeline(s.sky_pipeline);
    sg_destroy_pipeline(s.depth_reset_pipeline);
    sg_destroy_shader(s.lit_shader);
    sg_destroy_shader(s.background_shader);
    sg_destroy_sampler(s.sampler_linear);
    sg_destroy_sampler(s.sampler_nearest);
    sg_destroy_view(s.white_view);
    sg_destroy_image(s.white_image);
    s = RenderState{};
}

void three_before_passes() {}

void three_draw_layers() {
    const int pass_count = static_cast<int>(g_three.passes.size());
    const int fb_w = frame_width(), fb_h = frame_height();
    for (int layer = 0; layer <= pass_count; ++layer) {
        sgl_draw_layer(layer);
        if (layer < pass_count) render_pass(g_three.passes[layer], fb_w, fb_h);
    }
}

void three_end_frame() {
    g_three.passes.clear();
    g_three.current_layer = 0;
    g_three.stats_last_frame = g_three.stats_this_frame;
    g_three.stats_this_frame = RenderStats{};
}

} // namespace thistle::detail
