// Minimal end-to-end check: opens a window, draws a frame, fires an HTTP
// request. Not a game — just proof that a real executable links against the
// engine and its platform glue on whatever platform it's built for. Build
// with -DTHISTLE_BUILD_SMOKETEST=ON.
#include <thistle.hpp>
#include <cmath>
using namespace thistle;

int main() {
    App app{{.title = "Thistle Smoketest", .width = 640, .height = 480}};

    Http req = Http::get("https://example.com");
    bool reported = false;

    // IAP check: no real product is configured anywhere (no App Store
    // Connect app/products exist for this yet), so this can't prove a
    // purchase succeeds — but it DOES exercise the real StoreKit request
    // round trip end to end: canMakePayments(), an async SKProductsRequest
    // for a bogus id actually going out and coming back, and confirming
    // "ready" ends up true with an appropriately empty result instead of
    // hanging or crashing.
    log_info("smoketest: iap_can_make_payments=" + std::string(iap_can_make_payments() ? "true" : "false"));
    iap_fetch_products({"com.stacik.thistle.smoketest.nonexistent"});
    bool iap_reported = false;

    app.update([&](Frame f) {
        f.clear(rgb(0.08f, 0.10f, 0.16f));

        // Minor-3D check: a spinning cube + a ground plane + an axis line,
        // depth-tested against each other.
        Camera3D cam;
        const float t = static_cast<float>(f.time);
        cam.eye = {3.0f * std::cos(t), 2.0f, 3.0f * std::sin(t)};
        cam.target = {0.0f, 0.0f, 0.0f};
        f.camera3d(cam);
        f.plane3d({0, -1, 0}, 6.0f, 6.0f, rgb(0.3f, 0.5f, 0.3f));
        f.cube({0, 0, 0}, {1, 1, 1}, rgb(0.85f, 0.35f, 0.3f));
        f.line3d({0, -1, 0}, {0, 2, 0}, rgb(1, 1, 0.3f));

        f.camera({0, 0});   // back to 2D for the HUD rect below
        f.rect({40, 40}, {200, 80}, rgb(0.3f, 0.6f, 0.9f));

        if (!reported && req.done()) {
            log_info("smoketest: http status=" + std::to_string(req.status()) +
                      " bytes=" + std::to_string(req.body().size()));
            reported = true;
        }
        if (!iap_reported && iap_products_ready()) {
            log_info("smoketest: iap products_ready, count=" + std::to_string(iap_products().size()));
            iap_reported = true;
        }
    });

    return app.run();
}
