// Visual check for the thistle::three renderer: lit shapes, a flat-shaded
// low-poly rock, a vertex-colored custom mesh, a see-through sphere, 2D
// drawn both before render() (must be hidden by the sky) and after (must be
// on top), and a second render() into a corner viewport (split-screen).
// Press P to toggle a post effect (3D rendered through the offscreen pass).
// Fly around with a FlyCamera (click to capture the mouse, Esc releases,
// WASD/E/Q, Shift = fast); whatever model is under the cursor (or the
// crosshair, while captured) gets a yellow outline — that's raycast() and
// Camera::screen_ray() working together. Render stats go to the log.
// Opens a window and never quits on its own, like thistle_smoketest, so it
// is built by CI but not run there — run it yourself and look at it.
#include <thistle.hpp>
#include <cmath>
#include <string>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "thistle three smoketest", .width = 1280, .height = 720}};

    World world;
    Camera camera;
    camera.position = {0.0f, 3.0f, 9.0f};
    FlyCamera fly;
    fly.look.pitch = -0.28f;

    MeshData rock_mesh = sphere_mesh(0.8f, 5, 7);
    rock_mesh.make_flat();
    const Model rock = make_model(rock_mesh, Material{.color = rgb(0.55f, 0.52f, 0.48f)});

    // One quad per corner color: checks vertex colors reach the shader.
    MeshData tile;
    tile.add_quad({{-1, 0, 1}, {0, 1, 0}, {0, 1}, rgb(1, 0.2f, 0.2f)},
                  {{1, 0, 1}, {0, 1, 0}, {1, 1}, rgb(0.2f, 1, 0.2f)},
                  {{1, 0, -1}, {0, 1, 0}, {1, 0}, rgb(0.2f, 0.2f, 1)},
                  {{-1, 0, -1}, {0, 1, 0}, {0, 0}, rgb(1, 1, 0.2f)});
    const Model rainbow = make_model(tile);

    const Model glass = make_model(sphere_mesh(0.7f),
        Material{.color = rgba{0.6f, 0.85f, 1.0f, 0.35f}, .specular = 0.8f, .shininess = 64.0f, .alpha = AlphaMode::Blend});

    Camera overhead;
    overhead.position = {0.0f, 14.0f, 0.01f};
    overhead.look_at({0.0f, 0.0f, 0.0f});

    bool post = false;
    app.update([&](Frame f) {
        const float t = static_cast<float>(f.time);
        if (f.key_pressed(Key::P)) {
            post = !post;
            set_post_effect(post ? PostEffect::Vignette : PostEffect::None);
        }
        fly.update(camera, f);
        f.rect({40, 40}, {200, 200}, coral); // behind the sky: must NOT be visible

        world.plane({0, 0, 0}, {20, 20}, rgb(0.42f, 0.6f, 0.35f));
        world.box({-3.0f, 0.5f, 0.0f}, {1, 1, 1}, coral);
        world.sphere({-1.2f, 0.6f, 0.0f}, 0.6f, rgb(0.95f, 0.8f, 0.25f));
        world.cylinder({0.6f, 0.75f, 0.0f}, 0.5f, 1.5f, rgb(0.35f, 0.6f, 0.9f));
        world.cone({2.4f, 0.6f, 0.0f}, 0.6f, 1.2f, rgb(0.75f, 0.4f, 0.8f));
        const Transform rock_at{{4.2f, 0.6f, 0.0f}, quat::euler(0, t * 0.5f)};
        const Transform rainbow_at{0.0f, 0.02f, 3.0f};
        const Transform glass_at{-1.2f, 0.9f, 2.0f};
        world.draw(rock, rock_at);
        world.draw(rainbow, rainbow_at);
        world.draw(glass, glass_at);

        world.grid({0, 0.01f, 0}, 20.0f, 1.0f);
        world.line({0, 0, 0}, {1.5f, 0, 0}, rgb(1, 0.2f, 0.2f), true);
        world.line({0, 0, 0}, {0, 1.5f, 0}, rgb(0.2f, 1, 0.2f), true);
        world.line({0, 0, 0}, {0, 0, 1.5f}, rgb(0.3f, 0.5f, 1), true);

        const vec2 aim = mouse_locked() ? vec2{f.width * 0.5f, f.height * 0.5f} : f.mouse();
        const Ray ray = camera.screen_ray(aim, f);
        struct Pickable { Model model; const Transform* at; };
        for (const Pickable& p : {Pickable{rock, &rock_at}, Pickable{rainbow, &rainbow_at}, Pickable{glass, &glass_at}}) {
            if (raycast(ray, p.model, *p.at)) {
                world.wire_box(model_bounds(p.model).transformed(p.at->matrix()), rgb(1, 0.9f, 0.1f), true);
            }
        }
        world.render(f, camera);

        world.render(f, overhead, Rect{{static_cast<float>(f.width) - 330.0f, 10.0f}, {320.0f, 180.0f}});

        f.rect({40, static_cast<float>(f.height) - 80.0f}, {300, 40}, rgba{0, 0, 0, 0.6f}); // HUD on top
        f.rect({50, static_cast<float>(f.height) - 70.0f}, {280.0f * (0.5f + 0.5f * std::sin(t)), 20}, coral);
        if (mouse_locked()) f.circle({f.width * 0.5f, f.height * 0.5f}, 3.0f, white); // crosshair

        static int frames = 0;
        if (++frames % 120 == 0) {
            const RenderStats st = render_stats();
            log_info("three smoketest: draw_calls=" + std::to_string(st.draw_calls) + " triangles=" +
                     std::to_string(st.triangles) + " culled=" + std::to_string(st.culled));
        }
    });

    return app.run();
}
