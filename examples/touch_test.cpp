// Shows Frame::touches() on a phone or tablet, for checking multi-touch by
// hand. Each finger gets a circle in a color picked from its id, a line back
// to where it landed, a green ring when it begins and a red one when it ends.
// If two fingers ever swapped ids, their colors would visibly swap. The white
// ring is the mouse, which should follow only the first finger down: solid
// while it's held. On desktop the list stays empty and only the ring moves.
//
// Text (ids and counts) needs a font file, which a phone build won't have
// unless one is bundled; without it the colors carry the same information.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace thistle;

namespace {
rgba color_for(int id) {
    const float h = std::fmod(id * 0.618034f, 1.0f) * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    const int sector = static_cast<int>(h);
    const float r[6] = {1, x, 0, 0, x, 1}, g[6] = {x, 1, 1, x, 0, 0}, b[6] = {0, 0, x, 1, 1, x};
    return {0.25f + 0.75f * r[sector], 0.25f + 0.75f * g[sector], 0.25f + 0.75f * b[sector], 1.0f};
}
} // namespace

int main() {
    App app{{.title = "Thistle touch test", .width = 800, .height = 600}};
    // load_font() hands back a handle even for a missing file, so look first.
    Font font;
    for (const char* path : {"font.ttf",
#ifdef THISTLE_TOUCH_TEST_FONT
                             THISTLE_TOUCH_TEST_FONT,
#endif
         }) {
        if (std::FILE* fp = std::fopen(path, "rb")) {
            std::fclose(fp);
            font = load_font(path);
            break;
        }
    }

    struct Flash { vec2 pos; rgba color; double until; };
    std::vector<Flash> flashes;
    int began_total = 0, ended_total = 0;

    app.update([&](Frame f) {
        f.clear(midnight);
        const float r = f.width / 14.0f;

        for (const Touch& t : f.touches()) {
            if (t.began) { ++began_total; flashes.push_back({t.pos, rgba{0.3f, 0.9f, 0.4f, 1.0f}, f.time + 0.3}); }
            if (t.ended) { ++ended_total; flashes.push_back({t.pos, rgba{0.95f, 0.25f, 0.25f, 1.0f}, f.time + 0.3}); }
            f.line(t.start, t.pos, rgba{1, 1, 1, 0.4f}, 3.0f);
            f.circle(t.start, r * 0.25f, rgba{1, 1, 1, 0.3f});
            f.circle(t.pos, r, color_for(t.id));
            if (font.valid()) f.text(std::to_string(t.id), {t.pos.x - r * 0.3f, t.pos.y - r * 0.4f}, {.size = r * 0.8f, .color = black});
        }
        for (const Flash& fl : flashes) {
            if (f.time < fl.until) f.circle_outline(fl.pos, r * 1.4f, fl.color, 6.0f);
        }
        std::erase_if(flashes, [&](const Flash& fl) { return f.time >= fl.until; });

        if (f.mouse_down(Mouse::Left)) f.circle(f.mouse(), r * 0.3f, white);
        f.circle_outline(f.mouse(), r * 0.5f, rgba{1, 1, 1, 0.6f}, 4.0f);

        if (font.valid()) {
            char line[160];
            std::snprintf(line, sizeof(line), "fingers %d   began %d   ended %d   mouse %s",
                          static_cast<int>(f.touches().size()), began_total, ended_total,
                          f.mouse_down(Mouse::Left) ? "down" : "up");
            f.text(line, {20, 20}, {.size = f.width / 32.0f});
        }
    });
    return app.run();
}
