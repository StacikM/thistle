// Visual check for lighting and sky: point lights and a spotlight (press N
// for night, where they're the only real light), distance fog (F), the sun
// disc, and a cube-map skybox if you pass its 6 faces on the command line:
//   thistle_lights_smoketest right.png left.png top.png bottom.png front.png back.png
// Keys 1-6 point the camera right (+X), left, up, down, back (+Z) and front
// (-Z, the default view direction) so each face can be checked: its label
// must read un-mirrored, and front/back must be the right way round. Right-drag orbits. Opens a window and
// never quits on its own, so CI builds it but doesn't run it.
#include <thistle.hpp>
#include <cmath>
using namespace thistle;
using namespace thistle::three;

int main(int argc, char** argv) {
    App app{{.title = "thistle lights smoketest", .width = 1280, .height = 720}};

    World world;
    world.sun.direction = {-0.6f, -0.35f, -0.7f}; // low sun, so the disc is on screen
    if (argc == 7) world.sky.skybox = load_skybox(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);

    Camera camera;
    OrbitCamera orbit;
    orbit.target = {0.0f, 1.0f, 0.0f};
    orbit.distance = 14.0f;
    orbit.yaw = radians(200.0f);
    orbit.pitch = radians(12.0f);

    const Model pillar = make_model(box_mesh({0.6f, 3.0f, 0.6f}), Material{.color = rgb(0.85f, 0.85f, 0.82f)});
    MeshData rock_mesh = sphere_mesh(1.0f, 5, 7);
    rock_mesh.make_flat();
    const Model rock = make_model(rock_mesh, Material{.color = rgb(0.6f, 0.58f, 0.55f), .specular = 0.05f});
    const Model shiny = make_model(sphere_mesh(0.7f, 24, 32), Material{.color = rgb(0.9f, 0.9f, 0.95f), .specular = 0.9f, .shininess = 80.0f});
    const Model lamp = make_model(sphere_mesh(0.12f, 8, 12), Material{.unlit = true}); // tinted per light below

    bool night = false;
    bool free_look = true;
    app.update([&](Frame f) {
        const float t = static_cast<float>(f.time);
        if (f.key_pressed(Key::N)) night = !night;
        if (f.key_pressed(Key::F)) world.fog.enabled = !world.fog.enabled;
        const Key axis_keys[6] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6};
        const vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int i = 0; i < 6; ++i) {
            if (f.key_pressed(axis_keys[i])) {
                free_look = false;
                camera.position = {0, 1, 0};
                camera.look_at(camera.position + axes[i], i == 2 || i == 3 ? vec3{0, 0, -1} : vec3{0, 1, 0});
            }
        }
        if (f.mouse_down(Mouse::Right)) free_look = true;
        if (free_look) orbit.update(camera, f);

        world.sun.intensity = night ? 0.05f : 1.0f;
        world.ambient = night ? 0.08f : 0.55f;
        world.sky.top = night ? rgb(0.02f, 0.03f, 0.07f) : rgb(0.30f, 0.52f, 0.85f);
        world.sky.horizon = night ? rgb(0.06f, 0.07f, 0.12f) : rgb(0.72f, 0.82f, 0.92f);
        world.sky.sun_disc = !night;

        world.plane({0, 0, 0}, {60, 60}, rgb(0.45f, 0.5f, 0.42f));
        for (int i = 0; i < 8; ++i) {
            const float a = i * (2.0f * pi / 8.0f);
            world.draw(pillar, {std::cos(a) * 6.0f, 1.5f, std::sin(a) * 6.0f});
        }
        world.draw(rock, Transform{{-2.5f, 0.6f, 1.0f}, quat::euler(0.3f, 1.0f)});
        world.draw(shiny, {2.0f, 0.7f, -1.0f});
        for (int i = 0; i < 12; ++i) world.box({-15.0f + i * 3.0f, 1.0f, -25.0f - i * 4.0f}, {2, 2, 2}, coral); // into the fog

        const rgba colors[3] = {rgb(1.0f, 0.35f, 0.2f), rgb(0.2f, 0.6f, 1.0f), rgb(0.4f, 1.0f, 0.4f)};
        for (int i = 0; i < 3; ++i) {
            const float a = t * 0.7f + i * (2.0f * pi / 3.0f);
            const vec3 p{std::cos(a) * 3.5f, 1.2f, std::sin(a) * 3.5f};
            world.light(PointLight{.position = p, .color = colors[i], .range = 6.0f, .intensity = 2.0f});
            world.draw(lamp, Transform{p}, colors[i]);
        }
        world.light(SpotLight{.position = {0, 7, 0}, .direction = {0, -1, 0.15f}, .color = rgb(1, 0.95f, 0.8f),
                              .range = 12.0f, .intensity = 2.5f, .angle = radians(22.0f), .softness = 0.3f});
        world.render(f, camera);
    });
    return app.run();
}
