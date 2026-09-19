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

    // Post-processing check: a vignette should darken the corners of
    // everything drawn below, on every backend (Metal/D3D11/GLCore/GLES3).
    set_post_effect(PostEffect::Vignette, 1.0f);

    app.update([&](Frame f) {
        f.clear(rgb(0.08f, 0.10f, 0.16f));

        // Minor-3D check: a spinning cube + sphere/cylinder/cone + a ground
        // plane + an axis line, depth-tested against each other.
        Camera3D cam;
        const float t = static_cast<float>(f.time);
        cam.eye = {6.0f * std::cos(t * 0.5f), 3.0f, 6.0f * std::sin(t * 0.5f)};
        cam.target = {0.0f, 0.0f, 0.0f};
        f.camera3d(cam);
        f.plane3d({0, -1, 0}, 10.0f, 10.0f, rgb(0.3f, 0.5f, 0.3f));

        // 3D collision check: a sphere orbits through a static box — real
        // box3d_sphere3d_overlap() calls every frame, not just drawn to look
        // like they might touch. The box actually changes color on overlap,
        // so a wrong result (or one that never fires, or fires constantly)
        // is immediately visible instead of silently wrong.
        const Box3D box{{-2.4f, 0, 0}, {0.5f, 0.5f, 0.5f}};
        const Sphere3D orbiter{{-2.4f + std::cos(t) * 1.3f, 0, std::sin(t) * 1.3f}, 0.4f};
        const bool colliding = box3d_sphere3d_overlap(box, orbiter);
        f.cube(box.center, {1, 1, 1}, colliding ? rgb(0.95f, 0.2f, 0.2f) : rgb(0.85f, 0.35f, 0.3f));
        f.sphere3d(orbiter.center, orbiter.radius, colliding ? rgb(0.95f, 0.2f, 0.2f) : rgb(0.9f, 0.8f, 0.2f));

        f.cylinder3d({0.8f, 0, 0}, 0.5f, 1.2f, rgb(0.4f, 0.7f, 0.9f));
        f.cone3d({2.4f, 0, 0}, 0.6f, 1.2f, rgb(0.8f, 0.4f, 0.8f));
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
