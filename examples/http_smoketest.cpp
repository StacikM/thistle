// Checks Http on Linux (libcurl, loaded at run time) against a tiny HTTP
// server this test runs itself on a free local port: a GET's status and
// body, a POST's JSON body and Content-Type arriving, a 404 coming back as
// 404, a redirect being followed, and a refused connection ending as status
// 0 rather than hanging. Skips (ctest's SKIP_RETURN_CODE 77) on a machine
// without libcurl, where Http fails every request by design. No App, no
// window: Http doesn't need one.
#include <thistle.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace thistle;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", msg.c_str());
    if (!cond) ++g_failures;
}

std::string header_value(const std::string& req, const std::string& name) {
    const std::string key = "\r\n" + name + ": ";
    const auto at = req.find(key);
    if (at == std::string::npos) return "";
    const auto start = at + key.size();
    return req.substr(start, req.find("\r\n", start) - start);
}

void respond(int fd, const std::string& status, const std::string& body, const std::string& extra = "") {
    const std::string out = "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
                            "\r\nConnection: close\r\n" + extra + "\r\n" + body;
    (void)!write(fd, out.data(), out.size());
}

void serve_one(int fd) {
    std::string req;
    char buf[4096];
    for (;;) {
        const auto head_end = req.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const std::string len = header_value(req, "Content-Length");
            if (req.size() >= head_end + 4 + (len.empty() ? 0 : std::stoul(len))) break;
        }
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
    }
    const std::string first = req.substr(0, req.find("\r\n"));
    const std::string body = req.substr(req.find("\r\n\r\n") + 4);
    if (first.rfind("GET /hello ", 0) == 0) respond(fd, "200 OK", "hello from the test server");
    else if (first.rfind("POST /echo ", 0) == 0) respond(fd, "200 OK", header_value(req, "Content-Type") + "|" + body);
    else if (first.rfind("GET /redirect ", 0) == 0) respond(fd, "302 Found", "", "Location: /hello\r\n");
    else respond(fd, "404 Not Found", "nope");
    close(fd);
}

bool wait_done(Http& req) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!req.done()) {
        if (std::chrono::steady_clock::now() > give_up) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
} // namespace

int main() {
    if (!dlopen("libcurl.so.4", RTLD_NOW) && !dlopen("libcurl.so", RTLD_NOW)) {
        std::printf("libcurl isn't installed here; Http fails every request on such a machine, by design. Skipping.\n");
        return 77;
    }

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t addr_len = sizeof(addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 8) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        std::printf("couldn't open a local listening socket\n");
        return 1;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    std::atomic<bool> stop{false};
    std::thread server([&] {
        while (!stop) {
            const int fd = accept(listener, nullptr, nullptr);
            if (fd < 0) break;
            serve_one(fd);
        }
    });

    Http get = Http::get(base + "/hello");
    check(wait_done(get), "GET finishes");
    check(get.status() == 200 && get.ok(), "GET is 200 (got " + std::to_string(get.status()) + ")");
    check(get.body() == "hello from the test server", "GET body arrives whole");

    Http post = Http::post(base + "/echo", R"({"score":42})");
    check(wait_done(post), "POST finishes");
    check(post.body() == R"(application/json|{"score":42})", "POST sends the JSON with Content-Type application/json (got \"" + post.body() + "\")");

    Http missing = Http::get(base + "/missing");
    check(wait_done(missing), "404 finishes");
    check(missing.status() == 404 && !missing.ok(), "a 404 is status 404, not ok()");
    check(missing.body() == "nope", "a 404 still has its body");

    Http redirect = Http::get(base + "/redirect");
    check(wait_done(redirect), "redirect finishes");
    check(redirect.status() == 200 && redirect.body() == "hello from the test server", "a 302 is followed, like NSURLSession and WinHTTP do");

    stop = true;
    shutdown(listener, SHUT_RDWR);
    close(listener);
    server.join();

    Http refused = Http::get(base + "/hello");
    check(wait_done(refused), "a refused connection finishes instead of hanging");
    check(refused.status() == 0 && !refused.ok(), "a refused connection is status 0");

    Http unfinished = Http::get(base + "/hello");
    unfinished.reset();
    check(true, "resetting a request still in flight doesn't crash");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
