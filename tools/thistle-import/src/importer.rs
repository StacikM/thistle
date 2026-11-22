//! thistle import-unity: turns a Unity project (text-serialized) into Thistle assets. A port of
//! tools/thistle-cli/unity_import.py, held to the same tests; see docs/unity-import.md for what it converts.
use crate::mesh::mesh_asset_to_obj;
use crate::yaml::{parse_unity_yaml, Doc, UnityObject, Value};
use serde_json::{json, Map, Value as J};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::rc::Rc;

const BUILTIN_GUID: &str = "0000000000000000e000000000000000";
const MODEL_EXT: [&str; 3] = [".glb", ".gltf", ".obj"];
const CONVERT_MODEL_EXT: [&str; 8] = [".fbx", ".dae", ".blend", ".3ds", ".dxf", ".max", ".ma", ".mb"];
const COPY_EXT: [&str; 15] = [
    ".glb", ".gltf", ".obj", ".mtl", ".bin", ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".wav", ".ogg", ".mp3", ".flac", ".ttf",
];
const TEXTURE_EXT: [&str; 5] = [".png", ".jpg", ".jpeg", ".bmp", ".tga"];
const MESH_ASSET_EXT: &str = ".asset";

/// Run inside Blender: FBX in, GLB out. An FBX's takes come in as separate actions but only the active one would be
/// exported, so every action is attached to the armature as its own track and the tracks are what's exported: one
/// clip each, named without the "Armature|" prefix Blender's FBX importer gives them.
const BLENDER_FBX_TO_GLB: &str = "
import bpy, sys
src, dst = sys.argv[sys.argv.index('--') + 1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)
armatures = [o for o in bpy.data.objects if o.type == 'ARMATURE']
mode = 'ACTIVE_ACTIONS'
if armatures and bpy.data.actions:
    arm = armatures[0]
    data = arm.animation_data_create()
    data.action = None
    for track in list(data.nla_tracks):
        data.nla_tracks.remove(track)
    for action in bpy.data.actions:
        parts = action.name.split('|')
        while len(parts) > 1 and parts[0] == arm.name:
            parts = parts[1:]
        name = '|'.join(parts)
        track = data.nla_tracks.new()
        track.name = name
        strip = track.strips.new(name, int(action.frame_range[0]), action)
        strip.name = name
    mode = 'NLA_TRACKS'
bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB', export_animation_mode=mode)
";

pub struct Options {
    pub project: PathBuf,
    pub out: PathBuf,
    pub force: bool,
    pub scenes: Vec<String>,
    pub ambient_scale: f64,
    pub light_scale: f64,
    pub sun_scale: f64,
    pub blender: String,
    pub convert_fbx: bool,
}

// ---- small helpers -------------------------------------------------------------------------------------------

fn ext_of(rel: &str) -> String {
    let name = rel.rsplit('/').next().unwrap_or(rel);
    match name.rfind('.') {
        Some(i) => name[i..].to_lowercase(),
        None => String::new(),
    }
}

fn stem_of(rel: &str) -> String {
    let name = rel.rsplit('/').next().unwrap_or(rel);
    match name.rfind('.') {
        Some(i) if i > 0 => name[..i].to_string(),
        _ => name.to_string(),
    }
}

fn with_ext(rel: &str, ext: &str) -> String {
    let name_start = rel.rfind('/').map(|i| i + 1).unwrap_or(0);
    match rel[name_start..].rfind('.') {
        Some(i) if i > 0 => format!("{}{}", &rel[..name_start + i], ext),
        _ => format!("{}{}", rel, ext),
    }
}

fn parent_of(rel: &str) -> &str {
    rel.rfind('/').map(|i| &rel[..i]).unwrap_or("")
}

fn sanitize(s: &str) -> String {
    let mut out = String::new();
    let mut in_bad = false;
    for c in s.chars() {
        if c.is_ascii_alphanumeric() || c == '_' || c == '.' || c == '-' {
            out.push(c);
            in_bad = false;
        } else if !in_bad {
            out.push('_');
            in_bad = true;
        }
    }
    out
}

fn vecn<const N: usize>(v: Option<&Value>, names: &[&str; N], default: [f64; N]) -> [f64; N] {
    let mut out = default;
    if let Some(m) = v {
        if m.is_map() {
            for (i, n) in names.iter().enumerate() {
                if let Some(x) = m.get(n).and_then(|x| x.f64()) {
                    out[i] = x;
                }
            }
        }
    }
    out
}

fn color(v: Option<&Value>) -> [f64; 4] {
    vecn(v, &["r", "g", "b", "a"], [1.0, 1.0, 1.0, 1.0])
}

fn qmul(a: [f64; 4], b: [f64; 4]) -> [f64; 4] {
    let [ax, ay, az, aw] = a;
    let [bx, by, bz, bw] = b;
    [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]
}

fn qrot(q: [f64; 4], v: [f64; 3]) -> [f64; 3] {
    let [x, y, z, w] = q;
    let (cx, cy, cz) = (y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]);
    let (dx, dy, dz) = (y * cz - z * cy, z * cx - x * cz, x * cy - y * cx);
    [v[0] + 2.0 * (w * cx + dx), v[1] + 2.0 * (w * cy + dy), v[2] + 2.0 * (w * cz + dz)]
}

fn mirror_pos(p: [f64; 3]) -> [f64; 3] {
    [p[0], p[1], -p[2]]
}

fn mirror_rot(q: [f64; 4]) -> [f64; 4] {
    [-q[0], -q[1], q[2], q[3]]
}

fn rnd(x: f64, digits: i32) -> f64 {
    let m = 10f64.powi(digits);
    let r = (x * m).round() / m;
    if r == 0.0 { 0.0 } else { r }
}

fn nums(v: &[f64], digits: i32) -> J {
    J::Array(v.iter().map(|&x| J::from(rnd(x, digits))).collect())
}

// ---- the node tree -------------------------------------------------------------------------------------------

#[derive(Default, Clone)]
struct Look {
    texture: Option<String>,
    normal_texture: Option<String>,
    metallic: f64,
    roughness: f64,
    uv_scale: Option<[f64; 2]>,
    metallic_set: bool,
}

/// One thing in the scene, still in Unity's space, before the tree is flattened.
#[derive(Clone)]
struct Node {
    name: String,
    kind: String,
    pos: [f64; 3],
    rot: [f64; 4],
    scale: [f64; 3],
    color: [f64; 4],
    model: String,
    intensity: f64,
    range: f64,
    spot_angle: f64,
    properties: Vec<(String, String)>,
    children: Vec<usize>,
    sun_only: bool,
    tintable: bool,
    look: Look,
    world_parent_rot: [f64; 4],
}

impl Node {
    fn new(name: &str) -> Node {
        Node {
            name: name.to_string(),
            kind: "empty".into(),
            pos: [0.0; 3],
            rot: [0.0, 0.0, 0.0, 1.0],
            scale: [1.0; 3],
            color: [1.0; 4],
            model: String::new(),
            intensity: 1.0,
            range: 10.0,
            spot_angle: 30f64.to_radians(),
            properties: Vec::new(),
            children: Vec::new(),
            sun_only: false,
            tintable: false,
            look: Look::default(),
            world_parent_rot: [0.0, 0.0, 0.0, 1.0],
        }
    }
}

#[derive(Clone)]
struct Hit {
    node: usize,
    class_id: i32,
    type_name: String,
    script: String,
}

struct Expansion {
    roots: Vec<usize>,
    anchors: HashMap<i64, usize>,
    parent: i64,
    index: HashMap<i64, Hit>,
}

struct Built {
    roots: Vec<usize>,
    anchors: HashMap<i64, usize>,
    index: HashMap<i64, Hit>,
}

struct Sun {
    color: [f64; 4],
    intensity: f64,
    node: usize,
    shadows: bool,
}

#[derive(Default)]
struct Env {
    sun: Option<Sun>,
    fog: Option<(f64, f64, [f64; 4])>,
    sky: Option<([f64; 4], [f64; 4], [f64; 4])>,
    ambient: Option<f64>,
}

