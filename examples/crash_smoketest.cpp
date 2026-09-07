// Deliberately crashes the process one of three ways, so the crash handler
// (installed automatically by the App constructor — no window/App::run()
// needed for that) can be verified for real: does a report file actually
// get written, with the reason, the attached context, and recent log lines
// all present. Usage: ./thistle_crash_smoketest [segv|abort|throw] [popup]
// The popup arg is opt-in on purpose — without it this stays fully silent
// (no dialog), for quick/repeated file-only verification.
#include <thistle.hpp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace thistle;

int main(int argc, char** argv) {
    App app{{.title = "CrashSmoketest", .width = 640, .height = 480}};
    set_crash_popup(argc > 2 && std::string(argv[2]) == "popup");

    log_info("crash_smoketest: this line should appear in the crash report's log tail");
    set_crash_context("test_key", "test_value");

    // Printed (and flushed) before crashing so a test harness can find the
    // exact directory without hardcoding per-platform save-path logic.
    std::printf("CRASH_DIR:%s\n", crash_log_dir().c_str());
    std::fflush(stdout);

    std::string mode = argc > 1 ? argv[1] : "segv";
    if (mode == "abort") {
        std::abort();
    } else if (mode == "throw") {
        throw std::runtime_error("deliberate test exception");
    } else {
        volatile int* p = nullptr;
        *p = 42;   // SIGSEGV
    }
    return 0;
}
