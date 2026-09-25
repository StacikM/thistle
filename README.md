# Thistle

**Thistle is an AI-built game engine** — an explicit experiment in whether an AI can build something a beginner can actually pick up and use, not just something technically impressive. Existing engines in this space have a real reputation for the opposite: Cocos2d alone ships three incompatible codebases under one name (Cocos2d-x, Cocos2d-js, Cocos Creator), and its old project-generator CLI required Python 2 — dead since January 2020, and gone from macOS entirely. The bar this project is trying to clear isn't "does it render a triangle," it's "would a beginner's first hour with this be spent making something, or fighting the tooling."

A small, simple, cross-platform C++ game engine for 2D and 3D games, built on the [sokol](https://github.com/floooh/sokol) renderer, with Box2D and (opt-in) Jolt physics, miniaudio audio, fontstash text rendering, realtime networking, and in-app purchases — plus a `thistle` command-line tool for scaffolding, building, and running a project, and a 3D level editor. The entire public API is a single umbrella header, `thistle.hpp` — `#include` it and you have everything; the engine itself compiles to one `thistle` CMake target you link against (not literally header-only in the no-linking sense, since it's more than a handful of one-liner functions, but there's exactly one header to ever look at).

- **Graphics:** Metal (iOS/macOS), D3D11 (Windows), GLCore (Linux), GLES3 (Android, Web) — all through sokol_gfx/sokol_gl.
- **3D (`thistle::three`):** a lit renderer with models from glTF/GLB and OBJ, sun shadows, point and spot lights, sky and fog, instancing, particles, terrain, skeletal animation, block worlds (Minecraft- or Teardown-style, endless or not, MagicaVoxel import), 3D sound, and a built-in character controller. Only run on a software renderer so far, not on a GPU — see [docs/3d.md](docs/3d.md).
- **Physics:** Box2D for 2D; Jolt for 3D rigid bodies and Teardown-style voxel destruction (opt-in, [docs/physics3d.md](docs/physics3d.md)).
- **Debug UI:** Dear ImGui windows for tuning and stats (opt-in).
- **Audio:** miniaudio.
- **Text:** fontstash.
- **Networking:** async HTTP (NSURLSession/WinHTTP) plus a Mirror-flavored realtime client/server layer — synced fields, Commands, ClientRpcs — over TCP, and multiplayer block worlds on top of it.
- **In-app purchases:** StoreKit (iOS/macOS).
- **Minor 3D:** the older perspective drawing mode on `Frame` (`camera3d`, `cube`, `sphere3d`, `mesh3d`...) layered on the 2D renderer with one fixed light. It still works; new 3D games should use `thistle::three`.

See [docs/](docs/) for the real documentation — building, drawing, 3D, physics, networking, in-app purchases — written with the actual limitations spelled out, not glossed over.

## Built by AI, on purpose

Almost every line here (~99%) was written by Claude (Anthropic's AI), directed and verified at every step by a human who tested on real hardware, actually ran things instead of assuming they worked, and pushed back whenever something was hand-wavy. That's not a disclaimer tucked at the bottom of the page — it's the actual premise of the project: this is what an AI building real infrastructure looks like when it's held to a real "does it actually work" bar instead of "does it look plausible." Judge the code on that basis, and check `docs/` for the places this project is honest about where that bar hasn't been fully cleared yet.

## Quick start

```bash
curl -fsSL https://raw.githubusercontent.com/StacikM/thistle/main/install.sh | bash
thistle new mygame && cd mygame && thistle run
thistle new myshooter --template fps     # or third-person, or voxel: a 3D game to start from
```

On Windows, download [`install.bat`](install.bat) and run it locally (same defaults, no one-liner published yet).

Installs the engine and the `thistle` CLI (see [docs/cli.md](docs/cli.md)) — no dependencies to fight, standard library only.

## Building the engine directly

```
cmake -S . -B build
cmake --build build --target thistle
```

Requires a consumer project (game) that adds this as a subdirectory and links against the `thistle` target — see [docs/building.md](docs/building.md) if you're not going through the CLI. `-DTHISTLE_BUILD_SMOKETEST=ON` builds the example programs in `examples/` (the 2D smoketest, the 3D demos: `voxel_demo`, `forest_demo`, `terrain_demo`, `animation_demo`, `model_viewer` and more), the templates, and the headless tests `ctest` runs.

## Thistle Editor

A 3D level editor for `thistle::three`: place models, shapes, lights, trigger volumes and spawn points with move/rotate/scale gizmos, group them, give them properties your game reads, paint block objects (voxels) with brushes and a box tool, bring in models and MagicaVoxel files by dropping them on the window, set the sun, sky and fog, with undo for all of it. Levels save as `.scene.json` in your game's `assets/scenes/`, and the game loads them with `three::Scene3D` ([docs/scene3d.md](docs/scene3d.md)). It's drawn with the engine itself, not ImGui. See [tools/thistle-editor](tools/thistle-editor/README.md) for how to use it and exactly what's been tested.

```bash
thistle editor install   # build it and put `thistle-editor` on your PATH
thistle editor update    # pull the latest engine source, rebuild, reinstall
thistle editor run       # build (if needed) and run it on the project you're in, without installing
```

## Contributing

See [AGENTS.md](AGENTS.md) — repo layout, conventions, and the one rule that matters most here: don't claim something works unless you actually ran it.

## License

MIT — see [LICENSE](LICENSE).
