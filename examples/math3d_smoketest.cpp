// Checks thistle::three's math (quat/mat4/Transform) against hand-computed
// results: rotation directions, projection depth mapping, inverse round
// trips, transform composition. Pure math — no App/window/GPU. Everything the
// 3D renderer draws goes through these, so a sign error here would show up
// as a mirrored or inside-out world, not a crash.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near(vec3 a, vec3 b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
bool near_identity(const mat4& m) {
    const mat4 id;
    for (int i = 0; i < 16; ++i) if (!near(m.m[i], id.m[i], 1e-3f)) return false;
    return true;
}
bool same_rotation(quat a, quat b) { return near(std::fabs(dot(a, b)), 1.0f, 1e-4f); }
} // namespace

int main() {
    // vec3 helpers
    check(near(dot({1, 2, 3}, {4, -5, 6}), 12.0f), "dot product");
    check(near(cross({1, 0, 0}, {0, 1, 0}), {0, 0, 1}), "cross X x Y = Z (right-handed)");
    check(near(length({3, 4, 0}), 5.0f), "length 3-4-5");
    check(normalize(vec3{0, 0, 0}) == vec3{0, 0, 0}, "normalizing zero stays zero, not NaN");

    // quat conventions
    check(near(quat{}.forward(), {0, 0, -1}), "identity rotation looks down -Z");
    check(near(quat::axis_angle({0, 1, 0}, radians(90)) * vec3{1, 0, 0}, {0, 0, -1}), "90 deg about +Y takes +X to -Z");
    check(quat::euler(radians(30), 0).forward().y > 0.49f, "positive pitch looks up");
    check(quat::euler(0, radians(30)).forward().x < -0.49f, "positive yaw turns left");
    check(near(quat::euler(radians(90), 0).forward(), {0, 1, 0}), "pitch 90 looks straight up");
    {
        const quat q = quat::euler(radians(20), radians(135), radians(-10));
        check(same_rotation(q * q.inverse(), quat{}), "q * inverse(q) = identity");
    }
    check(near(quat::look_rotation({1, 0, 0}).forward(), {1, 0, 0}), "look_rotation points forward where asked");
    check(near(quat::look_rotation({1, 0, 0}).up(), {0, 1, 0}), "look_rotation keeps up as up");
    {
        const quat q = quat::look_rotation({0, 1, 0}); // straight up, parallel to the default up vector
        check(near(q.forward(), {0, 1, 0}) && !std::isnan(q.w), "look_rotation straight up doesn't NaN");
    }
    check(near(quat::from_to({1, 0, 0}, {0, 1, 0}) * vec3{1, 0, 0}, {0, 1, 0}), "from_to rotates from onto to");
    check(near(quat::from_to({1, 0, 0}, {-1, 0, 0}) * vec3{1, 0, 0}, {-1, 0, 0}), "from_to handles opposite directions");
    check(same_rotation(slerp(quat{}, quat::axis_angle({0, 1, 0}, radians(90)), 0.5f),
                        quat::axis_angle({0, 1, 0}, radians(45))), "slerp halfway = half the angle");

    // mat4
    {
        const Transform t{{1, 2, 3}, quat::euler(radians(10), radians(70), radians(5)), {2, 3, 4}};
        const vec3 p{0.5f, -1.0f, 2.0f};
        check(near(t.matrix().transform_point(p), t.apply(p)), "trs matrix agrees with Transform::apply");
        check(near_identity(inverse(t.matrix()) * t.matrix()), "inverse(M) * M = identity");
        check(near_identity(transpose(transpose(t.matrix())) * inverse(t.matrix())), "transpose twice is a no-op");
    }
    {
        const mat4 proj = mat4::perspective(radians(60), 16.0f / 9.0f, 0.1f, 100.0f);
        check(near(proj.transform_point({0, 0, -0.1f}).z, -1.0f), "near plane maps to NDC z = -1");
        check(near(proj.transform_point({0, 0, -100.0f}).z, 1.0f, 1e-3f), "far plane maps to NDC z = +1");
        const vec3 top = proj.transform_point({0, std::tan(radians(30)) * 10.0f, -10.0f});
        check(near(top.y, 1.0f), "top edge of the vertical FOV maps to NDC y = +1");
    }
    {
        const mat4 view = mat4::look_at({0, 0, 5}, {0, 0, 0});
        check(near(view.transform_point({0, 0, 0}), {0, 0, -5}), "look_at puts the target in front of the camera (-Z)");
        const vec3 eye{3, 4, -2}, target{-1, 0.5f, 6};
        Transform cam{eye};
        cam.look_at(target);
        const mat4 a = mat4::look_at(eye, target);
        const mat4 b = inverse(cam.matrix());
        bool same = true;
        for (int i = 0; i < 16; ++i) if (!near(a.m[i], b.m[i], 1e-3f)) same = false;
        check(same, "look_at == inverse of a camera Transform looking at the same target");
    }
    {
        const mat4 o = mat4::ortho(-2, 2, -1, 1, 0.1f, 10.0f);
        check(near(o.transform_point({2, 1, -0.1f}), {1, 1, -1}), "ortho maps top-right-near corner to (1,1,-1)");
        check(near(o.transform_point({-2, -1, -10}), {-1, -1, 1}), "ortho maps bottom-left-far corner to (-1,-1,1)");
    }
    check(near_identity(inverse(mat4::scale({0, 1, 1}))), "inverting a singular matrix returns identity, not garbage");

    // Transform composition
    {
        const Transform parent{{10, 0, 0}, quat::axis_angle({0, 1, 0}, radians(90))};
        const Transform child{{1, 0, 0}};
        check(near((parent * child).position, {10, 0, -1}), "child offset is rotated by the parent");
        const Transform scaled_parent{{0, 0, 0}, {}, {2, 2, 2}};
        check(near((scaled_parent * child).position, {2, 0, 0}), "child offset is scaled by the parent");
    }

    // JSON round trip (scene files and NetVar<Transform> depend on it)
    {
        const Transform t{{1, 2, 3}, quat::euler(0.3f, 0.2f, 0.1f), {1, 2, 1}};
        const Transform back = nlohmann::json(t).get<Transform>();
        check(near(back.position, t.position) && same_rotation(back.rotation, t.rotation) &&
              near(back.scale, t.scale), "Transform survives a JSON round trip");
    }

    // Rays
    {
        const Ray r{{0, 0, 10}, {0, 0, -1}};
        const RaycastHit h = raycast(r, Bounds{{-1, -1, -1}, {1, 1, 1}});
        check(h.hit && near(h.distance, 9.0f) && near(h.normal, {0, 0, 1}), "ray hits the near face of a box, normal facing the ray");
        check(!raycast(r, Bounds{{-1, -1, -1}, {1, 1, 1}}, 5.0f).hit, "max_distance cuts the ray short");
        check(!raycast(Ray{{3, 0, 10}, {0, 0, -1}}, Bounds{{-1, -1, -1}, {1, 1, 1}}).hit, "ray beside a box misses");
        const RaycastHit inside = raycast(Ray{{0, 0, 0}, {1, 0, 0}}, Bounds{{-1, -1, -1}, {1, 1, 1}});
        check(inside.hit && near(inside.distance, 1.0f), "ray starting inside a box hits its far side");
        {
            // A ray starting inside a model's bounds must still find the
            // model's own surfaces (it once didn't: the bounds pre-check
            // measured to where the ray *leaves* the box).
            const Model big = make_model(box_mesh({10, 10, 10}));
            const RaycastHit in_box = raycast(Ray{{0, 0, 0}, {0, -1, 0}}, big, Transform{});
            check(in_box.hit && near(in_box.distance, 5.0f), "raycast from inside a model's bounds hits the model");
        }
        const RaycastHit sh = raycast_sphere(r, {0, 0, 0}, 2.0f);
        check(sh.hit && near(sh.distance, 8.0f) && near(sh.normal, {0, 0, 1}), "ray hits a sphere's front");
        check(!raycast_sphere(Ray{{0, 5, 10}, {0, 0, -1}}, {0, 0, 0}, 2.0f).hit, "ray passing over a sphere misses");
        const RaycastHit ph = raycast_plane(Ray{{0, 5, 0}, normalize(vec3{1, -1, 0})}, {0, 0, 0}, {0, 1, 0});
        check(ph.hit && near(ph.point, {5, 0, 0}), "diagonal ray hits the ground plane where expected");
        const RaycastHit th = raycast_triangle(r, {-1, -1, 0}, {1, -1, 0}, {0, 1, 0});
        check(th.hit && near(th.distance, 10.0f), "ray hits a triangle in front of it");
        check(raycast_triangle(Ray{{0, 0, -10}, {0, 0, 1}}, {-1, -1, 0}, {1, -1, 0}, {0, 1, 0}).hit, "triangles are hit from behind too");
        check(!raycast_triangle(Ray{{2, 2, 10}, {0, 0, -1}}, {-1, -1, 0}, {1, -1, 0}, {0, 1, 0}).hit, "ray outside a triangle misses");
    }

    // Frustum
    {
        Camera cam;
        cam.position = {0, 0, 0};
        const Frustum f = cam.frustum(16.0f / 9.0f);
        check(f.contains({0, 0, -5}), "point in front of the camera is inside the frustum");
        check(!f.contains({0, 0, 5}), "point behind the camera is outside");
        check(!f.contains({0, 0, -2000}), "point beyond far_z is outside");
        check(!f.intersects(Bounds{{50, -1, -6}, {52, 1, -4}}), "box far off to the side is culled");
        check(f.intersects(Bounds{{-100, -1, -6}, {100, 1, -4}}), "huge box spanning the view is kept");
        check(f.intersects_sphere({0, 0, 1}, 1.5f), "sphere poking in from behind the near plane is kept");
    }

    // Camera screen <-> world
    {
        Camera cam;
        cam.position = {3, 2, 8};
        cam.look_at({0, 0, 0});
        const Rect vp{{0, 0}, {1280, 720}};
        const Ray center = cam.screen_ray({640, 360}, vp);
        check(near(center.direction, cam.forward()), "ray through the screen center goes straight ahead");
        vec2 px;
        check(cam.world_to_screen({0, 0, 0}, vp, px) && near(px.x, 640.0f, 0.05f) && near(px.y, 360.0f, 0.05f),
              "the look_at target projects to the screen center");
        const vec3 world_pt{1.5f, 0.7f, -2.0f};
        cam.world_to_screen(world_pt, vp, px);
        const Ray back = cam.screen_ray(px, vp);
        const vec3 closest = back.at(dot(world_pt - back.origin, back.direction));
        check(near(closest, world_pt, 1e-2f), "world_to_screen then screen_ray passes back through the point");
        check(!cam.world_to_screen({3, 2, 20}, vp, px), "a point behind the camera has no screen position");
        const Ray top_left = cam.screen_ray({0, 0}, vp);
        check(dot(top_left.direction, cam.up()) > 0.0f && dot(top_left.direction, cam.right()) < 0.0f,
              "pixel (0,0) is up and to the left (y-down screen, like the 2D API)");
    }

    // OrbitCamera placement
    {
        OrbitCamera orbit;
        orbit.target = {1, 2, 3};
        orbit.distance = 10.0f;
        orbit.yaw = 0.0f;
        orbit.pitch = 0.0f;
        Camera cam;
        orbit.apply(cam);
        check(near(cam.position, {1, 2, 13}) && near(cam.forward(), {0, 0, -1}), "orbit yaw=0 pitch=0 sits on +Z looking at the target");
        orbit.pitch = radians(90.0f) - 0.001f;
        orbit.apply(cam);
        check(cam.position.y > 11.9f, "orbit pitch up puts the camera above the target");
    }

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all math3d checks passed\n");
    return 0;
}
