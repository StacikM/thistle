#include "thistle_internal.h"

#include "sokol_gfx.h"
#include "sokol_gl.h"

#include "shaders/background.glsl.h"
#include "shaders/lines.glsl.h"
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
    std::vector<DrawCmd> draws;
    std::vector<Light> lights;
    std::vector<LineVertex> lines;
    std::vector<LineVertex> lines_on_top;
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
    // Kept for the life of the mesh (unlike `cpu`): raycasts and collision
    // need the triangles, not the normals/UVs/colors.
    std::vector<vec3> positions;
    std::vector<uint32_t> triangles;
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
    std::vector<WorldImpl::DrawCmd> draws;
    std::vector<WorldImpl::LineVertex> lines;
    std::vector<WorldImpl::LineVertex> lines_on_top;
    int line_first = 0; // vertex offsets into this frame's shared line buffer
    int on_top_first = 0;
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
    sg_image black_cube = {};
    sg_view black_cube_view = {};
    sg_sampler sky_sampler = {};
    std::vector<SkyboxRecord> skyboxes;
    sg_shader lines_shader = {};
    sg_pipeline line_pipeline = {};
    sg_pipeline line_on_top_pipeline = {};
    sg_buffer line_buffer = {};
    size_t line_capacity = 0; // in vertices

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
        for (const PartRecord& part : rec->parts) {
            MeshRecord& mesh = rec->meshes[part.mesh];
            upload_mesh(mesh);
            if (mesh.index_count == 0) continue;
            const Material* mat = cmd.override_material ? &cmd.material : &part.material;
            const bool blend = mat->alpha == AlphaMode::Blend;
            const PipelineKind kind = blend ? (mat->double_sided ? PipBlendDouble : PipBlend)
                                            : (mat->double_sided ? PipOpaqueDouble : PipOpaque);
            const mat4 world = cmd.world * part.local;
            if (!frustum.intersects(mesh.bounds.transformed(world))) {
                ++stats.culled;
                continue;
            }
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
    scene.sun_dir = {sun_dir.x, sun_dir.y, sun_dir.z, 0.0f};
    scene.sun_color = to_linear4(pass.sun.color, pass.sun.intensity);
    scene.sky_ambient = to_linear4(amb_top, pass.ambient);
    scene.ground_ambient = to_linear4(amb_ground, pass.ambient);
    if (pass.fog.enabled) {
        scene.fog_color = to_linear4(pass.fog.match_sky ? horizon : pass.fog.color, 1.0f);
        const float span = std::max(pass.fog.end - pass.fog.start, 1e-3f);
        scene.fog_params = {pass.fog.start, 1.0f / span, 1.0f, 0.0f};
    }
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
        const sg_view glow_view = mat.emissive_texture.valid() ? detail::texture_view(mat.emissive_texture) : sg_view{};
        bind.views[VIEW_emissive_tex] = glow_view.id != SG_INVALID_ID ? glow_view : g_three.white_view;
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

    if (!lines_drawn) draw_lines(g_three.line_pipeline, pass.line_first, static_cast<int>(pass.lines.size()), view_proj);
    draw_lines(g_three.line_on_top_pipeline, pass.on_top_first, static_cast<int>(pass.lines_on_top.size()), view_proj);

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
    m.positions.reserve(mesh.vertices.size());
    for (const Vertex& v : mesh.vertices) m.positions.push_back(v.position);
    m.triangles = mesh.indices;
    rec.bounds = m.bounds;
    rec.meshes.push_back(std::move(m));
    rec.parts.push_back({0, material, mat4{}});
    g_three.models.push_back(std::move(rec));
    return Model{static_cast<int>(g_three.models.size()) - 1};
}

