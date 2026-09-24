// Checks three::Physics3D by simulating at 60 Hz and comparing against
// hand-worked results: a box landing (resting height, impact speed in the
// contact event, falling asleep), a stack staying up, raycasts and
// overlaps, triggers (enter, no false exit when the body inside falls
// asleep, exit on leaving and on removal), collision layers, explode(),
// kinematic pushing, set_type(), every collider kind resting at the right
// height, and stale handles. Headless: models are never uploaded.
//
// Built without Jolt (THISTLE_PHYSICS3D off) it instead checks that the
// stand-in does nothing, safely.
#include <thistle.hpp>
#include <cmath>
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
bool near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }
std::string f2s(float v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

constexpr float dt = 1.0f / 60.0f;
void run(Physics3D& p, float seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 60.0f + 0.5f); ++i) p.step(dt);
}
RigidBody ground(Physics3D& p) { return p.add_box({0, -0.5f, 0}, {200, 1, 200}, BodyType::Static); }
} // namespace

int main() {
    if (!physics3d_available()) {
        std::printf("Physics3D is off in this build: checking the stand-in\n");
        Physics3D p;
        const RigidBody b = p.add_box({0, 1, 0}, {1, 1, 1});
        check(!b && !p.contains(b) && p.body_count() == 0, "add() returns an invalid handle and adds nothing");
        p.step(dt);
        check(p.contacts().empty() && p.trigger_events().empty(), "step() is safe and reports nothing");
        check(p.transform(b, {2, 2, 2}).scale == vec3{2, 2, 2}, "transform() still passes the scale through");
        check(!p.raycast(Ray{{0, 5, 0}, {0, -1, 0}}), "raycast() hits nothing");
        std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
        return g_failures ? 1 : 0;
    }

    std::printf("landing\n");
    {
        Physics3D p;
        const RigidBody floor = ground(p);
        const RigidBody box = p.add(BodySettings{.collider = Collider::box({1, 1, 1}), .position = {0, 5, 0}, .user = 42});
        check(box && p.contains(box) && p.body_count() == 2, "bodies are added");
        check(p.user(box) == 42, "user data round-trips");
        check(near(p.mass(box), 500.0f, 0.5f) && p.mass(floor) == 0.0f, "mass comes from volume * density (1 m^3 of wood = 500 kg); statics have none");
        int callbacks = 0, contact_events = 0;
        float impact = 0.0f;
        p.on_contact([&](const Contact&) { ++callbacks; });
        for (int i = 0; i < 180; ++i) {
            p.step(dt);
            for (const Contact& c : p.contacts()) {
                ++contact_events;
                const bool pair = (c.a == box && c.b == floor) || (c.a == floor && c.b == box);
                if (pair && impact == 0.0f) impact = c.speed;
            }
        }
        // Dropped from a 4.5 m gap: sqrt(2 * 9.81 * 4.5) = 9.4 m/s. Jolt adds
        // the contact a little before touching, so allow a bit under.
        check(impact > 8.5f && impact < 9.8f, "the landing contact reports the impact speed (" + f2s(impact) + " m/s, expect ~9.4)");
        check(callbacks == contact_events && callbacks >= 1, "on_contact fires once per reported contact");
        const vec3 pos = p.position(box);
        check(near(pos.y, 0.5f) && near(pos.x, 0.0f) && near(pos.z, 0.0f), "the box rests on the ground (y = " + f2s(pos.y) + ")");
        check(p.sleeping(box), "and falls asleep once settled");
        const Bounds b = p.bounds(box);
        check(near(b.min.y, 0.0f) && near(b.max.y, 1.0f) && near(b.size().x, 1.0f), "bounds() is the box's world box");
        const Transform t = p.transform(box, {1, 1, 1});
        check(t.position == pos && t.scale == vec3{1, 1, 1}, "transform() matches position()");
    }

    std::printf("stacking\n");
    {
        Physics3D p;
        ground(p);
        RigidBody stack[6];
        for (int i = 0; i < 6; ++i) stack[i] = p.add_box({5, 0.5f + i * 1.0f, 0}, {1, 1, 1});
        run(p, 5.0f);
        const vec3 top = p.position(stack[5]);
        check(near(top.y, 5.5f, 0.05f) && near(top.x, 5.0f, 0.05f) && near(top.z, 0.0f, 0.05f),
              "a 6-box stack stays standing (top at " + f2s(top.x) + ", " + f2s(top.y) + ")");
    }

    std::printf("queries\n");
    {
        Physics3D p;
        const RigidBody floor = ground(p);
        const RigidBody box = p.add_box({0, 0.5f, 0}, {1, 1, 1}, BodyType::Static);
        const PhysicsHit h = p.raycast(Ray{{0, 10, 0}, {0, -1, 0}});
        check(h && h.body == box && near(h.distance, 9.0f, 1e-3f) && near(h.point.y, 1.0f, 1e-3f) && near(h.normal.y, 1.0f, 1e-3f),
              "raycast down hits the box top at distance 9, normal up");
        const PhysicsHit past = p.raycast(Ray{{0, 10, 0}, {0, -1, 0}}, no_limit, box);
        check(past && past.body == floor && near(past.distance, 10.0f, 1e-3f), "ignore skips the box and hits the ground");
        check(!p.raycast(Ray{{0, 10, 0}, {0, -1, 0}}, 5.0f), "max_distance limits the ray");
        const PhysicsHit side = p.raycast(Ray{{-5, 0.5f, 0}, {1, 0, 0}});
        check(side && side.body == box && near(side.normal.x, -1.0f, 1e-3f) && near(side.distance, 4.5f, 1e-3f), "raycast from the side hits the -X face");
        const auto near_box = p.overlap_sphere({0, 1.2f, 0}, 0.3f);
        check(near_box.size() == 1 && near_box[0] == box, "overlap_sphere finds the box it touches, not the ground");
        const auto both = p.overlap_box(Bounds{{-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}});
        check(both.size() == 2, "overlap_box at the box's foot finds the box and the ground");
        check(p.overlap_sphere({0, 5, 0}, 0.5f).empty(), "overlap in empty air finds nothing");
    }

    std::printf("triggers\n");
    {
        Physics3D p;
        const RigidBody floor = ground(p);
        const RigidBody zone = p.add(BodySettings{.collider = Collider::box({4, 2, 4}), .type = BodyType::Static, .position = {10, 1, 0}, .trigger = true});
        check(p.type(zone) == BodyType::Static, "a static trigger reports itself as Static");
        const RigidBody ball = p.add_sphere({10, 6, 0}, 0.5f);
        int entered = 0, left = 0;
        for (int i = 0; i < 360; ++i) {
            p.step(dt);
            for (const TriggerEvent& e : p.trigger_events()) {
                check(e.trigger == zone && e.other == ball, "trigger events name the trigger and the body");
                (e.entered ? entered : left)++;
            }
        }
        check(near(p.position(ball).y, 0.5f), "the ball fell through the trigger and rests on the ground inside it");
        check(p.sleeping(ball), "and fell asleep there");
        check(entered == 1 && left == 0, "exactly one enter and no exit, even after falling asleep (" + std::to_string(entered) + "/" + std::to_string(left) + ")");
        const PhysicsHit h = p.raycast(Ray{{10, 10, 1.5f}, {0, -1, 0}});
        check(h && h.body == floor, "raycasts pass through triggers");
        const RigidBody ball2 = p.add_sphere({10, 3, 0}, 0.5f);
        int in2 = 0, out2 = 0;
        for (int i = 0; i < 60; ++i) {
            p.step(dt);
            for (const TriggerEvent& e : p.trigger_events()) {
                if (e.other == ball2) (e.entered ? in2 : out2)++;
            }
        }
        p.set_position(ball2, {-30, 0.5f, 0});
        for (int i = 0; i < 10; ++i) {
            p.step(dt);
            for (const TriggerEvent& e : p.trigger_events()) {
                if (e.other == ball2) (e.entered ? in2 : out2)++;
            }
        }
        check(in2 == 1 && out2 == 1, "leaving the trigger reports one exit (" + std::to_string(in2) + "/" + std::to_string(out2) + ")");
        const RigidBody ball3 = p.add_sphere({10, 3, 0}, 0.5f);
        int in3 = 0, out3 = 0;
        for (int i = 0; i < 60; ++i) {
            p.step(dt);
            for (const TriggerEvent& e : p.trigger_events()) {
                if (e.other == ball3) (e.entered ? in3 : out3)++;
            }
        }
        p.remove(ball3);
        p.step(dt);
        for (const TriggerEvent& e : p.trigger_events()) {
            if (e.other == ball3) (e.entered ? in3 : out3)++;
        }
        check(in3 == 1 && out3 == 1, "removing a body inside reports it leaving on the next step");
    }

    std::printf("layers\n");
    {
        Physics3D p;
        ground(p);
        p.set_layers_collide(0, 3, false);
        check(!p.layers_collide(0, 3) && !p.layers_collide(3, 0) && p.layers_collide(3, 3), "set_layers_collide is symmetric");
        const RigidBody ghost = p.add(BodySettings{.collider = Collider::box({1, 1, 1}), .position = {-5, 3, 0}, .layer = 3});
        const RigidBody solid = p.add(BodySettings{.collider = Collider::box({1, 1, 1}), .position = {-5, 5, 0}, .layer = 3});
        run(p, 2.0f);
        check(p.position(ghost).y < -3.0f, "a layer-3 box falls through the layer-0 ground");
        check(p.position(solid).y < -3.0f && std::fabs(p.position(solid).y - p.position(ghost).y) > 0.9f,
              "but still lands on the other layer-3 box");
    }

    std::printf("explode\n");
    {
        Physics3D p;
        ground(p);
        RigidBody boxes[4];
        const vec3 c{-10, 0, 0};
        const vec3 offs[4] = {{1.5f, 0, 0}, {-1.5f, 0, 0}, {0, 0, 1.5f}, {0, 0, -1.5f}};
        for (int i = 0; i < 4; ++i) boxes[i] = p.add_box(c + offs[i] + vec3{0, 0.5f, 0}, {1, 1, 1});
        const RigidBody far_box = p.add_box({-10, 0.5f, 20}, {1, 1, 1});
        run(p, 2.0f);
        check(p.sleeping(boxes[0]) && p.sleeping(far_box), "boxes settle and sleep");
        const int moved = p.explode(c + vec3{0, 0.2f, 0}, 5.0f, 10.0f);
        check(moved == 4, "explode() kicks the 4 boxes in range, not the far one (" + std::to_string(moved) + ")");
        bool all_out = true;
        for (int i = 0; i < 4; ++i) {
            const vec3 v = p.velocity(boxes[i]);
            const vec3 dir = normalize(offs[i]);
            all_out = all_out && dot(v, dir) > 2.0f && !p.sleeping(boxes[i]);
        }
        check(all_out, "each flies away from the center, awake");
        check(p.sleeping(far_box) && length(p.velocity(far_box)) == 0.0f, "the far box is untouched");
    }

    std::printf("kinematic & set_type\n");
    {
        Physics3D p;
        ground(p);
        const RigidBody pusher = p.add_box({0, 0.5f, 8}, {1, 1, 1}, BodyType::Kinematic);
        const RigidBody crate = p.add_box({2, 0.5f, 8}, {1, 1, 1});
        vec3 at{0, 0.5f, 8};
        for (int i = 0; i < 120; ++i) {
            at.x += 2.0f * dt;
            p.move_kinematic(pusher, at, {}, dt);
            p.step(dt);
        }
        check(near(p.position(pusher).x, 4.0f, 0.02f), "move_kinematic moves the kinematic box exactly (x = " + f2s(p.position(pusher).x) + ")");
        check(p.position(crate).x > 4.4f, "and it shoves the crate ahead of it (crate x = " + f2s(p.position(crate).x) + ")");

        const RigidBody held = p.add_box({-4, 3, 8}, {1, 1, 1});
        p.set_type(held, BodyType::Kinematic);
        p.set_velocity(held, {});
        run(p, 1.0f);
        check(p.type(held) == BodyType::Kinematic && near(p.position(held).y, 3.0f), "a body made Kinematic hangs in the air");
        p.set_type(held, BodyType::Dynamic);
        run(p, 2.0f);
        check(p.type(held) == BodyType::Dynamic && near(p.position(held).y, 0.5f), "and falls again once Dynamic");
        const RigidBody wall = p.add_box({0, 5, 20}, {1, 1, 1}, BodyType::Static);
        p.set_type(wall, BodyType::Dynamic);
        run(p, 0.5f);
        check(p.type(wall) == BodyType::Static && near(p.position(wall).y, 5.0f), "a body added Static stays static");
    }

    std::printf("colliders\n");
    {
        Physics3D p;
        ground(p);
        const RigidBody sphere = p.add_sphere({0, 3, 0}, 0.5f);
        const RigidBody cyl = p.add(BodySettings{.collider = Collider::cylinder(0.4f, 1.2f), .position = {3, 2, 0}});
        const RigidBody hull = p.add(BodySettings{.collider = Collider::convex(box_mesh({1, 2, 1})), .position = {6, 3, 0}});
        const RigidBody capsule = p.add(BodySettings{.collider = Collider::capsule(0.3f, 1.6f), .position = {9, 3, 0}});
        const RigidBody pair = p.add(BodySettings{
            .collider = Collider::compound({Collider::box({1, 1, 1}).at({-0.5f, 0, 0}), Collider::box({1, 1, 1}).at({0.5f, 0, 0})}),
            .position = {12, 3, 0}});
        const RigidBody lifted = p.add(BodySettings{.collider = Collider::box({1, 1, 1}).at({0, 0.5f, 0}), .position = {15, 3, 0}});
        const RigidBody meshy = p.add(BodySettings{.collider = Collider::mesh(box_mesh({1, 1, 1})), .position = {18, 3, 0}});
        const RigidBody heavy = p.add(BodySettings{.collider = Collider::box({1, 1, 1}), .position = {21, 3, 0}, .mass = 2.0f});
        run(p, 4.0f);
        check(near(p.position(sphere).y, 0.5f), "sphere rests at its radius");
        check(near(p.position(cyl).y, 0.6f), "upright cylinder rests at half its height");
        check(near(p.position(hull).y, 1.0f), "convex hull of a 1x2x1 box rests standing (y = " + f2s(p.position(hull).y) + ")");
        check(p.bounds(capsule).min.y > -0.02f && p.bounds(capsule).min.y < 0.02f, "capsule ends up on the ground");
        check(near(p.position(pair).y, 0.5f) && near(p.bounds(pair).size().x, 2.0f, 0.03f), "compound of two boxes rests as one 2 m body");
        check(near(p.position(lifted).y, 0.0f), "Collider::at offsets the shape within its body (origin at its bottom)");
        check(near(p.position(meshy).y, 0.5f), "a dynamic body with a mesh collider falls as its hull");
        check(near(p.mass(heavy), 2.0f, 1e-3f), "an explicit mass overrides density");

        const Model plane = make_model(plane_mesh(10, 10));
        const RigidBody level = p.add_static(plane, Transform{{40, 1, 40}});
        const RigidBody on_plane = p.add_sphere({40, 4, 40}, 0.5f);
        run(p, 3.0f);
        check(level && near(p.position(on_plane).y, 1.5f), "add_static(model) is solid from above (winding is right)");
        const PhysicsHit h = p.raycast(Ray{{42, 5, 42}, {0, -1, 0}});
        check(h && h.body == level && near(h.point.y, 1.0f, 1e-3f), "and raycasts hit it");

        Terrain terrain(8, 8, 1.0f);
        terrain.origin = {60, 2, 60};
        const RigidBody hill = p.add_static(terrain);
        const RigidBody on_terrain = p.add_sphere({64, 5, 64}, 0.5f);
        run(p, 3.0f);
        check(hill && near(p.position(on_terrain).y, 2.5f), "add_static(terrain) is solid");

        // A small fast body against a thin wall: "fast" keeps it from
        // skipping through between steps.
        p.add_box({80, 1, 0}, {0.1f, 2, 2}, BodyType::Static);
        const RigidBody bullet = p.add(BodySettings{.collider = Collider::sphere(0.05f), .position = {70, 1, 0},
                                                     .velocity = {300, 0, 0}, .gravity_scale = 0.0f, .fast = true});
        run(p, 0.5f);
        check(p.position(bullet).x < 80.0f, "a 'fast' bullet at 300 m/s doesn't tunnel through a 10 cm wall");
    }

    std::printf("handles\n");
    {
        Physics3D p;
        ground(p);
        const RigidBody box = p.add_box({0, 3, 0}, {1, 1, 1});
        p.remove(box);
        check(!p.contains(box) && p.body_count() == 1, "remove() takes it out");
        p.remove(box); // twice: must be harmless
        p.apply_impulse(box, {0, 10, 0});
        p.set_position(box, {1, 2, 3});
        check(p.position(box) == vec3{} && p.mass(box) == 0.0f && !p.sleeping(box) && !p.bounds(box).valid(),
              "a removed body's handle is safe and reads as zeroes");
        const RigidBody next = p.add_box({0, 3, 0}, {1, 1, 1});
        check(next != box && p.contains(next) && !p.contains(box), "a new body in the old slot gets a different handle");
        check(!p.contains(RigidBody{}) && !p.contains(RigidBody{0xFFFFFFu}), "invalid and made-up handles aren't contained");
        Physics3D other; // a second world alongside
        other.add_box({0, 0, 0}, {1, 1, 1});
        p.clear();
        check(p.body_count() == 0 && other.body_count() == 1, "clear() empties one world only");
        const Collider empty = Collider::convex(std::vector<vec3>{});
        check(!p.add(BodySettings{.collider = empty}), "an empty collider adds nothing (and says why)");
    }

    // Worlds can come and go: Jolt's global setup is redone after the last one.
    {
        Physics3D again;
        ground(again);
        const RigidBody b = again.add_box({0, 2, 0}, {1, 1, 1});
        run(again, 1.5f);
        check(near(again.position(b).y, 0.5f), "a world made after all others were destroyed still works");
    }

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
