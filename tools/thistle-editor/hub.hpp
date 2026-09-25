// The Thistle Editor's start screen: recent projects, New project (the GUI
// for `thistle new`), and opening a folder, which checks that it's a
// Thistle project first (a thistle.json in it) and asks when it isn't.
#pragma once

#include <thistle.hpp>

#include "platform.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace editor {

// What the hub hands the editor once a project has been picked.
struct OpenRequest {
    fs::path folder;
    std::string scene;          // a scene to open, relative to the folder ("" = the editor decides)
    bool build_and_run = false; // start Build & run once it's open
};

class Hub {
public:
    Hub();
    ~Hub();

    // Draws the whole window. Returns a project to open once one's picked.
    std::optional<OpenRequest> update(thistle::Frame& f);
    // Opens a folder the way picking it in the hub does: straight away if
    // it's a Thistle project, else it asks.
    void request_open(const fs::path& folder);
    // For the recent list: a project the editor has open, and its scene.
    void remember(const fs::path& folder, const std::string& scene);
    // The scene last open in a project ("" if none is remembered).
    std::string last_scene(const fs::path& folder) const;
    // Shown on the hub the next time it's drawn.
    void say(const std::string& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Build & run: `thistle run` in a project, as a child process, with what it
// prints turned into something to show: a stage, a percentage, a log.
class BuildRunner {
public:
    enum class State { Idle, Building, Running, Finished, Failed, Stopped };

    bool start(const fs::path& project); // false (and Failed, with why) if it can't
    void stop();
    void update(); // once a frame: reads what it printed
    State state() const { return state_; }
    bool active() const { return state_ != State::Idle; }
    void dismiss(); // back to Idle once finished
    float progress() const { return progress_; } // 0..1 while building, < 0 when unknown
    const std::string& headline() const { return headline_; }
    const std::string& last_line() const { return last_line_; }
    const std::vector<std::string>& log() const { return log_; }
    double since_finished() const; // seconds
    const fs::path& project() const { return project_; }

private:
    std::unique_ptr<Process> process_;
    State state_ = State::Idle;
    float progress_ = -1.0f;
    std::string headline_, last_line_, name_, first_error_;
    std::vector<std::string> log_;
    double finished_at_ = 0.0;
    fs::path project_;
};

} // namespace editor