Model make_model(const ModelData& data) {
    if (data.parts.empty()) return Model{};
    ModelRecord rec;
    for (const ModelData::Part& part : data.parts) {
        MeshRecord m;
        m.cpu = part.mesh;
        m.bounds = part.mesh.bounds();
        m.positions.reserve(part.mesh.vertices.size());
        for (const Vertex& v : part.mesh.vertices) m.positions.push_back(v.position);
        m.triangles = part.mesh.indices;
        const Bounds placed = m.bounds.transformed(part.transform);
        if (placed.valid()) { rec.bounds.add(placed.min); rec.bounds.add(placed.max); }
        rec.parts.push_back({static_cast<int>(rec.meshes.size()), part.material, part.transform});
        rec.meshes.push_back(std::move(m));
    }
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

RaycastHit raycast(const Ray& ray, Model model, const Transform& transform, float max_distance) {
    RaycastHit best;
    const ModelRecord* rec = model_record(model);
    if (!rec) return best;
    const mat4 model_matrix = transform.matrix();
    for (const PartRecord& part : rec->parts) {
        const MeshRecord& mesh = rec->meshes[part.mesh];
        const mat4 world = model_matrix * part.local;
        const float limit = best.hit ? best.distance : max_distance;
        if (!raycast(ray, mesh.bounds.transformed(world), limit)) continue;
        // Triangles are tested in world space, so hit distances stay in the
        // caller's units even under non-uniform scale.
        for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
            const vec3 a = world.transform_point(mesh.positions[mesh.triangles[i]]);
            const vec3 b = world.transform_point(mesh.positions[mesh.triangles[i + 1]]);
            const vec3 c = world.transform_point(mesh.positions[mesh.triangles[i + 2]]);
            const RaycastHit h = raycast_triangle(ray, a, b, c, best.hit ? best.distance : max_distance);
            if (h.hit) best = h;
        }
    }
    return best;
}

// --- world ---------------------------------------------------------------------

namespace {
WorldImpl& touch(std::unique_ptr<WorldImpl>& impl) {
    if (!impl) impl = std::make_unique<WorldImpl>();
    const uint64_t frame = detail::frame_index();
    if (impl->frame != frame) {
        impl->draws.clear();
        impl->lights.clear();
        impl->lines.clear();
        impl->lines_on_top.clear();
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
    s.lit_shader = sg_make_shader(lit_shader_desc(sg_query_backend()));
    s.background_shader = sg_make_shader(background_shader_desc(sg_query_backend()));
    s.ready = true;
    s.pipelines[PipOpaque] = make_lit_pipeline(false, false);
    s.pipelines[PipOpaqueDouble] = make_lit_pipeline(false, true);
    s.pipelines[PipBlend] = make_lit_pipeline(true, false);
    s.pipelines[PipBlendDouble] = make_lit_pipeline(true, true);
    s.sky_pipeline = make_background_pipeline(true);
    s.depth_reset_pipeline = make_background_pipeline(false);
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
    for (ModelRecord& rec : s.models) {
        for (MeshRecord& mesh : rec.meshes) destroy_mesh(mesh);
    }
    s.models.clear();
    s.passes.clear();
    for (SkyboxRecord& rec : s.skyboxes) {
        if (rec.view.id != SG_INVALID_ID) sg_destroy_view(rec.view);
        if (rec.image.id != SG_INVALID_ID) sg_destroy_image(rec.image);
    }
    sg_destroy_view(s.black_cube_view);
    sg_destroy_image(s.black_cube);
    sg_destroy_sampler(s.sky_sampler);
    for (sg_pipeline p : s.pipelines) sg_destroy_pipeline(p);
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
    // Every pass's lines go into one transient buffer, written once, before
    // any pass binds it (sokol only allows transient writes before the
    // first bind in a frame).
    RenderState& s = g_three;
    std::vector<WorldImpl::LineVertex> all;
    for (PassRecord& pass : s.passes) {
        pass.line_first = static_cast<int>(all.size());
        all.insert(all.end(), pass.lines.begin(), pass.lines.end());
        pass.on_top_first = static_cast<int>(all.size());
        all.insert(all.end(), pass.lines_on_top.begin(), pass.lines_on_top.end());
    }
    if (all.empty()) return;
    if (all.size() > s.line_capacity) {
        if (s.line_buffer.id != SG_INVALID_ID) sg_destroy_buffer(s.line_buffer);
        s.line_capacity = std::max<size_t>(all.size() * 2, 4096);
        sg_buffer_desc bd = {};
        bd.size = s.line_capacity * sizeof(WorldImpl::LineVertex);
        bd.usage.immutable = false;
        bd.usage.write_transient = true;
        bd.label = "three-lines";
        s.line_buffer = sg_make_buffer(&bd);
    }
    sg_write_buffer_desc wd = {};
    wd.src.data = {all.data(), all.size() * sizeof(WorldImpl::LineVertex)};
    wd.dst.buffer = s.line_buffer;
    sg_write_buffer_transient(&wd);
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
    g_three.passes.clear();
    g_three.current_layer = 0;
    g_three.stats_last_frame = g_three.stats_this_frame;
    g_three.stats_this_frame = RenderStats{};
}

} // namespace thistle::detail
