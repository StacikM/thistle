#!/usr/bin/env python3
"""Checks the parts of the `thistle` CLI the Thistle Editor relies on:
`new` (with --template and --with) and `init`. Run by ctest as cli_smoketest;
standard library only, like the CLI. Makes projects in a temporary folder
and never builds them (that's what CI's template targets are for)."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = Path(__file__).resolve().parent / "thistle.py"
failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures += 1


def thistle(*args: str, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(CLI), *args], cwd=cwd, capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)

    # new: template and modules end up in thistle.json, and the template's files are there
    r = thistle("new", "mygame", "--at", str(root / "mygame"), "--template", "fps", "--with", "physics3d", "--with", "debug_ui", cwd=root)
    check(r.returncode == 0, "new --template fps --with physics3d --with debug_ui succeeds")
    project = json.loads((root / "mygame" / "thistle.json").read_text())
    check(project.get("template") == "fps", "thistle.json records the template")
    check(project.get("modules") == {"physics3d": True, "debug_ui": True}, "thistle.json has both modules on")
    check((root / "mygame" / "assets" / "scenes" / "level.scene.json").exists(), "the fps template's level is there")
    check((root / "mygame" / "assets" / "fonts" / "inter-regular.ttf").exists(), "and its font")

    r = thistle("new", "plain", "--at", str(root / "plain"), cwd=root)
    project = json.loads((root / "plain" / "thistle.json").read_text())
    check(r.returncode == 0 and project.get("template") == "blank" and "modules" not in project, "a plain new is blank, with no modules")

    # new: what it refuses
    check(thistle("new", "mygame", "--at", str(root / "mygame"), cwd=root).returncode != 0, "new refuses a folder that exists")
    check(thistle("new", "3d-game", "--at", str(root / "x1"), cwd=root).returncode != 0, "new refuses a name starting with a digit")
    check(thistle("new", "ok", "--at", str(root / "x2"), "--with", "nope", cwd=root).returncode != 0, "new refuses an unknown module")
    check(not (root / "x2").exists(), "and makes nothing when it refuses")

    # init: adds what's missing, keeps what's there
    old = root / "old thing!"
    (old / "src").mkdir(parents=True)
    (old / "assets").mkdir()
    (old / "src" / "main.cpp").write_text("keep me\n")
    (old / "assets" / "data.txt").write_text("mine\n")
    r = thistle("init", cwd=old)
    check(r.returncode == 0, "init succeeds in a folder that isn't a project")
    project = json.loads((old / "thistle.json").read_text())
    check(project.get("name") == "old-thing", "the name comes from the folder, made valid ('old thing!' -> 'old-thing')")
    check((old / "src" / "main.cpp").read_text() == "keep me\n", "an existing src/main.cpp is kept as it was")
    check((old / "assets" / "data.txt").read_text() == "mine\n", "existing assets are untouched")
    for f in ("CMakeLists.txt", "src/version.hpp.in", "README.md", ".gitignore"):
        check((old / f).exists(), f"init added {f}")
    check("kept as they were: src/main.cpp" in r.stdout, "init says what it kept")
    check(thistle("init", cwd=old).returncode != 0, "a second init is refused")

    digits = root / "3d"
    digits.mkdir()
    r = thistle("init", cwd=digits)
    check(r.returncode == 0 and json.loads((digits / "thistle.json").read_text()).get("name") == "game-3d",
          "a folder name starting with a digit becomes a valid name ('3d' -> 'game-3d')")

    named = root / "whatever"
    named.mkdir()
    r = thistle("init", "--name", "space-race", cwd=named)
    check(r.returncode == 0 and json.loads((named / "thistle.json").read_text()).get("name") == "space-race", "init --name sets the name")

if failures:
    print(f"{failures} check(s) failed")
    sys.exit(1)
print("all CLI checks passed")
