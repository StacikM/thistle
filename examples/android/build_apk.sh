#!/usr/bin/env bash
# Builds the Android smoketest into an installable APK, entirely with NDK +
# command-line SDK build-tools — no Gradle, no Android Gradle Plugin, no
# network access beyond what sdkmanager already pulled down once. Mirrors
# thistle-fling/deploy_ios.sh's role for iOS: this is how you actually get
# bits onto a device/emulator, not just compile a .so and hope.
#
# Usage: ANDROID_HOME=/path/to/sdk ./build_apk.sh [install]
#   install   also `adb install -r` + launch on whatever device/emulator adb
#             currently sees.
#
# See docs/android.md for what this does and does not prove.
set -euo pipefail
cd "$(dirname "$0")"

: "${ANDROID_HOME:?Set ANDROID_HOME to your Android SDK root}"
BUILD_TOOLS="$ANDROID_HOME/build-tools/34.0.0"
PLATFORM_JAR="$ANDROID_HOME/platforms/android-34/android.jar"
NDK="$ANDROID_HOME/ndk/26.1.10909125"
ROOT="$(cd ../.. && pwd)"
BUILD_DIR="$ROOT/build-android"
HERE="$(pwd)"
OUT="$HERE/out"
ASSETS="$HERE/test_assets"

echo "==> Configuring + building libmain.so (arm64-v8a)"
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DTHISTLE_BUILD_SMOKETEST=ON >/dev/null
cmake --build "$BUILD_DIR" --target thistle_android_smoketest -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

rm -rf "$OUT"
mkdir -p "$OUT/lib/arm64-v8a"
cp "$BUILD_DIR/libmain.so" "$OUT/lib/arm64-v8a/libmain.so"

echo "==> Packaging APK (aapt)"
cd "$OUT"
# -A packages a directory's CONTENTS directly at the APK assets root — see
# android_asset_path() in src/thistle.cpp, which strips the "assets/" prefix
# game code uses (e.g. "assets/font.ttf") to match. Optional: the smoketest
# itself checks Texture/Font::valid() and just shows FAILED without it.
if [ -d "$ASSETS" ]; then
    "$BUILD_TOOLS/aapt" package -f -F base.apk -M ../AndroidManifest.xml -I "$PLATFORM_JAR" -A "$ASSETS"
else
    "$BUILD_TOOLS/aapt" package -f -F base.apk -M ../AndroidManifest.xml -I "$PLATFORM_JAR"
fi
"$BUILD_TOOLS/aapt" add base.apk lib/arm64-v8a/libmain.so >/dev/null

echo "==> Aligning + signing"
"$BUILD_TOOLS/zipalign" -f -p 4 base.apk aligned.apk

# Kept outside out/ (which gets wiped every run) — a fresh key each build
# would make adb install fail with INSTALL_FAILED_UPDATE_INCOMPATIBLE against
# whatever's already on the device.
KEYSTORE="$HERE/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -v -keystore "$KEYSTORE" -alias androiddebugkey \
        -storepass android -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Thistle Debug,O=Thistle,C=US" >/dev/null
fi
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out thistle_smoketest.apk aligned.apk

echo "==> Built $OUT/thistle_smoketest.apk"

if [ "${1:-}" = "install" ]; then
    ADB="$ANDROID_HOME/platform-tools/adb"
    echo "==> Installing on $("$ADB" get-state 2>/dev/null || echo 'no device')"
    "$ADB" install -r thistle_smoketest.apk
    "$ADB" shell am start -n dev.thistle.smoketest/android.app.NativeActivity
fi