#[derive(Clone)]
struct MatInfo {
    name: String,
    source: String,
    shader: String,
    color: [f64; 4],
    metallic: f64,
    roughness: f64,
    emissive: [f64; 3],
    alpha: String,
    alpha_cutoff: f64,
    texture: Option<String>,
    normal_texture: Option<String>,
    metallic_roughness_texture: Option<String>,
    occlusion_texture: Option<String>,
    emissive_texture: Option<String>,
    uv_scale: Option<[f64; 2]>,
    metallic_set: bool,
}

type Overrides = Vec<(i64, Vec<(String, Value, Value)>)>;

pub struct Importer {
    project: PathBuf,
    out: PathBuf,
    force: bool,
    only_scenes: Vec<String>,
    ambient_scale: f64,
    light_scale: f64,
    sun_scale: f64,
    blender_arg: String,
    convert_fbx: bool,
    assets: PathBuf,
    files: Vec<String>,
    guid_path: HashMap<String, String>,
    warnings: Vec<String>,
    counts: BTreeMap<String, usize>,
    skipped_types: BTreeMap<String, usize>,
    to_convert: Vec<String>,
    used: HashSet<String>,
    names: HashMap<(&'static str, String), String>,
    embedded: BTreeMap<(String, i64), (String, Value)>,
    unresolved: BTreeMap<String, usize>,
    ignored: BTreeMap<String, usize>,
    docs: HashMap<String, Option<Rc<Doc>>>,
    mats: HashMap<String, Option<MatInfo>>,
    env: Env,
    nodes: Vec<Node>,
}

fn list_files(root: &Path, base: &Path, out: &mut Vec<String>) {
    let Ok(entries) = fs::read_dir(root) else { return };
    for e in entries.flatten() {
        let path = e.path();
        let Ok(meta) = fs::metadata(&path) else { continue };
        if meta.is_dir() {
            list_files(&path, base, out);
        } else if meta.is_file() {
            if let Ok(rel) = path.strip_prefix(base) {
                out.push(rel.to_string_lossy().replace('\\', "/"));
            }
        }
    }
}

impl Importer {
    pub fn new(o: Options) -> Importer {
        let assets = o.project.join("Assets");
        Importer {
            project: o.project,
            out: o.out,
            force: o.force,
            only_scenes: o.scenes,
            ambient_scale: o.ambient_scale,
            light_scale: o.light_scale,
            sun_scale: o.sun_scale,
            blender_arg: o.blender,
            convert_fbx: o.convert_fbx,
            assets,
            files: Vec::new(),
            guid_path: HashMap::new(),
            warnings: Vec::new(),
            counts: BTreeMap::new(),
            skipped_types: BTreeMap::new(),
            to_convert: Vec::new(),
            used: HashSet::new(),
            names: HashMap::new(),
            embedded: BTreeMap::new(),
            unresolved: BTreeMap::new(),
            ignored: BTreeMap::new(),
            docs: HashMap::new(),
            mats: HashMap::new(),
            env: Env::default(),
            nodes: Vec::new(),
        }
    }

    // -- bookkeeping --

    fn warn(&mut self, message: impl Into<String>) {
        let m = message.into();
        if !self.warnings.contains(&m) {
            self.warnings.push(m);
        }
    }

    fn count(&mut self, what: &str, n: usize) {
        *self.counts.entry(what.to_string()).or_insert(0) += n;
    }

    fn skipped(&mut self, what: &str) {
        *self.skipped_types.entry(what.to_string()).or_insert(0) += 1;
    }

    fn new_node(&mut self, name: &str) -> usize {
        self.nodes.push(Node::new(name));
        self.nodes.len() - 1
    }

    fn set_property(&mut self, node: usize, key: &str, value: impl Into<String>) {
        let v = value.into();
        let props = &mut self.nodes[node].properties;
        props.retain(|p| p.0 != key);
        props.push((key.to_string(), v));
    }

    // -- scanning --

    fn scan_guids(&mut self) {
        let metas: Vec<String> = self.files.iter().filter(|f| f.ends_with(".meta")).cloned().collect();
        for rel in metas {
            let Ok(text) = fs::read_to_string(self.assets.join(&rel)) else { continue };
            for line in text.lines() {
                if let Some(g) = line.strip_prefix("guid: ") {
                    let g = g.trim();
                    if g.len() == 32 && g.bytes().all(|b| b.is_ascii_hexdigit()) {
                        self.guid_path.insert(g.to_string(), rel[..rel.len() - ".meta".len()].to_string());
                    }
                    break;
                }
            }
        }
    }

    /// Scenes, prefabs and materials are written as <name>.json in one folder per kind, so two files with the
    /// same name in different folders would overwrite each other. Decided over every file up front, so the
    /// names don't depend on which scenes you asked for.
    fn assign_names(&mut self) {
        for (kind, ext) in [("scene", ".unity"), ("prefab", ".prefab"), ("material", ".mat")] {
            let mut taken: HashSet<String> = HashSet::new();
            let list: Vec<String> = self.files.iter().filter(|f| ext_of(f) == ext).cloned().collect();
            for rel in list {
                let stem = sanitize(&stem_of(&rel));
                let parent = parent_of(&rel);
                let folder = sanitize(if parent.is_empty() { "Assets" } else { parent.rsplit('/').next().unwrap() });
                let mut name = stem.clone();
                let mut n = 1;
                if taken.contains(&name) {
                    name = format!("{}_{}", stem, folder);
                }
                while taken.contains(&name) {
                    n += 1;
                    name = format!("{}_{}_{}", stem, folder, n);
                }
                taken.insert(name.clone());
                self.names.insert((kind, rel), name);
            }
        }
    }

    /// The scene or prefab's objects, or None if it isn't text-serialized.
    fn load_doc(&mut self, rel: &str) -> Option<Rc<Doc>> {
        if let Some(d) = self.docs.get(rel) {
            return d.clone();
        }
        let bytes = fs::read(self.assets.join(rel)).unwrap_or_default();
        let text = String::from_utf8_lossy(&bytes);
        let doc = if !text.starts_with("%YAML") {
            self.warn(format!("{} is binary-serialized. Set Edit > Project Settings > Editor > Asset Serialization to Force Text in Unity, then import again.", rel));
            None
        } else {
            Some(Rc::new(parse_unity_yaml(&text)))
        };
        self.docs.insert(rel.to_string(), doc.clone());
        doc
    }

    // -- references --

    fn asset_of(&self, r: Option<&Value>) -> Option<String> {
        let g = r?.get("guid")?;
        let key = match g {
            Value::Str(s) => s.clone(),
            other => other.to_text(),
        };
        self.guid_path.get(&key).cloned()
    }

    fn model_path(&mut self, rel: &str) -> String {
        let ext = ext_of(rel);
        self.used.insert(rel.to_string());
        if ext == MESH_ASSET_EXT {
            return format!("assets/{}", with_ext(rel, ".obj"));
        }
        if CONVERT_MODEL_EXT.contains(&ext.as_str()) {
            if !self.to_convert.iter().any(|r| r == rel) {
                self.to_convert.push(rel.to_string());
            }
            return format!("assets/{}", with_ext(rel, ".glb"));
        }
        format!("assets/{}", rel)
    }

    /// The model path for a Mesh saved inside the scene or prefab file itself, or None.
    fn embedded_mesh(&mut self, doc_rel: &str, doc: &Doc, file_id: i64) -> Option<String> {
        let obj = doc.get(file_id)?;
        if obj.class_id != 43 {
            return None;
        }
        let key = (doc_rel.to_string(), file_id);
        if !self.embedded.contains_key(&key) {
            let name = sanitize(obj.data.get("m_Name").and_then(|v| v.str()).filter(|s| !s.is_empty()).unwrap_or("mesh"));
            let docname = sanitize(&stem_of(doc_rel));
            self.embedded.insert(key.clone(), (format!("meshes/{}/{}_{}.obj", docname, name, file_id), obj.data.clone()));
        }
        Some(format!("assets/{}", self.embedded[&key].0))
    }

    // -- one document -> a tree of nodes --

