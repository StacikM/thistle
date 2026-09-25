// log_info/log_warn/log_error, shared by the full engine and the headless
// server library.
#include "thistle_core.h"

#include <cstdio>

namespace thistle {
namespace {
// A function's static, not a global: a log line can come from another file's
// static initializer, before this file's globals would exist.
std::vector<std::string>& logs() {
    static std::vector<std::string> lines;
    return lines;
}
void (*g_sink)(const std::string& line) = nullptr;

void push_log(const char* level, const std::string& msg) {
    std::string line = std::string(level) + msg;
    if (g_sink) g_sink(line);
    else std::fprintf(stderr, "%s\n", line.c_str());
    std::vector<std::string>& lines = logs();
    lines.push_back(std::move(line));
    if (lines.size() > 1000) lines.erase(lines.begin(), lines.begin() + 200);
}
} // namespace

void log_info(const std::string& msg)  { push_log("[info] ", msg); }
void log_warn(const std::string& msg)  { push_log("[warn] ", msg); }
void log_error(const std::string& msg) { push_log("[error] ", msg); }

namespace detail {
std::vector<std::string>& log_lines() { return logs(); }
void set_log_sink(void (*sink)(const std::string& line)) { g_sink = sink; }
} // namespace detail

} // namespace thistle
