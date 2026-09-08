# Android

Android is the hardest platform this engine targets, for one reason: there is no `main()`. iOS, Windows, macOS, Linux all eventually call into a process entry point that your code owns. Android loads your engine as a shared library and jumps straight into a callback — everything here exists to paper over that difference so your game code doesn't have to know about it.

**Status: builds, links, packages, installs, and runs through the full native-activity lifecycle. Asset loading (textures, fonts, audio, tilemaps) is implemented but verified by code and build only — not by a rendered frame, because GPU rendering itself reliably fails on the one emulator this has been tested on.** Read this whole page before shipping anything on Android; there are real gaps, not just theoretical ones.

## The one thing you need to know: THISTLE_MAIN

Everywhere else, a Thistle game is:

```cpp
int main() {
    App app{{.title = "My Game", .width = 1280, .height = 720}};
    app.update([&](Frame f) { /* ... */ });
    return app.run();
}
```

On Android, wrap it instead:

```cpp
THISTLE_MAIN {
    App app{{.title = "My Game", .width = 1080, .height = 2280}};
    app.update([&](Frame f) { /* ... */ });
    THISTLE_RUN(app);
}
```

`THISTLE_MAIN`/`THISTLE_RUN` expand to exactly `int main()` / `return app.run();` on every platform except Android — writing them everywhere costs you nothing and makes your game buildable for Android later without touching this code again.

**Why this has to exist:** on Android there's no process for the OS to call `main()` on. Your engine is `dlopen`'d as a shared library, and sokol_app.h's own `ANativeActivity_onCreate` (baked into the library, not something you write) calls a `sokol_main()` it expects to return a config struct immediately — it can't block running a game loop the way `sapp_run()` does everywhere else. `THISTLE_MAIN` expands to a plain function that Thistle's own internal `sokol_main()` calls for you; `THISTLE_RUN(app)` still calls `App::run()`, but on Android that just stashes your window/callback config instead of blocking, and Thistle hands it back to sokol_app.h itself. You never see any of this — it's why the wrapper exists.

## Building the toolchain

You need the NDK, at minimum. No Android Studio required — everything here uses command-line tools only.

```bash
brew install --cask android-commandlinetools
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
yes | sdkmanager --sdk_root="$ANDROID_HOME" --licenses
sdkmanager --sdk_root="$ANDROID_HOME" \
  "platform-tools" "platforms;android-34" "build-tools;34.0.0" \
  "ndk;26.1.10909125" "emulator" "system-images;android-34;google_apis;arm64-v8a"
```

That's ~9 GB total (mostly the emulator + system image — skip both if you're testing on a real device instead, which is what you should actually be doing, see below).

## Building the engine

Point CMake at the NDK's own toolchain file — no other flags needed beyond what any other CMake Android project uses:

```bash
NDK="$ANDROID_HOME/ndk/26.1.10909125"
cmake -S . -B build-android \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DTHISTLE_BUILD_SMOKETEST=ON
cmake --build build-android
```

This produces `libthistle.a` for `arm64-v8a` like any other platform, plus (when `THISTLE_BUILD_SMOKETEST=ON`) a `libmain.so` — a `SHARED` library, not an executable, because NativeActivity only ever loads a `.so`. See `examples/android_smoketest.cpp` for the minimal `THISTLE_MAIN`-wrapped example it builds from, and the `if(ANDROID)` block in `CMakeLists.txt` for how the target gets named `main` (matching the `android.app.lib_name` convention below).

## Packaging: no Gradle

`examples/android/build_apk.sh` builds and packages an installable APK using only `aapt`, `zipalign`, and `apksigner` from the SDK's `build-tools` — the same command-line tools Gradle's Android plugin calls under the hood, invoked directly. No Gradle, no Android Gradle Plugin, no network dependency beyond the SDK components you already downloaded once.

```bash
ANDROID_HOME=/opt/homebrew/share/android-commandlinetools ./examples/android/build_apk.sh install
```

`examples/android/AndroidManifest.xml` is the whole trick: `android:hasCode="false"` plus `<activity android:name="android.app.NativeActivity">` means the app ships zero Java/Kotlin of its own — `NativeActivity` is a stock OS class every Android device already has. The `android.app.lib_name` meta-data value (`"main"`) has to match your `.so`'s base name, which is why the CMake target sets `OUTPUT_NAME "main"`.

A real game will probably still want Gradle eventually — for Play Store App Bundles, ProGuard, multi-ABI splits, that ecosystem. Nothing about Thistle requires it; this script is enough to get a real APK on a real device to check your game actually runs.

## Asset loading

Textures, fonts, audio, and tilemaps (Tiled CSV/JSON) all used plain `fopen()`/`std::ifstream` before, which doesn't work once assets are packaged inside an APK — they live in the APK's zip, reachable only through `AAssetManager`, not a normal filesystem path. This is now wired up:

