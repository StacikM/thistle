// The dedicated server's console: prints lines and hands back the lines typed
// in. Engine-internal (DedicatedServer uses it).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace thistle::detail {

class ServerConsole {
public:
    ServerConsole();
    ~ServerConsole();

    // Starts reading typed lines. `words` are offered by Tab (command names).
    void open(std::vector<std::string> words);
    void close();
    // One line of output. Safe from any thread.
    void print(const std::string& line);
    // The next typed line, if there's one waiting.
    bool poll(std::string& line);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace thistle::detail
