// Skeletal animation.
//
//   thistle_animation_demo              a crowd of low-poly robots, built in code
//                                       (skeleton, mesh and walk/idle clips: no files)
//   thistle_animation_demo model.glb    any animated glTF: its clips on 1..9
//
// Right-drag orbits, scroll zooms. With a file: 1..9 switch clips (0.3 s
// crossfades), Space pauses, a yellow ball rides the first joint whose name
// has "hand", "head" or "neck" in it (Animator::joint_transform). Stats go
// to the log. Opens a window and never quits on its own, so CI builds it
// but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

namespace {

// A robot: 9 joints, every body part a box moved rigidly by one joint.
ModelData make_robot() {
    ModelData data;
    Skeleton& sk = data.skeleton;
    auto joint = [&](const char* name, int parent, vec3 at) {
        Skeleton::Joint j;
        j.name = name;
        j.parent = parent;
        j.rest.position = at;
        sk.joints.push_back(j);
        return static_cast<int>(sk.joints.size() - 1);
    };
    const int hips = joint("hips", -1, {0, 1.0f, 0});
    const int chest = joint("chest", hips, {0, 0.25f, 0});
    const int head = joint("head", chest, {0, 0.55f, 0});
    const int arm_l = joint("arm_l", chest, {-0.38f, 0.45f, 0});
    const int arm_r = joint("arm_r", chest, {0.38f, 0.45f, 0});
    const int thigh_l = joint("thigh_l", hips, {-0.15f, -0.05f, 0});
    const int shin_l = joint("shin_l", thigh_l, {0, -0.45f, 0});
    const int thigh_r = joint("thigh_r", hips, {0.15f, -0.05f, 0});
    const int shin_r = joint("shin_r", thigh_r, {0, -0.45f, 0});
    // Rest world positions (no rotations at rest), for inverse binds and placing boxes.
    std::vector<vec3> world(sk.joints.size());
    for (size_t i = 0; i < sk.joints.size(); ++i) {
        world[i] = sk.joints[i].rest.position + (sk.joints[i].parent >= 0 ? world[static_cast<size_t>(sk.joints[i].parent)] : vec3{});
        sk.joints[i].inverse_bind = mat4::translate(-world[i]);
    }
    MeshData mesh;
    auto part = [&](int j, vec3 size, vec3 offset, rgba color) {
        MeshData box = box_mesh(size);
        box.set_color(color);
        for (Vertex& v : box.vertices) {
            v.position = v.position + world[static_cast<size_t>(j)] + offset;
            v.joints = {static_cast<float>(j), 0, 0, 0};
            v.weights = {1, 0, 0, 0};
        }
        mesh.append(box);
    };
    const rgba body = rgb(0.85f, 0.45f, 0.2f), dark = rgb(0.25f, 0.27f, 0.3f), light = rgb(0.9f, 0.9f, 0.85f);
    part(hips, {0.45f, 0.25f, 0.3f}, {0, 0, 0}, dark);
    part(chest, {0.6f, 0.55f, 0.35f}, {0, 0.25f, 0}, body);
    part(head, {0.38f, 0.34f, 0.36f}, {0, 0.2f, 0}, light);
    part(head, {0.26f, 0.07f, 0.02f}, {0, 0.22f, -0.19f}, rgb(0.2f, 0.8f, 1.0f)); // visor
    part(arm_l, {0.16f, 0.62f, 0.16f}, {0, -0.28f, 0}, body);
    part(arm_r, {0.16f, 0.62f, 0.16f}, {0, -0.28f, 0}, body);
    part(thigh_l, {0.2f, 0.45f, 0.2f}, {0, -0.22f, 0}, dark);
    part(thigh_r, {0.2f, 0.45f, 0.2f}, {0, -0.22f, 0}, dark);
    part(shin_l, {0.18f, 0.5f, 0.22f}, {0, -0.25f, -0.02f}, body);
    part(shin_r, {0.18f, 0.5f, 0.22f}, {0, -0.25f, -0.02f}, body);
    mesh.make_flat();
    data.parts.push_back({"robot", mesh, Material{.specular = 0.3f}, mat4{}});

    // Clips: keyframed rotations, looped.
    auto rot_x = [](float deg) {
        const quat q = quat::axis_angle({1, 0, 0}, radians(deg));
        return vec4{q.x, q.y, q.z, q.w};
    };
    auto channel = [](int j, AnimationClip::Channel::Path path, std::vector<float> times, std::vector<vec4> values) {
        AnimationClip::Channel c;
        c.joint = j;
        c.path = path;
        c.times = std::move(times);
        c.values = std::move(values);
        return c;
    };
    using P = AnimationClip::Channel::Path;
    AnimationClip walk{"walk", 1.0f, {}};
    const std::vector<float> t4 = {0, 0.25f, 0.5f, 0.75f, 1.0f};
    walk.channels.push_back(channel(thigh_l, P::Rotation, t4, {rot_x(-30), rot_x(0), rot_x(30), rot_x(0), rot_x(-30)}));
    walk.channels.push_back(channel(thigh_r, P::Rotation, t4, {rot_x(30), rot_x(0), rot_x(-30), rot_x(0), rot_x(30)}));
    walk.channels.push_back(channel(shin_l, P::Rotation, t4, {rot_x(10), rot_x(45), rot_x(5), rot_x(5), rot_x(10)}));
    walk.channels.push_back(channel(shin_r, P::Rotation, t4, {rot_x(5), rot_x(5), rot_x(10), rot_x(45), rot_x(5)}));
    walk.channels.push_back(channel(arm_l, P::Rotation, t4, {rot_x(25), rot_x(0), rot_x(-25), rot_x(0), rot_x(25)}));
    walk.channels.push_back(channel(arm_r, P::Rotation, t4, {rot_x(-25), rot_x(0), rot_x(25), rot_x(0), rot_x(-25)}));
    walk.channels.push_back(channel(hips, P::Translation, t4, {{0, 1.0f, 0, 0}, {0, 1.06f, 0, 0}, {0, 1.0f, 0, 0}, {0, 1.06f, 0, 0}, {0, 1.0f, 0, 0}}));
    walk.channels.push_back(channel(head, P::Rotation, t4, {rot_x(-4), rot_x(2), rot_x(-4), rot_x(2), rot_x(-4)}));
    AnimationClip idle{"idle", 2.0f, {}};
    idle.channels.push_back(channel(chest, P::Rotation, {0, 1, 2}, {rot_x(0), rot_x(4), rot_x(0)}));
    idle.channels.push_back(channel(head, P::Rotation, {0, 1, 2}, {rot_x(5), rot_x(-3), rot_x(5)}));
    idle.channels.push_back(channel(arm_l, P::Rotation, {0, 1, 2}, {rot_x(0), rot_x(-6), rot_x(0)}));
    idle.channels.push_back(channel(arm_r, P::Rotation, {0, 1, 2}, {rot_x(0), rot_x(-6), rot_x(0)}));
    data.animations = {walk, idle};
    return data;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

int main(int argc, char** argv) {
    App app{{.title = "thistle animation demo", .width = 1280, .height = 720}};
    World world;
    world.sun.direction = {-0.5f, -0.8f, -0.4f};
    Camera camera;
    OrbitCamera orbit;
    orbit.pitch = radians(15.0f);

    if (argc >= 2) {
        const Model model = load_model(argv[1]);
        if (!model.valid() || model_animation_count(model) == 0) {
            log_error("animation demo: " + std::string(argv[1]) + " has no animations");
            return 1;
        }
        const Bounds b = model_bounds(model);
        const float size = std::max({b.size().x, b.size().y, b.size().z});
        const float scale = 2.0f / std::max(size, 1e-3f); // any model at about 2 m tall
        orbit.target = {0, 1.0f, 0};
        orbit.distance = 4.0f;
        Animator anim(model);
        anim.play(0);
        int marker = -1;
        for (int j = 0; model_skeleton(model) && j < static_cast<int>(model_skeleton(model)->joints.size()); ++j) {
            const std::string n = lower(model_skeleton(model)->joints[static_cast<size_t>(j)].name);
            if (n.find("hand") != std::string::npos || n.find("head") != std::string::npos || n.find("neck") != std::string::npos) {
                marker = j;
                break;
            }
        }
        std::string clips;
        for (int i = 0; i < model_animation_count(model); ++i) clips += " " + std::to_string(i + 1) + ":" + model_animation(model, i)->name;
        log_info("animation demo: " + std::to_string(model_skeleton(model) ? model_skeleton(model)->joints.size() : 0) + " joints, clips" + clips);
        bool paused = false;
        const Transform place{vec3{-b.center().x * scale, -b.min.y * scale, -b.center().z * scale}, {}, {scale, scale, scale}};
        app.update([&](Frame f) {
            orbit.update(camera, f);
            for (int i = 0; i < 9 && i < model_animation_count(model); ++i) {
                if (f.key_pressed(static_cast<Key>(static_cast<int>(Key::Num1) + i))) {
                    anim.play(i, 0.3f);
                    log_info("animation demo: playing " + anim.clip_name());
                }
            }
            if (f.key_pressed(Key::Space)) paused = !paused;
            if (!paused) anim.update(f.dt);
            world.plane({0, 0, 0}, {12, 12}, rgb(0.5f, 0.6f, 0.45f));
            world.draw(model, place, anim);
            if (marker >= 0) {
                const vec3 p = place.matrix().transform_point(anim.joint_matrix(marker).transform_point({0, 0, 0}));
                world.sphere(p, 0.06f, rgb(1.0f, 0.85f, 0.1f));
            }
            world.render(f, camera);
        });
        return app.run();
    }

    // The robot crowd: one Model, one Animator each, walking in circles.
    const Model robot = make_model(make_robot());
    struct Walker {
        Animator anim;
        float radius, angle, speed;
        bool idle;
    };
    std::vector<Walker> crowd;
    for (int i = 0; i < 24; ++i) {
        Walker w{Animator(robot), 3.0f + (i % 4) * 2.2f, i * 1.7f, 0.9f + 0.1f * (i % 3), i % 6 == 0};
        w.anim.play(w.idle ? "idle" : "walk", 0.0f);
        w.anim.set_time(i * 0.37f);
        w.anim.speed = w.speed;
        crowd.push_back(std::move(w));
    }
    orbit.target = {0, 1.0f, 0};
    orbit.distance = 16.0f;
    orbit.pitch = radians(25.0f);
    app.update([&](Frame f) {
        orbit.update(camera, f);
        world.plane({0, 0, 0}, {40, 40}, rgb(0.45f, 0.55f, 0.4f));
        for (Walker& w : crowd) {
            w.anim.update(f.dt);
            if (!w.idle) w.angle += f.dt * 1.1f * w.speed / w.radius; // about 1.1 m/s along the circle
            const vec3 p{std::cos(w.angle) * w.radius, 0, std::sin(w.angle) * w.radius};
            // Facing along the circle (the robot looks down -Z).
            const float yaw = w.idle ? -w.angle : -w.angle + pi;
            world.draw(robot, Transform{p, quat::euler(0, yaw)}, w.anim);
        }
        world.render(f, camera);
        static int frames = 0;
        if (++frames % 120 == 0) {
            const RenderStats st = render_stats();
            log_info("animation demo: " + std::to_string(crowd.size()) + " robots, draw_calls=" + std::to_string(st.draw_calls) +
                     " triangles=" + std::to_string(st.triangles) + " fps=" + std::to_string(static_cast<int>(1.0f / std::max(f.dt, 1e-4f))));
        }
    });
    return app.run();
}
