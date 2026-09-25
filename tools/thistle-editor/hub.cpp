#include "hub.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

using namespace thistle;

namespace editor {

namespace {

double seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The same rule as `thistle new`: letters, numbers, - and _, starting with a letter.
bool valid_name(const std::string& s) {
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    }
    return true;
}

struct ProjectInfo {
    bool thistle = false;
    std::string name, templ;
};

ProjectInfo read_info(const fs::path& folder) {
    ProjectInfo info;
    std::error_code ec;
    info.name = folder.filename().string();
    if (!fs::exists(folder / "thistle.json", ec)) return info;
    info.thistle = true;
    try {
        std::ifstream in(folder / "thistle.json");
        const nlohmann::json j = nlohmann::json::parse(in);
        if (j.contains("name") && j["name"].is_string()) info.name = j["name"].get<std::string>();
        if (j.contains("template") && j["template"].is_string()) info.templ = j["template"].get<std::string>();
    } catch (const std::exception&) {
        // A damaged thistle.json: still a project, just without the details.
    }
    return info;
}

// The Thistle project `start` is in (a folder with thistle.json, it or above), or empty.
fs::path project_root_of(fs::path start) {
    std::error_code ec;
    for (fs::path p = start; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "thistle.json", ec)) return p;
        if (p == p.parent_path()) break;
    }
    return {};
}

fs::path clean_path(const fs::path& p) {
    std::error_code ec;
    fs::path out = fs::absolute(p, ec).lexically_normal();
    if (out.filename().empty() && out.has_parent_path() && out != out.root_path()) out = out.parent_path(); // "a/b/" -> "a/b"
    return out;
}

std::string time_ago(long long when) {
    const long long d = static_cast<long long>(std::time(nullptr)) - when;
    if (when <= 0) return "";
    if (d < 60) return "just now";
    if (d < 3600) return std::to_string(d / 60) + " min ago";
    if (d < 86400) return std::to_string(d / 3600) + " h ago";
    if (d < 2 * 86400) return "yesterday";
    if (d < 30 * 86400) return std::to_string(d / 86400) + " days ago";
    char buf[32];
    const std::time_t t = static_cast<std::time_t>(when);
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
    return buf;
}

// The level a template comes with, opened when its project is.
std::string template_scene(const std::string& kind) {
    if (kind == "fps" || kind == "third-person") return "assets/scenes/level.scene.json";
    if (kind == "voxel") return "assets/scenes/spawn.scene.json";
    return "";
}

struct Choice {
    const char* id;
    const char* title;
    const char* about;
};
const Choice kTemplates[] = {
    {"fps", "First person", "Walk a level made in this editor, shoot the targets, reach the exit."},
    {"third-person", "Third person", "A character behind an orbiting camera: coins, a jump pad, a flag."},
    {"voxel", "Block world", "An endless Minecraft-style world: break, place, build, saved between runs."},
    {"blank", "Blank (2D)", "A window and two shapes: the smallest start. No 3D level."},
};
const Choice kModules[] = {
    {"physics3d", "3D physics (Jolt)", "Rigid bodies and voxel destruction. Adds a few minutes to the first build."},
    {"debug_ui", "Debug UI (Dear ImGui)", "Windows for tuning values and seeing stats while you make the game."},
};

// Splits text into lines that fit `width` at `size`.
std::vector<std::string> wrap(const Frame& f, const std::string& text, float width, float size) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty()) return;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && f.measure_text(candidate, {.size = size}).x > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

} // namespace

struct Hub::Impl {
    enum class Mode { Home, Browse, NewProject, NotThistle, InsideProject, Busy, Error };
    Mode mode = Mode::Home;

    struct Recent {
        std::string path, name, templ, scene;
        long long opened = 0;
        // Checked every few seconds, not every frame (a folder on a slow drive).
        bool exists = true, thistle = true;
    };
    std::vector<Recent> recents;
    double recents_checked = -100.0;
    float recent_scroll = 0.0f;

    std::string message;
    double message_time = -100.0;

    // Text fields: one at a time is typed into, and its string follows the
    // typing live (the name's check updates as you type).
    std::string active_field;
    bool active_field_drawn = false;
    bool was_typing = false; // at the start of this frame: then Esc ends the typing, not the dialog
    std::string paste_path;

    // The built-in folder browser.
    enum class Purpose { Open, Location } purpose = Purpose::Open;
    Mode browse_return = Mode::Home;
    fs::path browse_dir;
    std::string browse_text;
    struct Entry {
        std::string name;
        bool thistle = false;
    };
    std::vector<Entry> entries;
    std::string browse_error;
    float browse_scroll = 0.0f;

    fs::path pending_folder, pending_root;

    // New project.
    std::string new_name, new_location;
    int new_template = 0;
    bool new_modules[2] = {false, false};
    bool new_open = true, new_run = false;

    // A CLI command in progress (new, init).
    std::unique_ptr<Process> busy;
    std::string busy_title;
    std::vector<std::string> busy_output;
    std::function<void(int)> busy_done;