    fn build_nodes(&mut self, doc: &Doc, doc_rel: &str, depth: usize) -> Built {
        let mut anchors: HashMap<i64, usize> = HashMap::new();
        let mut index: HashMap<i64, Hit> = HashMap::new();
        let mut by_go: HashMap<i64, usize> = HashMap::new();
        for go in doc.objs.iter().filter(|o| o.class_id == 1) {
            let comps: Vec<i64> = go
                .data
                .get("m_Component")
                .map(|c| c.list())
                .unwrap_or(&[])
                .iter()
                .filter_map(|c| c.get("component").and_then(|c| c.get("fileID")).and_then(|v| v.i64()))
                .collect();
            let name = go.data.get("m_Name").and_then(|v| v.str()).filter(|s| !s.is_empty()).unwrap_or("GameObject").to_string();
            let node = self.new_node(&name);
            if go.data.get("m_IsActive").and_then(|v| v.i64()).unwrap_or(1) == 0 {
                self.nodes[node].properties.push(("active".into(), "false".into()));
            }
            if let Some(tag) = go.data.get("m_TagString").and_then(|v| v.str()) {
                if !tag.is_empty() && tag != "Untagged" {
                    self.nodes[node].properties.push(("tag".into(), tag.to_string()));
                }
            }
            if let Some(layer) = go.data.get("m_Layer").and_then(|v| v.i64()) {
                if layer != 0 {
                    self.nodes[node].properties.push(("layer".into(), layer.to_string()));
                }
            }
            let comp_objs: Vec<&UnityObject> = comps.iter().filter_map(|c| doc.get(*c)).collect();
            self.fill_node(node, &comp_objs, doc, doc_rel);
            by_go.insert(go.file_id, node);
            index.insert(go.file_id, Hit { node, class_id: 1, type_name: "GameObject".into(), script: String::new() });
            for c in &comp_objs {
                let script = if c.class_id == 114 { self.script_name(c) } else { String::new() };
                index.insert(c.file_id, Hit { node, class_id: c.class_id, type_name: c.type_name.clone(), script });
            }
            self.count("GameObjects", 1);
        }
        let transforms: Vec<&UnityObject> = doc.objs.iter().filter(|o| (o.class_id == 4 || o.class_id == 224) && !o.stripped).collect();
        for tr in &transforms {
            let go_id = tr.data.get("m_GameObject").and_then(|g| g.get("fileID")).and_then(|v| v.i64());
            let Some(node) = go_id.and_then(|g| by_go.get(&g)).copied() else { continue };
            let n = &mut self.nodes[node];
            n.pos = vecn(tr.data.get("m_LocalPosition"), &["x", "y", "z"], [0.0; 3]);
            n.rot = vecn(tr.data.get("m_LocalRotation"), &["x", "y", "z", "w"], [0.0, 0.0, 0.0, 1.0]);
            n.scale = vecn(tr.data.get("m_LocalScale"), &["x", "y", "z"], [1.0; 3]);
            if tr.class_id == 224 {
                self.skipped("UI RectTransform");
            }
            anchors.insert(tr.file_id, node);
        }
        // (node, parent transform fileID, order)
        let mut pending: Vec<(usize, i64, i64)> = Vec::new();
        for tr in &transforms {
            if let Some(&node) = anchors.get(&tr.file_id) {
                let father = tr.data.get("m_Father").and_then(|f| f.get("fileID")).and_then(|v| v.i64()).unwrap_or(0);
                let order = tr.data.get("m_RootOrder").and_then(|v| v.i64()).unwrap_or(0);
                pending.push((node, father, order));
            }
        }
        // The scene's "stripped" stand-ins for a prefab's own objects, by the instance that owns them: anything
        // that points at one (a child's m_Father, an override) means the object inside the expansion.
        let mut stand_ins: HashMap<i64, Vec<&UnityObject>> = HashMap::new();
        for st in doc.objs.iter().filter(|o| o.stripped) {
            if let Some(owner) = st.data.get("m_PrefabInstance").and_then(|p| p.get("fileID")).and_then(|v| v.i64()) {
                stand_ins.entry(owner).or_default().push(st);
            }
        }
        for inst in doc.objs.iter().filter(|o| o.class_id == 1001) {
            let Some(exp) = self.expand_prefab(inst, doc, doc_rel, depth) else { continue };
            for st in stand_ins.get(&inst.file_id).map(|v| v.as_slice()).unwrap_or(&[]) {
                let source = st.data.get("m_CorrespondingSourceObject").and_then(|p| p.get("fileID")).and_then(|v| v.i64()).unwrap_or(0);
                if let Some(hit) = exp.index.get(&source) {
                    index.insert(st.file_id, hit.clone());
                    if st.class_id == 4 || st.class_id == 224 {
                        anchors.insert(st.file_id, hit.node);
                    }
                }
            }
            for r in &exp.roots {
                pending.push((*r, exp.parent, 0));
            }
        }
        pending.sort_by_key(|p| p.2);
        let mut roots = Vec::new();
        for (node, parent_fid, _) in pending {
            let parent = if parent_fid != 0 { anchors.get(&parent_fid).copied() } else { None };
            match parent {
                Some(p) if p != node => self.nodes[p].children.push(node),
                _ => roots.push(node),
            }
        }
        Built { roots, anchors, index }
    }

    fn fill_node(&mut self, node: usize, comps: &[&UnityObject], doc: &Doc, doc_rel: &str) {
        let mesh_filter = comps.iter().find(|c| c.class_id == 33);
        let renderer = comps.iter().find(|c| c.class_id == 23);
        let mut extras: Vec<usize> = Vec::new();
        if let Some(mf) = mesh_filter {
            let r = mf.data.get("m_Mesh").cloned().unwrap_or(Value::Map(vec![]));
            self.set_mesh(node, &r, doc_rel, doc);
            if let Some(rend) = renderer {
                if let Some(first) = rend.data.get("m_Materials").and_then(|m| m.list().first()) {
                    self.set_material(node, first);
                }
            }
        } else if renderer.is_some() {
            self.skipped("renderer without a mesh");
        }
        for c in comps {
            match c.class_id {
                108 => {
                    if let Some(light) = self.light_node(node, c) {
                        if self.nodes[node].kind == "empty" {
                            let l = self.nodes[light].clone();
                            let n = &mut self.nodes[node];
                            n.kind = l.kind;
                            n.color = l.color;
                            n.intensity = l.intensity;
                            n.range = l.range;
                            n.spot_angle = l.spot_angle;
                        } else {
                            self.nodes[light].name = format!("{} (light)", self.nodes[node].name);
                            extras.push(light);
                        }
                    }
                }
                65 => {
                    if c.data.get("m_IsTrigger").map(|v| v.truthy()).unwrap_or(false) {
                        let t = self.new_node(&format!("{} (trigger)", self.nodes[node].name));
                        self.nodes[t].kind = "trigger".into();
                        self.nodes[t].pos = vecn(c.data.get("m_Center"), &["x", "y", "z"], [0.0; 3]);
                        self.nodes[t].scale = vecn(c.data.get("m_Size"), &["x", "y", "z"], [1.0; 3]);
                        extras.push(t);
                        self.count("trigger volumes", 1);
                    } else if self.nodes[node].kind == "model" {
                        // A model is solid by its triangles; a dense one is slow to walk on. Unity's collider is the
                        // usual answer to that, so ask for one box around the model (its size, not the collider's own).
                        self.set_property(node, "collider", "box");
                        self.count("solid BoxColliders on models (collider=box: one box around the whole model)", 1);
                    } else {
                        self.skipped("BoxCollider (solid: shapes and models are solid already)");
                    }
                }
                20 => {
                    let fov = c.data.get("field of view").and_then(|v| v.f64()).unwrap_or(60.0);
                    self.nodes[node].properties.push(("camera_fov".into(), crate::yaml::py_float(rnd(fov, 3))));
                    self.count("cameras (kept as properties on an empty)", 1);
                }
                114 => self.script_properties(node, c),
                4 | 224 | 1 | 33 | 23 => {}
                _ => self.skipped(&c.type_name.clone()),
            }
        }
        self.nodes[node].children.extend(extras);
    }

