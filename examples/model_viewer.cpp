// A small model viewer and a visual check for three::load_model():
//   thistle_model_viewer a.glb b.obj c.gltf ...
// Lines the models up side by side, each scaled so its largest side is 2
// units (sample files come in wildly different units), on a ground grid.
// Right-drag orbits, scroll zooms, middle-drag pans. Opens a window and
// never quits on its own, so CI builds it but doesn't run it.
#include <thistle.hpp>
#include <algorithm>
#include <string>
#include <vector>
using namespace thistle;
using namespace thistle::three;

int main(int argc, char** argv) {
    App app{{.title = "thistle model viewer", .width = 1280, .height = 720}};

    struct Placed { Model model; Transform at; };
    std::vector<Placed> placed;
    float x = 0.0f;
    for (int i = 1; i < argc; ++i) {
        const Model m = load_model(argv[i]);
        if (!m.valid()) continue;
        const Bounds b = model_bounds(m);
        const vec3 size = b.size();
        const float scale = 2.0f / std::max({size.x, size.y, size.z, 1e-6f});
        // Sit each model on the ground (y = 0), centered on its slot.
        const vec3 center = b.center();
        placed.push_back({m, Transform{{x - center.x * scale, -b.min.y * scale, -center.z * scale}, {}, {scale, scale, scale}}});
        log_info("model viewer: " + std::string(argv[i]) + " parts=" + std::to_string(model_part_count(m)));
        x += 3.0f;
    }

    World world;
    Camera camera;
    OrbitCamera orbit;
    orbit.target = {std::max(0.0f, x - 3.0f) * 0.5f, 1.0f, 0.0f};
    orbit.distance = 4.0f + x * 0.6f;

    app.update([&](Frame f) {
        orbit.update(camera, f);
        world.grid({orbit.target.x, 0.0f, 0.0f}, std::max(10.0f, x + 6.0f), 1.0f);
        for (const Placed& p : placed) world.draw(p.model, p.at);
        world.render(f, camera);
    });
    return app.run();
}
