// Checks skeletal animation against a tiny skinned glTF this test writes
// itself: a two-bone "arm" (6 vertices, the lower four on the root joint,
// the top two on the tip joint) under an armature node, with a mesh node
// transform that glTF says must be ignored, a "bend" clip (tip rotates 90
// degrees about Z over 1 s; root translation stepped at 0.5 s) and a
// one-key "straight" clip. Expected positions are worked out by hand.
// CPU only: nothing is drawn.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near3(vec3 a, vec3 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
std::string str(vec3 v) { char b[64]; std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z); return b; }

// Builds glTF buffers/bufferViews/accessors as data is added.
struct Builder {
    std::vector<uint8_t> bytes;
    nlohmann::json views = nlohmann::json::array(), accessors = nlohmann::json::array();
    int add(const void* data, size_t size, int component, int count, const char* type) {
        while (bytes.size() % 4) bytes.push_back(0);
        const size_t offset = bytes.size();
        bytes.insert(bytes.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        views.push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}});
        accessors.push_back({{"bufferView", views.size() - 1}, {"componentType", component}, {"count", count}, {"type", type}});
        return static_cast<int>(accessors.size() - 1);
    }
};

std::string base64(const std::vector<uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (in[i] << 16) | (i + 1 < in.size() ? in[i + 1] << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
        out += t[(n >> 18) & 63];
        out += t[(n >> 12) & 63];
        out += i + 1 < in.size() ? t[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? t[n & 63] : '=';
    }
    return out;
}

void write_arm(const std::string& path) {
    Builder b;
    const float pos[] = {-0.1f, 0, 0, 0.1f, 0, 0, -0.1f, 1, 0, 0.1f, 1, 0, -0.1f, 2, 0, 0.1f, 2, 0};
    const float nrm[] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint8_t jnt[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float wgt[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const uint16_t idx[] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
    // Joint world matrices at bind time: root = armature T(0,0,-2), tip = T(0,1,-2). Inverses:
    const float ibm[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 2, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 2, 1};
    const float s45 = std::sin(0.785398163f), c45 = std::cos(0.785398163f);
    const float bend_t[] = {0, 1}, bend_r[] = {0, 0, 0, 1, 0, 0, s45, c45};
    const float step_t[] = {0, 0.5f}, step_v[] = {0, 0, 0, 1, 0, 0};
    const float hold_t[] = {0}, hold_r[] = {0, 0, 0, 1};
    const int a_pos = b.add(pos, sizeof pos, 5126, 6, "VEC3");
    b.accessors[a_pos]["min"] = {-0.1, 0, 0};
    b.accessors[a_pos]["max"] = {0.1, 2, 0};
    const int a_nrm = b.add(nrm, sizeof nrm, 5126, 6, "VEC3");
    const int a_jnt = b.add(jnt, sizeof jnt, 5121, 6, "VEC4"); // unsigned bytes: joint indices as plain integers
    const int a_wgt = b.add(wgt, sizeof wgt, 5126, 6, "VEC4");
    const int a_idx = b.add(idx, sizeof idx, 5123, 12, "SCALAR");
    const int a_ibm = b.add(ibm, sizeof ibm, 5126, 2, "MAT4");
    const int a_bt = b.add(bend_t, sizeof bend_t, 5126, 2, "SCALAR");
    b.accessors[a_bt]["min"] = {0.0};
    b.accessors[a_bt]["max"] = {1.0};
    const int a_br = b.add(bend_r, sizeof bend_r, 5126, 2, "VEC4");
    const int a_st = b.add(step_t, sizeof step_t, 5126, 2, "SCALAR");
    b.accessors[a_st]["min"] = {0.0};
    b.accessors[a_st]["max"] = {0.5};
    const int a_sv = b.add(step_v, sizeof step_v, 5126, 2, "VEC3");
    const int a_ht = b.add(hold_t, sizeof hold_t, 5126, 1, "SCALAR");
    b.accessors[a_ht]["min"] = {0.0};
    b.accessors[a_ht]["max"] = {0.0};
    const int a_hr = b.add(hold_r, sizeof hold_r, 5126, 1, "VEC4");
    while (b.bytes.size() % 4) b.bytes.push_back(0);
    nlohmann::json g = {
        {"asset", {{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {3, 2}}}}},
        {"nodes", {{{"name", "root"}, {"children", {1}}},
                   {{"name", "tip"}, {"translation", {0, 1, 0}}},
                   {{"name", "arm_mesh"}, {"mesh", 0}, {"skin", 0}, {"translation", {5, 0, 0}}},
                   {{"name", "Armature"}, {"translation", {0, 0, -2}}, {"children", {0}}}}},
        {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", a_pos}, {"NORMAL", a_nrm}, {"JOINTS_0", a_jnt}, {"WEIGHTS_0", a_wgt}}},
                                      {"indices", a_idx}}}}}}},
        {"skins", {{{"joints", {0, 1}}, {"inverseBindMatrices", a_ibm}}}},
        {"animations", {{{"name", "bend"},
                         {"samplers", {{{"input", a_bt}, {"output", a_br}, {"interpolation", "LINEAR"}},
                                       {{"input", a_st}, {"output", a_sv}, {"interpolation", "STEP"}}}},
                         {"channels", {{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}},
                                       {{"sampler", 1}, {"target", {{"node", 0}, {"path", "translation"}}}}}}},
                        {{"name", "straight"},
                         {"samplers", {{{"input", a_ht}, {"output", a_hr}}}},
                         {"channels", {{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}}}}}}},
        {"buffers", {{{"byteLength", b.bytes.size()}, {"uri", "data:application/octet-stream;base64," + base64(b.bytes)}}}},
        {"bufferViews", b.views},
        {"accessors", b.accessors},
    };
    std::ofstream(path) << g.dump();
}
} // namespace

