// Physics3D showcase (needs THISTLE_PHYSICS3D): a crate pyramid, a plank
// tower, barrels, convex rocks, a line of dominoes, a ramp and a goal zone
// (a trigger that counts what rolls into it).
//
//   WASD + mouse   fly (Shift = faster; click to capture the mouse, Esc frees it)
//   left click     throw a ball
//   E              blast whatever you're aiming at
//   Q              drop a crate where you're aiming
//   F              show the colliders
//   R              rebuild the scene
//
// Hard impacts kick up dust (from the contact events). Stats go to the log;
// built with the debug UI module (THISTLE_DEBUG_UI) there's also a stats
// window and a panel for gravity, blast strength and the collider view.
// Opens a window and never quits on its own, so CI builds it but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>
#if THISTLE_DEBUG_UI
#include <imgui.h>
#endif
using namespace thistle;
using namespace thistle::three;

namespace {

enum class Shape { Box, Sphere, Cylinder, Rock };

struct Thing {
    RigidBody body;
    Shape shape;
    vec3 size; // Box: full size. Sphere: diameter. Cylinder: {diameter, height, diameter}
    rgba color;
};

// A lumpy low-poly rock: a coarse sphere with each corner pushed in or out.
MeshData rock_mesh() {
    MeshData m = sphere_mesh(0.6f, 5, 7);
    for (Vertex& v : m.vertices) {
        const float n = perlin3(v.position.x * 3.1f, v.position.y * 3.1f, v.position.z * 3.1f, 7);
        v.position = v.position * (1.0f + 0.35f * n);
        v.position.y *= 0.7f;
    }
    m.make_flat();
    return m;
}

rgba shade(rgba c, float k) { return rgba{c.r * k, c.g * k, c.b * k, c.a}; }

rgba wood(int i) {
    const float k = 0.85f + 0.15f * std::sin(i * 12.9898f);
    return rgb(0.72f * k, 0.52f * k, 0.32f * k);
}

} // namespace