    std::string error_title;
    std::vector<std::string> error_lines;

    std::optional<OpenRequest> result;

    // This frame's input.
    Frame* f = nullptr;
    vec2 m{};
    bool click = false; // left button pressed this frame
    bool inert = false; // drawing what's under a dialog: looks the same, doesn't react

    // ------------------------------------------------------------ recents
    void load_recents() {
        recents.clear();
        try {
            const nlohmann::json j = nlohmann::json::parse(save::get("recent_projects", "[]"));
            for (const auto& r : j) {
                Recent rec;
                rec.path = r.value("path", "");
                rec.name = r.value("name", "");
                rec.templ = r.value("template", "");
                rec.scene = r.value("scene", "");
                rec.opened = r.value("opened", 0LL);
                if (!rec.path.empty()) recents.push_back(rec);
            }
        } catch (const std::exception&) {
            recents.clear(); // a damaged list: start a new one
        }
    }
    void save_recents() {
        nlohmann::json j = nlohmann::json::array();
        for (const Recent& r : recents) {
            j.push_back({{"path", r.path}, {"name", r.name}, {"template", r.templ}, {"scene", r.scene}, {"opened", r.opened}});
        }
        save::set("recent_projects", j.dump());
    }
    void check_recents() {
        std::error_code ec;
        for (Recent& r : recents) {
            r.exists = fs::is_directory(r.path, ec);
            r.thistle = r.exists && fs::exists(fs::path(r.path) / "thistle.json", ec);
        }
        recents_checked = seconds();
    }
    void remember(const fs::path& folder, const std::string& scene) {
        const fs::path p = clean_path(folder);
        const ProjectInfo info = read_info(p);
        auto it = std::find_if(recents.begin(), recents.end(), [&](const Recent& r) { return fs::path(r.path) == p; });
        Recent rec = it != recents.end() ? *it : Recent{};
        if (it != recents.end()) recents.erase(it);
        rec.path = p.string();
        rec.name = info.name;
        rec.templ = info.templ;
        if (!scene.empty()) rec.scene = scene;
        rec.opened = static_cast<long long>(std::time(nullptr));
        recents.insert(recents.begin(), rec);
        if (recents.size() > 20) recents.resize(20);
        save_recents();
        recents_checked = -100.0;
    }

    void say(const std::string& s) {
        message = s;
        message_time = seconds();
        log_info("Thistle Editor: " + s);
    }

    // ------------------------------------------------------------ opening
    void request_open(const fs::path& folder) {
        const fs::path p = clean_path(folder);
        std::error_code ec;
        if (!fs::is_directory(p, ec)) {
            say("There's no folder at " + pretty_path(p));
            return;
        }
        if (fs::exists(p / "thistle.json", ec)) {
            result = OpenRequest{p, "", false};
            return;
        }
        pending_folder = p;
        pending_root = project_root_of(p.parent_path());
        mode = pending_root.empty() ? Mode::NotThistle : Mode::InsideProject;
    }

    // The `thistle` CLI, for new and init: the same code as the command line.
    void run_cli(const std::string& title, const std::vector<std::string>& cli_args, const fs::path& cwd, std::function<void(int)> done) {
        const auto& py = python_command();
        const fs::path script = cli_script();
        if (py.empty() || script.empty()) {
            error_title = title + ": can't";
            error_lines = {py.empty() ? "Python 3 wasn't found. The editor runs the `thistle` command-line tool for this, which needs it."
                                      : "The engine's tools/thistle-cli/thistle.py wasn't found next to the editor.",
                           py.empty() ? "Install it from https://www.python.org (on Windows, tick \"Add python.exe to PATH\"), then restart the editor."
                                      : "Engine folder the editor looked in: " + engine_dir().string()};
            mode = Mode::Error;
            return;
        }
        std::vector<std::string> args = py;
        args.push_back(script.string());
        args.insert(args.end(), cli_args.begin(), cli_args.end());
        busy = std::make_unique<Process>();
        busy_title = title;
        busy_output.clear();
        busy_done = std::move(done);
        mode = Mode::Busy;
        std::string shown;
        for (const std::string& a : cli_args) shown += " " + a;
        log_info("Thistle Editor: running thistle" + shown);
        busy->start(args, cwd);
    }
    void show_error(const std::string& title, const std::vector<std::string>& lines) {
        error_title = title;
        error_lines = lines;
        mode = Mode::Error;
    }

    void create_project() {
        const fs::path location = clean_path(new_location);
        const fs::path folder = location / new_name;
        const std::string kind = kTemplates[new_template].id;
        std::vector<std::string> args = {"new", new_name, "--at", folder.string(), "--template", kind};
        for (int i = 0; i < 2; ++i) {
            if (new_modules[i]) {
                args.push_back("--with");
                args.push_back(kModules[i].id);
            }
        }
        std::error_code ec;
        const fs::path cwd = fs::is_directory(location, ec) ? location : home_dir();
        const bool open = new_open, run = new_open && new_run;
        const std::string name = new_name;
        run_cli("Creating " + name, args, cwd, [this, folder, kind, open, run, name](int code) {
            if (code != 0) {
                show_error("Couldn't create " + name, busy_output);
                return;
            }
            save::set("new_project_location", folder.parent_path().string());
            remember(folder, template_scene(kind));
            mode = Mode::Home; // (coming back to the hub later mustn't find this dialog still up)
            if (open) result = OpenRequest{folder, template_scene(kind), run};
            else say("Created " + pretty_path(folder));
        });
    }

