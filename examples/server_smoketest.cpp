// DedicatedServer, headless (it links thistle_server): the tick loop,
// server.json and flags, commands and their arguments, built-ins, kicks
// reaching a client, max players, the password, save data and the log file.
// Runs in a temporary folder, since a server keeps server.json, logs/ and
// its save data where it runs.
//
// Built with -DTHISTLE_BUILD_SMOKETEST=ON, run by ctest as server_smoketest.
#include <thistle.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace thistle;

namespace {

int fails = 0;
void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "  ok " : "FAIL ", what.c_str());
    if (!cond) ++fails;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string log_text() {
    std::string all;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("logs", ec)) all += read_file(e.path());
    return all;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

int main() {
    const fs::path home = fs::current_path();
    const fs::path dir = fs::temp_directory_path() / "thistle_server_smoketest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    fs::current_path(dir);

    // Ports: free ones the system picks (a fixed port can be taken, even by an
    // earlier connection lingering in TIME_WAIT). A server listening on one
    // and stopped leaves it free for --port to use.
    auto free_port = [] {
        NetServer probe;
        return probe.listen(0) ? probe.port() : 0;
    };

    // --- a first run: flags, server.json, the loop, commands, a client -------
    {
        const std::string port = std::to_string(free_port());
        const char* args[] = {"server", "--port", port.c_str(), "--max-players", "1", "--mine", nullptr};
        DedicatedServer server({.port = 47200, .tick_rate = 50, .name = "Test server"}, 6, const_cast<char**>(args));
        check(fs::exists("server.json"), "the first run writes server.json");
        check(server.config().port == std::stoi(port) && server.config().max_players == 1, "--port and --max-players override");
        check(server.setting("world_seed", 99) == 99, "setting() gives its fallback the first time");
        const nlohmann::json written = nlohmann::json::parse(read_file("server.json"));
        check(written.value("port", 0) == 47200 && written.value("name", "") == "Test server" && written.value("world_seed", 0) == 99,
              "server.json holds the code's defaults (not the flags) and the game's own setting");

        int ticks = 0;
        float dt_seen = 0.0f;
        bool started = false, stopped = false;
        std::string echoed;
        std::vector<int> joined, left;
        NetClient a, b;
        server.command("echo", "echo <words>: for this test", [&](const ServerCommand& c) {
            echoed = c.name + "|" + c.arg(0) + "|" + c.arg(1) + "|" + std::to_string(c.integer(2, -1)) + "|" + c.rest(1) + "|" + c.arg(9);
            c.reply("echoed it");
        });
        server.on_join([&](int id) { joined.push_back(id); });
        server.on_leave([&](int id) { left.push_back(id); });
        server.start([&] {
            started = true;
            save::set("where", "next to server.json");
        });
        server.update([&](float dt) {
            ++ticks;
            dt_seen = dt;
            if (ticks == 2) {
                check(a.connect("127.0.0.1", server.net().port()), "a client joins");
                check(!b.connect("127.0.0.1", server.net().port()) && b.disconnect_reason() == "The server is full",
                      "a second one is turned away (--max-players 1)");
                server.run_command("/ECHO first \"two words\" 42");
                server.run_command("nope");
                server.run_command("list");
                server.run_command("status");
                server.run_command("kick 99");
                server.run_command("kick 1 spamming");
            }
            a.update();
            if (ticks == 20) server.quit();
        });
        server.stop([&] { stopped = true; });
        const auto t0 = std::chrono::steady_clock::now();
        const int code = server.run();
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        check(code == 0, "run() returns 0 after quit()");
        check(started && stopped, "the start and stop functions ran");
        check(ticks == 20, "update ran until quit()");
        check(dt_seen == 1.0f / 50.0f, "dt is 1 / tick_rate");
        check(took > 0.3 && took < 2.0, "ticks are paced: 20 at 50 a second took " + std::to_string(took) + " s");
        check(echoed == "echo|first|two words|42|two words 42|", "a command's name and arguments: quoted words together, integer(), rest()");
        check(!a.connected() && a.disconnect_reason() == "Kicked: spamming", "kick <id> <reason> reached the client");
        check(joined == std::vector<int>{1} && left == std::vector<int>{1}, "on_join and on_leave ran");
        check(has(read_file("save.dat"), "where=next to server.json"), "save:: data goes next to server.json");

        const std::string log = log_text();
        check(has(log, "> /ECHO first \"two words\" 42"), "the log file shows what was typed");
        check(has(log, "echoed it"), "... the replies");
        check(has(log, "Unknown command \"nope\""), "... an unknown command");
        check(has(log, "1 player online"), "... list");
        check(has(log, "Ticks: 50 a second"), "... status");
        check(has(log, "No player 99 (see list)"), "... kick of nobody");
        check(has(log, "Kicked 1 (spamming)"), "... the kick");
        check(has(log, "player 1 joined from 127.0.0.1:"), "... joins, with the address");
        check(has(log, "--mine isn't a server option"), "... and an unknown flag");
    }

    // --- a second run: an edited server.json, a password, a replaced built-in --
    {
        nlohmann::json edited = nlohmann::json::parse(read_file("server.json"));
        const int port = free_port();
        edited["port"] = port;
        edited["password"] = "hunter2";
        std::ofstream("server.json") << edited.dump(2);

        DedicatedServer server({.port = 47200, .tick_rate = 50}, 0, nullptr);
        check(server.config().port == port && server.config().password == "hunter2", "a later run reads server.json");
        check(server.setting("world_seed", 5) == 99, "setting() reads the game's value back");
        bool mine = false;
        server.command("kick", "kick: the game's own", [&](const ServerCommand&) { mine = true; });
        int ticks = 0;
        NetClient a;
        server.update([&](float) {
            if (++ticks == 2) {
                check(!a.connect("127.0.0.1", port, "wrong") && a.disconnect_reason() == "Wrong password", "a wrong password is refused");
                check(a.connect("127.0.0.1", port, "hunter2"), "the right one gets in");
                server.run_command("kick 1");
            }
            if (ticks == 5) server.quit();
        });
        check(server.run() == 0, "the second run stops cleanly");
        check(mine, "command() replaces a built-in");
    }

    // --- a broken server.json, and a port that's taken -----------------------
    {
        NetServer squatter;
        check(squatter.listen(0), "something else takes a port");
        const int taken = squatter.port();
        std::ofstream("server.json") << "{ not json";
        DedicatedServer server({.port = taken}, 0, nullptr);
        check(server.config().port == taken, "a broken server.json: the code's defaults are used");
        check(read_file("server.json") == "{ not json", "... and the file is left as it was");
        check(server.run() == 1, "run() returns 1 when it can't listen on its port");
        check(has(log_text(), "couldn't listen on port " + std::to_string(taken)), "... and says why");
    }

    fs::current_path(home);
    fs::remove_all(dir, ec);
    std::printf(fails == 0 ? "\nALL PASS\n" : "\n%d CHECK(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
