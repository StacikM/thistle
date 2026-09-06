// Minimal end-to-end check: opens a window, draws a frame, fires an HTTP
// request. Not a game — just proof that a real executable links against the
// engine and its platform glue on whatever platform it's built for. Build
// with -DTHISTLE_BUILD_SMOKETEST=ON.
#include <thistle.hpp>
using namespace thistle;

int main() {
    App app{{.title = "Thistle Smoketest", .width = 640, .height = 480}};

    Http req = Http::get("https://example.com");
    bool reported = false;

    app.update([&](Frame f) {
        f.clear(rgb(0.08f, 0.10f, 0.16f));
        f.rect({40, 40}, {200, 80}, rgb(0.3f, 0.6f, 0.9f));

        if (!reported && req.done()) {
            log_info("smoketest: http status=" + std::to_string(req.status()) +
                      " bytes=" + std::to_string(req.body().size()));
            reported = true;
        }
    });

    return app.run();
}