    // ------------------------------------------------------------ browsing
    void browse_to(const fs::path& dir) {
        std::error_code ec;
        const fs::path d = clean_path(dir);
        entries.clear();
        browse_error.clear();
        browse_scroll = 0.0f;
        fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            browse_error = "Can't look inside " + pretty_path(d) + ": " + ec.message();
            browse_dir = d;
            browse_text = d.string();
            return;
        }
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || name[0] == '$') continue; // hidden, and Windows' system folders
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            entries.push_back({name, fs::exists(it->path() / "thistle.json", e2)});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); });
        browse_dir = d;
        browse_text = d.string();
    }
    void open_browser(Purpose p, const fs::path& start) {
        purpose = p;
        browse_return = mode;
        std::error_code ec;
        browse_to(fs::is_directory(start, ec) ? start : home_dir());
        mode = Mode::Browse;
    }
    void chose_folder(const fs::path& folder) {
        if (purpose == Purpose::Location) {
            new_location = folder.string();
            mode = Mode::NewProject;
        } else {
            mode = Mode::Home;
            request_open(folder);
        }
    }
    // Open folder / Browse: the system's own dialog where there is one.
    void pick(Purpose p, const fs::path& start) {
        if (!can_pick_folder()) {
            open_browser(p, start);
            return;
        }
        const std::string chosen = pick_folder(p == Purpose::Open ? "Open a folder" : "Where to make the project", start.string());
        if (chosen.empty()) return; // cancelled
        purpose = p;
        chose_folder(chosen);
    }

    // ------------------------------------------------------------ widgets
    bool hovering(Rect r) const { return !inert && r.contains(m); }
    bool clicked(Rect r) const { return click && hovering(r); }
    void label(const std::string& t, vec2 p, rgba c = theme::dim, float size = 13.0f) { f->text(t, p, {.size = size, .color = c}); }
    // Cut to fit a width (long paths in command output).
    std::string fit(std::string t, float room, float size) const {
        const float w = f->measure_text(t, {.size = size}).x;
        if (w <= room || t.empty()) return t;
        t.resize(static_cast<size_t>(t.size() * room / w * 0.97f));
        return t + "...";
    }
    bool button(const std::string& t, Rect r, bool primary = false, bool enabled = true, float size = 14.0f) {
        const bool hot = enabled && hovering(r);
        rgba bg = primary ? rgb(0.78f, 0.44f, 0.13f) : theme::field;
        if (hot) bg = primary ? theme::accent : theme::field_hover;
        if (!enabled) bg = rgb(0.18f, 0.18f, 0.19f);
        f->rect(r.pos, r.size, bg);
        vec2 tsz = f->measure_text(t, {.size = size});
        float s = size;
        if (tsz.x > r.size.x - 12) {
            s = size * (r.size.x - 12) / tsz.x;
            tsz = f->measure_text(t, {.size = s});
        }
        f->text(t, {r.pos.x + (r.size.x - tsz.x) * 0.5f, r.pos.y + (r.size.y - s) * 0.5f - 1},
                {.size = s, .color = enabled ? (primary ? rgb(1, 1, 1) : theme::text) : theme::faint});
        return enabled && clicked(r);
    }
    bool checkbox(const std::string& t, bool& v, vec2 p, bool enabled = true) {
        const Rect r{p, {18, 18}};
        const Rect hit{p, {f->measure_text(t, {.size = 14}).x + 30, 18}};
        f->rect(r.pos, r.size, !enabled ? rgb(0.18f, 0.18f, 0.19f) : hovering(hit) ? theme::field_hover : theme::field);
        if (v) f->rect(r.pos + vec2{4, 4}, {10, 10}, enabled ? theme::accent : theme::faint);
        label(t, p + vec2{28, 1}, enabled ? theme::text : theme::faint, 14);
        if (enabled && clicked(hit)) {
            v = !v;
            return true;
        }
        return false;
    }
    // A text field. Returns true when Enter is pressed in it.
    bool field(const std::string& id, Rect r, std::string& value, const std::string& placeholder = "") {
        const bool active = active_field == id;
        if (!active && clicked(r)) {
            if (!active_field.empty()) end_text_input();
            active_field = id;
            begin_text_input(value, 1024);
        }
        bool enter = false;
        if (active_field == id) {
            active_field_drawn = true;
            value = text_input();
            if (f->key_pressed(Key::Enter) || f->key_pressed(Key::KeypadEnter)) {
                enter = true;
                end_text_input();
                active_field.clear();
            } else if (f->key_pressed(Key::Escape) || (click && !r.contains(m))) {
                end_text_input();
                active_field.clear();
            }
        }
        const bool typing = active_field == id;
        f->rect(r.pos, r.size, typing ? theme::field_active : hovering(r) ? theme::field_hover : theme::field);
        std::string shown = typing ? value + "|" : value;
        rgba color = theme::text;
        if (shown.empty()) {
            shown = placeholder;
            color = theme::faint;
        }
        const float size = 14.0f;
        const float room = r.size.x - 16;
        if (f->measure_text(shown, {.size = size}).x > room) {
            // Too long: show the end, where the typing is.
            size_t cut = 0;
            while (cut < shown.size() && f->measure_text("..." + shown.substr(cut), {.size = size}).x > room) ++cut;
            shown = "..." + shown.substr(cut);
        }
        f->text(shown, {r.pos.x + 8, r.pos.y + (r.size.y - size) * 0.5f - 1}, {.size = size, .color = color});
        return enter;
    }
    // A dialog box: a dark backdrop over everything, then its panel.
    Rect dialog(vec2 size) {
        const float W = static_cast<float>(f->width), H = static_cast<float>(f->height);
        f->rect({0, 0}, {W, H}, rgba{0, 0, 0, 0.55f});
        const Rect box{{std::floor((W - size.x) * 0.5f), std::floor(std::max(20.0f, (H - size.y) * 0.4f))}, size};
        f->rect(box.pos - vec2{1, 1}, box.size + vec2{2, 2}, theme::accent);
        f->rect(box.pos, box.size, theme::chrome);
        return box;
    }

    // ------------------------------------------------------------ screens
    void draw_home() {
        const float W = static_cast<float>(f->width), H = static_cast<float>(f->height);
        constexpr float TOP = 48.0f, LEFT = 320.0f;
        f->rect({0, 0}, {W, H}, theme::panel);
        f->rect({0, 0}, {W, TOP}, theme::chrome);
        label("Thistle Editor", {20, 13}, theme::text, 20);
        const std::string engine = engine_dir().empty() ? "engine: not found" : "engine: " + pretty_path(engine_dir());
        label(engine, {W - f->measure_text(engine, {.size = 12}).x - 16, 18}, theme::faint, 12);

        // Left: what to do.
        f->rect({0, TOP}, {LEFT, H - TOP}, rgb(0.13f, 0.13f, 0.14f));
        float y = TOP + 26;
        if (button("+  New project", Rect{{20, y}, {LEFT - 40, 46}}, true, true, 16)) {
            new_name.clear();
            if (new_location.empty()) {
                const std::string last = save::get("new_project_location", "");
                std::error_code ec;
                new_location = !last.empty() && fs::is_directory(last, ec) ? last : home_dir().string();
            }
            mode = Mode::NewProject;
        }
        y += 58;
        if (button("Open folder...", Rect{{20, y}, {LEFT - 40, 40}}, false, true, 15)) pick(Purpose::Open, home_dir());
        y += 48;
        if (can_pick_folder()) { // else Open folder already is the editor's own browser
            if (button("Browse inside the editor", Rect{{20, y}, {LEFT - 40, 30}}, false, true, 13)) open_browser(Purpose::Open, home_dir());
            y += 44;
        }
        y += 8;
        label("Or type or paste a folder's path:", {20, y}, theme::dim, 13);
        if (field("paste", Rect{{20, y + 20}, {LEFT - 40, 30}}, paste_path, "then press Enter") && !paste_path.empty()) {
            std::string p = paste_path;
            while (!p.empty() && (p.back() == ' ' || p.back() == '"' || p.back() == '\'')) p.pop_back();
            while (!p.empty() && (p.front() == ' ' || p.front() == '"' || p.front() == '\'')) p.erase(0, 1);
            request_open(p);
        }
        y += 64;
        label("Or drop a folder on this window.", {20, y}, theme::faint, 13);

        // What New project and Build & run need, found or not.
        const bool py = !python_command().empty(), cli = !cli_script().empty();
        float by = H - 58;
        label(std::string(py ? "Python 3: found" : "Python 3: not found (New project and Build & run need it)"), {20, by},
              py ? theme::faint : theme::bad, 12);
        label(std::string(cli ? "thistle CLI: found" : "thistle CLI: not found next to the editor"), {20, by + 18}, cli ? theme::faint : theme::bad, 12);

        // Right: recent projects.
        if (seconds() - recents_checked > 3.0) check_recents();
        const float x0 = LEFT + 28, w = W - LEFT - 56;
        label("Recent projects", {x0, TOP + 24}, theme::text, 18);
        const float list_top = TOP + 62, list_h = H - list_top - 16;
        const Rect list{{x0, list_top}, {w, list_h}};
        constexpr float ROW = 64.0f, GAP = 6.0f;
        if (recents.empty()) {
            label("No recent projects yet.", {x0, list_top + 8}, theme::dim, 15);
            label("Make one with New project, or open a folder that has one.", {x0, list_top + 32}, theme::faint, 13);
        }
        const float content = recents.size() * (ROW + GAP);
        if (hovering(list) && f->mouse_scroll() != 0.0f) recent_scroll -= f->mouse_scroll() * 40.0f;
        recent_scroll = std::clamp(recent_scroll, 0.0f, std::max(0.0f, content - list_h));
        int forget = -1;
        for (size_t i = 0; i < recents.size(); ++i) {
            const Recent& r = recents[i];
            const float ry = list_top + i * (ROW + GAP) - recent_scroll;
            if (ry < list_top - 1 || ry + ROW > list_top + list_h + 1) continue; // only whole rows (there's no clipping)
            const Rect row{{x0, ry}, {w, ROW}};
            const Rect x_btn{{x0 + w - 34, ry + 20}, {24, 24}};
            const bool over_x = hovering(x_btn);
            f->rect(row.pos, row.size, hovering(row) && !over_x ? theme::field_hover : theme::field);
            label(r.name.empty() ? fs::path(r.path).filename().string() : r.name, {x0 + 14, ry + 11}, r.exists ? theme::text : theme::dim, 16);
            std::string badge;
            rgba badge_color = theme::accent;
            if (!r.exists) {
                badge = "folder not found";
                badge_color = theme::bad;
            } else if (!r.thistle) {
                badge = "not a Thistle project";
                badge_color = theme::faint;
            } else {
                badge = r.templ;
            }
            const float name_w = f->measure_text(r.name.empty() ? fs::path(r.path).filename().string() : r.name, {.size = 16}).x;
            if (!badge.empty()) label(badge, {x0 + 26 + name_w, ry + 14}, badge_color, 13);
            label(pretty_path(r.path), {x0 + 14, ry + 38}, theme::faint, 12);
            const std::string when = time_ago(r.opened);
            label(when, {x0 + w - 48 - f->measure_text(when, {.size = 12}).x, ry + 13}, theme::dim, 12);
            f->rect(x_btn.pos, x_btn.size, over_x ? theme::field_hover : rgba{0, 0, 0, 0});
            label("x", x_btn.pos + vec2{8, 3}, over_x ? theme::text : theme::faint, 14);
            if (clicked(x_btn)) {
                forget = static_cast<int>(i);
            } else if (clicked(row)) {
                if (!r.exists) say("That folder isn't there any more (moved or deleted?). The x forgets it.");
                else request_open(r.path);
            }
        }
        if (forget >= 0) {
            recents.erase(recents.begin() + forget);
            save_recents();
        }

        if (!message.empty() && seconds() - message_time < 6.0) {
            const vec2 sz = f->measure_text(message, {.size = 14});
            const vec2 p{W - sz.x - 36, H - 44};
            f->rect(p - vec2{12, 8}, sz + vec2{24, 16}, theme::chrome);
            label(message, p, theme::accent, 14);
        }
    }

    void draw_new_project() {
        const Rect box = dialog({660, 600});
        const vec2 o = box.pos;
        label("New project", o + vec2{24, 18}, theme::text, 20);

        label("Name", o + vec2{24, 64}, theme::dim, 13);
        field("new_name", Rect{o + vec2{24, 84}, {612, 32}}, new_name, "my-game");
        const fs::path location = clean_path(new_location.empty() ? home_dir() : fs::path(new_location));
        const fs::path folder = location / new_name;
        std::error_code ec;
        std::string problem;
        if (new_name.empty()) problem = "";
        else if (!valid_name(new_name)) problem = "Use letters, numbers, - and _, starting with a letter.";
        else if (fs::exists(folder, ec)) problem = pretty_path(folder) + " already exists.";
        label(problem.empty() ? "Letters, numbers, - and _. It's the game's name and its folder's." : problem, o + vec2{24, 122},
              problem.empty() ? theme::faint : theme::bad, 12);

        label("Location", o + vec2{24, 150}, theme::dim, 13);
        const bool native = can_pick_folder();
        const float fw = native ? 612 - 196 : 612 - 104;
        field("new_location", Rect{o + vec2{24, 170}, {fw, 32}}, new_location, home_dir().string());
        if (button("Browse...", Rect{o + vec2{24 + fw + 8, 170}, {96, 32}})) pick(Purpose::Location, location);
        if (native && button("In editor", Rect{o + vec2{24 + fw + 108, 170}, {84, 32}}, false, true, 13)) open_browser(Purpose::Location, location);
        label("Makes " + pretty_path(new_name.empty() ? location / "<name>" : folder), o + vec2{24, 208}, theme::faint, 12);

        label("Start from", o + vec2{24, 238}, theme::dim, 13);
        for (int i = 0; i < 4; ++i) {
            const Rect card{o + vec2{24.0f + (i % 2) * 310.0f, 258.0f + (i / 2) * 78.0f}, {302, 70}};
            const bool on = new_template == i;
            f->rect(card.pos - vec2{1, 1}, card.size + vec2{2, 2}, on ? theme::accent : theme::chrome);
            f->rect(card.pos, card.size, on ? theme::field_active : hovering(card) ? theme::field_hover : theme::field);
            label(kTemplates[i].title, card.pos + vec2{12, 8}, theme::text, 15);
            float ly = card.pos.y + 30;
            for (const std::string& line : wrap(*f, kTemplates[i].about, card.size.x - 24, 12)) {
                label(line, {card.pos.x + 12, ly}, theme::dim, 12);
                ly += 15;
            }
            if (clicked(card)) new_template = i;
        }

        label("Optional engine modules", o + vec2{24, 422}, theme::dim, 13);
        for (int i = 0; i < 2; ++i) {
            const vec2 p = o + vec2{24, 444.0f + i * 34.0f};
            checkbox(kModules[i].title, new_modules[i], p);
            label(kModules[i].about, p + vec2{28, 18}, theme::faint, 11);
        }

        checkbox("Open it in the editor", new_open, o + vec2{24, 520});
        checkbox("Then build and run it", new_run, o + vec2{250, 520}, new_open);
        if (new_open && new_run) label("The first build compiles the engine: a few minutes.", o + vec2{278, 540}, theme::faint, 11);

        const bool tools = !python_command().empty() && !cli_script().empty();
        const bool ok = tools && valid_name(new_name) && problem.empty();
        if (button("Create project", Rect{o + vec2{24, 556}, {200, 32}}, true, ok)) create_project();
        if (button("Cancel", Rect{o + vec2{236, 556}, {110, 32}}) || (!was_typing && f->key_pressed(Key::Escape))) mode = Mode::Home;
        if (!tools) label("Needs Python 3 and the thistle CLI (see the bottom left).", o + vec2{360, 564}, theme::bad, 12);
    }

    void draw_browser() {
        const Rect box = dialog({700, 580});
        const vec2 o = box.pos;
        label(purpose == Purpose::Open ? "Open a folder" : "Where to make the project", o + vec2{24, 18}, theme::text, 20);
        if (field("browse_path", Rect{o + vec2{24, 58}, {652, 30}}, browse_text) && !browse_text.empty()) browse_to(browse_text);
        float bx = o.x + 24;
        auto small = [&](const std::string& t, float w) {
            const bool r = button(t, Rect{{bx, o.y + 96}, {w, 28}}, false, true, 13);
            bx += w + 6;
            return r;
        };
        if (small("Up", 56) && browse_dir.has_parent_path() && browse_dir != browse_dir.root_path()) browse_to(browse_dir.parent_path());
        if (small("Home", 64)) browse_to(home_dir());
        const std::vector<fs::path> roots = filesystem_roots();
        if (roots.size() > 1) {
            for (const fs::path& r : roots) {
                if (small(r.string().substr(0, 2), 40)) browse_to(r);
            }
        }

        const Rect list{o + vec2{24, 134}, {652, 360}};
        f->rect(list.pos, list.size, theme::panel);
        constexpr float ROW = 28.0f;
        if (!browse_error.empty()) label(browse_error, list.pos + vec2{10, 10}, theme::bad, 13);
        else if (entries.empty()) label("No folders in here.", list.pos + vec2{10, 10}, theme::faint, 13);
        if (hovering(list) && f->mouse_scroll() != 0.0f) browse_scroll -= f->mouse_scroll() * 40.0f;
        browse_scroll = std::clamp(browse_scroll, 0.0f, std::max(0.0f, entries.size() * ROW - list.size.y));
        fs::path go;
        for (size_t i = 0; i < entries.size(); ++i) {
            const float ry = list.pos.y + i * ROW - browse_scroll;
            if (ry < list.pos.y - 1 || ry + ROW > list.pos.y + list.size.y + 1) continue;
            const Entry& e = entries[i];
            const Rect row{{list.pos.x, ry}, {list.size.x, ROW - 2}};
            const Rect open_btn{{row.pos.x + row.size.x - 72, ry + 3}, {64, ROW - 8}};
            const bool can_open_it = e.thistle && purpose == Purpose::Open;
            const bool over_open = can_open_it && hovering(open_btn);
            if (hovering(row) && !over_open) f->rect(row.pos, row.size, theme::field_hover);
            label(e.name + "/", row.pos + vec2{12, 6}, theme::text, 14);
            if (e.thistle) {
                const float nw = f->measure_text(e.name + "/", {.size = 14}).x;
                label("Thistle project", row.pos + vec2{24 + nw, 8}, theme::accent, 12);
            }
            if (can_open_it && button("Open", open_btn, false, true, 12)) {
                chose_folder(browse_dir / e.name);
                return;
            }
            if (!over_open && clicked(row)) go = browse_dir / e.name;
        }
        if (!go.empty()) browse_to(go);

        const std::string here = browse_dir.filename().empty() ? browse_dir.string() : browse_dir.filename().string();
        const std::string verb = purpose == Purpose::Open ? "Open \"" + here + "\"" : "Make it in \"" + here + "\"";
        if (button(verb, Rect{o + vec2{24, 508}, {300, 34}}, true, browse_error.empty())) {
            chose_folder(browse_dir);
            return;
        }
        if (button("Cancel", Rect{o + vec2{336, 508}, {110, 34}}) || (!was_typing && f->key_pressed(Key::Escape))) mode = browse_return;
        label("Click a folder to go into it.", o + vec2{24, 552}, theme::faint, 12);
    }

    void draw_not_thistle() {
        const Rect box = dialog({620, 250});
        const vec2 o = box.pos;
        const std::string name = pending_folder.filename().string();
        label("\"" + name + "\" isn't a Thistle project", o + vec2{24, 20}, theme::text, 19);
        label("There's no thistle.json in " + pretty_path(pending_folder) + ".", o + vec2{24, 56}, theme::dim, 14);
        const bool tools = !python_command().empty() && !cli_script().empty();
        if (button("Open anyway", Rect{o + vec2{24, 96}, {160, 34}})) {
            result = OpenRequest{pending_folder, "", false};
            mode = Mode::Home;
        }
        label("Edit its assets/ as it is.", o + vec2{24, 138}, theme::faint, 12);
        if (button("Make it a Thistle project", Rect{o + vec2{196, 96}, {240, 34}}, true, tools)) {
            const fs::path folder = pending_folder;
            run_cli("Making " + name + " a Thistle project", {"init"}, folder, [this, folder](int code) {
                if (code != 0) {
                    show_error("Couldn't make it a Thistle project", busy_output);
                    return;
                }
                remember(folder, "");
                result = OpenRequest{folder, "", false};
                mode = Mode::Home;
            });
        }
        float ly = o.y + 138;
        for (const std::string& line : wrap(*f, "Adds thistle.json, CMakeLists.txt and a starter src/main.cpp, like `thistle new` would (it runs `thistle init`). Nothing already there is changed.", 400, 12)) {
            label(line, {o.x + 196, ly}, theme::faint, 12);
            ly += 15;
        }
        if (!tools) label("Needs Python 3 and the thistle CLI.", {o.x + 196, ly + 4}, theme::bad, 12);
        if (button("Cancel", Rect{o + vec2{448, 96}, {148, 34}}) || (!was_typing && f->key_pressed(Key::Escape))) mode = Mode::Home;
    }

    void draw_inside_project() {
        const Rect box = dialog({620, 190});
        const vec2 o = box.pos;
        const ProjectInfo info = read_info(pending_root);
        label("This folder is inside a Thistle project", o + vec2{24, 20}, theme::text, 19);
        label(pretty_path(pending_folder) + " is part of \"" + info.name + "\",", o + vec2{24, 56}, theme::dim, 14);
        label("which is " + pretty_path(pending_root) + ".", o + vec2{24, 78}, theme::dim, 14);
        if (button("Open \"" + info.name + "\"", Rect{o + vec2{24, 124}, {260, 36}}, true)) {
            result = OpenRequest{pending_root, "", false};
            mode = Mode::Home;
        }
        if (button("Cancel", Rect{o + vec2{296, 124}, {120, 36}}) || f->key_pressed(Key::Escape)) mode = Mode::Home;
    }

    void draw_busy() {
        const Rect box = dialog({620, 230});
        const vec2 o = box.pos;
        const int dots = static_cast<int>(seconds() * 3.0) % 4;
        label(busy_title + std::string(static_cast<size_t>(dots), '.'), o + vec2{24, 20}, theme::text, 19);
        const size_t from = busy_output.size() > 8 ? busy_output.size() - 8 : 0;
        for (size_t i = from; i < busy_output.size(); ++i) label(fit(busy_output[i], box.size.x - 48, 12), o + vec2{24, 60 + (i - from) * 18.0f}, theme::faint, 12);
    }

    void draw_error() {
        const size_t n = std::min<size_t>(error_lines.size(), 14);
        const Rect box = dialog({700, 130 + n * 18.0f});
        const vec2 o = box.pos;
        label(error_title, o + vec2{24, 20}, theme::bad, 19);
        const size_t from = error_lines.size() - n;
        float ly = o.y + 58;
        for (size_t i = from; i < error_lines.size(); ++i) {
            label(fit(error_lines[i], box.size.x - 48, 12), {o.x + 24, ly}, theme::dim, 12);
            ly += 18;
        }
        if (button("OK", Rect{{o.x + 24, box.pos.y + box.size.y - 50}, {110, 34}}) || f->key_pressed(Key::Escape) || f->key_pressed(Key::Enter)) mode = Mode::Home;
    }

    std::optional<OpenRequest> update(Frame& frame) {
        f = &frame;
        m = frame.mouse();
        click = frame.mouse_pressed(Mouse::Left);
        active_field_drawn = false;
        was_typing = !active_field.empty();

        // A command finishing.
        if (busy) {
            for (std::string& line : busy->take_lines()) busy_output.push_back(std::move(line));
            if (busy->finished()) {
                const int code = busy->exit_code();
                busy.reset();
                auto done = std::move(busy_done);
                busy_done = nullptr;
                if (done) done(code);
            }
        }
        // A folder (or a file in one) dropped on the window.
        if (mode == Mode::Home) {
            for (const std::string& dropped : frame.dropped_files()) {
                std::error_code ec;
                const fs::path p(dropped);
                if (fs::is_directory(p, ec)) {
                    request_open(p);
                } else {
                    const fs::path root = project_root_of(p.parent_path());
                    request_open(root.empty() ? p.parent_path() : root);
                }
                break;
            }
        }

        // What's under a dialog is drawn too, but doesn't react.
        inert = mode != Mode::Home;
        const Mode under = mode;
        draw_home();
        inert = false;
        switch (under) {
            case Mode::NewProject: draw_new_project(); break;
            case Mode::Browse: draw_browser(); break;
            case Mode::NotThistle: draw_not_thistle(); break;
            case Mode::InsideProject: draw_inside_project(); break;
            case Mode::Busy: draw_busy(); break;
            case Mode::Error: draw_error(); break;
            default: break;
        }
        // A field that wasn't drawn this frame (its dialog closed) stops taking typing.
        if (!active_field.empty() && !active_field_drawn) {
            end_text_input();
            active_field.clear();
        }
        std::optional<OpenRequest> out;
        out.swap(result);
        return out;
    }
};

