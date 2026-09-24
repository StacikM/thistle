// Visual check for Terrain + noise: a low-poly island (fbm hills plus ridged
// mountains, faded to sea at the edges), a sea plane, 3,000 instanced trees
// dropped onto the surface with height_at(), and a walkable character
// (click to capture the mouse, WASD + Space, Tab = fly). Opens a window and
// never quits on its own, so CI builds it but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "thistle terrain demo", .width = 1280, .height = 720}};

    Terrain island(160, 160, 1.5f);
    island.origin = {-120.0f, 0.0f, -120.0f};
    island.generate([](float x, float z) {
        const float d = std::sqrt(x * x + z * z) / 115.0f;
        const float falloff = std::clamp(1.2f - d * d * 1.4f, -0.4f, 1.0f); // an island: sinks into the sea at the edge
        const float hills = 6.0f * fbm(x * 0.012f, z * 0.012f, 5, 3);
        const float peaks = 28.0f * std::pow(ridged(x * 0.008f + 3.1f, z * 0.008f, 5, 11), 3.0f);
        return (hills + peaks + 4.0f) * falloff;
    });

    ModelData tree_data;
    MeshData trunk = cylinder_mesh(0.15f, 1.2f, 5);
    trunk.make_flat();
    MeshData crown = sphere_mesh(0.9f, 3, 5);
    crown.make_flat();
    tree_data.parts.push_back({"trunk", trunk, Material{.color = rgb(0.42f, 0.3f, 0.2f)}, mat4::translate({0, 0.6f, 0})});
    tree_data.parts.push_back({"crown", crown, Material{.color = rgb(0.33f, 0.55f, 0.27f)}, mat4::translate({0, 1.7f, 0})});
    const Model tree = make_model(tree_data);
    std::vector<Transform> trees;
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-110.0f, 110.0f), r01(0.0f, 1.0f);
    while (trees.size() < 3000) {
        const float x = u(rng), z = u(rng);
        const float h = island.height_at(x, z);
        // Only on gentle grassy ground above the beach.
        if (h < 1.5f || h > 14.0f || island.normal_at(x, z).y < 0.85f) continue;
        const float s = 0.7f + r01(rng) * 0.8f;
        trees.push_back(Transform{{x, h - 0.1f, z}, quat::euler(0, r01(rng) * 6.28f), {s, s, s}});
    }
    const Model sea = make_model(plane_mesh(600, 600), Material{.color = rgba{0.2f, 0.45f, 0.65f, 0.85f}, .specular = 0.6f,
                                                                  .shininess = 64.0f, .alpha = AlphaMode::Blend});

    CollisionWorld level;
    level.add(island);
    CharacterController player;
    player.position = {0.0f, island.height_at(0.0f, 60.0f) + 45.0f, 60.0f}; // drops in from above
    MouseLook look;
    look.pitch = radians(-25.0f);
    bool flying = false;

    World world;
    world.sun.direction = {-0.55f, -0.55f, -0.4f};
    world.sun.shadow_distance = 80.0f;
    world.fog.enabled = true;
    world.fog.start = 80.0f;
    world.fog.end = 260.0f;
    Camera camera;
    camera.far_z = 600.0f;

    app.update([&](Frame f) {
        look.update(camera, f);
        if (f.key_pressed(Key::Tab)) flying = !flying;
        if (flying) {
            vec3 m = look.move_input(f) * 25.0f;
            if (f.key_down(Key::Space)) m.y += 20.0f;
            if (f.key_down(Key::LeftShift)) m.y -= 20.0f;
            player.position += m * f.dt;
            player.velocity = {};
        } else {
            player.update(level, look.move_input(f), f.key_pressed(Key::Space), f.dt);
        }
        camera.position = player.eye();
        world.draw(island);
        world.draw_many(tree, trees);
        world.draw(sea, {0.0f, 0.4f, 0.0f});
        world.render(f, camera);
    });
    return app.run();
}
