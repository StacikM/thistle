// Draws a known picture for the THISTLE_SCREENSHOT test (tools/test_screenshot.py): a lit green box in 3D,
// then a red and a blue rectangle in 2D on top of it, in a 320x240 window. Run it by hand to see it.
#include <thistle.hpp>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "screenshot smoketest", .width = 320, .height = 240}};
    World world;
    world.sun.shadows = false;
    Camera camera;
    camera.position = {0.0f, 0.0f, 4.0f};
    camera.rotation = quat{};
    app.update([&](Frame f) {
        f.clear(rgb(0.1f, 0.1f, 0.1f));
        world.box({0.0f, 0.0f, 0.0f}, {1.5f, 1.5f, 1.5f}, rgb(0.2f, 0.85f, 0.2f));
        world.render(f, camera);
        f.rect({20.0f, 20.0f}, {60.0f, 60.0f}, rgb(1.0f, 0.0f, 0.0f));
        f.rect({240.0f, 160.0f}, {60.0f, 60.0f}, rgb(0.0f, 0.0f, 1.0f));
    });
    app.run();
    return 0;
}
