// The editor's operating-system helpers: where things are, and running the
// `thistle` CLI (new, init, run) as a child process whose output the editor
// shows. Kept out of main.cpp, which is the editor itself.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace editor {

namespace fs = std::filesystem;

// The folder the editor's executable is in (its UI font is next to it).
fs::path executable_dir();
// The engine checkout the editor was built from (tools/thistle-cli is in it).
fs::path engine_dir();
// tools/thistle-cli/thistle.py in that checkout; empty if it isn't there.
fs::path cli_script();
// How to start Python 3 here ({"python3"}, {"py", "-3"}, ...); empty if
// none was found. Looked for once, the first time it's asked.
const std::vector<std::string>& python_command();
fs::path home_dir();
// "~/games/myfps" for a path in the home folder, else the path as is.
std::string pretty_path(const fs::path& p);
// Windows: the drive letters that exist ("C:\\", "D:\\"). Elsewhere: "/".
std::vector<fs::path> filesystem_roots();

// A child process, with its output (stdout and stderr together) collected
// line by line on a background thread. Everything it starts is stopped
// with it: stop(), or the Process going away.
class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // False if it couldn't be started (the reason is in the first output line).
    bool start(const std::vector<std::string>& args, const fs::path& working_dir);
    bool running() const { return started_ && !finished_; }
    bool finished() const { return finished_; }
    int exit_code() const { return exit_code_; } // once finished(); -1 if it was stopped
    // Lines printed since the last call.
    std::vector<std::string> take_lines();
    void stop();

private:
    void read_loop();
    void add_output(const char* data, size_t n);

    std::atomic<bool> started_{false}, finished_{false}, stopping_{false};
    std::atomic<int> exit_code_{0};
    std::mutex mutex_;
    std::vector<std::string> lines_;
    std::string partial_;
    std::thread reader_;
#if defined(_WIN32)
    void* process_ = nullptr; // HANDLE
    void* job_ = nullptr;     // HANDLE: everything it starts, so stop() gets them all
    void* out_read_ = nullptr;
#else
    int pid_ = -1;
    int out_read_ = -1;
#endif
};

// Runs a command to the end (for quick things: `thistle new`, `init`) and
// returns its exit code, with its output in `output`. -1 if it couldn't
// start, or took longer than `timeout_seconds` (then it's stopped).
int run_and_wait(const std::vector<std::string>& args, const fs::path& working_dir, std::vector<std::string>& output,
                 double timeout_seconds = 60.0);

} // namespace editor
