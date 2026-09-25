#!/usr/bin/env python3
"""thistle — a tiny CLI for Thistle engine projects.

Standard library only, on purpose: the whole point of this tool is that
starting a project shouldn't require fighting a package manager or (as with
old Cocos2d tooling) an end-of-life Python 2 install. If this runs at all,
it has everything it needs.

Commands:
    thistle new <name> [--at PATH] [--template KIND] [--with MODULE]...
                                     create a new project
    thistle init [--name NAME]       make the current folder a project (overwrites nothing)
    thistle build [--release]        configure + build the project in cwd
    thistle run [--release]          build, then run the result
    thistle version show             print the current version
    thistle version set X.Y.Z        set an exact version
    thistle version bump PART        bump major/minor/patch by one
    thistle modules                  list the optional engine modules, and which are on
    thistle enable MODULE            turn one on for this project (e.g. physics3d)
    thistle disable MODULE           turn it back off
    thistle editor install           build tools/thistle-editor, put `thistle-editor` on PATH
    thistle editor update            pull the latest engine source, then rebuild+reinstall the editor
    thistle editor run               build (if needed) and run the editor without installing it
"""
import argparse
import json
import os
import re
import shutil
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


def find_project_root(start: Path, required: bool = True):
    p = start.resolve()
    for candidate in (p, *p.parents):
        if (candidate / "thistle.json").exists():
            return candidate
    if not required:
        return None
    die("not inside a thistle project (no thistle.json here or in a parent directory)")