    /// Make `node` the shape or model a MeshFilter's m_Mesh reference points at.
    fn set_mesh(&mut self, node: usize, r: &Value, doc_rel: &str, doc: &Doc) {
        let guid = match r.get("guid") {
            Some(Value::Str(s)) => s.clone(),
            Some(other) => other.to_text(),
            None => String::new(),
        };
        let file_id = r.get("fileID").and_then(|v| v.i64()).unwrap_or(0);
        if guid == BUILTIN_GUID {
            let shape = match file_id {
                10202 => "box",
                10206 => "cylinder",
                10207 => "sphere",
                10208 => "capsule",
                10209 => "plane",
                10210 => "quad",
                _ => "",
            };
            match shape {
                "box" | "sphere" | "cylinder" | "plane" => {
                    let n = &mut self.nodes[node];
                    n.kind = shape.into();
                    n.model.clear();
                    n.tintable = true;
                    if shape == "cylinder" {
                        n.properties.push(("unity_scale_note".into(), "cylinder: height doubled to match Unity's 2 m cylinder".into()));
                    }
                }
                "capsule" => {
                    let n = &mut self.nodes[node];
                    n.kind = "cylinder".into();
                    n.model.clear();
                    n.tintable = true;
                    n.properties.push(("unity_scale_note".into(), "capsule: imported as a cylinder".into()));
                    self.warn("A Unity capsule was imported as a cylinder of the same height and width.");
                }
                _ => {
                    let what = if shape.is_empty() { file_id.to_string() } else { shape.to_string() };
                    self.warn(format!("Built-in Unity mesh {} has no Thistle shape; the object was kept as an empty.", what));
                    return;
                }
            }
            self.count("built-in shapes", 1);
            return;
        }
        if guid.is_empty() && file_id != 0 {
            let Some(path) = self.embedded_mesh(doc_rel, doc, file_id) else {
                self.warn(format!("{}: a mesh reference to object {} that isn't a Mesh saved in the same file; the object was kept as an empty.", doc_rel, file_id));
                return;
            };
            let n = &mut self.nodes[node];
            n.kind = "model".into();
            n.model = path;
            n.tintable = true;
            self.count("models", 1);
            return;
        }
        let Some(rel) = self.asset_of(Some(r)) else {
            self.warn("A mesh reference points at a file that isn't in Assets/ (a package or a missing asset); the object was kept as an empty.");
            return;
        };
        let path = self.model_path(&rel);
        let n = &mut self.nodes[node];
        n.kind = "model".into();
        n.model = path;
        n.tintable = rel.to_lowercase().ends_with(MESH_ASSET_EXT); // a converted mesh has no material of its own; a glTF/FBX model does
        self.count("models", 1);
        if file_id != 0 && file_id != 4300000 {
            self.warn(format!("{}: an object uses one mesh inside this file; Thistle draws the whole model there.", rel));
        }
    }

    fn set_material(&mut self, node: usize, r: &Value) {
        let Some(rel) = self.asset_of(Some(r)) else { return };
        if !rel.ends_with(".mat") {
            return;
        }
        self.used.insert(rel.clone());
        let info = self.read_material(&rel);
        if let Some(info) = info {
            if self.nodes[node].tintable {
                let n = &mut self.nodes[node];
                n.color = info.color;
                let mut look = Look::default();
                look.texture = info.texture.clone();
                look.normal_texture = info.normal_texture.clone();
                look.metallic = info.metallic;
                look.roughness = info.roughness;
                look.uv_scale = info.uv_scale;
                look.metallic_set = info.metallic_set;
                n.look = look;
            }
        }
    }

    fn light_node(&mut self, node: usize, c: &UnityObject) -> Option<usize> {
        let kind = c.data.get("m_Type").and_then(|v| v.i64()).unwrap_or(2);
        let col = color(c.data.get("m_Color"));
        let intensity = c.data.get("m_Intensity").and_then(|v| v.f64()).unwrap_or(1.0);
        if kind == 1 {
            let shadows = c.data.get("m_Shadows").and_then(|s| s.get("m_Type")).and_then(|v| v.i64()).unwrap_or(0) != 0;
            self.env.sun = Some(Sun { color: col, intensity: intensity * self.sun_scale, node, shadows });
            self.nodes[node].sun_only = true;
            self.count("directional lights (became the scene's sun)", 1);
            return None;
        }
        let name = self.nodes[node].name.clone();
        let light = self.new_node(&name);
        if kind == 3 {
            self.warn("A Unity area light was imported as a point light.");
        }
        let n = &mut self.nodes[light];
        n.kind = if kind == 0 { "spot_light".into() } else { "point_light".into() };
        n.color = col;
        n.intensity = intensity * self.light_scale;
        n.range = c.data.get("m_Range").and_then(|v| v.f64()).unwrap_or(10.0);
        n.spot_angle = c.data.get("m_SpotAngle").and_then(|v| v.f64()).unwrap_or(60.0).to_radians() / 2.0;
        self.count("point and spot lights", 1);
        Some(light)
    }

    fn script_name(&self, c: &UnityObject) -> String {
        match self.asset_of(c.data.get("m_Script")) {
            Some(s) => stem_of(&s),
            None => "MonoBehaviour".into(),
        }
    }

    fn script_properties(&mut self, node: usize, c: &UnityObject) {
        let name = self.script_name(c);
        if !self.nodes[node].properties.iter().any(|p| p.0 == "unity_script" && p.1 == name) {
            self.nodes[node].properties.push(("unity_script".into(), name.clone()));
        }
        for (key, value) in c.data.entries() {
            if key.starts_with("m_") || key == "serializedVersion" || matches!(value, Value::Map(_) | Value::List(_) | Value::Null) {
                continue;
            }
            self.set_property(node, &format!("{}.{}", name, key), value.to_text());
        }
        self.skipped(&format!("C# script {}", name));
    }

    fn render_settings(&mut self, d: &Value) {
        if d.get("m_Fog").map(|v| v.truthy()).unwrap_or(false) {
            let mode = d.get("m_FogMode").and_then(|v| v.i64()).unwrap_or(3);
            let mut start = d.get("m_LinearFogStart").and_then(|v| v.f64()).unwrap_or(0.0);
            let mut end = d.get("m_LinearFogEnd").and_then(|v| v.f64()).unwrap_or(300.0);
            if mode != 1 {
                let density = d.get("m_FogDensity").and_then(|v| v.f64()).unwrap_or(0.01).max(1e-4);
                start = 0.0;
                end = 4.6 / density;
                self.warn("Exponential fog was approximated as linear fog that reaches full strength where Unity's fades to 1%.");
            }
            self.env.fog = Some((start, end, color(d.get("m_FogColor"))));
        }
        let mode = d.get("m_AmbientMode").and_then(|v| v.i64()).unwrap_or(0);
        let ambient = d.get("m_AmbientIntensity").and_then(|v| v.f64()).unwrap_or(1.0) * self.ambient_scale;
        if mode == 1 {
            self.env.sky = Some((color(d.get("m_AmbientSkyColor")), color(d.get("m_AmbientEquatorColor")), color(d.get("m_AmbientGroundColor"))));
            self.env.ambient = Some(ambient);
        } else if mode == 3 {
            let flat = color(d.get("m_AmbientSkyColor"));
            self.env.sky = Some((flat, flat, flat));
            self.env.ambient = Some(ambient);
        } else if d.get("m_SkyboxMaterial").and_then(|m| m.get("fileID")).map(|v| v.truthy()).unwrap_or(false) {
            self.warn("The scene uses a skybox material; Thistle's sky colors were left at their defaults (set world.sky.skybox in code).");
        }
        self.count("render settings", 1);
    }

    // -- prefabs --

