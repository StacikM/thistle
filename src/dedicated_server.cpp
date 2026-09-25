// DedicatedServer: a headless server's loop, console, commands and settings.
// Only in thistle_server. See docs/dedicated-servers.md.
#include "thistle_core.h"
#include "server_console.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace fs = std::filesystem;

namespace thistle {
namespace {

using Clock = std::chrono::steady_clock;

// --- stopping from outside: Ctrl+C, SIGTERM, the console window closing ------
// The handlers only raise a flag; the loop sees it at the next tick and stops
// the normal way, running the game's stop function (its save). A second
// Ctrl+C while that runs ends the process at once, for a stop that hangs.
std::atomic<int> g_stop_requests{0};
std::atomic<bool> g_stopped{false};

#if defined(_WIN32)
BOOL WINAPI on_console_event(DWORD event) {
    if (g_stop_requests.fetch_add(1) >= 1 && (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)) {
        detail::restore_console();
        ExitProcess(130);
    }
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        // Windows ends the process as soon as this returns (and a few
        // seconds after the event regardless): wait here for the save.
        for (int i = 0; i < 450 && !g_stopped; ++i) Sleep(10);
    }
    return TRUE;
}
void install_stop_handlers() { SetConsoleCtrlHandler(on_console_event, TRUE); }
#else
extern "C" void on_stop_signal(int) {
    if (g_stop_requests.fetch_add(1) >= 1) {
        detail::restore_console();
        std::_Exit(130);
    }
}
void install_stop_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    // SIGHUP: the terminal (an SSH session) went away. Stop and save rather
    // than die mid-write.
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN); // a vanished client mid-send isn't a reason to exit
}
#endif

std::string two(int v) { return (v < 10 ? "0" : "") + std::to_string(v); }

// Only called with ServerLog's mutex held: std::localtime isn't thread-safe,
// and the thread-safe versions differ between compilers.
std::tm local_now() {
    const std::time_t t = std::time(nullptr);
    return *std::localtime(&t);
}

std::string duration_text(double seconds) {
    long s = static_cast<long>(seconds);
    const long d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    s %= 60;
    std::string out;
    if (d) out += std::to_string(d) + "d ";
    if (d || h) out += std::to_string(h) + "h ";
    if (d || h || m) out += std::to_string(m) + "m ";
    return out + std::to_string(s) + "s";
}

std::string ms_text(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f ms", seconds * 1000.0);
    return buf;
}

// Words, with "quoted words" kept together.
std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (*end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

struct Command {
    std::string help;
    std::function<void(const ServerCommand&)> fn;
};

// The console, and logs/server-<date>.log with everything it shows.
struct ServerLog {
    detail::ServerConsole console;
    std::mutex mutex;
    std::ofstream file;
    std::string day; // the day `file` is for

    void print(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        const std::tm now = local_now();
        const std::string line = "[" + two(now.tm_hour) + ":" + two(now.tm_min) + ":" + two(now.tm_sec) + "] " + text;
        console.print(line);
        write(now, line);
    }
    // Into the file only: what was typed, which the console already shows.
    void note(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        const std::tm now = local_now();
        write(now, "[" + two(now.tm_hour) + ":" + two(now.tm_min) + ":" + two(now.tm_sec) + "] " + text);
    }

private:
    void write(const std::tm& now, const std::string& line) {
        const std::string today = std::to_string(now.tm_year + 1900) + "-" + two(now.tm_mon + 1) + "-" + two(now.tm_mday);
        if (today != day) {
            file.close();
            std::error_code ec;
            fs::create_directories("logs", ec);
            file.open(fs::path("logs") / ("server-" + today + ".log"), std::ios::app);
            day = today;
        }
        if (file) {
            file << line << '\n';
            file.flush();
        }
    }
};

ServerLog* g_log = nullptr; // the running server's: for log lines and ServerCommand::reply

void server_log_sink(const std::string& line) {
    if (g_log) g_log->print(line);
    else std::fprintf(stderr, "%s\n", line.c_str());
}

} // namespace

struct DedicatedServer::Impl {
    ServerConfig config;
    fs::path config_path = "server.json";
    nlohmann::json file = nlohmann::json::object(); // server.json as read
    bool file_ok = true;                            // false: it's broken, don't overwrite it
    bool show_help = false;
    std::vector<std::string> startup_warnings; // logged once run() has the console up

    NetServer net;
    std::map<std::string, Command> commands;
    std::function<void()> on_start, on_stop;
    std::function<void(float)> on_update;
    std::function<void(int)> on_join, on_leave;

