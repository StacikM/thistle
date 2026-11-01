// Guide 9: a level made of tiles, and a player who walks and jumps in it
// without any physics engine. Arrow keys / A D to move, Space (or W) to jump.
//
// The trick for solid tiles: move on ONE axis, fix any overlap on that axis,
// then do the other axis. Doing both at once makes you snag on corners.
#include <thistle.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
using namespace thistle;

// '#' is a wall, '.' is air, 'o' is a coin, 'P' is where the player starts.
static const std::vector<std::string> kLevel = {
    "................................",
    "................................",
    "...............ooo..............",
    "..........#####.................",
    "................................",
    "......ooo.................ooo...",
    "....#####..........#####........",
    "................................",
    "..P.........######..........oo..",
    "################################",
};

static const float kTile = 40.0f;

struct World {
    std::vector<std::string> tiles = kLevel;
    int rows() const { return static_cast<int>(tiles.size()); }
    int cols() const { return static_cast<int>(tiles[0].size()); }
    bool solid(int col, int row) const {
        if (col < 0 || col >= cols()) return true;   // the sides count as walls
        if (row < 0 || row >= rows()) return false;
        return tiles[row][col] == '#';
    }
};

// Calls `fn(tile_rect)` for every solid tile the rect overlaps.
template <typename Fn>
static void for_each_solid_tile_touching(const World& w, const Rect& r, Fn fn) {
    const int c0 = static_cast<int>(std::floor(r.pos.x / kTile));
    const int c1 = static_cast<int>(std::floor((r.pos.x + r.size.x - 0.01f) / kTile));
    const int r0 = static_cast<int>(std::floor(r.pos.y / kTile));
    const int r1 = static_cast<int>(std::floor((r.pos.y + r.size.y - 0.01f) / kTile));
    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            if (w.solid(col, row)) fn(Rect{{col * kTile, row * kTile}, {kTile, kTile}});
        }
    }
}

struct Player {
    Rect body{{0, 0}, {28, 36}};
    vec2 velocity{0, 0};
    bool on_ground = false;
};

static void move_player(const World& w, Player& p, float dt) {
    // Horizontal first.
    p.body.pos.x += p.velocity.x * dt;
    for_each_solid_tile_touching(w, p.body, [&](const Rect& tile) {
        if (!p.body.overlaps(tile)) return;
        if (p.velocity.x > 0) p.body.pos.x = tile.pos.x - p.body.size.x;
        else if (p.velocity.x < 0) p.body.pos.x = tile.pos.x + tile.size.x;
        p.velocity.x = 0;
    });

    // Then vertical.
    p.body.pos.y += p.velocity.y * dt;
    p.on_ground = false;
    for_each_solid_tile_touching(w, p.body, [&](const Rect& tile) {
        if (!p.body.overlaps(tile)) return;
        if (p.velocity.y > 0) { p.body.pos.y = tile.pos.y - p.body.size.y; p.on_ground = true; }
        else if (p.velocity.y < 0) p.body.pos.y = tile.pos.y + tile.size.y;
        p.velocity.y = 0;
    });
}

THISTLE_MAIN {
    App app{{.title = "Tiles", .width = 1280, .height = 400}};
    load_font("assets/font.ttf");

    World world;
    Player player;
    int coins = 0;
    for (int row = 0; row < world.rows(); ++row) {
        for (int col = 0; col < world.cols(); ++col) {
            if (world.tiles[row][col] == 'P') {
                player.body.pos = {col * kTile + 6, row * kTile + 2};
                world.tiles[row][col] = '.';
            }
        }
    }

    app.update([&](Frame f) {
        const float dt = std::min(f.dt, 1.0f / 30.0f);   // never let one slow frame teleport the player

        float move = 0.0f;
        if (f.key_down(Key::Left)  || f.key_down(Key::A)) move -= 1.0f;
        if (f.key_down(Key::Right) || f.key_down(Key::D)) move += 1.0f;
        player.velocity.x = move * 240.0f;
        player.velocity.y += 1500.0f * dt;   // gravity
        // A cap on falling speed. Without it, a long drop gets fast enough to
        // move further than a tile in one frame and skip right through the floor.
        player.velocity.y = std::min(player.velocity.y, 900.0f);
        if (player.on_ground && (f.key_pressed(Key::Space) || f.key_pressed(Key::W))) {
            player.velocity.y = -560.0f;
        }
        move_player(world, player, dt);

        // Coins: remove the tile when the player touches it.
        for (int row = 0; row < world.rows(); ++row) {
            for (int col = 0; col < world.cols(); ++col) {
                if (world.tiles[row][col] != 'o') continue;
                const Rect coin{{col * kTile + 10, row * kTile + 10}, {20, 20}};
                if (player.body.overlaps(coin)) { world.tiles[row][col] = '.'; ++coins; }
            }
        }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (int row = 0; row < world.rows(); ++row) {
            for (int col = 0; col < world.cols(); ++col) {
                const char c = world.tiles[row][col];
                const vec2 at{col * kTile, row * kTile};
                if (c == '#') f.rect(at, {kTile, kTile}, rgb(0.32f, 0.36f, 0.5f));
                else if (c == 'o') f.circle(at + vec2{kTile / 2, kTile / 2}, 9, rgb(1.0f, 0.82f, 0.25f));
            }
        }
        f.rect(player.body.pos, player.body.size, rgb(0.35f, 0.75f, 0.95f));
        f.text("coins " + std::to_string(coins), Anchor::TopRight, {-20, 16}, {.size = 26});
    });

    THISTLE_RUN(app);
}