    fn expand_prefab(&mut self, inst: &UnityObject, doc: &Doc, doc_rel: &str, depth: usize) -> Option<Expansion> {
        let src = inst.data.get("m_SourcePrefab").filter(|v| v.truthy()).or_else(|| inst.data.get("m_ParentPrefab"));
        let rel = self.asset_of(src);
        let mod_ = inst.data.get("m_Modification");
        let parent = mod_.and_then(|m| m.get("m_TransformParent")).and_then(|p| p.get("fileID")).and_then(|v| v.i64()).unwrap_or(0);
        let Some(rel) = rel else {
            self.warn("A prefab instance points at a prefab that isn't in Assets/; it was skipped.");
            return None;
        };
        self.used.insert(rel.clone());
        let mut overrides: Overrides = Vec::new();
        for m in mod_.and_then(|m| m.get("m_Modifications")).map(|m| m.list()).unwrap_or(&[]) {
            if !m.is_map() {
                continue;
            }
            let target = m.get("target").and_then(|t| t.get("fileID")).and_then(|v| v.i64()).unwrap_or(0);
            let path = m.get("propertyPath").map(|v| v.to_text()).unwrap_or_default();
            let value = m.get("value").cloned().unwrap_or(Value::Null);
            let oref = m.get("objectReference").cloned().unwrap_or(Value::Null);
            let slot = match overrides.iter().position(|(t, _)| *t == target) {
                Some(i) => i,
                None => {
                    overrides.push((target, Vec::new()));
                    overrides.len() - 1
                }
            };
            let props = &mut overrides[slot].1;
            match props.iter().position(|p| p.0 == path) {
                Some(i) => props[i] = (path, value, oref),
                None => props.push((path, value, oref)),
            }
        }
        let ext = ext_of(&rel);
        if ext == ".prefab" {
            if depth > 8 {
                self.warn(format!("Prefab nesting deeper than 8 levels at {}; the inner part was skipped.", rel));
                return None;
            }
            let sub = self.load_doc(&rel)?;
            let built = self.build_nodes(&sub, &rel, depth + 1);
            self.apply_overrides(&overrides, &built.index, &rel, doc, doc_rel);
            self.count("prefab instances", 1);
            return Some(Expansion { roots: built.roots, anchors: built.anchors, parent, index: built.index });
        }
        if MODEL_EXT.contains(&ext.as_str()) || CONVERT_MODEL_EXT.contains(&ext.as_str()) {
            let node = self.new_node(&stem_of(&rel));
            let path = self.model_path(&rel);
            self.nodes[node].kind = "model".into();
            self.nodes[node].model = path;
            // An imported model's own objects have fileIDs we can't see; its root is 100000 (object) and 400000 (transform).
            let mut index = HashMap::new();
            index.insert(100000, Hit { node, class_id: 1, type_name: "GameObject".into(), script: String::new() });
            index.insert(400000, Hit { node, class_id: 4, type_name: "Transform".into(), script: String::new() });
            self.apply_overrides(&overrides, &index, &rel, doc, doc_rel);
            self.count("models", 1);
            let mut anchors = HashMap::new();
            anchors.insert(400000, node);
            return Some(Expansion { roots: vec![node], anchors, parent, index });
        }
        self.warn(format!("A prefab instance of {} ({}) was skipped.", rel, ext));
        None
    }

    /// Apply a prefab instance's property overrides to whatever object each one names, wherever it is in the prefab.
    fn apply_overrides(&mut self, overrides: &Overrides, index: &HashMap<i64, Hit>, rel: &str, doc: &Doc, doc_rel: &str) {
        for (target, props) in overrides {
            let Some(hit) = index.get(target) else {
                for (path, _, _) in props {
                    *self.unresolved.entry(format!("{}.{}", stem_of(rel), path)).or_insert(0) += 1;
                }
                continue;
            };
            for (path, value, oref) in props {
                if !self.apply_override(hit, path, value, oref, doc, doc_rel) {
                    *self.ignored.entry(format!("{}.{}", hit.type_name, path)).or_insert(0) += 1;
                }
            }
        }
    }

    fn apply_override(&mut self, hit: &Hit, path: &str, value: &Value, oref: &Value, doc: &Doc, doc_rel: &str) -> bool {
        let node = hit.node;
        let cls = hit.class_id;
        if cls == 1 && path == "m_Name" {
            self.nodes[node].name = value.to_text();
            return true;
        }
        if cls == 1 && path == "m_IsActive" {
            if value.i64() == Some(0) {
                self.set_property(node, "active", "false");
            } else {
                self.nodes[node].properties.retain(|p| p.0 != "active");
            }
            return true;
        }
        if cls == 4 || cls == 224 {
            for (prefix, size) in [("m_LocalPosition", 3usize), ("m_LocalRotation", 4), ("m_LocalScale", 3)] {
                if let Some(axis) = path.strip_prefix(prefix).and_then(|s| s.strip_prefix('.')) {
                    let idx = match axis {
                        "x" => 0,
                        "y" => 1,
                        "z" => 2,
                        "w" => 3,
                        _ => return false,
                    };
                    if idx >= size {
                        return false;
                    }
                    let Some(v) = value.f64() else { return false };
                    let n = &mut self.nodes[node];
                    match prefix {
                        "m_LocalPosition" => n.pos[idx] = v,
                        "m_LocalRotation" => n.rot[idx] = v,
                        _ => n.scale[idx] = v,
                    }
                    return true;
                }
            }
            return false;
        }
        if cls == 33 && path == "m_Mesh" && oref.is_map() {
            self.set_mesh(node, oref, doc_rel, doc);
            return true;
        }
        if cls == 23 && path.starts_with("m_Materials.Array.data[") && oref.is_map() {
            if path == "m_Materials.Array.data[0]" {
                self.set_material(node, oref);
            }
            return true;
        }
        if cls == 114 && !path.starts_with("m_") && !path.starts_with("Array") {
            self.set_property(node, &format!("{}.{}", hit.script, path), value.to_text());
            return true;
        }
        false
    }

    // -- materials --

    fn read_material(&mut self, rel: &str) -> Option<MatInfo> {
        if let Some(m) = self.mats.get(rel) {
            return m.clone();
        }
        let info = self.read_material_uncached(rel);
        self.mats.insert(rel.to_string(), info.clone());
        info
    }

    fn read_material_uncached(&mut self, rel: &str) -> Option<MatInfo> {
        let doc = self.load_doc(rel)?;
        let mat = doc.objs.iter().find(|o| o.class_id == 21)?;
        let saved = mat.data.get("m_SavedProperties");
        let mut colors: HashMap<String, Value> = HashMap::new();
        let mut floats: HashMap<String, f64> = HashMap::new();
        if let Some(s) = saved {
            for e in s.get("m_Colors").map(|c| c.list()).unwrap_or(&[]) {
                for (k, v) in e.entries() {
                    colors.insert(k.clone(), v.clone());
                }
            }
            for e in s.get("m_Floats").map(|c| c.list()).unwrap_or(&[]) {
                for (k, v) in e.entries() {
                    if let Some(x) = v.f64() {
                        floats.insert(k.clone(), x);
                    }
                }
            }
        }
        let mut textures: HashMap<String, String> = HashMap::new();
        let mut scales: HashMap<String, [f64; 2]> = HashMap::new();
        if let Some(s) = saved {
            for e in s.get("m_TexEnvs").map(|c| c.list()).unwrap_or(&[]) {
                for (key, tex) in e.entries() {
                    let Some(path) = self.asset_of(tex.get("m_Texture")) else { continue };
                    if !TEXTURE_EXT.contains(&ext_of(&path).as_str()) {
                        let e = ext_of(&path);
                        self.warn(format!("{}: texture {} is a {} file, which Thistle can't load; convert it to PNG.", rel, path, e));
                        continue;
                    }
                    textures.insert(key.clone(), format!("assets/{}", path));
                    scales.insert(key.clone(), vecn(tex.get("m_Scale"), &["x", "y"], [1.0, 1.0]));
                    self.used.insert(path);
                }
            }
        }
        let base = colors.get("_BaseColor").or_else(|| colors.get("_Color"));
        let smooth = floats.get("_Smoothness").or_else(|| floats.get("_Glossiness")).copied().unwrap_or(0.5);
        let mode_f = floats.get("_Mode").copied().unwrap_or(0.0) as i64;
        let surface = if floats.contains_key("_Surface") {
            floats["_Surface"] as i64
        } else {
            match mode_f {
                0 => 0,
                1 => 1,
                _ => 2,
            }
        };
        let cutout = mode_f == 1 || floats.get("_AlphaClip").copied().unwrap_or(0.0) == 1.0;
        let main_tex = textures.get("_BaseMap").or_else(|| textures.get("_MainTex")).cloned();
        let metallic = floats.get("_Metallic").copied().unwrap_or(0.0);
        let em = vecn(colors.get("_EmissionColor"), &["r", "g", "b", "a"], [0.0, 0.0, 0.0, 1.0]);
        Some(MatInfo {
            name: mat.data.get("m_Name").map(|v| v.to_text()).unwrap_or_else(|| stem_of(rel)),
            source: rel.to_string(),
            shader: mat.data.get("m_Shader").and_then(|s| s.get("guid")).map(|v| v.to_text()).unwrap_or_default(),
            color: base.map(|b| color(Some(b))).unwrap_or([1.0; 4]),
            metallic,
            roughness: rnd(1.0 - smooth, 4),
            emissive: [em[0], em[1], em[2]],
            alpha: if cutout { "cutout".into() } else if surface != 0 { "blend".into() } else { "opaque".into() },
            alpha_cutoff: floats.get("_Cutoff").copied().unwrap_or(0.5),
            texture: main_tex.clone(),
            normal_texture: textures.get("_BumpMap").cloned(),
            metallic_roughness_texture: textures.get("_MetallicGlossMap").cloned(),
            occlusion_texture: textures.get("_OcclusionMap").cloned(),
            emissive_texture: textures.get("_EmissionMap").cloned(),
            uv_scale: if main_tex.is_some() {
                Some(scales.get("_BaseMap").or_else(|| scales.get("_MainTex")).copied().unwrap_or([1.0, 1.0]))
            } else {
                None
            },
            metallic_set: metallic >= 0.05 || textures.contains_key("_BumpMap"),
        })
    }

