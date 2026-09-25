# The `thistle` CLI

`tools/thistle-cli/thistle.py` — one file, standard library only, no dependencies to install. That's not a minimalism flex, it's the actual point: the thing this tool exists to fix is exactly the problem old Cocos2d tooling had — its project generator required Python 2, dead since January 2020 and gone from macOS entirely. If Python 3 runs at all, this has everything it needs.

## Installing

The CLI itself needs only Python 3 and Git. Building a game needs **CMake** (3.20 or newer) and a **C++20 compiler**:

- **Windows:** Visual Studio 2022, or its free Build Tools, with the "Desktop development with C++" workload. CMake from https://cmake.org/download/ or `winget install Kitware.CMake`. Visual Studio also bundles a CMake, but it's only on the `PATH` inside "Developer PowerShell for VS 2022".
- **macOS:** Xcode's command line tools (`xcode-select --install`) and `brew install cmake`.
- **Linux:** GCC or Clang, CMake, and the X11/OpenGL headers (on Debian/Ubuntu: `libgl1-mesa-dev libx11-dev libxi-dev libxcursor-dev libxrandr-dev`).

Without CMake, `thistle build` and `thistle run` stop and say what to install.

```bash
curl -fsSL https://raw.githubusercontent.com/StacikM/thistle/main/install.sh | bash
```

Clones the engine to `~/.thistle` (override with `THISTLE_HOME`) and symlinks `thistle.py` as `thistle` on your `PATH` (`~/.local/bin` by default, override with `THISTLE_BIN_DIR`). Re-running it later just does a `git pull` — that's the whole update mechanism, there's no separate "upgrade" command. Verified for real: a clean clone via this exact script, through `thistle new` → `thistle build`, produces a real linked binary — see `install.sh`'s own comment for why there's no prebuilt package instead.

If you'd rather not run someone else's install script blind (fair), do the two steps it does by hand:

```bash
git clone https://github.com/StacikM/thistle.git ~/.thistle
ln -s ~/.thistle/tools/thistle-cli/thistle.py ~/.local/bin/thistle   # anywhere on your PATH works
chmod +x ~/.thistle/tools/thistle-cli/thistle.py
```

Then it's `thistle new`/`thistle build`/`thistle run` from anywhere.

### Windows

```bat
install.bat
```

Same defaults, same env var names (`THISTLE_HOME`, `THISTLE_BIN_DIR`), same idea — clone/pull to `%USERPROFILE%\.thistle`, then a `thistle` command on your `PATH` (`%USERPROFILE%\.local\bin` by default). It writes a one-line `thistle.bat` wrapper (`python "<engine>\tools\thistle-cli\thistle.py" %*`) instead of a symlink — creating a real symlink on Windows needs Developer Mode or an elevated prompt, and this needs neither. Requires `git` and `python` (or the `py` launcher) on `PATH`; there's no `curl | bash`-equivalent one-liner published yet, download `install.bat` from the repo and run it locally.

## What `new` actually generates

A complete, ordinary CMake project — nothing about it depends on the CLI continuing to exist. If you never touch this tool again after scaffolding, `cmake -S . -B build && cmake --build build` still works, because that's literally what `thistle build` runs under the hood. The CLI is a convenience layer over a real project, not a black box you're locked into.

```
mygame/
  CMakeLists.txt     # wired to the engine via THISTLE_DIR, calls thistle_bundle_assets()
  thistle.json        # {"name": "mygame", "version": "1.0.0"} (+ "modules" once you enable one)
  src/
    main.cpp           # starter: a moving circle + a rect, zero assets required
    version.hpp.in      # configure_file() template — becomes generated/version.hpp
  assets/               # empty; drop textures/audio/fonts here
  README.md
  .gitignore
```

The starter `main.cpp` deliberately draws shapes, not text — text needs a `.ttf` loaded via `load_font()` first, there's no built-in font, and a starter that silently renders a blank black window because nobody dropped a font in `assets/` yet is a real bug that shipped once during this feature's own testing (screenshotted, confirmed blank, fixed). The version instead shows up in the window title bar (`"mygame v1.0.0"`), which needs no font at all.

## Commands

- **`thistle new <name> [--at PATH] [--template KIND]`** — scaffold a project. Default location is next to the engine (matching the existing convention every project in this repo already uses); `--at` overrides it. `--template` picks what it starts as:
  - `blank` (the default): a window and two shapes, the smallest 2D start.
  - `fps`: first person. Walk a level made in the Thistle Editor, shoot the targets, reach the exit. Mouse look, sprint, jump, a HUD.
  - `third-person`: a character behind an orbiting camera that pulls in instead of going through walls. Collect coins, ride a jump pad, reach the flag. Falling in the sea respawns you.
  - `voxel`: an endless textured block world, generated as you walk. Break, place and build with a hotbar, fly, a day/night cycle. The changed chunks are saved between runs, and a house made as a block object in the editor is stamped in at the start.

  The three 3D templates come with their level in `assets/scenes/` (open it with `thistle editor run`), a UI font in `assets/fonts/` (Inter, OFL, license included), and a README section on what the code reads from the level. Each is one `src/main.cpp` of 200–350 lines, meant to be read and changed. Every template sets `AppConfig::save_name` to the project name, so the version in the window title can change without moving the save data (see [platform-and-networking.md](platform-and-networking.md)).
