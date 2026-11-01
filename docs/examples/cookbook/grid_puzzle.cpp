// Recipe: a Sokoban-style grid puzzle with undo. Arrows / WASD push crates onto
// the marked spots; Z undoes a move; R restarts. The trick that makes undo
// trivial: the whole game state is a small value (a copy of the grid), so
// "undo" is just keeping a list of old copies.
#include <thistle.hpp>
#include <string>
#include <vector>
using namespace thistle;

// '#' wall, '.' floor, 'x' goal, 'c' crate, 'C' crate on a goal, '@' player, 'X' player on a goal
static const std::vector<std::string> kStart = {
    "##########",
    "#........#",
    "#..c..x..#",
    "#.@..c...#",
    "#...x....#",
    "#........#",
    "##########",
};
static const float kTile = 64.0f;

struct Board {
    std::vector<std::string> cells = kStart;
    int px = 0, py = 0;

    Board() { for (int y = 0; y < rows(); ++y) for (int x = 0; x < cols(); ++x) if (cells[y][x] == '@') { px = x; py = y; cells[y][x] = '.'; } }
    int rows() const { return static_cast<int>(cells.size()); }
    int cols() const { return static_cast<int>(cells[0].size()); }
    bool is_crate(int x, int y) const { return cells[y][x] == 'c' || cells[y][x] == 'C'; }
    bool is_free(int x, int y) const { return cells[y][x] != '#' && !is_crate(x, y); }

    // Tries to move the player; returns true if anything changed.
    bool move(int dx, int dy) {
        const int nx = px + dx, ny = py + dy;
        if (cells[ny][nx] == '#') return false;
        if (is_crate(nx, ny)) {
            const int cx = nx + dx, cy = ny + dy;
            if (!is_free(cx, cy)) return false;                       // blocked: crate behind crate, or wall
            cells[ny][nx] = (cells[ny][nx] == 'C') ? 'x' : '.';       // crate leaves (its goal stays)
            cells[cy][cx] = (cells[cy][cx] == 'x') ? 'C' : 'c';       // crate arrives
        }
        px = nx; py = ny;
        return true;
    }
    bool won() const {
        for (const std::string& row : cells) if (row.find('c') != std::string::npos) return false;   // a crate off a goal
        return true;
    }
};

THISTLE_MAIN {
    App app{{.title = "Grid puzzle", .width = 700, .height = 520}};
    load_font("assets/font.ttf");

    Board board;
    std::vector<Board> history;   // every earlier state, newest last

    app.update([&](Frame f) {
        int dx = 0, dy = 0;
        if (f.key_pressed(Key::Left)  || f.key_pressed(Key::A)) dx = -1;
        else if (f.key_pressed(Key::Right) || f.key_pressed(Key::D)) dx = 1;
        else if (f.key_pressed(Key::Up)    || f.key_pressed(Key::W)) dy = -1;
        else if (f.key_pressed(Key::Down)  || f.key_pressed(Key::S)) dy = 1;
        if ((dx || dy) && !board.won()) {
            Board before = board;
            if (board.move(dx, dy)) history.push_back(before);
        }
        if (f.key_pressed(Key::Z) && !history.empty()) { board = history.back(); history.pop_back(); }
        if (f.key_pressed(Key::R)) { board = Board{}; history.clear(); }

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        const vec2 origin{(f.width - board.cols() * kTile) * 0.5f, 40.0f};
        for (int y = 0; y < board.rows(); ++y) {
            for (int x = 0; x < board.cols(); ++x) {
                const char c = board.cells[y][x];
                const vec2 at = origin + vec2{x * kTile, y * kTile};
                if (c == '#') { f.rect(at, {kTile, kTile}, rgb(0.32f, 0.36f, 0.5f)); continue; }
                f.rect(at, {kTile - 1, kTile - 1}, rgb(0.13f, 0.15f, 0.23f));
                if (c == 'x' || c == 'C') f.circle_outline(at + vec2{kTile, kTile} * 0.5f, 14, rgb(0.4f, 0.9f, 0.5f), 3.0f);
                if (c == 'c' || c == 'C') f.rect(at + vec2{8, 8}, {kTile - 16, kTile - 16}, c == 'C' ? rgb(0.4f, 0.8f, 0.45f) : rgb(0.85f, 0.55f, 0.3f));
            }
        }
        f.circle(origin + vec2{board.px + 0.5f, board.py + 0.5f} * kTile, 20, rgb(0.35f, 0.75f, 0.95f));
        f.text(board.won() ? "solved in " + std::to_string(history.size()) + " moves!   (R: again)" : "Z: undo   R: restart   moves " + std::to_string(history.size()),
               Anchor::Bottom, {0, -16}, {.size = 26});
    });

    THISTLE_RUN(app);
}
