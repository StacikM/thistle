// Async HTTP on Linux through libcurl, loaded at run time with dlopen rather
// than linked: building the engine needs no curl headers or -dev package,
// and a game on a machine without libcurl still runs (every request just
// fails, status 0, which is what Http did on Linux before this existed).
// Every desktop distro ships libcurl.so.4 (Arch and CachyOS as part of the
// base system; Debian and Ubuntu as libcurl4, which apt, git and most
// desktops already pull in).
//
// Same shape as win32_support.cpp's WinHTTP version: one detached thread per
// request, state shared with it through a shared_ptr so freeing the handle
// mid-request is safe. libcurl's API and the option numbers below are part
// of its stable ABI (unchanged since the SONAME became libcurl.so.4 in 2006),
// which is what makes declaring them here instead of including curl.h safe.
#include <atomic>
#include <cstddef>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" void* thistle_http_start(const char* method, const char* url, const char* body);
extern "C" int thistle_http_poll(void* h, int* status, const char** body, int* len);
extern "C" void thistle_http_free(void* h);

namespace {

constexpr int CURLOPT_TIMEOUT = 13;
constexpr int CURLOPT_FOLLOWLOCATION = 52;
constexpr int CURLOPT_POSTFIELDSIZE = 60;
constexpr int CURLOPT_MAXREDIRS = 68;
constexpr int CURLOPT_CONNECTTIMEOUT = 78;
constexpr int CURLOPT_NOSIGNAL = 99;
constexpr int CURLOPT_WRITEDATA = 10001;
constexpr int CURLOPT_URL = 10002;
constexpr int CURLOPT_POSTFIELDS = 10015;
constexpr int CURLOPT_USERAGENT = 10018;
constexpr int CURLOPT_HTTPHEADER = 10023;
constexpr int CURLOPT_CUSTOMREQUEST = 10036;
constexpr int CURLOPT_WRITEFUNCTION = 20011;
constexpr int CURLINFO_RESPONSE_CODE = 0x200002;
constexpr long CURL_GLOBAL_DEFAULT = 3;

struct Curl {
    int (*global_init)(long) = nullptr;
    void* (*easy_init)() = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    bool ok = false;
};

// curl_global_init isn't thread-safe, so it runs once, here, on whichever
// thread starts the first request — before that request's thread exists.
const Curl& curl() {
    static Curl c;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return;
        c.global_init = reinterpret_cast<int (*)(long)>(dlsym(lib, "curl_global_init"));
        c.easy_init = reinterpret_cast<void* (*)()>(dlsym(lib, "curl_easy_init"));
        c.easy_setopt = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(lib, "curl_easy_setopt"));
        c.easy_perform = reinterpret_cast<int (*)(void*)>(dlsym(lib, "curl_easy_perform"));
        c.easy_getinfo = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(lib, "curl_easy_getinfo"));
        c.easy_cleanup = reinterpret_cast<void (*)(void*)>(dlsym(lib, "curl_easy_cleanup"));
        c.slist_append = reinterpret_cast<void* (*)(void*, const char*)>(dlsym(lib, "curl_slist_append"));
        c.slist_free_all = reinterpret_cast<void (*)(void*)>(dlsym(lib, "curl_slist_free_all"));
        c.ok = c.global_init && c.easy_init && c.easy_setopt && c.easy_perform && c.easy_getinfo &&
               c.easy_cleanup && c.slist_append && c.slist_free_all && c.global_init(CURL_GLOBAL_DEFAULT) == 0;
    });
    return c;
}

struct ThistleHttp {
    std::atomic<bool> done{false};
    int status = 0;
    std::string body;
};
using HttpPtr = std::shared_ptr<ThistleHttp>;

size_t append_body(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

void run_request(const Curl& c, HttpPtr sp, std::string method, std::string url, std::string body) {
    void* h = c.easy_init();
    if (!h) { sp->done.store(true); return; }
    void* headers = nullptr;
    std::string out;
    c.easy_setopt(h, CURLOPT_URL, url.c_str());
    c.easy_setopt(h, CURLOPT_NOSIGNAL, 1L); // a timeout must not raise SIGALRM in a game's thread
    c.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    c.easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    c.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L); // NSURLSession and WinHTTP both follow redirects
    c.easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    c.easy_setopt(h, CURLOPT_USERAGENT, "Thistle/1.0");
    c.easy_setopt(h, CURLOPT_WRITEFUNCTION, &append_body);
    c.easy_setopt(h, CURLOPT_WRITEDATA, &out);
    if (!body.empty()) {
        headers = c.slist_append(nullptr, "Content-Type: application/json");
        c.easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        c.easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
        c.easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    if (method != "GET" && method != "POST") c.easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    else if (method == "POST" && body.empty()) {
        c.easy_setopt(h, CURLOPT_POSTFIELDS, "");
        c.easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
    }

    if (c.easy_perform(h) == 0) {
        long code = 0;
        c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        sp->status = static_cast<int>(code);
        sp->body = std::move(out);
    }
    if (headers) c.slist_free_all(headers);
    c.easy_cleanup(h);
    sp->done.store(true);
}

} // namespace

void* thistle_http_start(const char* method, const char* url, const char* body) {
    auto sp = std::make_shared<ThistleHttp>();
    const Curl& c = curl();
    if (!url || !c.ok) { sp->done.store(true); return new HttpPtr(sp); }
    std::thread(run_request, std::cref(c), sp, std::string(method && *method ? method : "GET"),
                std::string(url), std::string(body ? body : "")).detach();
    return new HttpPtr(sp);
}

int thistle_http_poll(void* handle, int* status, const char** body, int* len) {
    auto* spp = static_cast<HttpPtr*>(handle);
    if (!spp || !(*spp) || !(*spp)->done.load()) return 0;
    ThistleHttp& h = **spp;
    if (status) *status = h.status;
    if (body) *body = h.body.c_str();
    if (len) *len = static_cast<int>(h.body.size());
    return 1;
}

void thistle_http_free(void* handle) { delete static_cast<HttpPtr*>(handle); }
