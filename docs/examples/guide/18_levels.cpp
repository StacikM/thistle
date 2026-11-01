// Guide 18: levels in files. The map is a text file (assets/level1.txt), one
// character per tile, so you can edit it in any text editor and see the change
// on the next run. Other data (enemies, a time limit) lives in a JSON file
// (assets/level1.json). Press R to reload both without restarting.
//
// Use read_asset_text(), not std::ifstream: it also works on Android, where
// assets live inside the APK.
#include <thistle.hpp>
#include <string>
#include <vector>
using namespace thistle;

struct Level {
    std::vector<std::string> tiles;
    std::string name;
    float time_limit = 0.0f;
    struct Enemy { vec2 pos; float speed; };
    std::vector<Enemy> enemies;
    vec2 start{0, 0};
};

static const float kTile = 40.0f;

static bool load_level(Level& out, const std::string& map_path, const std::string& json_path) {
    Level level;
    std::string text;
    if (!read_asset_text(map_path, text)) { log_error("could not read " + map_path); return false; }

    // Split into lines.
    std::string line;
    for (const char c : text) {
        if (c == '\n') { if (!line.empty()) level.tiles.push_back(line); line.clear(); }
        else if (c != '\r') line += c;
    }
    if (!line.empty()) level.tiles.push_back(line);

    // Pull 'P' (player start) out of the map and leave floor behind.
    for (size_t row = 0; row < level.tiles.size(); ++row) {
        for (size_t col = 0; col < level.tiles[row].size(); ++col) {
            if (level.tiles[row][col] == 'P') {
                level.start = {col * kTile, row * kTile};
                level.tiles[row][col] = '.';
            }
        }
    }

    if (!read_asset_text(json_path, text)) { log_error("could not read " + json_path); return false; }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) { log_error(json_path + " is not valid JSON"); return false; }
    level.name = j.value("name", "untitled");
    level.time_limit = j.value("time_limit", 0.0f);
    for (const auto& e : j.value("enemies", nlohmann::json::array())) {
        level.enemies.push_back({e.at("pos").get<vec2>(), e.value("speed", 100.0f)});   // vec2 reads straight from [x, y]
    }
    out = level;
    return true;
}

THISTLE_MAIN {
    App app{{.title = "Levels from files", .width = 1280, .height = 400}};
    load_font("assets/font.ttf");

    Level level;
    load_level(level, "assets/level1.txt", "assets/level1.json");

    app.update([&](Frame f) {
        if (f.key_pressed(Key::R)) load_level(level, "assets/level1.txt", "assets/level1.json");

        f.clear(rgb(0.08f, 0.09f, 0.14f));
        for (size_t row = 0; row < level.tiles.size(); ++row) {
            for (size_t col = 0; col < level.tiles[row].size(); ++col) {
                const vec2 at{col * kTile, row * kTile};
                const char c = level.tiles[row][col];
                if (c == '#') f.rect(at, {kTile, kTile}, rgb(0.32f, 0.36f, 0.5f));
                else if (c == 'o') f.circle(at + vec2{kTile / 2, kTile / 2}, 9, rgb(1.0f, 0.82f, 0.25f));
                else if (c == 'E') f.rect(at + vec2{6, 6}, {28, 28}, rgb(0.9f, 0.3f, 0.3f));
            }
        }
        f.rect(level.start + vec2{6, 2}, {28, 36}, rgb(0.35f, 0.75f, 0.95f));
        for (const auto& e : level.enemies) f.circle(e.pos, 14, rgb(0.9f, 0.45f, 0.3f));
        f.text(level.name + "   time limit " + std::to_string(static_cast<int>(level.time_limit)) + " s   enemies " +
                   std::to_string(level.enemies.size()) + "   (R reloads)",
               Anchor::BottomLeft, {20, -12}, {.size = 22});
    });

    THISTLE_RUN(app);
}
