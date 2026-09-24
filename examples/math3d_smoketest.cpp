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

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all math3d checks passed\n");
    return 0;
}