Hub::Hub() : impl_(std::make_unique<Impl>()) { impl_->load_recents(); }
Hub::~Hub() = default;
std::optional<OpenRequest> Hub::update(Frame& f) { return impl_->update(f); }
void Hub::request_open(const fs::path& folder) { impl_->request_open(folder); }
void Hub::remember(const fs::path& folder, const std::string& scene) { impl_->remember(folder, scene); }
void Hub::say(const std::string& message) { impl_->say(message); }
std::string Hub::last_scene(const fs::path& folder) const {
    const fs::path p = clean_path(folder);
    for (const auto& r : impl_->recents) {
        if (fs::path(r.path) == p) return r.scene;
    }
    return "";
}

// ------------------------------------------------------------------ BuildRunner

bool BuildRunner::start(const fs::path& project) {
    stop();
    process_.reset();
    log_.clear();
    progress_ = -1.0f;
    last_line_.clear();
    project_ = project;
    name_ = read_info(project).name;
    const auto& py = python_command();
    const fs::path script = cli_script();
    if (py.empty() || script.empty()) {
        state_ = State::Failed;
        headline_ = py.empty() ? "Build & run needs Python 3, which wasn't found" : "Build & run needs the engine's thistle CLI, which wasn't found";
        finished_at_ = seconds();
        return false;
    }
    std::vector<std::string> args = py;
    args.push_back(script.string());
    args.push_back("run");
    process_ = std::make_unique<Process>();
    state_ = State::Building;
    headline_ = "Building " + name_;
    if (!process_->start(args, project)) {
        update();
        state_ = State::Failed;
        headline_ = "Couldn't start the build";
        finished_at_ = seconds();
        return false;
    }
    return true;
}

