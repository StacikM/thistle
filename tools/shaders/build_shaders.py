#!/usr/bin/env python3
"""Regenerates src/shaders/*.glsl.h from src/shaders/*.glsl with sokol-shdc.

Only engine developers who edit a shader ever run this. The generated headers
are committed, so building the engine (or a game) never needs sokol-shdc —
same no-extra-installs rule as the thistle CLI.

    python3 tools/shaders/build_shaders.py            # all shaders
    python3 tools/shaders/build_shaders.py lit sky    # just these

sokol-shdc comes from the pinned sokol-tools-bin commit below, downloaded
once into build/_sokol-tools. Set SOKOL_SHDC=/path/to/sokol-shdc to use your
own binary instead. Keep SOKOL_TOOLS_COMMIT in step with the sokol GIT_TAG in
CMakeLists.txt: the generated code fills in sokol_gfx's sg_shader_desc,
whose layout changes between sokol versions.
"""
import os
import platform
import stat
import subprocess
import sys
from pathlib import Path

SOKOL_TOOLS_REPO = "https://github.com/floooh/sokol-tools-bin.git"
SOKOL_TOOLS_COMMIT = "11d0cf678105d614d675e6d9bd2aaf3eeff12f8c"

# glsl410: Linux (GLCore). glsl300es: Android/Web. hlsl5: Windows (D3D11).
# metal_*: macOS, iOS devices, iOS simulator.
SLANGS = "glsl410:glsl300es:hlsl5:metal_macos:metal_ios:metal_sim"

ENGINE_ROOT = Path(__file__).resolve().parents[2]
SHADER_DIR = ENGINE_ROOT / "src" / "shaders"
TOOLS_DIR = ENGINE_ROOT / "build" / "_sokol-tools"


def host_bin_dir() -> str:
    system = platform.system()
    arm = platform.machine().lower() in ("arm64", "aarch64")
    if system == "Darwin":
        return "osx_arm64" if arm else "osx"
    if system == "Windows":
        return "win32"
    return "linux_arm64" if arm else "linux"


def fetch_sokol_shdc() -> Path:
    exe = "sokol-shdc.exe" if platform.system() == "Windows" else "sokol-shdc"
    binary = TOOLS_DIR / "bin" / host_bin_dir() / exe
    if binary.exists():
        return binary
    print(f"fetching sokol-tools-bin @ {SOKOL_TOOLS_COMMIT[:10]} ...")
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    git = ["git", "-C", str(TOOLS_DIR)]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["fetch", "-q", "--depth", "1", SOKOL_TOOLS_REPO, SOKOL_TOOLS_COMMIT], check=True)
    subprocess.run(git + ["checkout", "-q", "FETCH_HEAD"], check=True)
    if platform.system() != "Windows":
        binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
    return binary


def main() -> int:
    shdc = Path(os.environ["SOKOL_SHDC"]) if os.environ.get("SOKOL_SHDC") else fetch_sokol_shdc()
    wanted = set(sys.argv[1:])
    sources = sorted(p for p in SHADER_DIR.glob("*.glsl") if not wanted or p.stem in wanted)
    if not sources:
        print("no matching shaders in src/shaders/", file=sys.stderr)
        return 1
    for src in sources:
        out = src.with_suffix(".glsl.h")
        # Relative names (with cwd=SHADER_DIR) so the command line sokol-shdc
        # records in the generated header has no machine-specific paths.
        cmd = [str(shdc), "-i", src.name, "-o", out.name, "-l", SLANGS]
        print(f"  {src.name} -> {out.name}")
        result = subprocess.run(cmd, cwd=SHADER_DIR)
        if result.returncode != 0:
            return result.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