    fn material_json(m: &MatInfo) -> J {
        let opt = |s: &Option<String>| s.as_ref().map(|x| J::from(x.as_str())).unwrap_or(J::Null);
        json!({
            "name": m.name, "source": m.source, "shader": m.shader,
            "color": nums(&m.color, 6),
            "metallic": m.metallic, "roughness": m.roughness,
            "emissive": nums(&m.emissive, 6),
            "alpha": m.alpha, "alpha_cutoff": m.alpha_cutoff,
            "texture": opt(&m.texture), "normal_texture": opt(&m.normal_texture),
            "uv_scale": m.uv_scale.map(|u| nums(&u, 6)).unwrap_or(J::Null),
            "metallic_roughness_texture": opt(&m.metallic_roughness_texture),
            "occlusion_texture": opt(&m.occlusion_texture), "emissive_texture": opt(&m.emissive_texture),
        })
    }

    // -- flattening and writing --

    fn flatten(&self, roots: &[usize], parent: i64, out: &mut Vec<(usize, i64)>) {
        for &n in roots {
            let node = &self.nodes[n];
            if node.sun_only && node.kind == "empty" && node.children.is_empty() && node.properties.is_empty() {
                continue;
            }
            out.push((n, parent));
            let index = out.len() as i64 - 1;
            self.flatten(&node.children.clone(), index, out);
        }
    }

    fn entity_json(&self, n: usize, parent: i64) -> J {
        let node = &self.nodes[n];
        let mut scale = node.scale;
        if node.kind == "cylinder" {
            scale[1] *= 2.0;
        } else if node.kind == "plane" {
            scale[0] *= 10.0;
            scale[2] *= 10.0;
        }
        let mut e = Map::new();
        e.insert("name".into(), J::from(node.name.as_str()));
        e.insert("kind".into(), J::from(node.kind.as_str()));
        e.insert("parent".into(), J::from(parent));
        e.insert("position".into(), nums(&mirror_pos(node.pos), 6));
        e.insert("rotation".into(), nums(&mirror_rot(node.rot), 6));
        e.insert("scale".into(), nums(&scale, 6));
        e.insert("color".into(), nums(&node.color, 6));
        if node.kind == "model" {
            e.insert("model".into(), J::from(node.model.as_str()));
        }
        let look = &node.look;
        if let Some(t) = &look.texture {
            e.insert("texture".into(), J::from(t.as_str()));
        }
        if let Some(t) = &look.normal_texture {
            e.insert("normal_texture".into(), J::from(t.as_str()));
        }
        if look.metallic_set {
            e.insert("metallic".into(), J::from(rnd(look.metallic, 4)));
            e.insert("roughness".into(), J::from(rnd(look.roughness, 4)));
        }
        if let Some(uv) = look.uv_scale {
            if uv != [1.0, 1.0] {
                e.insert("uv_scale".into(), nums(&uv, 4));
            }
        }
        if node.kind == "point_light" || node.kind == "spot_light" {
            e.insert("intensity".into(), J::from(rnd(node.intensity, 6)));
            e.insert("range".into(), J::from(rnd(node.range, 6)));
        }
        if node.kind == "spot_light" {
            e.insert("spot_angle".into(), J::from(rnd(node.spot_angle, 6)));
        }
        let props: Vec<J> = node.properties.iter().filter(|p| p.0 != "unity_scale_note").map(|p| json!([p.0, p.1])).collect();
        if !props.is_empty() {
            e.insert("properties".into(), J::Array(props));
        }
        J::Object(e)
    }

    fn scene_json(&self, roots: &[usize]) -> J {
        let mut flat = Vec::new();
        self.flatten(roots, -1, &mut flat);
        let env = &self.env;
        let mut fog = json!({"enabled": false, "start": 30, "end": 150, "color": [0.7, 0.8, 0.9, 1], "match_sky": true});
        let mut sky = json!({"top": [0.35, 0.55, 0.9, 1], "horizon": [0.75, 0.85, 0.95, 1], "ground": [0.35, 0.33, 0.3, 1], "visible": true, "sun_disc": true});
        let mut sun = json!({"direction": [-0.35, -1.0, -0.45], "color": [1.0, 0.96, 0.88, 1], "intensity": 1.0, "shadows": true, "shadow_distance": 60.0, "shadow_strength": 1.0});
        if let Some((start, end, c)) = env.fog {
            fog = json!({"enabled": true, "start": start, "end": end, "color": nums(&c, 4), "match_sky": false});
        }
        if let Some((t, h, g)) = env.sky {
            sky["top"] = nums(&t, 6);
            sky["horizon"] = nums(&h, 6);
            sky["ground"] = nums(&g, 6);
        }
        if let Some(s) = &env.sun {
            let node = &self.nodes[s.node];
            let forward = qrot(qmul(node.world_parent_rot, node.rot), [0.0, 0.0, 1.0]);
            sun["direction"] = nums(&mirror_pos(forward), 6);
            sun["color"] = nums(&s.color, 4);
            sun["intensity"] = J::from(s.intensity);
            sun["shadows"] = J::from(s.shadows);
        }
        let entities: Vec<J> = flat.iter().map(|(n, p)| self.entity_json(*n, *p)).collect();
        json!({
            "thistle_scene": 1,
            "environment": {"ambient": env.ambient.unwrap_or(0.55), "fog": fog, "sky": sky, "sun": sun},
            "entities": entities,
        })
    }

    fn write_json(&mut self, path: &Path, data: &J) -> bool {
        if path.exists() && !self.force {
            let rel = path.strip_prefix(&self.out).unwrap_or(path).to_string_lossy().replace('\\', "/");
            self.warn(format!("{} already exists; kept it (use --force to overwrite).", rel));
            return false;
        }
        if let Some(p) = path.parent() {
            let _ = fs::create_dir_all(p);
        }
        let mut text = serde_json::to_string_pretty(data).unwrap_or_default();
        text.push('\n');
        fs::write(path, text).is_ok()
    }

    fn convert_doc(&mut self, rel: &str, folder: &str, kind: &str, kind_key: &'static str) {
        let Some(doc) = self.load_doc(rel) else { return };
        self.env = Env::default();
        for o in doc.objs.iter().filter(|o| o.class_id == 104) {
            self.render_settings(&o.data);
        }
        let built = self.build_nodes(&doc, rel, 0);
        if let Some(sun_node) = self.env.sun.as_ref().map(|s| s.node) {
            self.give_world_rotation(&built.roots, sun_node, [0.0, 0.0, 0.0, 1.0]);
        }
        let name = self.names.get(&(kind_key, rel.to_string())).cloned().unwrap_or_else(|| sanitize(&stem_of(rel)));
        let dest = self.out.join("assets").join(folder).join(format!("{}.scene.json", name));
        let data = self.scene_json(&built.roots);
        if self.write_json(&dest, &data) {
            self.count(kind, 1);
        }
    }