int main() {
    const std::string path = "thistle_animation_smoketest_arm.gltf";
    write_arm(path);

    std::printf("loading\n");
    const ModelData data = load_model_data(path);
    check(data.parts.size() == 1 && data.skeleton.joints.size() == 2, "one mesh part, a two-joint skeleton");
    check(data.skeleton.joints.size() == 2 && data.skeleton.joints[0].name == "root" && data.skeleton.joints[1].parent == 0 &&
              data.skeleton.joints[0].parent == -1,
          "joint names and parents");
    check(data.skeleton.joints.size() == 2 && near(data.skeleton.joints[0].root_offset(2, 3), -2.0f),
          "the armature node above the root becomes its root_offset");
    check(!data.parts.empty() && data.parts[0].mesh.vertices.size() == 6 && near(data.parts[0].mesh.vertices[5].joints.x, 1.0f) &&
              near(data.parts[0].mesh.vertices[5].weights.x, 1.0f),
          "joints (unsigned bytes) and weights read per vertex");
    check(data.animations.size() == 2 && data.animations[0].name == "bend" && near(data.animations[0].duration, 1.0f) &&
              data.animations[0].channels.size() == 2 && data.animations[0].channels[1].step,
          "two clips; bend lasts 1 s and its translation channel steps");

    const Model arm = make_model(data);
    const Bounds rest = model_bounds(arm);
    check(near3(rest.min, {-0.1f, 0, 0}) && near3(rest.max, {0.1f, 2, 0}),
          "rest pose sits where it was modeled, ignoring the mesh node's (5,0,0) " + str(rest.min) + " " + str(rest.max));
    check(model_animation_count(arm) == 2 && find_animation(arm, "straight") == 1 && find_animation(arm, "nope") == -1 &&
              model_skeleton(arm) && model_skeleton(arm)->find("tip") == 1,
          "model queries");

    std::printf("posing\n");
    Animator anim(arm);
    check(anim.clip() == -1 && anim.skin_matrices().size() == 2 && near3(anim.skin_matrices()[1].transform_point({0.1f, 2, 0}), {0.1f, 2, 0}),
          "no clip: rest pose (skin matrices are identity)");
    anim.play("bend", 0.0f, false);
    anim.set_time(1.0f);
    const vec3 top = anim.skin_matrices()[1].transform_point({0.1f, 2, 0});
    // The tip joint turns (0.1, 2, 0) 90 degrees about (0, 1, 0) to (-1, 1.1, 0),
    // and by 1 s the root has stepped +1 in x: (0, 1.1, 0).
    check(near3(top, {0.0f, 1.1f, 0}), "bent 90 degrees (plus the stepped root): (0.1, 2, 0) goes to (0, 1.1, 0), got " + str(top));
    check(near3(anim.joint_matrix(1).transform_point({0, 0, 0}), {1, 1, -2}),
          "joint_matrix: the tip joint sits at (1, 1, -2): stepped root offset plus the armature, got " +
              str(anim.joint_matrix(1).transform_point({0, 0, 0})));
    anim.set_time(0.5f);
    const vec3 half = anim.skin_matrices()[1].transform_point({0, 2, 0});
    check(near3(half, {-std::sin(0.7853982f) + 1.0f, 1.0f + std::cos(0.7853982f), 0}), "halfway: 45 degrees (slerped), got " + str(half));
    anim.set_time(0.4f);
    check(near(anim.joint_matrix(0).transform_point({0, 0, 0}).x, 0.0f), "stepped channel holds its first key before 0.5 s");
    anim.set_time(0.6f);
    check(near(anim.joint_matrix(0).transform_point({0, 0, 0}).x, 1.0f), "and jumps to the second after");

    anim.set_time(0.9f);
    anim.update(0.5f);
    check(anim.finished() && near(anim.time(), 1.0f), "a non-looping clip stops at its end");
    anim.play("bend", 0.0f, true); // same clip: now loops, no restart
    anim.update(0.25f);
    check(near(anim.time(), 0.25f), "switching the same clip to looping wraps past the end (1.0 + 0.25 -> 0.25)");

    // The old clip keeps playing while it fades out, so hold "bend" at its
    // end (not looping) to get a clean 90 -> 0 blend.
    anim.play("bend", 0.0f, false);
    anim.set_time(1.0f);
    anim.play("straight", 1.0f);
    anim.update(0.5f);
    const Transform mid = anim.joint_transform(1);
    const float angle = 2.0f * std::acos(std::fabs(mid.rotation.w));
    check(near(angle, 0.7853982f, 0.02f), "crossfading from bent to straight over 1 s: 45 degrees at 0.5 s (" + std::to_string(angle) + " rad)");
    anim.update(0.6f);
    check(near(2.0f * std::acos(std::min(1.0f, std::fabs(anim.joint_transform(1).rotation.w))), 0.0f, 0.02f), "and straight once the fade is over");
    check(anim.clip_name() == "straight" && anim.find_joint("tip") == 1, "clip_name / find_joint");

    anim.stop(0.0f);
    check(anim.clip() == -1 && near3(anim.skin_matrices()[1].transform_point({0.1f, 2, 0}), {0.1f, 2, 0}), "stop(): back to rest");

    Animator unrelated;
    check(unrelated.skin_matrices().empty() && unrelated.clip() == -1, "a default Animator is empty and harmless");
    std::remove(path.c_str());
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
