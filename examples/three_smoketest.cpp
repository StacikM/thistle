// Visual check for the thistle::three renderer: lit shapes, a flat-shaded
// low-poly rock, a vertex-colored custom mesh, a see-through sphere, 2D
// drawn both before render() (must be hidden by the sky) and after (must be
// on top), and a second render() into a corner viewport (split-screen).
// Press P to toggle a post effect (3D rendered through the offscreen pass).
// Opens a window and never quits on its own, like thistle_smoketest, so it
// is built by CI but not run there — run it yourself and look at it.
#include <thistle.hpp>
#include <cmath>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "thistle three smoketest", .width = 1280, .height = 720}};

    World world;
    Camera camera;
    camera.position = {0.0f, 3.0f, 9.0f};
    camera.look_at({0.0f, 0.5f, 0.0f});

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
        f.rect({40, 40}, {200, 200}, coral); // behind the sky: must NOT be visible

        world.plane({0, 0, 0}, {20, 20}, rgb(0.42f, 0.6f, 0.35f));
        world.box({-3.0f, 0.5f, 0.0f}, {1, 1, 1}, coral);
        world.sphere({-1.2f, 0.6f, 0.0f}, 0.6f, rgb(0.95f, 0.8f, 0.25f));
        world.cylinder({0.6f, 0.75f, 0.0f}, 0.5f, 1.5f, rgb(0.35f, 0.6f, 0.9f));
        world.cone({2.4f, 0.6f, 0.0f}, 0.6f, 1.2f, rgb(0.75f, 0.4f, 0.8f));
        world.draw(rock, Transform{{4.2f, 0.6f, 0.0f}, quat::euler(0, t * 0.5f)});
        world.draw(rainbow, Transform{{0.0f, 0.02f, 3.0f}});
        world.draw(glass, {-1.2f, 0.9f, 2.0f});
        world.render(f, camera);

        world.render(f, overhead, Rect{{static_cast<float>(f.width) - 330.0f, 10.0f}, {320.0f, 180.0f}});

        f.rect({40, static_cast<float>(f.height) - 80.0f}, {300, 40}, rgba{0, 0, 0, 0.6f}); // HUD on top
        f.rect({50, static_cast<float>(f.height) - 70.0f}, {280.0f * (0.5f + 0.5f * std::sin(t)), 20}, coral);
    });

    return app.run();
}
