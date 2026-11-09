// Checks thistle::three's level-of-detail picking and the CPU occlusion
// buffer: which level a distance picks (and the bias), that a wall hides what
// is fully behind it and nothing else, that a box crossing the wall's edge
// or in front of it stays visible, and that a floor under the camera (a
// triangle cut by the near plane) still occludes. No GPU needed.
#include <thistle.hpp>
#include <cstdio>
#include <string>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}

Bounds box_at(vec3 c, vec3 half) { return Bounds{c - half, c + half}; }

// A wall facing the camera: x, y in [-w, w], at z = depth.
struct Quad { std::vector<vec3> p; std::vector<uint32_t> t; };
Quad wall(float half_w, float half_h, float z) {
    return {{{-half_w, -half_h, z}, {half_w, -half_h, z}, {half_w, half_h, z}, {-half_w, half_h, z}}, {0, 1, 2, 0, 2, 3}};
}

mat4 view_proj_at_origin() {
    Camera cam;
    cam.position = {0.0f, 0.0f, 0.0f};
    return cam.projection(2.0f) * cam.view();
}
} // namespace

int main() {
    std::printf("LOD picking\n");
    const Model lod0 = make_model(box_mesh({1, 1, 1}));
    const Model lod1 = make_model(box_mesh({1, 1, 1}));
    const Model lod2 = make_model(box_mesh({1, 1, 1}));
    check(pick_model_lod(lod0, 1000.0f).id == lod0.id, "no levels: the model itself, however far");
    set_model_lods(lod0, {{lod1, 40.0f}, {lod2, 120.0f}});
    check(pick_model_lod(lod0, 0.0f).id == lod0.id, "up close: level 0");
    check(pick_model_lod(lod0, 39.9f).id == lod0.id, "just before the first distance: level 0");
    check(pick_model_lod(lod0, 40.0f).id == lod1.id, "at the first distance: level 1");
    check(pick_model_lod(lod0, 119.0f).id == lod1.id, "between the distances: level 1");
    check(pick_model_lod(lod0, 500.0f).id == lod2.id, "past the last distance: level 2");
    check(pick_model_lod(lod0, 30.0f, 0.5f).id == lod1.id, "bias 0.5 switches at half the distance");
    check(pick_model_lod(lod0, 60.0f, 2.0f).id == lod0.id, "bias 2 switches at twice the distance");
    set_model_lods(lod0, {{lod1, 50.0f}, {lod2, 20.0f}});
    check(pick_model_lod(lod0, 30.0f).id == lod0.id && pick_model_lod(lod0, 60.0f).id == lod1.id,
          "a level whose distance doesn't increase is dropped, the rest kept");
    set_model_lods(lod0, {{lod0, 10.0f}, {Model{}, 20.0f}});
    check(pick_model_lod(lod0, 1000.0f).id == lod0.id, "itself and invalid models are not levels");
    set_model_lods(lod0, {});
    check(pick_model_lod(lod0, 1000.0f).id == lod0.id, "empty list clears the levels");

    std::printf("occluder flag\n");
    check(!is_model_occluder(lod0), "off by default");
    set_model_occluder(lod0, true);
    check(is_model_occluder(lod0), "can be turned on");
    set_model_occluder(lod0, false);
    check(!is_model_occluder(lod0), "and off");

    std::printf("occlusion buffer\n");
    OcclusionBuffer buf;
    buf.begin(view_proj_at_origin());
    check(buf.is_visible(box_at({0, 0, -20}, {1, 1, 1})), "no occluders: everything is visible");

    const Quad w = wall(8.0f, 6.0f, -10.0f);
    buf.add_occluder(w.p, w.t, mat4{});
    check(buf.triangles_drawn() == 2, "the wall's two triangles were drawn");
    check(!buf.is_visible(box_at({0, 0, -20}, {1, 1, 1})), "a box straight behind the wall is hidden");
    check(!buf.is_visible(box_at({3, 2, -30}, {2, 2, 2})), "farther and off-center, still behind it: hidden");
    check(buf.is_visible(box_at({0, 0, -5}, {1, 1, 1})), "a box in front of the wall is visible");
    check(buf.is_visible(box_at({16, 0, -20}, {1, 1, 1})), "a box beside the wall (past its edge on screen) is visible");
    check(buf.is_visible(box_at({12, 0, -20}, {4, 1, 1})), "a box sticking out past the wall's edge on screen is visible");
    check(buf.is_visible(box_at({0, 0, -10}, {1, 1, 0.5f})), "a box poking through the wall is visible");
    check(buf.is_visible(box_at({0, 0, 0.5f}, {1, 1, 1})), "a box around the camera is visible");
    check(buf.is_visible(Bounds{}), "an empty box is not this test's call");

    buf.begin(view_proj_at_origin());
    check(buf.is_visible(box_at({0, 0, -20}, {1, 1, 1})), "begin() empties it");

    std::printf("a floor under the camera still occludes\n");
    Camera cam;
    cam.position = {0.0f, 1.0f, 0.0f};
    buf.begin(cam.projection(2.0f) * cam.view());
    const std::vector<vec3> floor_p{{-50, 0, 50}, {50, 0, 50}, {50, 0, -50}, {-50, 0, -50}};
    buf.add_occluder(floor_p, {0, 1, 2, 0, 2, 3}, mat4{});
    check(buf.triangles_drawn() > 0, "the floor's triangles survive the near-plane clip");
    check(!buf.is_visible(box_at({0, -3, -20}, {1, 1, 1})), "a box under the floor is hidden");
    check(buf.is_visible(box_at({0, 2, -20}, {1, 1, 1})), "a box above the floor is visible");

    std::printf("a wall seen at an angle, with a transform\n");
    buf.begin(view_proj_at_origin());
    mat4 moved = mat4::translate({0.0f, 0.0f, -30.0f});
    buf.add_occluder(w.p, w.t, moved);
    check(!buf.is_visible(box_at({0, 0, -60}, {1, 1, 1})), "the transform moves the occluder: box behind it hidden");
    check(buf.is_visible(box_at({0, 0, -20}, {1, 1, 1})), "and a box in front of it is visible");

    std::printf("%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
