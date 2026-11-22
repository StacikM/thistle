//! thistle-import: `thistle import-unity` in Rust. Same arguments, same output files and report as the Python
//! implementation in tools/thistle-cli/unity_import.py, but fast on big projects.
mod importer;
mod mesh;
mod yaml;

use importer::{Importer, Options};
use std::path::PathBuf;
use std::process::exit;

const HELP: &str = "thistle-import <unity_project> [--out DIR] [--force] [--scene NAME]... [--ambient-scale X] [--light-scale X]
              [--sun-scale X] [--blender PATH] [--no-fbx]

Converts a Unity project's scenes, prefabs, materials and assets into Thistle ones. See docs/unity-import.md.
Normally run through `thistle import-unity`, which uses this when it's built.";

fn main() {
    let mut args = std::env::args().skip(1);
    let mut project: Option<String> = None;
    let mut o = Options {
        project: PathBuf::new(),
        out: std::env::current_dir().unwrap_or_default(),
        force: false,
        scenes: Vec::new(),
        ambient_scale: 0.4,
        light_scale: 0.28,
        sun_scale: 1.0,
        blender: String::new(),
        convert_fbx: true,
    };
    let fail = |msg: String| -> ! {
        eprintln!("thistle-import: {}", msg);
        exit(2)
    };
    while let Some(a) = args.next() {
        let mut value = |name: &str| -> String { args.next().unwrap_or_else(|| fail(format!("{} needs a value", name))) };
        let num = |name: &str, s: String| -> f64 { s.parse().unwrap_or_else(|_| fail(format!("{} needs a number, not '{}'", name, s))) };
        match a.as_str() {
            "-h" | "--help" => {
                println!("{}", HELP);
                return;
            }
            "--out" => o.out = PathBuf::from(value("--out")),
            "--force" => o.force = true,
            "--scene" => o.scenes.push(value("--scene")),
            "--ambient-scale" => o.ambient_scale = num("--ambient-scale", value("--ambient-scale")),
            "--light-scale" => o.light_scale = num("--light-scale", value("--light-scale")),
            "--sun-scale" => o.sun_scale = num("--sun-scale", value("--sun-scale")),
            "--blender" => o.blender = value("--blender"),
            "--no-fbx" => o.convert_fbx = false,
            s if s.starts_with("--") => fail(format!("unknown option {}", s)),
            s => {
                if project.is_some() {
                    fail(format!("one Unity project at a time (got a second: {})", s));
                }
                project = Some(s.to_string());
            }
        }
    }
    let Some(project) = project else { fail("which Unity project? (the folder with Assets/ in it)".into()) };
    o.project = std::fs::canonicalize(&project).unwrap_or_else(|_| PathBuf::from(&project));
    o.out = std::fs::canonicalize(&o.out).unwrap_or(o.out);
    let mut imp = Importer::new(o);
    exit(imp.run());
}
