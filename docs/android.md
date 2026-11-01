# Android

Android is the hardest platform this engine targets, for one reason: there is no `main()`. iOS, Windows, macOS, Linux all eventually call into a process entry point that your code owns. Android loads your engine as a shared library and jumps straight into a callback — everything here exists to paper over that difference so your game code doesn't have to know about it.

**Status: builds, links, packages, installs, and runs on two physical devices (Android 9 and 13) without crashing, including taps and swipes. Asset loading (textures, fonts, audio, tilemaps) is implemented but verified by code and build only.** Read this whole page before shipping anything on Android; there are real gaps, not just theoretical ones.

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

- **Rendering looks right on real hardware, but only one rendered frame has been looked at by a person.** The smoketest was run on two physical devices: a Galaxy A8+ (2018, Android 9, Mali) and a Galaxy Tab S6 Lite (SM-P615, Android 13, arm64). Both start, stay alive, and survive taps and swipes. On the A8+ the owner saw the blue rect drawn. The Tab S6 Lite run is verified by logcat (no crash, no shader errors), not by anyone looking at its screen. Nothing here has checked textures, text, or sound actually appearing, because the smoketest ships without its optional test assets.

  The earlier "GPU rendering is broken" finding came from an emulator and was a different bug, which is still unexplained there. On real devices the failures were two engine bugs, both fixed:

  1. **`text_impl.c` had no Android case in its backend `#if` chain**, so sokol_fontstash baked in a desktop `#version 410` shader and crashed the moment text was set up. (The `three::` renderer's shaders pick their variant at runtime and were fine.)
  2. **`THISTLE_MAIN` returned before the first frame.** On Android `App::run()` only stashes the window config, so the entry point's locals (the `App`, textures, anything the update lambda captures by reference) were destroyed while the frame callback kept using them. It crashed whenever that dead stack memory got reused, which looked like a touch-input bug on the A8+ and an instant crash on the tablet. `THISTLE_MAIN` now runs on its own thread and `THISTLE_RUN` parks it, so those locals live for the whole process.
- **Gamepad and haptics.** Apple (GameController.framework), Windows (XInput), and Linux (kernel joystick API) all have real backends now — Android and web still get the no-op branch in `thistle.cpp`'s dispatch. Not specifically hard to add, just not done yet.
- **A real game's look and feel.** Fling (a separate game that uses the engine) installs and runs on the Tab S6 Lite without crashing, taps and swipes included; its on-screen output and audio haven't been checked yet. Games should read bundled text files with `read_asset_text()` rather than `std::ifstream`, which can't see inside an APK. `Http` works on Android through `HttpURLConnection` (src/android_support.cpp): the manifest needs `<uses-permission android:name="android.permission.INTERNET" />`, and URLs must be `https://` because Android 9+ blocks cleartext by default.
