// save::, shared by the full engine and the headless server library.
#include "thistle_core.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif
#if defined(__APPLE__) && !defined(THISTLE_SERVER)
extern "C" const char* thistle_apple_writable_dir(void);
#endif
#if defined(__ANDROID__)
#include <android/native_activity.h>
extern "C" const void* sapp_android_get_native_activity(void);
#endif

namespace thistle {

// --- save --------------------------------------------------------------

namespace save {
namespace {
std::map<std::string, std::string> g_data;
bool g_loaded = false;
std::string (*g_name_source)() = nullptr;
std::string g_dir;

void make_one(const std::string& p) {
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

// Create a directory and all missing parents (ignores "already exists").
void make_dirs(const std::string& path) {
    std::string cur;
    for (char c : path) {
        cur += c;
        if ((c == '/' || c == '\\') && cur.size() > 1) make_one(cur);
    }
    make_one(path);
}

std::string base_dir() {
    if (!g_dir.empty()) {
        make_dirs(g_dir);
        return g_dir;
    }
    std::string base;
#if defined(__APPLE__) && !defined(THISTLE_SERVER)
    base = thistle_apple_writable_dir();
#elif defined(__APPLE__)
    // The server library has no Foundation to ask; this is the same folder.
    const char* home = std::getenv("HOME");
    base = std::string((home && *home) ? home : ".") + "/Library/Application Support/Thistle";
#elif defined(__ANDROID__)
    // The only path an Android app can actually write to; there's no HOME or
    // XDG env var here. Set on the activity before sokol_main() ever runs
    // (see ANativeActivity_onCreate in sokol_app.h), so this is safe anywhere.
    const auto* activity = static_cast<const ANativeActivity*>(sapp_android_get_native_activity());
    base = (activity && activity->internalDataPath) ? activity->internalDataPath : "/data/local/tmp";
#elif defined(_WIN32)
    const char* ad = std::getenv("APPDATA");
    base = (ad && *ad) ? ad : ".";
    base += "\\Thistle";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = (home && *home) ? home : ".";
        base += "/.local/share";
    }
    base += "/thistle";
#endif
    const std::string title = g_name_source ? g_name_source() : std::string("Thistle");
    std::string safe;
    for (char c : title) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    base += "/" + safe;
    make_dirs(base);
    return base;
}

std::string file_path() { return base_dir() + "/save.dat"; }

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o += (n == 'n') ? '\n' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(file_path());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_data[line.substr(0, eq)] = unescape(line.substr(eq + 1));
    }
}

void write() {
    std::ofstream out(file_path(), std::ios::trunc);
    for (const auto& [k, v] : g_data) out << k << '=' << escape(v) << '\n';
}
} // namespace

void set(const std::string& key, const std::string& value) {
    ensure_loaded();
    g_data[key] = value;
    write();
}
void set_int(const std::string& key, int value) { set(key, std::to_string(value)); }
void set_float(const std::string& key, float value) { set(key, std::to_string(value)); }

std::string get(const std::string& key, const std::string& fallback) {
    ensure_loaded();
    const auto it = g_data.find(key);
    return it == g_data.end() ? fallback : it->second;
}
int get_int(const std::string& key, int fallback) {
    ensure_loaded();
    const auto it = g_data.find(key);
    if (it == g_data.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}
float get_float(const std::string& key, float fallback) {
    ensure_loaded();
    const auto it = g_data.find(key);
    if (it == g_data.end()) return fallback;
    try { return std::stof(it->second); } catch (...) { return fallback; }
}
bool has(const std::string& key) { ensure_loaded(); return g_data.count(key) > 0; }
void remove(const std::string& key) { ensure_loaded(); g_data.erase(key); write(); }
void clear() { ensure_loaded(); g_data.clear(); write(); }
std::string path() { return file_path(); }
} // namespace save

namespace detail {
void set_save_name_source(std::string (*source)()) { save::g_name_source = source; }
void set_save_dir(const std::string& dir) {
    if (dir == save::g_dir) return;
    save::g_dir = dir;
    save::g_loaded = false; // read the file in the new place
    save::g_data.clear();
}
} // namespace detail

} // namespace thistle
