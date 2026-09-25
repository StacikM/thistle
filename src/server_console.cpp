// See server_console.h.
#include "server_console.h"

#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

namespace thistle::detail {

// Shared with the reading thread, which can outlive the console: it's
// detached, since it can't be woken out of a blocking read at exit.
struct TypedLines {
    std::mutex mutex;
    std::deque<std::string> lines;
};

struct ServerConsole::Impl {
    std::shared_ptr<TypedLines> typed = std::make_shared<TypedLines>();
    std::mutex print_mutex;
    bool reading = false;
};

ServerConsole::ServerConsole() : impl_(std::make_unique<Impl>()) {}
ServerConsole::~ServerConsole() { close(); }

void ServerConsole::open(std::vector<std::string>) {
    if (impl_->reading) return;
    impl_->reading = true;
    std::thread([typed = impl_->typed] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::lock_guard<std::mutex> lock(typed->mutex);
            typed->lines.push_back(line);
        }
    }).detach();
}

void ServerConsole::close() {}

void ServerConsole::print(const std::string& line) {
    std::lock_guard<std::mutex> lock(impl_->print_mutex);
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool ServerConsole::poll(std::string& line) {
    TypedLines& typed = *impl_->typed;
    std::lock_guard<std::mutex> lock(typed.mutex);
    if (typed.lines.empty()) return false;
    line = std::move(typed.lines.front());
    typed.lines.pop_front();
    return true;
}

} // namespace thistle::detail