    fn give_world_rotation(&mut self, roots: &[usize], target: usize, rot: [f64; 4]) -> bool {
        for &n in roots {
            self.nodes[n].world_parent_rot = rot;
            if n == target {
                return true;
            }
            let child_rot = qmul(rot, self.nodes[n].rot);
            let children = self.nodes[n].children.clone();
            if self.give_world_rotation(&children, target, child_rot) {
                return true;
            }
        }
        false
    }

    fn write_mesh(&mut self, text: &str, dest: &Path) {
        if dest.exists() && !self.force {
            let rel = dest.strip_prefix(&self.out).unwrap_or(dest).to_string_lossy().replace('\\', "/");
            self.warn(format!("{} already exists; kept it (use --force to overwrite).", rel));
            return;
        }
        if let Some(p) = dest.parent() {
            let _ = fs::create_dir_all(p);
        }
        if fs::write(dest, text).is_ok() {
            self.count("meshes converted to .obj", 1);
        }
    }

    fn convert_mesh_assets(&mut self) {
        let list: Vec<String> = self.files.iter().filter(|f| ext_of(f) == MESH_ASSET_EXT).cloned().collect();
        for rel in list {
            if !self.only_scenes.is_empty() && !self.used.contains(&rel) {
                continue;
            }
            let bytes = fs::read(self.assets.join(&rel)).unwrap_or_default();
            let text = String::from_utf8_lossy(&bytes);
            if !text.starts_with("%YAML") || !text.contains("--- !u!43 ") {
                continue;
            }
            let doc = parse_unity_yaml(&text);
            for obj in doc.objs.iter().filter(|o| o.class_id == 43) {
                match mesh_asset_to_obj(&obj.data) {
                    Ok((obj_text, _)) => {
                        let dest = self.out.join("assets").join(with_ext(&rel, ".obj"));
                        self.write_mesh(&obj_text, &dest);
                    }
                    Err(why) => self.warn(format!("Mesh {} was not converted: {}.", rel, why)),
                }
            }
        }
        let embedded: Vec<((String, i64), (String, Value))> = self.embedded.iter().map(|(k, v)| (k.clone(), v.clone())).collect();
        for ((doc_rel, file_id), (out_rel, data)) in embedded {
            match mesh_asset_to_obj(&data) {
                Ok((obj_text, _)) => {
                    let dest = self.out.join("assets").join(&out_rel);
                    self.write_mesh(&obj_text, &dest);
                }
                Err(why) => self.warn(format!("A mesh saved inside {} (object {}) was not converted: {}.", doc_rel, file_id, why)),
            }
        }
    }