- **Textures**: `thistle_stbi_load()` reads the asset into memory via `AAssetManager_open`/`AAsset_read`, then decodes with `stbi_load_from_memory` instead of `stbi_load`. Used by both `load_texture()` and `reload_texture()`.
- **Fonts**: `android_fons_add_font()` does the same read, then hands fontstash the buffer via `fonsAddFontMem(..., freeData=1)` — fontstash frees it itself once done, the same ownership convention `fonsAddFont`'s own file-loading path already uses internally.
- **Tilemaps**: `Tilemap::load_csv`/`load_tiled_json` now read the whole file into a `std::string` first (via the same AAssetManager path), then parse from that instead of an open `std::ifstream`. Everything downstream (the CSV tokenizer, `nlohmann::json::parse`) is unchanged.
- **Audio**: rather than patch every miniaudio call site, this installs a custom `ma_vfs_callbacks` backed by `AAssetManager` (open/close/read/seek/tell/info; read-only, no write/wide-path support needed) into a `ma_resource_manager`, then points the engine's `ma_engine_config.pResourceManager` at it. `play_sound()`/`play_music()`/`stop_music()` needed zero code changes — every file load miniaudio does internally now transparently goes through AAssetManager.
- Game code keeps writing paths the same way it always has — `load_texture("assets/foo.png")` — because `examples/android/build_apk.sh` packages the assets directory's *contents* directly at the APK's assets root (`aapt package -A`), and `android_asset_path()` strips the literal `"assets/"` prefix before handing the path to `AAssetManager_open` so the two conventions line up.

**Verified by code review and successful compilation/linking against the real stb_image/fontstash/miniaudio APIs only** — `cmake --build` produces a clean `libmain.so` with no undefined symbols. It has **not** been verified by an actual rendered texture or an actual sound coming out of a speaker, because the emulator can't get past the rendering bug below to prove it. `examples/android_smoketest.cpp` loads a real font/texture/sound and draws "texture: OK"/"font: OK" text plus a sprite when you have test assets in `examples/android/test_assets/` (gitignored — drop your own `icon.png`/`font.ttf`/`nav.wav` there) — that's the thing to run once rendering actually works on a real device.

## What's verified

Run on an arm64-v8a emulator (Pixel 6 profile, Android 14) via the steps above:

- **The engine cross-compiles cleanly for Android** with the NDK toolchain — `libthistle.a`, no source changes needed beyond what's already in this repo.
- **The full NativeActivity lifecycle runs correctly**: `libmain.so` loads, `ANativeActivity_onCreate` → `sokol_main()` → your `THISTLE_MAIN` body → `App::run()` → back through sokol_app.h → window creation → input queue → EGL context creation, confirmed end-to-end via logcat (`ANDROID_NATIVE_ACTIVITY_CREATE_SUCCESS` through `ANDROID_MSG_FOCUS`, no crash).
- **Headless engine code runs for real on-device**, not just compiles: `thistle_net_smoketest` (the full NetVar/Command/ClientRpc test suite) pushed via `adb push` and run via `adb shell` — all 15 checks pass, real TCP sockets, real ARM64 code, zero crashes.
- **The crash handler works on Android**: `thistle_crash_smoketest segv` run via `adb shell` writes a correct report (reason, context, log tail) to the right place. No backtrace — Bionic doesn't ship `execinfo.h`/`backtrace()`, so `crash_backtrace()` writes `"(backtrace not available on Android)"` there instead of guessing; a real unwinder would need `<unwind.h>`'s `_Unwind_Backtrace` plus a symbolizer, not done. The popup (`osascript`/`zenity`-style) has no Android equivalent implemented — it hits the same `fork()`+`zenity` code path Linux does, which just silently no-ops since `zenity` isn't on Android.
- **Save data goes to the right place**: `save::base_dir()` uses `ANativeActivity::internalDataPath` (the only path an Android app can actually write to — there's no `HOME` or XDG env var), falling back to `/data/local/tmp` when run headless outside an Activity (as the smoketests above do).

## What's not verified

- **GPU rendering — confirmed broken on this emulator, reliably, not just once.** The graphical smoketest (`libmain.so`) reaches EGL context creation successfully, then fails compiling sokol_gl's GLSL ES 3.0 shader (`invalid version directive`, `'layout' : syntax error`) every time — 10/10 runs across two separate test sessions, not a one-off. One early run did render successfully; every run since (including after rebuilding with the asset-loading changes) has crashed at the identical spot, so that one success looks like the fluke, not the crashes.

  Root cause, as far as it can be pinned down without patching vendored code: `_sg_gl_init_limits()` in sokol_gfx.h calls `glGetIntegerv(GL_MAJOR_VERSION/MINOR_VERSION)` and gets back a real "3.1" — so the driver is genuinely reporting a GLES 3.1 context. But the same context's shader compiler then rejects `#version 300 es`, which is exactly the directive a real GLES3 driver requires and sokol_gl correctly emits for `SOKOL_GLES3`. A context that claims 3.1 via one query and can't parse ES3 shader syntax is an internal contradiction — not something in Thistle's or sokol's control. This emulator's GLES path always goes through ANGLE regardless of the `-gpu` flag (`emulator -gpu swiftshader` still logs `gles_mode_selected:swangle` — ANGLE-over-SwiftShader is not optional here), which points at an ANGLE/SwiftShader bug on this specific host (Apple Silicon Mac) rather than anything fixable by changing `sapp_desc` fields — forcing `d.gl.minor_version = 0` to request ES 3.0 instead of sokol's default 3.1 changed nothing, consistent with EGL's context-version request being a minimum the driver is free to exceed regardless.

  **This needs a real Android device to know whether it's an emulator-only problem or a real engine bug.** Given the "one success, then reliably reproducible after" pattern and the internal contradiction in the driver's own version reporting, an emulator/ANGLE bug is the more likely explanation, but that is not confirmed.
- **Gamepad and haptics.** Apple (GameController.framework), Windows (XInput), and Linux (kernel joystick API) all have real backends now — Android and web still get the no-op branch in `thistle.cpp`'s dispatch. Not specifically hard to add, just not done yet.
- **A real physical device.** Everything above was checked on one emulator only (Pixel 6 profile, Android 14, arm64-v8a, swangle GLES backend). A real device is the actual next step before trusting any of this — rendering especially — for a real game.
