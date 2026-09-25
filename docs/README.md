# Thistle docs

You want to make a game, not read a novel. Here's the map:

- [building.md](building.md) — how this thing actually compiles and links. Read this first, it's not optional.
- [drawing.md](drawing.md) — putting pixels on the screen: shapes, sprites, text, camera, blend modes, post-processing, and the "minor 3D" mode.
- [ui-and-scenes.md](ui-and-scenes.md) — immediate-mode widgets, `Menu`, the `Node` scene graph, particles, tilemaps, `App::scene`, and the optional Dear ImGui debug UI.
- [input-and-audio.md](input-and-audio.md) — keyboard, mouse, touch, gamepad, sound, music, and 3D positional sound.
- [physics.md](physics.md) — the Box2D wrapper. 2D only. Read the caveats before you build something that needs them.
- [3d.md](3d.md) — the 3D engine (`thistle::three`): the `World`, cameras, models, materials, lights, shadows, sky and fog, instancing, particles, terrain, raycasts, and walking around with the built-in character controller. Start here for 3D, and read its "What's been verified" section: so far it's been run on a software renderer, not a GPU.
- [voxels.md](voxels.md) — block worlds (`three::VoxelWorld`): Minecraft- or Teardown-style blocks, textured or flat-colored, break and place, endless generated worlds, saving, MagicaVoxel import.
- [animation.md](animation.md) — skeletal animation: animated glTF characters, crossfades, attaching things to joints, and skeletons built in code.
- [scene3d.md](scene3d.md) — 3D levels made in the Thistle Editor (`three::Scene3D`): loading and drawing one, spawn points, triggers, properties, and making physics bodies from it.
- [physics3d.md](physics3d.md) — opt-in 3D rigid bodies on Jolt (`three::Physics3D`) and Teardown-style voxel destruction (`VoxelDestruction`): turning it on, colliders, triggers, what falls and why, and exactly what's been verified where.
- [platform-and-networking.md](platform-and-networking.md) — save data, HTTP, clipboard, haptics, text input, logging, platform queries.
- [dedicated-servers.md](dedicated-servers.md) — a headless server for your game: the engine without graphics (`thistle_server`, builds on a bare VPS), `DedicatedServer` with a console you type commands into (`kick 3`, `list`, your own), settings in `server.json`, a log, and a clean stop that saves. `thistle new --template multiplayer` has one working.
- [networking.md](networking.md) — realtime client/server (Mirror-flavored `NetVar`/Command/ClientRpc over TCP), multiplayer block worlds (`VoxelSync`) and smoothing remote movement. Read the scope section before you plan a game around it.
- [iap.md](iap.md) — in-app purchases via StoreKit (iOS/macOS only). Read the "what's actually been verified" section before you assume a purchase has ever succeeded.
- [cli.md](cli.md) — the `thistle` CLI: scaffold a project (blank, or a playable fps, third-person, voxel or multiplayer game), build/run it (and its dedicated server), manage its version, turn optional modules on, run the editor. No dependencies to install, on purpose.
- [crash-handler.md](crash-handler.md) — local-only crash reporting: a report file plus a native popup, automatically, on every crash. No telemetry, ever.
- [android.md](android.md) — building, packaging (no Gradle needed), and what is and isn't verified on Android yet. Read the "what's not verified" section before you assume rendering works.

## What this actually is

A small immediate-mode game engine, 2D and 3D, glued together from other people's good work: [sokol](https://github.com/floooh/sokol) for the window and GPU, Box2D for 2D physics, Jolt for 3D physics (opt-in), cgltf for glTF models, miniaudio for sound, fontstash for text, Dear ImGui for debug windows (opt-in), nlohmann/json if you need it. It runs on iOS, macOS, Windows, Linux, and in principle the web (untested — nobody's tried it). Android builds, links, and packages into a real APK, but rendering isn't verified working yet — see [android.md](android.md).

In 2D it draws rectangles, circles, sprites, and text through one function call each, every frame, and gets out of your way. In 3D (`thistle::three`, see [3d.md](3d.md)) it's the same idea: draw models, shapes, block worlds and lights every frame, then `render()`. There's a level editor for the 3D side ([tools/thistle-editor](../tools/thistle-editor/README.md)), and `thistle new --template fps|third-person|voxel|multiplayer` gives you a playable game to start from (the last one with a dedicated server, see [dedicated-servers.md](dedicated-servers.md)).

It is not Unreal. It is not even close to Unreal. The lighting is classic (a sun with shadows, point and spot lights, sky-colored ambient), not physically-based: no normal maps, no reflections, no global illumination, no material editor. The level editor places things and paints blocks; it doesn't script, animate or build a game for you. And the 3D renderer has been run on Windows with a real GPU (D3D11) and on a software renderer under Linux, but not yet on a Mac (Metal), a phone or the web — [3d.md](3d.md#whats-been-verified) says exactly what's been checked where. Know all that before you invest a week in it.

## The one idea that matters

Everything happens inside one callback:

```cpp
#include <thistle.hpp>
using namespace thistle;

int main() {
    App app{{.title = "My Game", .width = 1280, .height = 720}};

    app.update([&](Frame f) {
        f.clear(rgb(0.05f, 0.05f, 0.08f));
        f.rect({100, 100}, {200, 80}, rgb(0.3f, 0.6f, 0.9f));
        f.text("hello", {110, 120}, {.size = 32, .color = white});
    });

    return app.run();
}
```

`app.update()` registers a lambda that runs once per frame. `Frame f` is your entire interface to the world that frame — input, drawing, time. There is no separate render pass, no command buffer you manage, no "flush" call. You draw a rect, it's drawn. If you're used to retained-mode engines this will feel wrong for about ten minutes and then you'll stop thinking about it.

State lives in whatever you capture into the lambda (`[&]`), or in an `App::scene` if you want named states with enter/exit hooks. There's no hidden global game object. This is deliberate — look at [ui-and-scenes.md](ui-and-scenes.md) if you want structure instead of one giant lambda.

## How much of this was written by a human

Almost none of the code. This engine and the game built on top of it (Fling) were built by Claude (Anthropic's AI), directed and verified by one person at every step — every feature was tested on real hardware or screenshotted before being called done, not just typed and trusted. If a doc here contradicts the code, the code is right and this file is stale; open an issue against yourself and fix it.
