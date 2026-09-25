# Dedicated servers

A dedicated server is a program with no window that runs your game's rules for everyone: players connect to it, it decides what's true, and it keeps running when nobody's playing. Thistle has two pieces for that:

- **`thistle_server`**, the engine without a window, graphics or audio. A server program links it instead of `thistle`, so it builds and runs on a machine with no graphics libraries: a VPS, a Docker image, a Raspberry Pi in a cupboard.
- **`DedicatedServer`**, the loop around your code: a fixed tick, a console you can type commands into (`kick 3`, `list`, your own), settings in `server.json`, a log file, and a clean stop that runs your save code whether you typed `stop`, pressed Ctrl+C or the system shut it down.

The quickest way in is the template, which is a working game and its server:

```bash
thistle new mygame --template multiplayer
cd mygame
thistle run --server     # the server: leave it running
thistle run              # in another terminal: a player (start as many as you like)
```

The networking itself (synced fields, Commands, ClientRpcs) is [networking.md](networking.md). This page is about the server program around it.

## Writing one

A project gets a server by having `src/server.cpp`. The `CMakeLists.txt` from `thistle new` then builds it as `<name>-server`, on `thistle_server`. (A project made by an older `thistle new` doesn't have those lines; `thistle build --server` says which to add.)

```cpp
#include <thistle.hpp>
using namespace thistle;

int main(int argc, char** argv) {
    DedicatedServer server({.port = 47000, .tick_rate = 30, .name = "My server"}, argc, argv);
    const int seed = server.setting("world_seed", 1234); // your own settings, in server.json

    three::VoxelWorld world;
    three::VoxelSync sync(world);

    server.command("save", "save: saves the world now", [&](const ServerCommand& c) {
        world.save("world.dat");
        c.reply("Saved");
    });
    server.on_join([&](int id) { /* net_spawn() their player */ });
    server.on_leave([&](int id) { /* net_despawn() it */ });

    server.start([&] { world.load("world.dat"); sync.host(); });
    server.update([&](float dt) { sync.update(); });
    server.stop([&] { world.save("world.dat"); });
    return server.run(); // until `stop`, Ctrl+C or the system stops it
}
```

`run()` listens on the port, calls `start` once, then `update` every tick with `dt = 1 / tick_rate`, and returns 0 when it stops (1 if it couldn't open the port, with a message saying so). `server.net()` is the `NetServer` it runs, for `kick()`, `connections()` and the rest. Use `on_join`/`on_leave` rather than `net().on_connect`: the server logs joins and leaves through those itself.

`DedicatedServer` is only declared where `THISTLE_SERVER` is defined, which linking `thistle_server` does. Code shared by the game and its server (like the template's `src/shared.hpp`) can check `#if THISTLE_SERVER` for the parts that differ.

## What's in `thistle_server`, and what isn't

In: networking (`NetServer`, `NetClient`, `NetObject`), `save::`, logging, JSON, the maths, and the data side of the 3D engine: `VoxelWorld` and `VoxelSync`, `CollisionWorld` and `CharacterController`, `Scene3D` (loading levels, spawn points, triggers, `add_colliders`), `Terrain`, `load_model()` (as geometry: raycasts and collisions work; its textures come out as empty handles), and the `Box3D`/`Sphere3D` helpers.

Not in: anything that draws, plays sound or reads input (`App`, `Frame`, `World`, textures, sounds), 2D physics, `Physics3D`, HTTP, in-app purchases. Calling one of those from a server program is a link error, on purpose: a server that quietly did nothing when asked to draw would hide a mistake.

## Commands

Everything typed into the server's terminal is a command. These are built in:

| Command | What it does |
|---|---|
| `help` | lists the commands, with the help text each was given |
| `list` | the players: id, address, how long they've been on |
| `kick <id> [reason]` | disconnects a player; their game gets "Kicked: reason" (`NetClient::disconnect_reason()`) |
| `stop` | runs your `stop` function (your save) and exits |
| `status` | uptime, players, and how long ticks take, against how long they may |

`server.command(name, help, fn)` adds one, or replaces the one with that name, built-ins included: a game can have its own `kick` (with its own ban list, say) by registering one. `remove_command(name)` takes one away. A leading `/` is dropped and names are case-insensitive, so `/Kick 3` works too. `run_command(line)` runs a line as if it had been typed, from your own code.

What a handler gets:

```cpp
server.command("give", "give <id> <item> [count]", [&](const ServerCommand& c) {
    const int who = c.integer(0, -1);      // "3" -> 3; -1 if missing or not a number
    const std::string& item = c.arg(1);    // "" past the end
    const int count = c.integer(2, 1);
    c.reply("Gave " + std::to_string(count) + " " + item + " to " + std::to_string(who));
});
```

`c.args` has the words after the name, with `"quoted words"` kept together; `c.rest(1)` joins everything from the second word on (a kick's reason); `c.line` is the whole line. `c.reply()` prints on the console and into the log file.

## Settings: `server.json` and flags

A server starts with three layers of settings, each overriding the one before:

1. what the code passes (`ServerConfig`: `port`, `tick_rate`, `max_players`, `password`, `name`)
2. `server.json` next to the server, written with the code's values the first time it runs
3. command-line flags: `--port 47001`, `--tick 20`, `--max-players 8`, `--password hunter2`, `--name "Friday server"`, `--config other.json` (`--flag=value` works too, `--help` lists them)

Flags are for one run; they're never written back into `server.json`. Other arguments are left for your own code to read (a flag the server doesn't know gets a warning, since it's usually a typo).

`server.setting("key", fallback)` reads a setting of the game's own from `server.json`. The first time, the key isn't there: it's added with the fallback, so whoever runs the server finds it and can change it. The type comes from the fallback (`int`, `float`, `bool`, `std::string`, anything JSON-convertible); a value of the wrong type in the file is reported and the fallback used.

A `server.json` that isn't valid JSON is reported, the defaults are used, and the file is left alone, so a typo never costs anyone their settings.

## The console and the log

In a terminal (a console window, an SSH session), the line you're typing stays at the bottom while output scrolls above it: Left/Right, Home/End, Backspace/Delete, Ctrl+U and Ctrl+W, Up/Down for the last 200 commands, Tab to complete a command's name. What you typed stays in the scrollback as `> line`.

Anywhere else (a pipe, `docker logs`, systemd's journal) it's plain lines in and out, with no escape codes to garble the log. Commands still work there, one per line on stdin.

Everything the console shows, with timestamps, also goes to `logs/server-<date>.log` next to the server, along with every command typed. A new file starts each day.

## Stopping, and saving

`stop`, Ctrl+C, SIGTERM (what Docker and systemd send), SIGHUP (the terminal or SSH session went away) and closing the console window on Windows all stop the server the same way: the current tick finishes, your `stop` function runs, everyone is told "The server stopped", and the program exits. Put your saving there and it happens however the server ends.

If a stop hangs (a save stuck on a network drive), a second Ctrl+C ends the program at once, and puts the terminal back the way it was.

`save::` data goes into the server's own folder (`save.dat` next to `server.json`), not the user's app-data folder where a game's would go: that's where a server's admin looks for it and backs it up.

## Ticks

The server runs at a fixed `tick_rate`. A tick that runs late is made up by running the next ones back to back; more than a second behind, it skips ahead and logs "can't keep up". `status` shows the average and worst tick times over the last ten seconds against the time each tick has: that's the first thing to look at when players say it's laggy.

## Players: passwords, a full server, kicks

These are `NetServer` features, so they work in any server, dedicated or not ([networking.md](networking.md#joining-and-leaving)). `DedicatedServer` sets `max_players` and `password` from its settings. A player turned away or kicked sees why in their game: `NetClient::connect()` returns false and `disconnect_reason()` says "Wrong password" or "The server is full"; a kick or the server stopping ends the connection with the reason set before `on_disconnect` runs. The template's connect screen shows it in red.

## Building and running it

```bash
thistle build                  # the game and its server
thistle build --server         # only the server, in build-server/, with no graphics
thistle run --server           # build it and run it (from its folder)
thistle run --server -- --port 47001 --max-players 8
```

`thistle run --server` runs it from its build folder, like `thistle run` does the game: its `assets/` are copied there, and its `server.json`, `logs/` and save data are written there.

`--server` builds with `-DTHISTLE_SERVER_ONLY=ON`: only `thistle_server` and your server, none of the windowing, graphics or audio code, so it needs nothing beyond a C++20 compiler, CMake and git. On a VPS: install those, clone the engine and your game, `thistle build --server --release`, and run `build-server/<name>-server` (it's a plain program; its `assets/` folder has to be next to it).

In Docker, run it with `-it` to type into its console (`docker attach` gets you back to it). Without a terminal it still reads commands from stdin, one per line; `docker stop` sends SIGTERM, which saves and stops it like typing `stop`.

## What's been verified

- **Linux, for real:** everything on this page. A pseudo-terminal drove the console, checked with a terminal emulator (19 checks: Tab, history, editing, a line longer than the terminal, UTF-8, output arriving while you type, Ctrl+C). The terminal is restored after a normal stop and after a double Ctrl+C. SIGTERM stops a server within 150 ms of its save. The multiplayer template was played with a server and two players under Xvfb (software OpenGL, one machine): joining, moving, `list`, a kick with its reason, `stop`, a wrong password, a full server.
- **CI, every push:** `-DTHISTLE_SERVER_ONLY=ON` builds on macOS and Windows runners, and in a bare `ubuntu:24.04` container with only a compiler, CMake and git; the headless tests run against `thistle_server` there, `server_smoketest` included (a real `DedicatedServer`: settings, commands, a kick reaching a client, a full server, a password, the log file, a taken port). In the container, a check that nothing graphical or audio got linked. The full builds compile the multiplayer template's game and server too.
- **Windows, under Wine only:** the server built with MinGW ran under Wine: commands through a pipe, exiting while stdin stayed open, and in Wine's console window the keys (Tab, Backspace, Enter) and Ctrl+C running the save. Wine's console window prints the escape codes the input line uses instead of acting on them; a real Windows 10 console understands them, but that hasn't been seen yet. Not run on a real Windows machine.
- **macOS:** builds and its tests pass in CI; the console hasn't been used there.
- **Not tried:** a real network between machines (latency, packet loss: everything so far was one machine over loopback), a real VPS, Docker.
