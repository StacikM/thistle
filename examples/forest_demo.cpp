// Visual check for World::draw_many(): 10,000 low-poly trees (a two-part
// model: trunk + canopy) with random size, rotation and tint, plus rocks —
// in a handful of draw calls, with shadows. Orbit with the right mouse
// button; render stats go to the log. Opens a window and never quits on its
// own, so CI builds it but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "thistle forest demo", .width = 1280, .height = 720}};

    ModelData tree_data;
    MeshData trunk = cylinder_mesh(0.12f, 1.0f, 6);
    trunk.make_flat();
    MeshData canopy = cone_mesh(0.7f, 2.2f, 7);
    canopy.make_flat();
    tree_data.parts.push_back({"trunk", trunk, Material{.color = rgb(0.45f, 0.3f, 0.2f)}, mat4::translate({0, 0.5f, 0})});
    tree_data.parts.push_back({"canopy", canopy, Material{.color = rgb(0.3f, 0.6f, 0.3f)}, mat4::translate({0, 1.9f, 0})});
    const Model tree = make_model(tree_data);

    MeshData rock_mesh = sphere_mesh(0.5f, 4, 6);
    rock_mesh.make_flat();
    const Model rock = make_model(rock_mesh, Material{.color = rgb(0.55f, 0.55f, 0.58f)});

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(-100.0f, 100.0f), unit(0.0f, 1.0f);
    std::vector<Transform> trees, rocks;
    std::vector<rgba> tree_tints;
    for (int i = 0; i < 10000; ++i) {
        const float s = 0.7f + unit(rng) * 0.9f;
        trees.push_back(Transform{{pos(rng), 0.0f, pos(rng)}, quat::euler(0, unit(rng) * 6.28f), {s, s * (0.8f + unit(rng) * 0.5f), s}});
        const float g = 0.75f + unit(rng) * 0.35f;
        tree_tints.push_back(rgb(g, g * (0.9f + unit(rng) * 0.2f), g));
    }
    for (int i = 0; i < 1500; ++i) {
        const float s = 0.3f + unit(rng) * 1.2f;
        rocks.push_back(Transform{{pos(rng), 0.1f, pos(rng)}, quat::euler(unit(rng), unit(rng) * 6.28f, unit(rng)), {s * 1.3f, s * 0.7f, s}});
    }

    World world;
    world.fog.enabled = true;
    world.fog.start = 40.0f;
    world.fog.end = 140.0f;
    world.sun.direction = {-0.45f, -0.7f, -0.3f};
    Camera camera;
    OrbitCamera orbit;
    orbit.target = {0.0f, 1.0f, 0.0f};
    orbit.distance = 28.0f;
    orbit.pitch = radians(18.0f);

    app.update([&](Frame f) {
        orbit.update(camera, f);
        world.plane({0, 0, 0}, {220, 220}, rgb(0.42f, 0.55f, 0.32f));
        world.draw_many(tree, trees, tree_tints);
        world.draw_many(rock, rocks);
        world.render(f, camera);
        static int frames = 0;
        if (++frames % 120 == 3) {
            const RenderStats st = render_stats();
            log_info("forest demo: draw_calls=" + std::to_string(st.draw_calls) + " triangles=" + std::to_string(st.triangles) +
                     " fps=" + std::to_string(static_cast<int>(1.0f / std::max(f.dt, 1e-4f))));
        }
    });
    return app.run();
}
