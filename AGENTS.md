# Working on Thistle

Thistle is a small, cross-platform 2D game engine on [sokol](https://github.com/floooh/sokol). Almost all of it was written by Claude (see the README's "Built by AI, on purpose") — not as a disclaimer, but because it set the actual bar this project holds itself to: **every claim of "this works" is backed by something that was actually run**, not just compiled or reasoned about. That standard applies to you too, human or agent. Read this whole file before opening a PR.

## The one rule everything else follows

**"Compiles" and "works" are different words. Don't confuse them in a commit message, a doc, or a PR description.**

CI (`.github/workflows/ci.yml`) builds the engine and both example programs on real macOS, Windows, and Linux runners, and runs exactly one real functional test: `thistle_net_smoketest` (headless networking). That's the extent of what CI can prove — it has no GPU or display, so anything visual (rendering, gamepad input, the crash popup, post-processing shaders) still needs an actual human running the thing on actual hardware. Every doc in `docs/` says explicitly which of its claims are backed by real hardware and which are "compiles clean, not yet confirmed by a human" — keep that distinction sharp when you touch anything. If you can't test a platform yourself, say so in the PR and in the doc, in those words. Don't round "the code looks right" up to "it works," and don't use a translation layer (Wine, an emulator with known driver quirks) as a substitute for the real thing without saying that's what it is — `docs/building.md`'s Wine section and `docs/android.md`'s emulator section are both examples of exactly this happening and being written up honestly instead of papered over.

## Build & test

```bash
cmake -S . -B build -DTHISTLE_BUILD_SMOKETEST=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` currently runs `net_smoketest` (networking) and `scene_smoketest` (Node save/load round-trip) — both headless, both real assertions, not smoke in the "did it not crash" sense. `thistle_smoketest` (window + 2D/3D drawing + HTTP + IAP + post-processing) and `thistle_crash_smoketest` (deliberately crashes) are built by CI but not executed there — run those yourself when you touch anything they exercise. See `docs/building.md` for the full per-platform build story, including the specific MinGW/MSVC gotchas already found and fixed.

**If you add a feature that can be tested headlessly (no window, no GPU), add a real ctest for it** — `examples/net_smoketest.cpp` and `examples/scene_smoketest.cpp` are the pattern: construct real objects, make real assertions, `return 1` on failure. This is a much stronger signal than "I read the code and it looks right," and it's the difference between this project's test suite actually catching regressions versus just decorating the repo.

## Layout

- `include/thistle.hpp` — the entire public API. One header, on purpose.
- `src/thistle.cpp` — the engine. Most things live here; platform-specific code splits out only when it has to (Objective-C++, Windows headers, etc.).
- `src/ios_support.mm`, `src/win32_support.cpp`, `src/linux_gamepad.cpp` — platform glue that needs a different compiler mode or platform headers thistle.cpp can't include directly.
- `src/sokol_impl.c`, `src/*_impl.c` — single translation units that instantiate the header-only vendored libraries (sokol, stb, miniaudio, fontstash) and pick the graphics backend by platform. Don't touch backend selection without reading `docs/building.md`'s table first.
- `examples/` — smoketests, not sample games. Each one exists to prove something specific works; read the comment at the top of each file before assuming what it covers.
- `examples/android/` — the Android APK build, entirely Gradle-free (`build_apk.sh` uses `aapt`/`zipalign`/`apksigner` directly). See `docs/android.md`.
- `docs/` — one file per feature area, written for the person about to use it, not as a spec. If you change behavior, update the doc in the same PR — a doc that's wrong is worse than no doc.
- `tools/thistle-cli` — the `thistle` command-line tool for scaffolding/building/running a game project. See `docs/cli.md`.

## Conventions

- **No comments explaining what code does** — names should do that. A comment earns its place only by explaining a non-obvious *why*: a workaround for a specific platform bug, a constraint that isn't visible from the code itself, a decision that would look wrong without context. If you'd delete the comment and nothing would be lost, delete it.
- **Don't add abstraction ahead of a second real use case.** Three similar lines beat a premature helper. This engine has been kept small on purpose across every feature added to it — see how many docs end with a line like "don't reach for this by default."
- **Match the existing doc voice** if you're writing or editing `docs/`: direct, specific, willing to say "this doesn't work yet" or "this was a real bug, here's exactly what broke." Padding a doc with hedging or marketing language is a regression even if the code is fine.
- **New platform code needs the same honesty treatment as everything else**: if you add support for something and can only prove it compiles, say exactly that — don't imply it's verified because it built. The Android, Windows-gamepad, and Linux-gamepad docs are the current reference examples for how to phrase "implemented, verified by code/build only."
- **Commit messages explain why, briefly** — look at recent `git log` output for the actual tone/length this project uses. When Claude makes the commit, it ends with `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` (or whatever the current model is) — keep doing that for AI-authored commits, don't drop it partway through a session.

## Before you claim a platform works

1. Actually build it — real toolchain, not a guess about what a real toolchain would do.
2. Actually run it, on the real platform if you can get access to one. An emulator/simulator is a reasonable first pass but say so explicitly if that's all you had — see `docs/android.md`'s emulator-vs-real-device section for exactly how much that distinction matters in practice (an emulator-only bug there turned out to plausibly be the emulator's own driver, not the engine — nobody will know that's even a live question unless you write it down).
3. Update the relevant `docs/` file with what you actually verified, in plain terms, in the same PR.

If you skip step 2, the PR description and the doc both need to say so. That's not optional — it's the entire reason anyone can trust a "works on Windows" claim in this repo at all.
