# Thistle

A small, cross-platform 2D (and now minor-3D) game engine built on [sokol](https://github.com/floooh/sokol), with Box2D physics, miniaudio audio, and fontstash text rendering.

- **Graphics:** Metal (iOS/macOS), D3D11 (Windows), GLCore (Linux), GLES3 (Web) — all through sokol_gfx/sokol_gl.
- **Physics:** Box2D (2D only).
- **Audio:** miniaudio.
- **Text:** fontstash.
- **Networking:** async HTTP via NSURLSession (Apple) / WinHTTP (Windows).
- **Minor 3D:** a perspective/depth-tested drawing mode (`camera3d`, `cube`, `plane3d`, `line3d`) layered on top of the 2D renderer — flat-shaded primitives, not a full lit 3D pipeline.

## AI disclosure

This engine was built almost entirely (~99%) by Claude (Anthropic's AI), working with a human (the repo owner) who reviewed, tested on real devices, and directed every change — nothing here shipped without human verification. If you're evaluating this code, treat it accordingly: it's been checked by a person, but it is AI-authored.

## Building

```
cmake -S . -B build
cmake --build build --target thistle
```

Requires a consumer project (game) that adds this as a subdirectory and links against the `thistle` target. See `-DTHISTLE_BUILD_SMOKETEST=ON` for a minimal example executable (`examples/smoketest.cpp`) that opens a window and exercises 2D + minor-3D drawing and HTTP.