def read_project(root: Path) -> dict:
    try:
        return json.loads((root / "thistle.json").read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        die(f"thistle.json is not valid JSON: {e}")


def write_project(root: Path, data: dict) -> None:
    (root / "thistle.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


MISSING_TOOL_HINTS = {
    "cmake": "Thistle builds games with CMake. Install it from https://cmake.org/download/ "
             "(Windows: `winget install Kitware.CMake`, macOS: `brew install cmake`, Linux: your "
             "package manager), then open a new terminal. On Windows you also need Visual Studio's "
             "C++ compiler: the \"Desktop development with C++\" workload of Visual Studio 2022 or its Build Tools.",
    "git": "Install Git from https://git-scm.com/downloads, then open a new terminal.",
}


def run(cmd: list) -> None:
    print("$ " + " ".join(str(c) for c in cmd))
    try:
        result = subprocess.run(cmd)
    except FileNotFoundError:
        # Without this, a missing tool is a Python traceback.
        die(f"`{cmd[0]}` isn't installed, or isn't on your PATH. " + MISSING_TOOL_HINTS.get(cmd[0], ""))
    if result.returncode != 0:
        sys.exit(result.returncode)


# --- new ---------------------------------------------------------------

def engine_dir_for(project: Path) -> str:
    """Where the engine is, as the project's CMakeLists.txt should say it:
    relative to the project, so the pair can move together, unless they're
    on different Windows drives (C: and D:), where there's no relative path."""
    try:
        rel = os.path.relpath(ENGINE_ROOT, project).replace("\\", "/")
        return "${CMAKE_CURRENT_SOURCE_DIR}/" + rel
    except ValueError:
        return ENGINE_ROOT.as_posix()


def render(template_name: str, **subs: str) -> str:
    text = (TEMPLATES / template_name).read_text(encoding="utf-8")
    for key, value in subs.items():
        text = text.replace("{{" + key + "}}", value)
    return text


# `thistle new --template <kind>`: a starter game. "blank" is the 2D one in
# templates/ itself; the others each have a folder under templates/ with a
# main.cpp.in, an about.md for the README, and an assets/ folder (levels
# made with the Thistle Editor) that's copied as is.
TEMPLATE_KINDS = {
    "blank": "a window and two shapes: the smallest 2D start",
    "fps": "first person: walk an editor-made level, shoot the targets, reach the exit",
    "third-person": "a character behind an orbiting camera: collect coins, reach the flag",
    "voxel": "an endless Minecraft-style block world: break, place, build, saved between runs",
}


def cmd_new(args) -> None:
    name = args.name
    kind = args.template
    if not VALID_NAME.match(name):
        die(f"'{name}' isn't a valid project name — use letters, numbers, - and _, starting with a letter")
    for m in args.modules:
        if m not in MODULES:
            die(f"no module called '{m}' — `thistle modules` lists them")

    dest = Path(args.at).resolve() if args.at else (ENGINE_ROOT.parent / name)
    if dest.exists():
        die(f"{dest} already exists")

    engine_dir = engine_dir_for(dest)

    dest.mkdir(parents=True)
    (dest / "src").mkdir()
    (dest / "assets").mkdir()

    (dest / "CMakeLists.txt").write_text(render("CMakeLists.txt.in", name=name, engine_dir=engine_dir), encoding="utf-8")
    main_template = "main.cpp.in" if kind == "blank" else f"{kind}/main.cpp.in"
    (dest / "src" / "main.cpp").write_text(render(main_template, name=name), encoding="utf-8")
    (dest / "src" / "version.hpp.in").write_text((TEMPLATES / "version.hpp.in").read_text(encoding="utf-8"), encoding="utf-8")
    about = "" if kind == "blank" else render(f"{kind}/about.md", name=name)
    (dest / "README.md").write_text(render("README.md.in", name=name, about=about), encoding="utf-8")
    if kind != "blank":
        shutil.copytree(TEMPLATES / kind / "assets", dest / "assets", dirs_exist_ok=True)
        # The 3D templates draw a HUD, and text needs a font: the same one
        # the Thistle Editor uses (Inter, OFL), with its license.
        fonts = dest / "assets" / "fonts"
        fonts.mkdir(parents=True, exist_ok=True)
        editor_assets = ENGINE_ROOT / "tools" / "thistle-editor" / "editor_assets"
        for f in ("inter-regular.ttf", "inter-OFL-LICENSE.txt"):
            shutil.copy2(editor_assets / f, fonts / f)
    (dest / ".gitignore").write_text("build/\n.DS_Store\n", encoding="utf-8")
    project = {"name": name, "version": "1.0.0", "template": kind}
    if args.modules:
        project["modules"] = {m: True for m in args.modules}
    write_project(dest, project)

    print(f"created {dest}" + ("" if kind == "blank" else f" ({kind} template)")
          + ("" if not args.modules else f", with {', '.join(args.modules)}"))
    print("next:")
    print(f"  cd {dest}")
    print("  thistle run")


def cmd_init(args) -> None:
    """Turns the current folder into a Thistle project: adds what `thistle
    new` would have made, but never overwrites a file that's already there."""
    dest = Path.cwd().resolve()
    if (dest / "thistle.json").exists():
        die(f"{dest} is already a Thistle project (it has a thistle.json)")
    name = args.name
    if not name:
        # The folder's name, made valid: "My Game!" -> "My-Game".
        name = re.sub(r"[^A-Za-z0-9_-]+", "-", dest.name).strip("-_")
        if not name or not name[0].isalpha():
            name = "game-" + name if name else "game"
    if not VALID_NAME.match(name):
        die(f"'{name}' isn't a valid project name — use letters, numbers, - and _, starting with a letter")

    engine_dir = engine_dir_for(dest)
    added, kept = [], []

    def add(rel: str, text: str) -> None:
        path = dest / rel
        if path.exists():
            kept.append(rel)
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        added.append(rel)

    add("CMakeLists.txt", render("CMakeLists.txt.in", name=name, engine_dir=engine_dir))
    add("src/main.cpp", render("main.cpp.in", name=name))
    add("src/version.hpp.in", (TEMPLATES / "version.hpp.in").read_text(encoding="utf-8"))
    add("README.md", render("README.md.in", name=name, about=""))
    add(".gitignore", "build/\n.DS_Store\n")
    if not (dest / "assets").exists():
        (dest / "assets").mkdir()
        added.append("assets/")
    write_project(dest, {"name": name, "version": "1.0.0", "template": "blank"})
    added.append("thistle.json")

    print(f"made {dest} a Thistle project called '{name}'")
    print("added: " + ", ".join(added))
    if kept:
        print("kept as they were: " + ", ".join(kept))
    if "CMakeLists.txt" in kept:
        print("note: your own CMakeLists.txt was kept, so `thistle build` builds whatever it describes. To use the")
        print("engine from it, see docs/building.md (add_subdirectory + target_link_libraries(... thistle)).")


# --- build / run ---------------------------------------------------------

def _config_name(args) -> str:
    return "Release" if args.release else "Debug"


def cmd_build(args) -> None:
    root = find_project_root(Path.cwd())
    build_dir = root / "build"
    config = _config_name(args)
    if not (build_dir / "CMakeCache.txt").exists():
        run(["cmake", "-S", str(root), "-B", str(build_dir), f"-DCMAKE_BUILD_TYPE={config}"])
    # All cores: without it Make builds one file at a time, and the first
    # build (the whole engine) takes many times longer than it needs to.
    run(["cmake", "--build", str(build_dir), "--config", config, "--parallel", str(os.cpu_count() or 2)])


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
    # Run it from its own folder, where the build put its assets/ (as a
    # player's copy runs), not from wherever this was typed: from src/ the
    # game would find no assets at all. (On macOS the engine moves into the
    # app bundle's Resources itself.)
    os.chdir(exe.parent)
    os.execv(str(exe), [str(exe)])


# --- editor --------------------------------------------------------------
# `thistle editor` operates on the engine checkout itself (ENGINE_ROOT), not
# a scaffolded game project — so, unlike new/build/run/version, it works
# from any directory, not just inside a project with a thistle.json.

def _editor_dir() -> Path:
    return ENGINE_ROOT / "tools" / "thistle-editor"


def _default_bin_dir() -> Path:
    # Same default (and same env var) install.sh/install.bat use for the
    # `thistle` command itself, so a launcher installed here lands somewhere
    # already on PATH for anyone who installed thistle the normal way.
    env = os.environ.get("THISTLE_BIN_DIR")
    return Path(env) if env else Path.home() / ".local" / "bin"


def _build_editor() -> Path:
    editor_dir = _editor_dir()
    build_dir = editor_dir / "build"
    if not (build_dir / "CMakeCache.txt").exists():
        run(["cmake", "-S", str(editor_dir), "-B", str(build_dir)])
    run(["cmake", "--build", str(build_dir), "--parallel", str(os.cpu_count() or 2)])
    # "Debug" matches what a multi-config generator (Visual Studio, Xcode)
    # produces when no --config is passed to the build step above.
    exe = _find_executable(build_dir, "thistle_editor", "Debug")
    if not exe:
        die("built, but couldn't find thistle_editor's executable — check the build output above")
    return exe


def _install_launcher(bin_dir: Path, name: str, target: Path) -> Path:
    bin_dir.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        # No symlinks on Windows without Developer Mode or an elevated
        # prompt — a one-line wrapper needs neither. It calls the real
        # binary by absolute path, so it stays correct across rebuilds (the
        # build output path doesn't change, only its contents do).
        path = bin_dir / f"{name}.bat"
        path.write_text(f'@echo off\n"{target}" %*\n')
    else:
        path = bin_dir / name
        if path.exists() or path.is_symlink():
            path.unlink()
        path.symlink_to(target)
        path.chmod(0o755)
    return path


def _print_path_hint(bin_dir: Path, command: str) -> None:
    if str(bin_dir) in os.environ.get("PATH", "").split(os.pathsep):
        print(f"Try: {command}")
    else:
        print(f"{bin_dir} isn't on your PATH yet — add it the same way install.sh/install.bat told you to when")
        print(f"you installed `thistle` itself, then: {command}")


def cmd_editor(args) -> None:
    if args.editor_cmd == "update":
        run(["git", "-C", str(ENGINE_ROOT), "pull", "--ff-only"])

    if args.editor_cmd == "run":
        exe = _build_editor()
        # The editor edits one game's assets/ and scenes/: the folder given,
        # else the project we're in, else (outside any project) its own folder.
        folder = Path(args.project).resolve() if args.project else find_project_root(Path.cwd(), required=False)
        argv = [str(exe)] + ([str(folder)] if folder else [])
        print("$ " + " ".join(argv))
        os.execv(str(exe), argv)

    # install, or update falling through to reinstall with the fresh build
    exe = _build_editor()
    bin_dir = _default_bin_dir()
    launcher = _install_launcher(bin_dir, "thistle-editor", exe)
    print(f"==> Editor built: {exe}")
    print(f"==> Launcher:     {launcher}")
    print()
    _print_path_hint(bin_dir, "thistle-editor")


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


# --- optional engine modules ---------------------------------------------------
# Big engine features a game has to ask for. Each is a CMake option in the
# engine that the project's CMakeLists.txt sets from thistle.json's
# "modules" object before adding the engine (see templates/CMakeLists.txt.in).

MODULES = {
    "physics3d": ("THISTLE_PHYSICS3D",
                  "3D rigid-body physics on Jolt Physics: three::Physics3D (adds a few minutes to the first build)"),
    "debug_ui": ("THISTLE_DEBUG_UI",
                 "Dear ImGui debug windows: #include <imgui.h> in your game, debug_stats_window()"),
}


def _project_reads_module(root: Path, option: str) -> bool:
    try:
        return option in (root / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def cmd_modules(args) -> None:
    root = find_project_root(Path.cwd())
    enabled = read_project(root).get("modules", {})
    for name, (_, description) in MODULES.items():
        state = "on " if enabled.get(name) else "off"
        print(f"  {state}  {name:<12} {description}")


def _set_module(args, on: bool) -> None:
    if args.module not in MODULES:
        die(f"no module called '{args.module}' — `thistle modules` lists them")
    root = find_project_root(Path.cwd())
    proj = read_project(root)
    modules = proj.setdefault("modules", {})
    if bool(modules.get(args.module)) == on:
        print(f"{args.module} is already {'on' if on else 'off'}")
        return
    modules[args.module] = on
    write_project(root, proj)
    print(f"{args.module} is now {'on' if on else 'off'}; the next `thistle build` picks it up")
    option = MODULES[args.module][0]
    if not _project_reads_module(root, option):
        # Projects made before modules existed don't read them from
        # thistle.json. Say how to fix it rather than editing their CMake.
        print(f"warning: this project's CMakeLists.txt doesn't read modules from thistle.json yet (it was made by an", file=sys.stderr)
        print(f"older `thistle new`), so this won't take effect. Add these lines before its add_subdirectory(...):", file=sys.stderr)
        print(f'  file(READ "${{CMAKE_CURRENT_SOURCE_DIR}}/thistle.json" _project_json)', file=sys.stderr)
        print(f'  string(JSON {option} ERROR_VARIABLE _no_module GET "${{_project_json}}" modules {args.module})', file=sys.stderr)
        print(f"  if(_no_module)\n      set({option} OFF)\n  endif()", file=sys.stderr)


def cmd_enable(args) -> None:
    _set_module(args, True)


def cmd_disable(args) -> None:
    _set_module(args, False)


# --- main --------------------------------------------------------------

def main() -> None:
    # Output into a pipe (the Thistle Editor reads it that way) is UTF-8 on
    # every OS. Otherwise Windows uses the locale's code page, and some of
    # those (Japanese, for one) can't encode the "—" in these messages.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure") and not stream.isatty():
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(prog="thistle", description="Thistle engine project tool")
    sub = parser.add_subparsers(dest="command", required=True)

    p_new = sub.add_parser("new", help="create a new project")
    p_new.add_argument("name")
    p_new.add_argument("--at", help="where to create it (default: next to the engine)")
    p_new.add_argument("--template", "-t", choices=list(TEMPLATE_KINDS), default="blank",
                       help="what to start from: " + "; ".join(f"{k}: {v}" for k, v in TEMPLATE_KINDS.items()))
    p_new.add_argument("--with", dest="modules", action="append", default=[], metavar="MODULE",
                       help="turn an optional engine module on from the start (repeatable): " + ", ".join(MODULES))
    p_new.set_defaults(func=cmd_new)

    p_init = sub.add_parser("init", help="make the current folder a Thistle project (adds what's missing, overwrites nothing)")
    p_init.add_argument("--name", help="the project's name (default: the folder's name)")
    p_init.set_defaults(func=cmd_init)

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

    sub.add_parser("modules", help="list the optional engine modules, and which are on").set_defaults(func=cmd_modules)
    p_enable = sub.add_parser("enable", help="turn an optional engine module on (e.g. physics3d)")
    p_enable.add_argument("module")
    p_enable.set_defaults(func=cmd_enable)
    p_disable = sub.add_parser("disable", help="turn an optional engine module off")
    p_disable.add_argument("module")
    p_disable.set_defaults(func=cmd_disable)

    p_editor = sub.add_parser("editor", help="install/update/run the Thistle Editor (tools/thistle-editor)")
    esub = p_editor.add_subparsers(dest="editor_cmd", required=True)
    esub.add_parser("install", help="build the editor and put `thistle-editor` on your PATH").set_defaults(func=cmd_editor)
    esub.add_parser("update", help="pull the latest engine source, then rebuild+reinstall the editor").set_defaults(func=cmd_editor)
    p_erun = esub.add_parser("run", help="build (if needed) and run the editor without installing it")
    p_erun.add_argument("project", nargs="?", default=None,
                        help="the game folder to edit (default: the project you're in; its assets/ and scenes/ are what the editor shows)")
    p_erun.set_defaults(func=cmd_editor)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
