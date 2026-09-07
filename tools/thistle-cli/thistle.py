#!/usr/bin/env python3
"""thistle — a tiny CLI for Thistle engine projects.

Standard library only, on purpose: the whole point of this tool is that
starting a project shouldn't require fighting a package manager or (as with
old Cocos2d tooling) an end-of-life Python 2 install. If this runs at all,
it has everything it needs.

Commands:
    thistle new <name> [--at PATH]   create a new project
    thistle build [--release]        configure + build the project in cwd
    thistle run [--release]          build, then run the result
    thistle version show             print the current version
    thistle version set X.Y.Z        set an exact version
    thistle version bump PART        bump major/minor/patch by one
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# tools/thistle-cli/thistle.py -> engine root is two directories up.
ENGINE_ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = Path(__file__).resolve().parent / "templates"

VALID_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")
VALID_SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def die(msg: str) -> "None":
    print(f"thistle: error: {msg}", file=sys.stderr)
    sys.exit(1)


def find_project_root(start: Path) -> Path:
    p = start.resolve()
    for candidate in (p, *p.parents):
        if (candidate / "thistle.json").exists():
            return candidate
    die("not inside a thistle project (no thistle.json here or in a parent directory)")


def read_project(root: Path) -> dict:
    try:
        return json.loads((root / "thistle.json").read_text())
    except json.JSONDecodeError as e:
        die(f"thistle.json is not valid JSON: {e}")


def write_project(root: Path, data: dict) -> None:
    (root / "thistle.json").write_text(json.dumps(data, indent=2) + "\n")


def run(cmd: list) -> None:
    print("$ " + " ".join(str(c) for c in cmd))
    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit(result.returncode)


# --- new ---------------------------------------------------------------

def render(template_name: str, **subs: str) -> str:
    text = (TEMPLATES / template_name).read_text()
    for key, value in subs.items():
        text = text.replace("{{" + key + "}}", value)
    return text


def cmd_new(args) -> None:
    name = args.name
    if not VALID_NAME.match(name):
        die(f"'{name}' isn't a valid project name — use letters, numbers, - and _, starting with a letter")

    dest = Path(args.at).resolve() if args.at else (ENGINE_ROOT.parent / name)
    if dest.exists():
        die(f"{dest} already exists")

    rel_engine = os.path.relpath(ENGINE_ROOT, dest).replace("\\", "/")

    dest.mkdir(parents=True)
    (dest / "src").mkdir()
    (dest / "assets").mkdir()

    (dest / "CMakeLists.txt").write_text(render("CMakeLists.txt.in", name=name, engine_dir=rel_engine))
    (dest / "src" / "main.cpp").write_text(render("main.cpp.in", name=name))
    (dest / "src" / "version.hpp.in").write_text((TEMPLATES / "version.hpp.in").read_text())
    (dest / "README.md").write_text(render("README.md.in", name=name))
    (dest / ".gitignore").write_text("build/\n.DS_Store\n")
    write_project(dest, {"name": name, "version": "1.0.0"})

    print(f"created {dest}")
    print("next:")
    print(f"  cd {dest}")
    print("  thistle run")


# --- build / run ---------------------------------------------------------

def _config_name(args) -> str:
    return "Release" if args.release else "Debug"


def cmd_build(args) -> None:
    root = find_project_root(Path.cwd())
    build_dir = root / "build"
    config = _config_name(args)
    if not (build_dir / "CMakeCache.txt").exists():
        run(["cmake", "-S", str(root), "-B", str(build_dir), f"-DCMAKE_BUILD_TYPE={config}"])
    run(["cmake", "--build", str(build_dir), "--config", config])


def _find_executable(build_dir: Path, name: str, config: str) -> "Path | None":
    candidates = [
        build_dir / f"{name}.app" / "Contents" / "MacOS" / name,  # macOS .app bundle
        build_dir / config / f"{name}.exe",                        # Windows (multi-config generator)
        build_dir / config / name,                                  # Xcode/VS single-target, non-bundle
        build_dir / f"{name}.exe",                                  # Windows (single-config generator)
        build_dir / name,                                            # Linux / macOS single-config
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def cmd_run(args) -> None:
    cmd_build(args)
    root = find_project_root(Path.cwd())
    proj = read_project(root)
    exe = _find_executable(root / "build", proj["name"], _config_name(args))
    if not exe:
        die("built, but couldn't find the executable to run — check the build output above")
    print("$ " + str(exe))
    os.execv(str(exe), [str(exe)])


# --- version ---------------------------------------------------------------

def cmd_version(args) -> None:
    root = find_project_root(Path.cwd())
    proj = read_project(root)

    if args.version_cmd == "show":
        print(proj["version"])
        return

    if args.version_cmd == "set":
        if not VALID_SEMVER.match(args.value):
            die(f"'{args.value}' doesn't look like a version — expected X.Y.Z (e.g. 1.2.0)")
        proj["version"] = args.value
    else:  # bump
        major, minor, patch = (int(x) for x in proj["version"].split("."))
        if args.part == "major":
            major, minor, patch = major + 1, 0, 0
        elif args.part == "minor":
            minor, patch = minor + 1, 0
        else:
            patch += 1
        proj["version"] = f"{major}.{minor}.{patch}"

    write_project(root, proj)
    print(proj["version"])


# --- main --------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(prog="thistle", description="Thistle engine project tool")
    sub = parser.add_subparsers(dest="command", required=True)

    p_new = sub.add_parser("new", help="create a new project")
    p_new.add_argument("name")
    p_new.add_argument("--at", help="where to create it (default: next to the engine)")
    p_new.set_defaults(func=cmd_new)

    p_build = sub.add_parser("build", help="configure + build the project in the current directory")
    p_build.add_argument("--release", action="store_true")
    p_build.set_defaults(func=cmd_build)

    p_run = sub.add_parser("run", help="build (if needed), then run the result")
    p_run.add_argument("--release", action="store_true")
    p_run.set_defaults(func=cmd_run)

    p_version = sub.add_parser("version", help="show or change the project's version")
    vsub = p_version.add_subparsers(dest="version_cmd", required=True)
    vsub.add_parser("show").set_defaults(func=cmd_version)
    p_set = vsub.add_parser("set")
    p_set.add_argument("value")
    p_set.set_defaults(func=cmd_version)
    p_bump = vsub.add_parser("bump")
    p_bump.add_argument("part", choices=["major", "minor", "patch"])
    p_bump.set_defaults(func=cmd_version)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