    /// Files a glTF or OBJ model reads next to itself (buffers, textures, .mtl), Assets-relative.
    fn dependencies(&self, rel: &str) -> Vec<String> {
        let base = parent_of(rel);
        let join = |name: &str| if base.is_empty() { name.to_string() } else { format!("{}/{}", base, name) };
        let mut found: Vec<String> = Vec::new();
        let lower = rel.to_lowercase();
        if lower.ends_with(".gltf") {
            if let Ok(text) = fs::read_to_string(self.assets.join(rel)) {
                if let Ok(doc) = serde_json::from_str::<J>(&text) {
                    for key in ["buffers", "images"] {
                        for entry in doc.get(key).and_then(|v| v.as_array()).cloned().unwrap_or_default() {
                            if let Some(uri) = entry.get("uri").and_then(|v| v.as_str()) {
                                if !uri.is_empty() && !uri.starts_with("data:") {
                                    found.push(join(uri));
                                }
                            }
                        }
                    }
                }
            }
        } else if lower.ends_with(".obj") {
            if let Ok(text) = fs::read_to_string(self.assets.join(rel)) {
                for line in text.lines() {
                    if let Some(name) = line.strip_prefix("mtllib ") {
                        let mtl = join(name.trim());
                        found.push(mtl.clone());
                        if let Ok(mtl_text) = fs::read_to_string(self.assets.join(&mtl)) {
                            let mtl_base = parent_of(&mtl).to_string();
                            for l in mtl_text.lines() {
                                let low = l.trim().to_lowercase();
                                if low.starts_with("map_kd") || low.starts_with("map_bump") || low.starts_with("map_ke") || low.starts_with("bump") {
                                    if let Some(last) = l.split_whitespace().last() {
                                        found.push(if mtl_base.is_empty() { last.to_string() } else { format!("{}/{}", mtl_base, last) });
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        found.into_iter().filter(|f| self.assets.join(f).exists()).collect()
    }

    fn copy_assets(&mut self) {
        let wanted: Option<HashSet<String>> = if self.only_scenes.is_empty() {
            None
        } else {
            let mut w = self.used.clone();
            let deps: Vec<String> = w.iter().flat_map(|r| self.dependencies(r)).collect();
            w.extend(deps);
            Some(w)
        };
        let files = self.files.clone();
        for rel in files {
            let ext = ext_of(&rel);
            if ext == ".meta" || ext == ".unity" || ext == ".prefab" || ext == ".mat" || ext == MESH_ASSET_EXT {
                continue;
            }
            if let Some(w) = &wanted {
                if !w.contains(&rel) {
                    continue;
                }
            }
            if COPY_EXT.contains(&ext.as_str()) || ext == ".otf" {
                let dest = self.out.join("assets").join(&rel);
                if dest.exists() && !self.force {
                    continue;
                }
                if let Some(p) = dest.parent() {
                    let _ = fs::create_dir_all(p);
                }
                if fs::copy(self.assets.join(&rel), &dest).is_ok() {
                    self.count("files copied", 1);
                }
            } else if CONVERT_MODEL_EXT.contains(&ext.as_str()) {
                if !self.to_convert.contains(&rel) {
                    self.to_convert.push(rel.clone());
                }
            } else if wanted.is_none() {
                match ext.as_str() {
                    ".cs" => self.skipped("C# scripts (not translated)"),
                    ".shader" => self.skipped("shaders"),
                    ".anim" => self.skipped("animation clips"),
                    ".controller" => self.skipped("animator controllers"),
                    ".asmdef" | ".asmref" | ".dll" | ".cginc" | ".hlsl" | ".json" | ".txt" | ".md" => {}
                    "" => self.skipped("no extension files"),
                    e => self.skipped(&format!("{} files", e)),
                }
            }
        }
    }

    fn find_converter(&mut self) -> Option<(bool, String)> {
        // (is_fbx2gltf, path)
        fn on_path(name: &str) -> Option<String> {
            let paths = std::env::var_os("PATH")?;
            for dir in std::env::split_paths(&paths) {
                for candidate in [name.to_string(), format!("{}.exe", name)] {
                    let p = dir.join(&candidate);
                    if p.is_file() {
                        return Some(p.to_string_lossy().into_owned());
                    }
                }
            }
            None
        }
        if !self.blender_arg.is_empty() {
            let exe = on_path(&self.blender_arg).or_else(|| if Path::new(&self.blender_arg).exists() { Some(self.blender_arg.clone()) } else { None });
            let Some(exe) = exe else {
                let a = self.blender_arg.clone();
                self.warn(format!("--blender '{}' isn't a program or a file.", a));
                return None;
            };
            let is_fbx = Path::new(&exe).file_name().map(|n| n.to_string_lossy().to_lowercase().contains("fbx2gltf")).unwrap_or(false);
            return Some((is_fbx, exe));
        }
        if let Some(p) = on_path("FBX2glTF").or_else(|| on_path("fbx2gltf")) {
            return Some((true, p));
        }
        if let Ok(b) = std::env::var("BLENDER") {
            if !b.is_empty() {
                return Some((false, b));
            }
        }
        if let Some(p) = on_path("blender") {
            return Some((false, p));
        }
        let mut candidates: Vec<String> = Vec::new();
        if let Ok(dir) = fs::read_dir("C:\\Program Files\\Blender Foundation") {
            let mut versions: Vec<PathBuf> = dir.flatten().map(|e| e.path()).collect();
            versions.sort();
            for v in versions.into_iter().rev() {
                candidates.push(v.join("blender.exe").to_string_lossy().into_owned());
            }
        }
        candidates.push("/Applications/Blender.app/Contents/MacOS/Blender".into());
        candidates.into_iter().find(|c| Path::new(c).exists()).map(|c| (false, c))
    }

    /// Run each needed .fbx through Blender (or FBX2glTF) into the .glb the scenes already point at.
    fn convert_models(&mut self) {
        let mut todo: Vec<String> = self.to_convert.iter().filter(|r| r.to_lowercase().ends_with(".fbx")).cloned().collect();
        todo.sort();
        todo.dedup();
        if todo.is_empty() || !self.convert_fbx {
            return;
        }
        let Some((is_fbx2gltf, exe)) = self.find_converter() else {
            self.warn("Blender or FBX2glTF wasn't found, so the .fbx models below were not converted (install Blender, or pass --blender PATH).");
            return;
        };
        let exe_name = Path::new(&exe).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        for rel in todo {
            let src = self.assets.join(&rel);
            let dest = self.out.join("assets").join(with_ext(&rel, ".glb"));
            if dest.exists() && !self.force {
                self.to_convert.retain(|r| r != &rel);
                continue;
            }
            if let Some(p) = dest.parent() {
                let _ = fs::create_dir_all(p);
            }
            let mut cmd = Command::new(&exe);
            if is_fbx2gltf {
                cmd.arg("--binary").arg("--input").arg(&src).arg("--output").arg(dest.with_extension(""));
            } else {
                cmd.args(["--background", "--factory-startup", "--python-expr", BLENDER_FBX_TO_GLB, "--"]).arg(&src).arg(&dest);
            }
            let result = cmd.output();
            match result {
                Err(why) => {
                    self.warn(format!("{}: couldn't run {}: {}", rel, exe_name, why));
                    continue;
                }
                Ok(r) => {
                    if !r.status.success() || !dest.exists() {
                        let all = format!("{}{}", String::from_utf8_lossy(&r.stdout), String::from_utf8_lossy(&r.stderr));
                        let lines: Vec<&str> = all.trim().lines().collect();
                        let tail = lines[lines.len().saturating_sub(3)..].join(" | ");
                        let tail: String = tail.chars().take(300).collect();
                        self.warn(format!("{}: {} didn't produce a .glb ({})", rel, exe_name, tail));
                        continue;
                    }
                }
            }
            self.to_convert.retain(|r| r != &rel);
            self.count(&format!("FBX models converted to .glb with {} (animations kept)", if is_fbx2gltf { "FBX2glTF" } else { "Blender" }), 1);
        }
    }

    fn pick_scenes(&self, scenes: &[String]) -> Option<Vec<String>> {
        if self.only_scenes.is_empty() {
            return Some(scenes.to_vec());
        }
        let mut chosen: Vec<String> = Vec::new();
        for want in &self.only_scenes {
            let w = want.to_lowercase().replace('\\', "/");
            let w = w.strip_suffix(".unity").unwrap_or(&w).to_string();
            let hits: Vec<&String> = scenes
                .iter()
                .filter(|s| {
                    let l = s.to_lowercase();
                    let l = l.strip_suffix(".unity").unwrap_or(&l).to_string();
                    stem_of(s).to_lowercase() == w || l == w || l.ends_with(&format!("/{}", w))
                })
                .collect();
            if hits.is_empty() {
                println!("error: no scene matches --scene '{}'. Scenes in this project:", want);
                for s in scenes.iter().take(60) {
                    println!("  {}", stem_of(s));
                }
                if scenes.len() > 60 {
                    println!("  ... and {} more", scenes.len() - 60);
                }
                return None;
            }
            for h in hits {
                if !chosen.contains(h) {
                    chosen.push(h.clone());
                }
            }
        }
        Some(chosen)
    }

    pub fn run(&mut self) -> i32 {
        if !self.assets.is_dir() {
            println!("error: {} has no Assets/ folder - is it a Unity project?", self.project.display());
            return 1;
        }
        let mut files = Vec::new();
        list_files(&self.assets, &self.assets, &mut files);
        files.sort();
        self.files = files;
        self.scan_guids();
        self.assign_names();
        let all_scenes: Vec<String> = self.files.iter().filter(|f| ext_of(f) == ".unity").cloned().collect();
        let Some(scenes) = self.pick_scenes(&all_scenes) else { return 1 };
        for scene in &scenes {
            self.convert_doc(scene, "scenes", "scenes", "scene");
        }
        let prefabs: Vec<String> = self.files.iter().filter(|f| ext_of(f) == ".prefab").cloned().collect();
        for rel in prefabs {
            if self.only_scenes.is_empty() || self.used.contains(&rel) {
                self.convert_doc(&rel, "prefabs", "prefabs", "prefab");
            }
        }
        let mats: Vec<String> = self.files.iter().filter(|f| ext_of(f) == ".mat").cloned().collect();
        for rel in mats {
            if !self.only_scenes.is_empty() && !self.used.contains(&rel) {
                continue;
            }
            if let Some(info) = self.read_material(&rel) {
                let name = self.names.get(&("material", rel.clone())).cloned().unwrap_or_default();
                let dest = self.out.join("assets").join("materials").join(format!("{}.json", name));
                if self.write_json(&dest, &Self::material_json(&info)) {
                    self.count("materials", 1);
                }
            }
        }
        self.convert_mesh_assets();
        self.copy_assets();
        self.convert_models();
        self.write_report();
        0
    }

    fn write_report(&mut self) {
        let mut lines: Vec<String> = vec![
            "# Unity import report".into(),
            String::new(),
            format!("From `{}`. See docs/unity-import.md for what this does and doesn't carry over.", self.project.display()),
            String::new(),
        ];
        if !self.only_scenes.is_empty() {
            lines.push(format!("Only the scene(s) {} and the files they use were converted.", self.only_scenes.join(", ")));
            lines.push(String::new());
        }
        lines.push("## Converted".into());
        lines.push(String::new());
        if self.counts.is_empty() {
            lines.push("- nothing".into());
        }
        for (what, n) in &self.counts {
            lines.push(format!("- {} {}", n, what));
        }
        if !self.to_convert.is_empty() {
            lines.push(String::new());
            lines.push("## Models Thistle can't read yet (convert to .glb, same name, same folder under assets/)".into());
            lines.push(String::new());
            let mut list = self.to_convert.clone();
            list.sort();
            list.dedup();
            for p in list {
                lines.push(format!("- `{}` -> `assets/{}`", p, with_ext(&p, ".glb")));
            }
        }
        if !self.skipped_types.is_empty() {
            lines.push(String::new());
            lines.push("## Left behind".into());
            lines.push(String::new());
            for (what, n) in &self.skipped_types {
                lines.push(format!("- {} x {}", n, what));
            }
        }
        let top = |m: &BTreeMap<String, usize>, limit: usize| -> Vec<(String, usize)> {
            let mut v: Vec<(String, usize)> = m.iter().map(|(k, n)| (k.clone(), *n)).collect();
            v.sort_by(|a, b| b.1.cmp(&a.1));
            v.truncate(limit);
            v
        };
        if !self.unresolved.is_empty() {
            lines.push(String::new());
            lines.push("## Prefab overrides that name an object this tool couldn't find in the prefab (not applied)".into());
            lines.push(String::new());
            for (key, n) in top(&self.unresolved, 40) {
                lines.push(format!("- {} x `{}`", n, key));
            }
        }
        if !self.ignored.is_empty() {
            lines.push(String::new());
            lines.push("## Prefab overrides with no Thistle equivalent (ignored)".into());
            lines.push(String::new());
            for (key, n) in top(&self.ignored, 25) {
                lines.push(format!("- {} x `{}`", n, key));
            }
        }
        if !self.warnings.is_empty() {
            lines.push(String::new());
            lines.push("## Warnings".into());
            lines.push(String::new());
            for w in &self.warnings {
                lines.push(format!("- {}", w));
            }
        }
        let text = lines.join("\n") + "\n";
        let _ = fs::write(self.out.join("unity-import-report.md"), &text);
        println!("{}", text);
    }
}
