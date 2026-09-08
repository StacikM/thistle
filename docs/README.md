# Thistle docs

You want to make a game, not read a novel. Here's the map:

- [building.md](building.md) — how this thing actually compiles and links. Read this first, it's not optional.
- [drawing.md](drawing.md) — putting pixels on the screen: shapes, sprites, text, camera, blend modes, post-processing, and the "minor 3D" mode.
- [ui-and-scenes.md](ui-and-scenes.md) — immediate-mode widgets, `Menu`, the `Node` scene graph, particles, tilemaps, and `App::scene`.
- [input-and-audio.md](input-and-audio.md) — keyboard, mouse, touch, gamepad, sound, music.
- [physics.md](physics.md) — the Box2D wrapper. 2D only. Read the caveats before you build something that needs them.
- [platform-and-networking.md](platform-and-networking.md) — save data, HTTP, clipboard, haptics, text input, logging, platform queries.
- [networking.md](networking.md) — realtime client/server (Mirror-flavored `NetVar`/Command/ClientRpc over TCP). Read the scope section before you plan a game around it.
- [iap.md](iap.md) — in-app purchases via StoreKit (iOS/macOS only). Read the "what's actually been verified" section before you assume a purchase has ever succeeded.
- [cli.md](cli.md) — the `thistle` CLI: scaffold a project, build/run it, manage its version. No dependencies to install, on purpose.
- [crash-handler.md](crash-handler.md) — local-only crash reporting: a report file plus a native popup, automatically, on every crash. No telemetry, ever.
- [android.md](android.md) — building, packaging (no Gradle needed), and what is and isn't verified on Android yet. Read the "what's not verified" section before you assume rendering works.

## What this actually is

A small immediate-mode 2D game engine glued together from other people's good work: [sokol](https://github.com/floooh/sokol) for the window and GPU, Box2D for physics, miniaudio for sound, fontstash for text, nlohmann/json if you need it. It runs on iOS, macOS, Windows, Linux, and in principle the web (untested — nobody's tried it). Android builds, links, and packages into a real APK, but rendering isn't verified working yet — see [android.md](android.md).

It is not Unreal. It is not even close to Unreal. It draws rectangles, circles, sprites, and text through one function call each, every frame, and gets out of your way. If you want a scene editor, a material system, or physically-based rendering, this is the wrong engine and you should know that before you invest a week in it.

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