int main() {
    App app{{.title = "thistle physics demo", .width = 1280, .height = 720}};
    log_info("physics demo: WASD+mouse fly, left click throws, E blasts, Q drops a crate, F colliders, R rebuilds");

    const MeshData rock = rock_mesh();
    const Model rock_model = make_model(rock, Material{.color = white});
    const Collider rock_collider = Collider::convex(rock);

    Physics3D physics;
    std::vector<Thing> things;
    std::deque<RigidBody> balls;
    RigidBody goal;
    const vec3 goal_pos{-11, 1, 9}, goal_size{5, 2, 5};
    const Transform ramp{{-5.5f, 1.0f, 9.0f}, quat::euler(0.0f, 0.0f, radians(14.0f)), {8, 0.3f, 4}}; // high end away from the goal
    // Low walls on three sides of the goal (the ramp side is open), so what
    // rolls in stays in.
    const Transform walls[3] = {
        {{goal_pos.x - goal_size.x * 0.5f, 0.3f, goal_pos.z}, {}, {0.3f, 0.6f, goal_size.z}},
        {{goal_pos.x, 0.3f, goal_pos.z - goal_size.z * 0.5f}, {}, {goal_size.x, 0.6f, 0.3f}},
        {{goal_pos.x, 0.3f, goal_pos.z + goal_size.z * 0.5f}, {}, {goal_size.x, 0.6f, 0.3f}},
    };
    int in_goal = 0;

    auto add = [&](const BodySettings& s, Shape shape, vec3 size, rgba color) {
        const RigidBody b = physics.add(s);
        if (b) things.push_back({b, shape, size, color});
        return b;
    };
    auto add_box = [&](vec3 pos, vec3 size, rgba color, quat rot = {}) {
        return add(BodySettings{.collider = Collider::box(size), .position = pos, .rotation = rot, .friction = 0.6f}, Shape::Box, size, color);
    };

    auto build = [&] {
        physics.clear();
        things.clear();
        balls.clear();
        in_goal = 0;
        physics.add(BodySettings{.collider = Collider::box({120, 1, 120}), .type = BodyType::Static, .position = {0, -0.5f, 0}, .friction = 0.8f});

        // A pyramid of crates.
        int n = 0;
        for (int row = 0; row < 8; ++row) {
            const int count = 8 - row;
            for (int i = 0; i < count; ++i) {
                add_box({(i - (count - 1) * 0.5f) * 1.02f, 0.5f + row * 1.0f, -8.0f}, {1, 1, 1}, wood(n++));
            }
        }
        // A plank tower, Jenga style.
        for (int level = 0; level < 14; ++level) {
            for (int i = -1; i <= 1; ++i) {
                const float y = 0.15f + level * 0.3f;
                const rgba c = shade(rgb(0.86f, 0.74f, 0.52f), 0.9f + 0.1f * ((level + i) & 1));
                if (level % 2 == 0) add_box({9.0f, y, -4.0f + i * 0.8f}, {2.4f, 0.3f, 0.75f}, c);
                else add_box({9.0f + i * 0.8f, y, -4.0f}, {0.75f, 0.3f, 2.4f}, c);
            }
        }
        // Dominoes along an arc; the first one is already tipping.
        for (int i = 0; i < 26; ++i) {
            const float a = -0.3f + i * 0.09f;
            const vec3 p{4.0f + std::sin(a) * 9.0f, 0.6f, 15.0f - std::cos(a) * 9.0f};
            // Thin side along the arc, so each one falls onto the next.
            // Its -Z points along the arc; the first leans that way.
            const quat r = quat::euler(i == 0 ? -0.25f : 0.0f, -pi * 0.5f - a);
            add_box(p, {0.6f, 1.2f, 0.18f}, i % 2 ? rgb(0.95f, 0.95f, 0.92f) : rgb(0.15f, 0.15f, 0.18f), r);
        }
        // Barrels, two layers.
        for (int i = 0; i < 7; ++i) {
            const int layer = i < 4 ? 0 : 1;
            const float x = -8.0f + (layer == 0 ? i * 0.85f : 0.42f + (i - 4) * 0.85f);
            add(BodySettings{.collider = Collider::cylinder(0.4f, 1.1f), .position = {x, 0.55f + layer * 1.1f, -3.0f}, .friction = 0.5f},
                Shape::Cylinder, {0.8f, 1.1f, 0.8f}, i % 3 == 0 ? rgb(0.75f, 0.2f, 0.15f) : rgb(0.25f, 0.45f, 0.65f));
        }
        // Rocks: convex hulls of a lumpy mesh.
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.8f;
            add(BodySettings{.collider = rock_collider, .position = {-4.0f + std::cos(a) * 3.0f, 1.0f + i * 0.4f, 4.0f + std::sin(a) * 3.0f},
                             .rotation = quat::euler(a, a * 1.7f), .density = 2500.0f},
                Shape::Rock, {1, 1, 1}, shade(rgb(0.55f, 0.54f, 0.52f), 0.85f + 0.05f * (i % 4)));
        }
        // A ramp down into the goal.
        physics.add(BodySettings{.collider = Collider::box(ramp.scale), .type = BodyType::Static, .position = ramp.position, .rotation = ramp.rotation});
        for (const Transform& w : walls) physics.add_box(w.position, w.scale, BodyType::Static);
        goal = physics.add(BodySettings{.collider = Collider::box(goal_size), .type = BodyType::Static, .position = goal_pos, .trigger = true});
        for (int i = 0; i < 5; ++i) {
            add(BodySettings{.collider = Collider::sphere(0.35f), .position = {-3.0f - i * 0.1f, 3.0f + i, 9.0f}},
                Shape::Sphere, {0.7f, 0.7f, 0.7f}, rgb(0.95f, 0.8f, 0.2f));
        }
    };
    build();

    World world;
    world.sun.direction = {-0.45f, -0.8f, -0.35f};
    world.fog.enabled = true;
    world.fog.start = 45.0f;
    world.fog.end = 110.0f;
    Camera camera;
    camera.position = {2.0f, 7.0f, 24.0f};
    FlyCamera fly;
    fly.look.pitch = radians(-14.0f);
    bool debug = false;
    float blast_speed = 14.0f, blast_radius = 6.0f;

    ParticleSystem dust;
    dust.settings = ParticleSettings{.start_color = rgba{0.85f, 0.8f, 0.7f, 0.7f}, .end_color = rgba{0.85f, 0.8f, 0.7f, 0.0f},
                                     .start_size = 0.25f, .end_size = 0.8f, .lifetime = 0.9f, .velocity = {0, 0.6f, 0},
                                     .spread = 1.5f, .gravity = {0, -0.5f, 0}, .drag = 2.0f};
    const ParticleSettings fire{.start_color = rgb(1.0f, 0.75f, 0.3f), .end_color = rgba{0.9f, 0.2f, 0.05f, 0.0f}, .start_size = 0.6f,
                                .end_size = 0.1f, .lifetime = 0.6f, .velocity = {0, 2, 0}, .spread = 9.0f, .gravity = {0, -2, 0},
                                .drag = 2.5f, .additive = true};

    physics.on_trigger([&](const TriggerEvent& e) {
        if (e.trigger != goal) return;
        in_goal += e.entered ? 1 : -1;
        if (e.entered) log_info("physics demo: something rolled into the goal (" + std::to_string(in_goal) + " inside)");
    });

    app.update([&](Frame f) {
        fly.update(camera, f);
        const Ray aim{camera.position, camera.rotation.forward()};

        if (f.key_pressed(Key::R)) build();
        if (f.key_pressed(Key::F)) debug = !debug;
        if (f.mouse_pressed(Mouse::Left) && mouse_locked()) {
            const RigidBody ball = add(BodySettings{.collider = Collider::sphere(0.25f), .position = aim.at(1.0f),
                                                    .velocity = aim.direction * 35.0f, .density = 3000.0f, .bounciness = 0.35f, .fast = true},
                                       Shape::Sphere, {0.5f, 0.5f, 0.5f}, rgb(0.9f, 0.3f, 0.2f));
            balls.push_back(ball);
            if (balls.size() > 60) { // keep it tidy: the oldest ball goes
                const RigidBody old = balls.front();
                balls.pop_front();
                physics.remove(old);
                things.erase(std::remove_if(things.begin(), things.end(), [&](const Thing& t) { return t.body == old; }), things.end());
            }
        }
        if (f.key_pressed(Key::E)) {
            if (const PhysicsHit hit = physics.raycast(aim, 200.0f)) {
                const int n = physics.explode(hit.point, blast_radius, blast_speed);
                dust.emit(hit.point, 80, fire);
                dust.emit(hit.point, 60);
                log_info("physics demo: blast moved " + std::to_string(n) + " bodies");
            }
        }
        if (f.key_pressed(Key::Q)) {
            if (const PhysicsHit hit = physics.raycast(aim, 200.0f)) add_box(hit.point + vec3{0, 6, 0}, {1, 1, 1}, wood(static_cast<int>(things.size())));
        }

#if THISTLE_DEBUG_UI
        debug_stats_window();
        ImGui::SetNextWindowPos(ImVec2(10, 150), ImGuiCond_FirstUseEver);
        ImGui::Begin("Physics", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("%d bodies", physics.body_count());
        ImGui::SliderFloat("gravity", &physics.gravity.y, -30.0f, 5.0f);
        ImGui::SliderFloat("blast speed", &blast_speed, 1.0f, 40.0f);
        ImGui::SliderFloat("blast radius", &blast_radius, 1.0f, 15.0f);
        ImGui::Checkbox("show colliders", &debug);
        if (ImGui::Button("rebuild")) build();
        ImGui::End();
#endif
        physics.step(f.dt);
        for (const Contact& c : physics.contacts()) {
            if (c.speed > 4.0f) dust.emit(c.point, std::min(24, static_cast<int>(c.speed * 2.0f)));
        }
        dust.update(f.dt);

        world.plane({0, 0, 0}, {120, 120}, rgb(0.45f, 0.58f, 0.36f));
        world.box(ramp, rgb(0.5f, 0.5f, 0.55f));
        for (const Transform& w : walls) world.box(w, rgb(0.5f, 0.5f, 0.55f));
        for (const Thing& t : things) {
            const rgba c = physics.sleeping(t.body) || !debug ? t.color : shade(t.color, 1.15f);
            switch (t.shape) {
                case Shape::Box: world.box(physics.transform(t.body, t.size), c); break;
                case Shape::Sphere: world.sphere(physics.transform(t.body, t.size), c); break;
                case Shape::Cylinder: world.cylinder(physics.transform(t.body, t.size), c); break;
                case Shape::Rock: world.draw(rock_model, physics.transform(t.body), c); break;
            }
        }
        const rgba goal_color = in_goal > 0 ? rgb(0.3f, 1.0f, 0.4f) : rgb(1.0f, 0.85f, 0.2f);
        world.wire_box(Bounds{goal_pos - goal_size * 0.5f, goal_pos + goal_size * 0.5f}, goal_color);
        world.plane(goal_pos - vec3{0, 0.99f, 0}, {goal_size.x, goal_size.z}, shade(goal_color, 0.8f));
        world.draw(dust);
        if (debug) physics.draw_debug(world);
        world.render(f, camera);

        const float cx = f.width * 0.5f, cy = f.height * 0.5f;
        f.rect({cx - 1, cy - 8}, {2, 16}, rgba{1, 1, 1, 0.8f});
        f.rect({cx - 8, cy - 1}, {16, 2}, rgba{1, 1, 1, 0.8f});

        static int frames = 0;
        if (++frames % 120 == 0) {
            int awake = 0;
            for (const Thing& t : things) awake += t.body && !physics.sleeping(t.body);
            const RenderStats st = render_stats();
            log_info("physics demo: bodies=" + std::to_string(physics.body_count()) + " awake=" + std::to_string(awake) +
                     " in_goal=" + std::to_string(in_goal) + " draw_calls=" + std::to_string(st.draw_calls) +
                     " fps=" + std::to_string(static_cast<int>(1.0f / std::max(f.dt, 1e-4f))));
        }
    });
    return app.run();
}