double BuildRunner::since_finished() const { return seconds() - finished_at_; }

void BuildRunner::stop() {
    if (process_ && process_->running()) process_->stop();
}

void BuildRunner::dismiss() {
    if (state_ == State::Building || state_ == State::Running) return;
    state_ = State::Idle;
}

void BuildRunner::update() {
    if (!process_) return;
    for (std::string& line : process_->take_lines()) {
        if (state_ == State::Building) {
            // "[ 45%] Building ..." (Make), "[12/85] Building ..." (Ninja).
            int a = 0, b = 0;
            if (std::sscanf(line.c_str(), "[ %d%%]", &a) == 1) progress_ = a / 100.0f;
            else if (std::sscanf(line.c_str(), "[%d/%d]", &a, &b) == 2 && b > 0) progress_ = static_cast<float>(a) / b;
            // `thistle run` prints the game's path just before starting it.
            if (line.rfind("$ ", 0) == 0 && line.rfind("$ cmake", 0) != 0) {
                state_ = State::Running;
                headline_ = name_ + " is running";
                progress_ = 1.0f;
            }
        }
        const std::string l = lower(line);
        if (state_ == State::Building && first_error_.empty() && (l.find("error") != std::string::npos)) first_error_ = line;
        last_line_ = line;
        log_.push_back(std::move(line));
    }
    if (log_.size() > 4000) log_.erase(log_.begin(), log_.begin() + 2000);
    if (process_->finished() && (state_ == State::Building || state_ == State::Running)) {
        const int code = process_->exit_code();
        const bool ran = state_ == State::Running;
        if (code == -1) {
            state_ = State::Stopped;
            headline_ = ran ? name_ + " was stopped" : "Build stopped";
        } else if (ran || code == 0) {
            // (0 without the game's "$ " line: `thistle run` only succeeds
            // after running the game, so it ran; the line got lost. An older
            // CLI lost it into a pipe, and this said the build had failed.)
            state_ = code == 0 ? State::Finished : State::Failed;
            headline_ = code == 0 ? name_ + " closed" : name_ + " stopped with an error (exit code " + std::to_string(code) + ")";
        } else {
            state_ = State::Failed;
            headline_ = "The build failed";
            if (!first_error_.empty()) last_line_ = first_error_;
        }
        first_error_.clear();
        finished_at_ = seconds();
    }
}

} // namespace editor
