#include "platform.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace editor {

fs::path executable_dir() {
#if defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    char resolved[PATH_MAX];
    if (_NSGetExecutablePath(path, &size) == 0 && realpath(path, resolved)) return fs::path(resolved).parent_path();
#elif defined(_WIN32)
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0) return fs::path(path).parent_path();
#elif defined(__linux__)
    char path[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        return fs::path(path).parent_path();
    }
#endif
    return fs::current_path();
}

fs::path engine_dir() {
    static const fs::path dir = [] {
        std::error_code ec;
        auto has_cli = [&](const fs::path& d) { return fs::exists(d / "tools" / "thistle-cli" / "thistle.py", ec); };
#ifdef THISTLE_ENGINE_DIR
        // Where CMake found the engine when the editor was built.
        if (has_cli(THISTLE_ENGINE_DIR)) return fs::path(THISTLE_ENGINE_DIR).lexically_normal();
#endif
        // Else up from the executable (it's built inside the engine checkout).
        for (fs::path d = executable_dir(); !d.empty(); d = d.parent_path()) {
            if (has_cli(d)) return d;
            if (d == d.parent_path()) break;
        }
        return fs::path{};
    }();
    return dir;
}

fs::path cli_script() {
    const fs::path e = engine_dir();
    return e.empty() ? fs::path{} : e / "tools" / "thistle-cli" / "thistle.py";
}

const std::vector<std::string>& python_command() {
    static const std::vector<std::string> cmd = [] {
#if defined(_WIN32)
        const std::vector<std::vector<std::string>> candidates = {{"py", "-3"}, {"python"}, {"python3"}};
#else
        const std::vector<std::vector<std::string>> candidates = {{"python3"}, {"python"}};
#endif
        for (const auto& c : candidates) {
            std::vector<std::string> args = c;
            args.push_back("--version");
            std::vector<std::string> out;
            // Windows' "python" may be the Microsoft Store's stand-in, which
            // doesn't print a version: only a real "Python 3.x" counts.
            if (run_and_wait(args, {}, out, 10.0) == 0) {
                for (const std::string& line : out) {
                    if (line.rfind("Python 3", 0) == 0) return c;
                }
            }
        }
        return std::vector<std::string>{};
    }();
    return cmd;
}

fs::path home_dir() {
#if defined(_WIN32)
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h && *h ? fs::path(h) : fs::current_path();
}

std::string pretty_path(const fs::path& p) {
#if defined(_WIN32)
    return p.string();
#else
    const std::string home = home_dir().string(), s = p.string();
    if (!home.empty() && s.rfind(home, 0) == 0 && (s.size() == home.size() || s[home.size()] == '/')) return "~" + s.substr(home.size());
    return s;
#endif
}

std::vector<fs::path> filesystem_roots() {
#if defined(_WIN32)
    std::vector<fs::path> roots;
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (drives & (1u << i)) roots.push_back(fs::path(std::string(1, static_cast<char>('A' + i)) + ":\\"));
    }
    return roots;
#else
    return {fs::path("/")};
#endif
}

// --- Process -------------------------------------------------------------------

Process::~Process() {
    stop();
    if (reader_.joinable()) reader_.join();
#if defined(_WIN32)
    if (process_) CloseHandle(static_cast<HANDLE>(process_));
    if (job_) CloseHandle(static_cast<HANDLE>(job_));
#endif
}

std::vector<std::string> Process::take_lines() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(lines_);
    return out;
}

void Process::add_output(const char* data, size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < n; ++i) {
        const char c = data[i];
        // \r ends a line too: progress output rewrites one line with it.
        if (c == '\n' || c == '\r') {
            if (!partial_.empty()) lines_.push_back(std::move(partial_));
            partial_.clear();
        } else {
            partial_ += c;
        }
    }
    if (lines_.size() > 20000) lines_.erase(lines_.begin(), lines_.begin() + 10000); // nobody's reading: don't grow forever
}

#if defined(_WIN32)

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
// One argument, quoted the way the C runtime splits a command line back up.
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++backslashes; }
        if (i == a.size()) {
            r.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
        } else {
            r.append(backslashes, L'\\');
        }
        r.push_back(a[i]);
    }
    r.push_back(L'"');
    return r;
}
} // namespace