- **`thistle build [--release]`** — configures on first run (creates `build/`), then just builds on every call after. Debug by default.
- **`thistle run [--release]`** — builds, then execs the resulting binary. Finds it under whatever your platform's generator actually produced (`.app` bundle on macOS, `Debug/`/`Release/` subfolder on Windows' multi-config generator, a plain binary on Linux) so you never have to know that path yourself. It runs it from that folder, where the build copied `assets/` (the way a player's copy runs), so it works from anywhere inside the project, `src/` included.
- **`thistle version show`** / **`set X.Y.Z`** / **`bump major|minor|patch`** — reads/writes `thistle.json`. No network behavior of any kind — this is metadata, not an updater (see below).
- **`thistle modules`** / **`enable <module>`** / **`disable <module>`** — optional engine modules, big enough that a game has to ask for them. Right now: `physics3d` (Jolt rigid bodies and voxel destruction, see [physics3d.md](physics3d.md)) and `debug_ui` (Dear ImGui debug windows, see [ui-and-scenes.md](ui-and-scenes.md)). Switching writes `"modules": {...}` into `thistle.json`; the generated `CMakeLists.txt` reads it before adding the engine, and the `CONFIGURE_DEPENDS` line below makes the next build reconfigure by itself. Projects made by an older `thistle new` don't read modules — `enable` notices and prints the lines to add rather than editing your CMake for you.
- **`thistle editor install`** — builds `tools/thistle-editor` and puts a `thistle-editor` launcher on your `PATH` (same default/override as the `thistle` command itself: `~/.local/bin`, or `THISTLE_BIN_DIR`). A symlink on Unix, a one-line wrapper `.bat` on Windows (same reasoning as `install.bat` above).
- **`thistle editor update`** — `git pull --ff-only` on the engine checkout (the editor's source lives in the same repo, so this is the same update mechanism as the engine itself), then rebuilds and reinstalls.
- **`thistle editor run [project]`** — builds (if needed) and execs the editor directly, without touching your `PATH`. It edits the project folder you name, or else the project you're in (the nearest folder up with a `thistle.json`). The installed `thistle-editor` takes the same optional folder argument.

Unlike `new`/`build`/`run`/`version`, `thistle editor` doesn't need a `thistle.json` project in your current directory — it builds from the engine checkout itself, so it works from anywhere. Run outside any project, the editor works in its own folder.

## How the version actually gets into your code

`thistle.json` → `configure_file()` in `CMakeLists.txt` → `generated/version.hpp` → `#include <version.hpp>` → `GAME_VERSION` as a `const char*`. The generation happens at CMake *configure* time, not build time, which means editing `thistle.json` alone wouldn't normally be picked up by a plain `cmake --build` — this bit a real test run of this exact tool (bump to 1.1.1, rebuild, header still said 1.0.0). The fix is `set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "thistle.json")` in the generated `CMakeLists.txt`, which tells the build system to check that file's timestamp and reconfigure automatically before building if it changed. Already in the template; if you ever hand-write a similar "bake a value from a file into a header" pattern elsewhere, remember this line or you'll chase the same "why is it stale" confusion.

## `thistle_bundle_assets()` — the actual bug this tool's existence surfaced

Every generated project calls this once, in its `CMakeLists.txt`. It's not part of the CLI — it's a `function()` defined in the *engine's* top-level `CMakeLists.txt`, because before this existed, only the Apple half of "copy assets next to the built game" had ever been written anywhere in this project (Fling's own `CMakeLists.txt` has `MACOSX_PACKAGE_LOCATION` handling; Windows and Linux never got the equivalent). `thistle_bundle_assets(target assets_dir)` fixes that for good: Apple gets the existing bundle treatment, Windows/Linux get `assets/` copied next to the executable on every build. That used to be a post-build step, which only runs when the program is relinked: a level saved in the Thistle Editor never reached a game whose code hadn't changed. Found by editing a template's level in the editor and rebuilding; now any build copies what changed. On Apple, a file added to `assets/` after the project was configured (a level saved under a new name, a model the editor imported) makes the next build reconfigure and bundle it. Any project — including eventually Fling, if it's worth migrating — can call this one function instead of re-deriving per-platform bundling logic from scratch.

## What this deliberately doesn't do (yet)

No interactive prompts, no dependency/package management. It scaffolds a project (blank, or one of the three 3D game templates) and gets out of the way. If that shape stops fitting as more games get built on this engine, extend the templates — don't build a second, fancier tool before the first one has proven itself on a real project. (Editor integration exists now — `thistle editor install`/`update`/`run` — see above; that's the one thing this section used to list as deliberately missing.)
