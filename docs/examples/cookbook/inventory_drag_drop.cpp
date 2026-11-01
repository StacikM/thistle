// Recipe: an inventory grid with drag and drop. Pick up an item by pressing on
// it, drop it on another slot to move it (or swap with what's there), drop it
// outside the grid to put it back. Hover for a tooltip.
#include <thistle.hpp>
#include <string>
#include <vector>
using namespace thistle;

struct Item { std::string name, description; rgba color; };

THISTLE_MAIN {
    App app{{.title = "Inventory", .width = 900, .height = 560}};
    load_font("assets/font.ttf");

    const int cols = 6, rows = 3;
    const float slot = 84.0f, gap = 8.0f;
    std::vector<Item> items(cols * rows);          // an empty name means an empty slot
    items[0] = {"Sword", "Sharp. Mostly.", rgb(0.8f, 0.8f, 0.9f)};
    items[1] = {"Potion", "Restores a bit of health.", rgb(0.9f, 0.3f, 0.4f)};
    items[2] = {"Key", "Opens something, somewhere.", rgb(0.95f, 0.8f, 0.3f)};
    items[7] = {"Map", "Mostly accurate.", rgb(0.5f, 0.8f, 0.6f)};

    int held = -1;            // index of the slot we picked an item up from
    bool dragging = false;

    app.update([&](Frame f) {
        const vec2 origin{(f.width - (cols * (slot + gap) - gap)) * 0.5f, 120.0f};
        auto slot_rect = [&](int i) { return Rect{origin + vec2{(i % cols) * (slot + gap), (i / cols) * (slot + gap)}, {slot, slot}}; };
        auto slot_at = [&](vec2 p) { for (int i = 0; i < cols * rows; ++i) if (slot_rect(i).contains(p)) return i; return -1; };

        const int hover = slot_at(f.mouse());
        if (f.mouse_pressed(Mouse::Left) && hover >= 0 && !items[hover].name.empty()) { held = hover; dragging = true; }
        if (dragging && !f.mouse_down(Mouse::Left)) {
            if (hover >= 0 && held >= 0) std::swap(items[held], items[hover]);   // moving onto an empty slot is just a swap too
            held = -1;
            dragging = false;
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        f.text("Inventory", Anchor::Top, {0, 30}, {.size = 44});
        for (int i = 0; i < cols * rows; ++i) {
            const Rect r = slot_rect(i);
            f.rounded_rect(r.pos, r.size, 10, i == hover ? rgb(0.24f, 0.28f, 0.42f) : rgb(0.15f, 0.17f, 0.26f));
            if (!items[i].name.empty() && !(dragging && i == held)) {
                f.rounded_rect(r.pos + vec2{16, 16}, r.size - vec2{32, 32}, 8, items[i].color);
            }
        }
        if (dragging && held >= 0) {   // the item follows the mouse, drawn on top of everything
            f.rounded_rect(f.mouse() - vec2{34, 34}, {68, 68}, 8, items[held].color);
        }
        if (!dragging && hover >= 0 && !items[hover].name.empty()) {
            const std::string tip = items[hover].name + "\n" + items[hover].description;
            const vec2 size = f.measure_text(tip, {.size = 22}) + vec2{24, 16};
            f.rounded_rect(f.mouse() + vec2{16, 16}, size, 8, rgba{0, 0, 0, 0.85f});
            f.text(tip, f.mouse() + vec2{28, 22}, {.size = 22});
        }
    });

    THISTLE_RUN(app);
}