    ServerLog log;
    bool quitting = false;
    Clock::time_point started = Clock::now();
    bool running = false;

    // Tick timing, for `status`: the last ~10 seconds of ticks.
    std::vector<double> tick_times;
    size_t tick_next = 0;
    long long ticks = 0;

    void write_config() {
        if (!file_ok) return;
        std::ofstream out(config_path, std::ios::trunc);
        out << file.dump(2) << '\n';
    }
    void note_tick(double seconds) {
        const size_t window = static_cast<size_t>(std::max(1, config.tick_rate) * 10);
        if (tick_times.size() < window) tick_times.push_back(seconds);
        else tick_times[tick_next % window] = seconds;
        ++tick_next;
        ++ticks;
    }
};

// --- ServerCommand ----------------------------------------------------------------

const std::string& ServerCommand::arg(size_t i) const {
    static const std::string none;
    return i < args.size() ? args[i] : none;
}

int ServerCommand::integer(size_t i, int fallback) const {
    int v = 0;
    return parse_int(arg(i), v) ? v : fallback;
}

std::string ServerCommand::rest(size_t from) const {
    std::string out;
    for (size_t i = from; i < args.size(); ++i) out += (out.empty() ? "" : " ") + args[i];
    return out;
}

void ServerCommand::reply(const std::string& text) const {
    if (g_log) g_log->print(text);
    else std::printf("%s\n", text.c_str());
}

// --- DedicatedServer ----------------------------------------------------------------

DedicatedServer::DedicatedServer(ServerConfig defaults, int argc, char** argv) : impl_(std::make_unique<Impl>()) {
    Impl& im = *impl_;
    im.config = defaults;

    // Flags first, for --config; applied after server.json below.
    std::map<std::string, std::string> flags;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) != 0) continue; // the game's own
        std::string key = a.substr(2), value;
        const size_t eq = key.find('=');
        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else if (key != "help" && i + 1 < argc) {
            value = argv[++i];
        }
        if (key == "help") im.show_help = true;
        else if (key == "port" || key == "tick" || key == "max-players" || key == "password" || key == "name" || key == "config") flags[key] = value;
        else im.startup_warnings.push_back("--" + key + " isn't a server option; the server ignored it (--help lists them)");
    }
    if (flags.count("config")) im.config_path = flags["config"];

    // server.json: the defaults above, where it doesn't say otherwise. The
    // first run writes it, so whoever runs the server can see what to change.
    std::ifstream in(im.config_path);
    if (in) {
        try {
            im.file = nlohmann::json::parse(in);
            if (!im.file.is_object()) throw std::runtime_error("not a JSON object");
        } catch (const std::exception& e) {
            im.file_ok = false;
            im.file = nlohmann::json::object();
            im.startup_warnings.push_back(im.config_path.string() + " isn't valid JSON, so the server uses its defaults and leaves the file as it is: " + e.what());
        }
    }
    auto read_int = [&](const char* key, int& into) {
        if (!im.file.contains(key)) { im.file[key] = into; return; }
        if (im.file[key].is_number()) into = im.file[key].get<int>();
    };
    auto read_string = [&](const char* key, std::string& into) {
        if (!im.file.contains(key)) { im.file[key] = into; return; }
        if (im.file[key].is_string()) into = im.file[key].get<std::string>();
    };
    read_int("port", im.config.port);
    read_int("tick_rate", im.config.tick_rate);
    read_int("max_players", im.config.max_players);
    read_string("password", im.config.password);
    read_string("name", im.config.name);
    if (!in) im.write_config();

    for (const auto& [key, value] : flags) {
        int v = 0;
        if (key == "port" && parse_int(value, v)) im.config.port = v;
        else if (key == "tick" && parse_int(value, v)) im.config.tick_rate = v;
        else if (key == "max-players" && parse_int(value, v)) im.config.max_players = v;
        else if (key == "password") im.config.password = value;
        else if (key == "name") im.config.name = value;
        else if (key != "config") im.startup_warnings.push_back("--" + key + " needs a number, not \"" + value + "\"; ignored");
    }
    im.config.tick_rate = std::clamp(im.config.tick_rate, 1, 1000);

    // The built-in commands. Any of them can be replaced with command().
    command("help", "help: lists the commands", [this](const ServerCommand& c) {
        c.reply("Commands:");
        for (const auto& [name, cmd] : impl_->commands) c.reply("  " + (cmd.help.empty() ? name : cmd.help));
    });
    command("list", "list: lists the players", [this](const ServerCommand& c) {
        const std::vector<NetConnection> players = impl_->net.connections();
        const std::string limit = impl_->config.max_players > 0 ? " (at most " + std::to_string(impl_->config.max_players) + ")" : "";
        if (players.empty()) { c.reply("No players online" + limit); return; }
        c.reply(std::to_string(players.size()) + (players.size() == 1 ? " player" : " players") + " online" + limit + ":");
        for (const NetConnection& p : players) {
            c.reply("  " + std::to_string(p.id) + "  " + p.address + "  connected " + duration_text(p.seconds));
        }
    });
    command("kick", "kick <id> [reason]: disconnects a player (ids are in list)", [this](const ServerCommand& c) {
        int id = 0;
        if (!parse_int(c.arg(0), id)) { c.reply("Usage: kick <id> [reason]. The ids are in list."); return; }
        const std::string reason = c.rest(1);
        if (!impl_->net.kick(id, reason)) { c.reply("No player " + std::to_string(id) + " (see list)"); return; }
        c.reply("Kicked " + std::to_string(id) + (reason.empty() ? "" : " (" + reason + ")"));
    });
    command("stop", "stop: saves and stops the server", [this](const ServerCommand&) { quit(); });
    command("status", "status: uptime, players, and whether the server keeps up", [this](const ServerCommand& c) {
        Impl& s = *impl_;
        const double budget = 1.0 / s.config.tick_rate;
        double sum = 0.0, worst = 0.0;
        for (double t : s.tick_times) { sum += t; worst = std::max(worst, t); }
        const double avg = s.tick_times.empty() ? 0.0 : sum / static_cast<double>(s.tick_times.size());
        c.reply(s.config.name + ", up " + duration_text(uptime()) + ", port " + std::to_string(s.config.port));
        c.reply("Players: " + std::to_string(s.net.connection_count()) +
                (s.config.max_players > 0 ? " of " + std::to_string(s.config.max_players) : "") +
                (s.config.password.empty() ? "" : " (password set)"));
        c.reply("Ticks: " + std::to_string(s.config.tick_rate) + " a second; over the last " +
                std::to_string(s.tick_times.size()) + ", one took " + ms_text(avg) + " on average and " + ms_text(worst) +
                " at most, of the " + ms_text(budget) + " each has" + (worst > budget ? " (falling behind)" : ""));
    });
}

