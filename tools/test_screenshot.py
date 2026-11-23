#!/usr/bin/env python3
"""Checks THISTLE_SCREENSHOT end to end: runs the screenshot_smoketest program with the variable set and reads the PNG
it wrote, with a PNG reader of its own (standard library only). Run by ctest as screenshot_smoketest, on Linux under
xvfb-run with Mesa's software OpenGL (no GPU needed). Usage: test_screenshot.py <path to thistle_screenshot_smoketest>"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures += 1


def read_png(path: Path):
    """(width, height, rows of RGBA bytes) for an 8-bit RGB/RGBA PNG, any filter."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and color in (2, 6), "only 8-bit RGB/RGBA"
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    channels = 4 if color == 6 else 3
    raw, stride = zlib.decompress(idat), width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prev[i], (prev[i - channels] if i >= channels else 0)
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return width, height, [[tuple(r[x * channels:x * channels + 3]) for x in range(width)] for r in rows]


def main() -> int:
    exe = sys.argv[1]
    runner = []
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY"):
        if not shutil.which("xvfb-run"):
            print("no display and no xvfb-run: skipping")
            return 0
        runner = ["xvfb-run", "-a", "-s", "-screen 0 640x480x24"]
    with tempfile.TemporaryDirectory() as tmp:
        shot = Path(tmp) / "shot.png"
        env = dict(os.environ, THISTLE_SCREENSHOT=str(shot), THISTLE_SCREENSHOT_FRAME="4", LIBGL_ALWAYS_SOFTWARE="1")
        r = subprocess.run(runner + [exe], env=env, capture_output=True, text=True, timeout=90)
        check(r.returncode == 0, f"the program writes a screenshot and exits 0 (exit code {r.returncode})")
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            return 1
        check("screenshot: wrote" in r.stdout + r.stderr and "frame 4" in r.stdout + r.stderr, "it says where and after which frame")
        w, h, px = read_png(shot)
        check((w, h) == (320, 240), f"the PNG is the window's size ({w}x{h})")
        check(px[50][50][0] > 240 and px[50][50][1] < 15 and px[50][50][2] < 15, f"a 2D red rectangle is red where it was drawn {px[50][50]}")
        check(px[190][270][2] > 240 and px[190][270][0] < 15 and px[190][270][1] < 15, f"...and the blue one is blue, in its own corner {px[190][270]} (so rows run top to bottom)")
        center = px[120][160]
        check(center[1] > 120 and center[1] > center[0] + 40 and center[1] > center[2] + 40, f"the lit 3D box is green at the middle of the screen {center}")
        corner = px[3][318]
        check(corner != (255, 0, 0) and corner != (0, 0, 255) and corner != center, f"the top-right corner is neither a rectangle nor the box {corner}")
        print("save_screenshot: several pictures in one run")
        shots = Path(tmp) / "several" / "nested"  # a folder that doesn't exist yet
        env = dict(os.environ, SCREENSHOT_TEST_DIR=str(shots), LIBGL_ALWAYS_SOFTWARE="1")
        env.pop("THISTLE_SCREENSHOT", None)
        r = subprocess.run(runner + [exe], env=env, capture_output=True, text=True, timeout=90)
        check(r.returncode == 0, f"the program saves three screenshots, keeps running, and quits itself with exit code 0 (exit code {r.returncode})")
        if r.returncode != 0:
            print(r.stdout + r.stderr)
        files = {n: shots / f"shot_{n}.png" for n in (3, 6, 9)}
        check(all(p.exists() for p in files.values()), "three PNGs exist, in a folder that was made for them")
        out = r.stdout + r.stderr
        check(all(f"frame {n})" in out for n in (3, 6, 9)), "each is logged with the frame it was taken on")
        pics = {n: read_png(p) for n, p in files.items() if p.exists()}
        check(all((w, h) == (320, 240) for w, h, _ in pics.values()), "each is the window's size")
        check(len({str(px) for _, _, px in pics.values()}) == 3, "and the three differ from each other")
        for n, (w, h, px) in pics.items():
            red_x = 20 + 20 * n  # where that frame's callback drew the red rectangle
            check(px[50][red_x + 30][0] > 240 and px[50][red_x + 30][1] < 15, f"frame {n}: the red rectangle is where frame {n}'s callback drew it ({px[50][red_x + 30]})")
        # no THISTLE_SCREENSHOT set means no exit(0) from the env path: the program ended on its own at frame 10
        check("frame 10" not in out, "it kept going until the program quit by itself (nothing exited at a screenshot)")

    print("all passed" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
