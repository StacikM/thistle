# Building

## The engine is a library, not a program

There's no `main()` in this repo except the optional smoketest. Thistle is a CMake `add_subdirectory` target called `thistle`. Your game is its own CMake project that pulls it in:

```cmake
cmake_minimum_required(VERSION 3.20)
project(mygame LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../thistle ${CMAKE_BINARY_DIR}/thistle)   # path to this repo

add_executable(mygame src/main.cpp)
target_link_libraries(mygame PRIVATE thistle)
```

That's it. Don't go looking for an installed package, a `find_package(Thistle)`, prebuilt binaries, or a package manager entry. There isn't one. You vendor the source and build it every time, like every other sane C++ project that isn't shipping a stable ABI to strangers.

`#include <thistle.hpp>` (or the umbrella `<thistle>` header some game code uses) and `using namespace thistle;`. Yes, `using namespace` in a header-adjacent context is normally a bad idea. This is a small engine used by exactly the games that vendor it; the tradeoff is worth it. If your project disagrees, don't write the `using namespace` line, nobody's forcing you.

## Building it standalone

```bash
cmake -S . -B build
cmake --build build --target thistle
```

If you want to actually run something, turn on the smoketest — a minimal window + 2D rect + 3D cube + HTTP request, useful for checking a new platform actually works instead of just compiles:

```bash
cmake -S . -B build -DTHISTLE_BUILD_SMOKETEST=ON
cmake --build build --target thistle_smoketest
./build/thistle_smoketest        # or .exe, or the .app on macOS if you bundle it
```

Compiling is not the same as working. `D3D11CreateDeviceAndSwapChain` link errors, missing runtime DLLs, and silent no-op HTTP calls all compiled fine before someone actually ran the thing. Run the smoketest on every platform you claim to support before you believe you support it.

## Backend selection (automatic, don't touch it)

`src/sokol_impl.c` and `src/text_impl.c` pick the graphics backend by preprocessor check:

| Platform | Backend |
|---|---|
| Apple (macOS/iOS) | Metal |
| Windows | D3D11 |
| Emscripten | GLES3 (WebGL2) |
| everything else (Linux, etc.) | desktop GL |

You don't set this. It's `#if defined(__APPLE__)` / `elif _WIN32` / `elif __EMSCRIPTEN__` / `else`. If you need a different backend on a platform sokol supports, that's a `sokol_impl.c` change, not a CMake flag.

## Per-platform notes, i.e. the things that will actually bite you

**Apple.** `sokol_impl.c` and `audio_impl.c` get compiled as Objective-C (`-x objective-c`) because Metal and AVAudioSession need it — CMake does this for you when `APPLE` is true, don't remove it. `src/ios_support.mm` (Objective-C++) is only compiled on Apple and carries the platform glue: haptics, HTTP via NSURLSession, working-directory setup so bundled assets resolve, clipboard, device name. iOS vs. macOS is split inside that one file with `TARGET_OS_IPHONE`, not with separate files — don't go hunting for an `ios/` vs `macos/` split that doesn't exist.

**Windows.** `src/win32_support.cpp` carries the same HTTP glue via WinHTTP (see [platform-and-networking.md](platform-and-networking.md)). Two things that will genuinely trip you up, both already fixed here but worth knowing so you don't reintroduce them:

1. sokol's `#pragma comment(lib, "d3d11")` auto-linking is an MSVC trick. MinGW's linker ignores it silently and you get an undefined `D3D11CreateDeviceAndSwapChain` at link time with no other hint. `CMakeLists.txt` links `d3d11`/`dxgi` explicitly for exactly this reason. If you're on MSVC you don't need it — it's linked twice, which is harmless, so it stays for everyone.
2. fontstash's Windows file-open path uses `MAX_PATH` / `MultiByteToWideChar` / `CP_UTF8` without including `windows.h`. `text_impl.c` includes it under `#if defined(_WIN32)` before pulling in fontstash. If you see those three symbols undefined while touching text rendering on Windows, this is why.
3. On MSVC, `WIN32_EXECUTABLE` (i.e. `/SUBSYSTEM:WINDOWS`) makes the linker look for `WinMain` as the entry point. `sokol_impl.c` sets `SOKOL_NO_ENTRY` because we provide a plain `main()` ourselves, so the smoketest's `CMakeLists.txt` also sets `target_link_options(... "/ENTRY:mainCRTStartup")` on MSVC to point the linker at the CRT startup that actually calls `main()`. Without it you get a real link error, not a warning — found and fixed by actually building on real Windows (MinGW cross-compilation doesn't hit this, MSVC does).

If you're testing via MinGW and the exe won't launch with a DLL-not-found error, that's `libstdc++-6.dll` / `libgcc_s_seh-1.dll` — MinGW dynamically links its own runtime by default. Either ship those DLLs next to the exe or link `-static-libgcc -static-libstdc++`. This is a MinGW quirk, not an engine bug, and it doesn't exist on MSVC builds.

This whole list exists because someone actually ran the smoketest on a real Windows machine and hit real errors, one at a time, in order. That's the only way this list gets longer and more useful — guessing what Windows will do doesn't work, per the Wine warning above.

**Don't use Wine to "test" Windows builds.** It got far enough to open a window and complete a real TLS handshake over WinHTTP, then died with "none of the requested D3D feature levels is supported on this GPU" — Wine's D3D-over-OpenGL/Vulkan translation on macOS is not a substitute for real Windows and will produce failures that mean nothing. If you don't have a Windows machine, get one or find someone who does. Guessing from a translation layer is worse than not testing at all, because it gives you false confidence.

**Linux/other UNIX.** Desktop GL, links `X11 Xi Xcursor GL EGL dl m pthread` via `find_package(OpenGL REQUIRED)`. Nobody has actually run a game on this path as far as the commit history shows. It should work — sokol supports it and the same code paths run on macOS's GL fallback conceptually — but "should work" and "verified working" are different sentences, don't confuse them in your own documentation either.

**Web (Emscripten).** The backend is selected and that's the extent of what's been done. No emcc link flags, no `.html` shell, no testing. If you want this, budget real time for it, don't assume it falls out for free.

## `THISTLE_DEBUG`

```bash
cmake -S . -B build -DTHISTLE_DEBUG=ON
```

Turns on an in-app draggable log overlay (a button that expands into the captured `log_info`/`log_warn`/`log_error` history, with a copy-to-clipboard button). Off by default. Turn it on when you're chasing something at runtime on a device where you can't attach a debugger, not as your permanent dev config — it's a debug overlay, not a console, and it'll be in front of your game.

## Vendored dependencies

`CMakeLists.txt` pulls sokol, stb, miniaudio, box2d (pinned to `v2.4.1`, everything else tracks `master` — yes, that's inconsistent, box2d pins because its API isn't stable across versions and the others are stable-enough header libraries that master is fine) via `FetchContent`. `third_party/nlohmann/json.hpp` is vendored directly as a single header because fetching the whole repo for one header is a waste of everyone's bandwidth.

If you're doing repeated cross-compiles or offline builds, point `FETCHCONTENT_BASE_DIR` at an already-populated `_deps` directory from a previous build instead of re-fetching:

```bash
cmake -S . -B build-other -DFETCHCONTENT_BASE_DIR=/path/to/existing/build/_deps
```

This works because the fetched sources aren't platform-specific — the same sokol/box2d/miniaudio checkout compiles for every target.