bool Process::start(const std::vector<std::string>& args, const fs::path& working_dir) {
    if (args.empty() || started_) return false;
    started_ = true;
    auto fail = [&](const std::string& why) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back("couldn't run " + args[0] + ": " + why);
        }
        exit_code_ = 127;
        finished_ = true;
        return false;
    };
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return fail("no pipe");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    std::wstring cmd;
    for (const std::string& a : args) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(widen(a));
    const std::wstring dir = working_dir.wstring();
    // Everything it starts goes in one job, so stopping it stops the lot (a
    // build's compilers, the game `thistle run` started), and so does the
    // editor closing.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                   nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    const DWORD err = GetLastError();
    CloseHandle(write_end);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read_end);
        if (job) CloseHandle(job);
        return fail(err == ERROR_FILE_NOT_FOUND ? "not found" : "error " + std::to_string(err));
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    job_ = job;
    out_read_ = read_end;
    reader_ = std::thread([this] { read_loop(); });
    return true;
}

void Process::read_loop() {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(static_cast<HANDLE>(out_read_), buf, sizeof(buf), &n, nullptr) && n > 0) add_output(buf, n);
    CloseHandle(static_cast<HANDLE>(out_read_));
    out_read_ = nullptr;
    // The output closes when everything writing to it has exited: a game
    // started by `thistle run` inherited it. (On Windows the CLI waits for
    // the game and exits with its exit code, see hand_over() in thistle.py.)
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    add_output("\n", 1);
    exit_code_ = stopping_ ? -1 : static_cast<int>(code);
    finished_ = true;
}

void Process::stop() {
    if (!running()) return;
    stopping_ = true;
    if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 1);
    else if (process_) TerminateProcess(static_cast<HANDLE>(process_), 1);
    if (reader_.joinable()) reader_.join();
}

#else // POSIX

bool Process::start(const std::vector<std::string>& args, const fs::path& working_dir) {
    if (args.empty() || started_) return false;
    started_ = true;
    auto fail = [&](const std::string& why) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back("couldn't run " + args[0] + ": " + why);
        }
        exit_code_ = 127;
        finished_ = true;
        return false;
    };
    int out[2], err[2];
    if (pipe(out) != 0) return fail(std::strerror(errno));
    // A second pipe tells us if exec failed: it closes by itself on success.
    if (pipe(err) != 0) {
        close(out[0]);
        close(out[1]);
        return fail(std::strerror(errno));
    }
    fcntl(err[1], F_SETFD, FD_CLOEXEC);
    // Everything the child needs is made before fork(): after it, only
    // async-signal-safe calls.
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (std::string& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    const std::string dir = working_dir.string();
    const pid_t pid = fork();
    if (pid < 0) {
        close(out[0]); close(out[1]); close(err[0]); close(err[1]);
        return fail(std::strerror(errno));
    }
    if (pid == 0) {
        setpgid(0, 0); // its own process group: stop() signals the whole group
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        for (int fd = 3; fd < 1024; ++fd) {
            if (fd != err[1]) close(fd); // the editor's own files, sockets and display connection
        }
        if (!dir.empty() && chdir(dir.c_str()) != 0) {
            const int e = errno;
            (void)!write(err[1], &e, sizeof(e));
            _exit(127);
        }
        execvp(argv[0], argv.data());
        const int e = errno;
        (void)!write(err[1], &e, sizeof(e));
        _exit(127);
    }
    setpgid(pid, pid); // from this side too: whichever runs first
    close(out[1]);
    close(err[1]);
    int child_errno = 0;
    ssize_t got;
    do { got = read(err[0], &child_errno, sizeof(child_errno)); } while (got < 0 && errno == EINTR);
    close(err[0]);
    if (got == static_cast<ssize_t>(sizeof(child_errno))) {
        waitpid(pid, nullptr, 0);
        close(out[0]);
        return fail(child_errno == ENOENT ? "not found" : std::strerror(child_errno));
    }
    pid_ = pid;
    out_read_ = out[0];
    reader_ = std::thread([this] { read_loop(); });
    return true;
}

void Process::read_loop() {
    char buf[4096];
    for (;;) {
        const ssize_t n = read(out_read_, buf, sizeof(buf));
        if (n > 0) add_output(buf, static_cast<size_t>(n));
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(out_read_);
    out_read_ = -1;
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    add_output("\n", 1);
    exit_code_ = stopping_ ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    finished_ = true;
}

void Process::stop() {
    if (!running()) return;
    stopping_ = true;
    kill(-pid_, SIGTERM);
    for (int i = 0; i < 150 && !finished_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!finished_) kill(-pid_, SIGKILL);
    if (reader_.joinable()) reader_.join();
}

#endif

int run_and_wait(const std::vector<std::string>& args, const fs::path& working_dir, std::vector<std::string>& output, double timeout_seconds) {
    Process p;
    const bool ok = p.start(args, working_dir);
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    while (ok && !p.finished() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool timed_out = ok && !p.finished();
    if (timed_out) p.stop();
    output = p.take_lines();
    return !ok || timed_out ? -1 : p.exit_code();
}

} // namespace editor
