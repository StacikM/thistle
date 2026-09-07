# The `thistle` CLI

`tools/thistle-cli/thistle.py` — one file, standard library only, no dependencies to install. That's not a minimalism flex, it's the actual point: the thing this tool exists to fix is exactly the problem old Cocos2d tooling had — its project generator required Python 2, dead since January 2020 and gone from macOS entirely. If Python 3 runs at all, this has everything it needs.

```bash
python3 tools/thistle-cli/thistle.py new mygame
cd ../mygame        # created next to the engine by default
python3 /path/to/thistle.py run
```

Put `thistle.py` on your `PATH` (or alias it to `thistle`) and it's just `thistle new`/`thistle build`/`thistle run` from anywhere.

## What `new` actually generates

A complete, ordinary CMake project — nothing about it depends on the CLI continuing to exist. If you never touch this tool again after scaffolding, `cmake -S . -B build && cmake --build build` still works, because that's literally what `thistle build` runs under the hood. The CLI is a convenience layer over a real project, not a black box you're locked into.

```
mygame/
  CMakeLists.txt     # wired to the engine via THISTLE_DIR, calls thistle_bundle_assets()
  thistle.json        # {"name": "mygame", "version": "1.0.0"}
  src/
    main.cpp           # starter: a moving circle + a rect, zero assets required
    version.hpp.in      # configure_file() template — becomes generated/version.hpp
  assets/               # empty; drop textures/audio/fonts here
  README.md
  .gitignore
```

The starter `main.cpp` deliberately draws shapes, not text — text needs a `.ttf` loaded via `load_font()` first, there's no built-in font, and a starter that silently renders a blank black window because nobody dropped a font in `assets/` yet is a real bug that shipped once during this feature's own testing (screenshotted, confirmed blank, fixed). The version instead shows up in the window title bar (`"mygame v1.0.0"`), which needs no font at all.

## Commands

- **`thistle new <name> [--at PATH]`** — scaffold a project. Default location is next to the engine (matching the existing convention every project in this repo already uses); `--at` overrides it.
- **`thistle build [--release]`** — configures on first run (creates `build/`), then just builds on every call after. Debug by default.
- **`thistle run [--release]`** — builds, then execs the resulting binary. Finds it under whatever your platform's generator actually produced (`.app` bundle on macOS, `Debug/`/`Release/` subfolder on Windows' multi-config generator, a plain binary on Linux) so you never have to know that path yourself.
- **`thistle version show`** / **`set X.Y.Z`** / **`bump major|minor|patch`** — reads/writes `thistle.json`. No network behavior of any kind — this is metadata, not an updater (see below).

## How the version actually gets into your code

`thistle.json` → `configure_file()` in `CMakeLists.txt` → `generated/version.hpp` → `#include <version.hpp>` → `GAME_VERSION` as a `const char*`. The generation happens at CMake *configure* time, not build time, which means editing `thistle.json` alone wouldn't normally be picked up by a plain `cmake --build` — this bit a real test run of this exact tool (bump to 1.1.1, rebuild, header still said 1.0.0). The fix is `set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "thistle.json")` in the generated `CMakeLists.txt`, which tells the build system to check that file's timestamp and reconfigure automatically before building if it changed. Already in the template; if you ever hand-write a similar "bake a value from a file into a header" pattern elsewhere, remember this line or you'll chase the same "why is it stale" confusion.

## `thistle_bundle_assets()` — the actual bug this tool's existence surfaced

Every generated project calls this once, in its `CMakeLists.txt`. It's not part of the CLI — it's a `function()` defined in the *engine's* top-level `CMakeLists.txt`, because before this existed, only the Apple half of "copy assets next to the built game" had ever been written anywhere in this project (Fling's own `CMakeLists.txt` has `MACOSX_PACKAGE_LOCATION` handling; Windows and Linux never got the equivalent). `thistle_bundle_assets(target assets_dir)` fixes that for good: Apple gets the existing bundle treatment, Windows/Linux get a post-build copy of the whole `assets/` directory next to the executable. Any project — including eventually Fling, if it's worth migrating — can call this one function instead of re-deriving per-platform bundling logic from scratch.

## What this deliberately doesn't do (yet)

No interactive prompts, no project templates for different game genres, no dependency/package management, no editor integration. It scaffolds one project shape and gets out of the way. If that shape stops fitting as more games get built on this engine, extend the templates — don't build a second, fancier tool before the first one has proven itself on a real project.