DedicatedServer::~DedicatedServer() {
    if (g_log == &impl_->log) {
        g_log = nullptr;
        detail::set_log_sink(nullptr);
    }
}

DedicatedServer& DedicatedServer::start(std::function<void()> fn) { impl_->on_start = std::move(fn); return *this; }
DedicatedServer& DedicatedServer::update(std::function<void(float)> fn) { impl_->on_update = std::move(fn); return *this; }
DedicatedServer& DedicatedServer::stop(std::function<void()> fn) { impl_->on_stop = std::move(fn); return *this; }
DedicatedServer& DedicatedServer::on_join(std::function<void(int)> fn) { impl_->on_join = std::move(fn); return *this; }
DedicatedServer& DedicatedServer::on_leave(std::function<void(int)> fn) { impl_->on_leave = std::move(fn); return *this; }

DedicatedServer& DedicatedServer::command(const std::string& name, const std::string& help, std::function<void(const ServerCommand&)> fn) {
    std::string key = name;
    if (!key.empty() && key[0] == '/') key.erase(0, 1);
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    impl_->commands[key] = {help, std::move(fn)};
    return *this;
}

void DedicatedServer::remove_command(const std::string& name) { impl_->commands.erase(name); }

void DedicatedServer::run_command(const std::string& text) {
    Impl& im = *impl_;
    ServerCommand c;
    c.line = text;
    std::vector<std::string> words = split_words(text);
    if (words.empty()) return;
    c.name = words[0];
    if (!c.name.empty() && c.name[0] == '/') c.name.erase(0, 1);
    for (char& ch : c.name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    c.args.assign(words.begin() + 1, words.end());
    im.log.note("> " + text); // the log file shows what was typed
    auto it = im.commands.find(c.name);
    if (it == im.commands.end()) {
        c.reply("Unknown command \"" + c.name + "\". Type help for the list.");
        return;
    }
    try {
        it->second.fn(c);
    } catch (const std::exception& e) {
        log_error("command \"" + c.name + "\" failed: " + e.what());
    }
}

nlohmann::json DedicatedServer::setting_json(const std::string& key, const nlohmann::json& fallback) {
    Impl& im = *impl_;
    if (im.file.contains(key)) {
        const nlohmann::json& v = im.file[key];
        const bool same = v.type() == fallback.type() || (v.is_number() && fallback.is_number());
        if (same) return v;
        log_warn(im.config_path.string() + ": \"" + key + "\" should be like " + fallback.dump() + "; using that");
        return fallback;
    }
    im.file[key] = fallback;
    im.write_config();
    return fallback;
}

NetServer& DedicatedServer::net() { return impl_->net; }
const ServerConfig& DedicatedServer::config() const { return impl_->config; }
double DedicatedServer::uptime() const { return std::chrono::duration<double>(Clock::now() - impl_->started).count(); }
void DedicatedServer::quit() { impl_->quitting = true; }

int DedicatedServer::run() {
    Impl& im = *impl_;
    if (im.show_help) {
        std::printf(
            "Options (they override server.json):\n"
            "  --port <n>          the port to listen on (now %d)\n"
            "  --tick <n>          updates a second (now %d)\n"
            "  --max-players <n>   0 = no limit (now %d)\n"
            "  --password <text>   players need it to join\n"
            "  --name <text>       the server's name\n"
            "  --config <file>     settings file instead of server.json\n",
            im.config.port, im.config.tick_rate, im.config.max_players);
        return 0;
    }

    g_log = &im.log;
    detail::set_log_sink(server_log_sink);
    // Save data (save::) goes next to server.json, where it's easy to find
    // and back up, not in the user's app data folder.
    std::error_code ec;
    detail::set_save_dir(fs::absolute(im.config_path, ec).parent_path().string());
    for (const std::string& w : im.startup_warnings) log_warn(w);

    im.net.max_players = im.config.max_players;
    im.net.password = im.config.password;
    im.net.on_connect = [&](int id) {
        std::string from;
        for (const NetConnection& c : im.net.connections()) if (c.id == id) from = c.address;
        log_info("player " + std::to_string(id) + " joined from " + from + " (" + std::to_string(im.net.connection_count()) + " online)");
        if (im.on_join) im.on_join(id);
    };
    im.net.on_disconnect = [&](int id) {
        log_info("player " + std::to_string(id) + " left (" + std::to_string(im.net.connection_count()) + " online)");
        if (im.on_leave) im.on_leave(id);
    };
    if (!im.net.listen(im.config.port)) {
        log_error("couldn't listen on port " + std::to_string(im.config.port) + ": is something else using it? (--port picks another)");
        g_log = nullptr;
        detail::set_log_sink(nullptr);
        return 1;
    }
    install_stop_handlers();
    log_info(im.config.name + " is listening on port " + std::to_string(im.config.port) + ", " + std::to_string(im.config.tick_rate) +
             " ticks a second" + (im.config.max_players > 0 ? ", at most " + std::to_string(im.config.max_players) + (im.config.max_players == 1 ? " player" : " players") : "") +
             (im.config.password.empty() ? "" : ", password required") + ". Type help for commands.");

    std::vector<std::string> names;
    for (const auto& [name, cmd] : im.commands) names.push_back(name);
    im.log.console.open(names);
    im.started = Clock::now();
    im.running = true;
    if (im.on_start) im.on_start();

    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / im.config.tick_rate));
    const float dt = 1.0f / static_cast<float>(im.config.tick_rate);
    auto next = Clock::now();
    while (!im.quitting) {
        std::string line;
        while (im.log.console.poll(line)) run_command(line);
        if (g_stop_requests > 0) im.quitting = true;
        if (im.quitting) break;

        const auto t0 = Clock::now();
        im.net.update();
        if (im.on_update) im.on_update(dt);
        im.note_tick(std::chrono::duration<double>(Clock::now() - t0).count());

        // Fixed ticks: a late one is made up by running the next ones back
        // to back; more than a second behind, the server skips ahead.
        next += period;
        const auto now = Clock::now();
        if (now - next > std::chrono::seconds(1)) {
            const long long behind = static_cast<long long>((now - next) / period);
            log_warn("can't keep up: " + std::to_string(behind) + " ticks behind, skipping them (status shows tick times)");
            next = now;
        }
        std::this_thread::sleep_until(next);
    }

    log_info("stopping");
    if (im.on_stop) im.on_stop();
    im.net.stop();
    log_info("stopped");
    im.running = false;
    g_stopped = true;
    im.log.console.close();
    detail::set_log_sink(nullptr);
    g_log = nullptr;
    return 0;
}

} // namespace thistle
