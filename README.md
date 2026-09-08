# Thistle

**Thistle is an AI-built game engine** — an explicit experiment in whether an AI can build something a beginner can actually pick up and use, not just something technically impressive. Existing engines in this space have a real reputation for the opposite: Cocos2d alone ships three incompatible codebases under one name (Cocos2d-x, Cocos2d-js, Cocos Creator), and its old project-generator CLI required Python 2 — dead since January 2020, and gone from macOS entirely. The bar this project is trying to clear isn't "does it render a triangle," it's "would a beginner's first hour with this be spent making something, or fighting the tooling."

A small, cross-platform 2D (and now minor-3D) game engine built on [sokol](https://github.com/floooh/sokol), with Box2D physics, miniaudio audio, fontstash text rendering, realtime networking, and in-app purchases.

- **Graphics:** Metal (iOS/macOS), D3D11 (Windows), GLCore (Linux), GLES3 (Android, Web) — all through sokol_gfx/sokol_gl.
- **Physics:** Box2D (2D only).
- **Audio:** miniaudio.
- **Text:** fontstash.
- **Networking:** async HTTP (NSURLSession/WinHTTP) plus a Mirror-flavored realtime client/server layer — synced fields, Commands, ClientRpcs — over TCP.
- **In-app purchases:** StoreKit (iOS/macOS).
- **Minor 3D:** a perspective/depth-tested drawing mode (`camera3d`, `cube`, `plane3d`, `line3d`) layered on top of the 2D renderer — flat-shaded primitives, not a full lit 3D pipeline.

See [docs/](docs/) for the real documentation — building, drawing, physics, networking, in-app purchases — written with the actual limitations spelled out, not glossed over.

## Built by AI, on purpose

Almost every line here (~99%) was written by Claude (Anthropic's AI), directed and verified at every step by a human who tested on real hardware, actually ran things instead of assuming they worked, and pushed back whenever something was hand-wavy. That's not a disclaimer tucked at the bottom of the page — it's the actual premise of the project: this is what an AI building real infrastructure looks like when it's held to a real "does it actually work" bar instead of "does it look plausible." Judge the code on that basis, and check `docs/` for the places this project is honest about where that bar hasn't been fully cleared yet.

## Building

```
cmake -S . -B build
cmake --build build --target thistle
```

Requires a consumer project (game) that adds this as a subdirectory and links against the `thistle` target. See `-DTHISTLE_BUILD_SMOKETEST=ON` for a minimal example executable (`examples/smoketest.cpp`) that opens a window and exercises 2D + minor-3D drawing and HTTP.

## License

MIT — see [LICENSE](LICENSE).
