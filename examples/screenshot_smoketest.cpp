// Draws a known picture for the screenshot tests (tools/test_screenshot.py): a lit green box in 3D, then a red and a
// blue rectangle in 2D on top of it, in a 320x240 window. Run it by hand to see it.
//
// With SCREENSHOT_TEST_DIR=<folder> it instead moves the red rectangle every frame and calls save_screenshot() on
// frames 3, 6 and 9 (shot_3.png, shot_6.png, shot_9.png), then quits on frame 10: one launch, several pictures.
// The red rectangle is drawn by the same callback that asks, so each file must show where it was on *that* frame.
#include <thistle.hpp>
#include <cstdlib>
#include <string>
using namespace thistle;
using namespace thistle::three;

int main() {
    App app{{.title = "screenshot smoketest", .width = 320, .height = 240}};
    World world;
    world.sun.shadows = false;
    Camera camera;
    camera.position = {0.0f, 0.0f, 4.0f};
    camera.rotation = quat{};
    const char* shot_dir = std::getenv("SCREENSHOT_TEST_DIR");
    int frame = 0;
    app.update([&](Frame f) {
        ++frame;
        f.clear(rgb(0.1f, 0.1f, 0.1f));
        world.box({0.0f, 0.0f, 0.0f}, {1.5f, 1.5f, 1.5f}, rgb(0.2f, 0.85f, 0.2f));
        world.render(f, camera);
        const float red_x = shot_dir ? 20.0f + 20.0f * static_cast<float>(frame) : 20.0f;
        f.rect({red_x, 20.0f}, {60.0f, 60.0f}, rgb(1.0f, 0.0f, 0.0f));
        f.rect({240.0f, 160.0f}, {60.0f, 60.0f}, rgb(0.0f, 0.0f, 1.0f));
        if (shot_dir) {
            if (frame == 3 || frame == 6 || frame == 9) {
                if (!save_screenshot(std::string(shot_dir) + "/shot_" + std::to_string(frame) + ".png")) std::exit(4);
            }
            if (frame >= 10) quit();
        }
    });
    app.run();
    return 0;
}
