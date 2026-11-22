"""`thistle import-unity`: turns a Unity project (text-serialized) into Thistle assets.

Standard library only, like the rest of the CLI. Reads Assets/ and writes:
  assets/scenes/<name>.scene.json   from each .unity scene (three::Scene3D files)
  assets/prefabs/<name>.scene.json  from each .prefab, as a scene fragment
  assets/materials/<name>.json      from each .mat (PBR numbers; nothing loads these by itself)
  assets/<same path as in Assets/>  models, textures and sounds Thistle can read, copied as they are
  unity-import-report.md            what was converted, approximated or left behind

Unity is left-handed (+Z forward); Thistle is right-handed like glTF (-Z forward),
so positions and rotations are mirrored across Z on the way through. See docs/unity-import.md
for what's approximate and what has and hasn't been verified.
"""
import json
import math
import re
import shutil
from pathlib import Path

# --- Unity's YAML ----------------------------------------------------------
# Unity writes a small, regular subset of YAML: block mappings and sequences
# (a sequence's dashes sit at its key's indent), flow `{a: 1, b: {c: 2}}` and
# `[]`, and plain or quoted scalars. That's all this reads. A general YAML
# library would be another install for every user of the CLI.

_INT = re.compile(r"^-?\d+$")
_FLOAT = re.compile(r"^-?(\d+\.\d*|\.\d+|\d+)([eE][-+]?\d+)?$")


def _scalar(text: str, key: str = ""):
    text = text.strip()
    if len(text) >= 2 and text[0] == "'" and text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        try:
            return json.loads(text)
        except ValueError:
            return text[1:-1]
    if key == "guid":  # 32 hex digits: "0000000000000000e000000000000000" would read as a float
        return text
    if _INT.match(text):
        return int(text)
    if _FLOAT.match(text):
        return float(text)
    if text in ("~", "null"):
        return None
    return text


def _split_top(text: str) -> list:
    """Splits a flow collection's inside on the commas that aren't nested or quoted."""
    parts, depth, quote, start = [], 0, "", 0
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = ""
        elif ch in "'\"":
            quote = ch
        elif ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    tail = text[start:]
    if tail.strip():
        parts.append(tail)
    return parts


def _key_split(text: str):
    """'key: value' -> (key, value); None when it isn't a mapping entry."""
    quote = ""
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = ""
        elif ch in "'\"":
            quote = ch
        elif ch == ":" and (i + 1 == len(text) or text[i + 1] == " "):
            return text[:i].strip(), text[i + 1:].strip()
        elif ch in "{[" and i == 0:
            return None
    return None


def _flow(text: str, key: str = ""):
    text = text.strip()
    if text.startswith("{") and text.endswith("}"):
        out = {}
        for part in _split_top(text[1:-1]):
            kv = _key_split(part.strip())
            if kv:
                out[kv[0]] = _flow(kv[1], kv[0])
        return out
    if text.startswith("[") and text.endswith("]"):
        return [_flow(p) for p in _split_top(text[1:-1])]
    return _scalar(text, key)


def _block(lines: list, i: int, indent: int):
    text = lines[i][1]
    if text == "-" or text.startswith("- "):
        return _sequence(lines, i, indent)
    if _key_split(text):
        return _mapping(lines, i, indent)
    return _flow(text), i + 1


def _mapping(lines: list, i: int, indent: int):
    out = {}
    while i < len(lines) and lines[i][0] == indent and not (lines[i][1] == "-" or lines[i][1].startswith("- ")):
        kv = _key_split(lines[i][1])
        if kv is None:
            i += 1
            continue
        key, value = kv
        i += 1
        if value:
            out[key] = _flow(value, key)
        elif i < len(lines) and (lines[i][0] > indent or (lines[i][0] == indent and lines[i][1].startswith("- "))):
            out[key], i = _block(lines, i, lines[i][0])
        else:
            out[key] = None
    return out, i


def _sequence(lines: list, i: int, indent: int):
    out = []
    while i < len(lines) and lines[i][0] == indent and (lines[i][1] == "-" or lines[i][1].startswith("- ")):
        rest = lines[i][1][1:].strip()
        if not rest:
            i += 1
            if i < len(lines) and lines[i][0] > indent:
                value, i = _block(lines, i, lines[i][0])
            else:
                value = None
            out.append(value)
        elif not rest.startswith(("{", "[")) and _key_split(rest):
            # "- key: value" starts a mapping whose later keys line up with `key`.
            lines[i] = (indent + 2, rest)
            value, i = _mapping(lines, i, indent + 2)
            out.append(value)
        else:
            out.append(_flow(rest))
            i += 1
    return out, i


class UnityObject:
    def __init__(self, class_id: int, file_id: int, type_name: str, data: dict, stripped: bool):
        self.class_id, self.file_id, self.type_name, self.data, self.stripped = class_id, file_id, type_name, data, stripped


def parse_unity_yaml(text: str) -> dict:
    """{fileID: UnityObject} for every `--- !u!<class> &<fileID>` document in the text."""
    objects = {}
    chunks = re.split(r"^--- !u!(\d+) &(-?\d+)( stripped)?[ \t]*$", text, flags=re.MULTILINE)
    for n in range(1, len(chunks) - 3, 4):
        class_id, file_id, stripped, body = int(chunks[n]), int(chunks[n + 1]), bool(chunks[n + 2]), chunks[n + 3]
        lines = []
        for raw in body.splitlines():
            if not raw.strip() or raw.lstrip().startswith("#"):
                continue
            lines.append((len(raw) - len(raw.lstrip(" ")), raw.strip()))
        if not lines:
            continue
        top, _ = _block(lines, 0, lines[0][0])
        if isinstance(top, dict) and len(top) == 1:
            type_name, data = next(iter(top.items()))
            objects[file_id] = UnityObject(class_id, file_id, type_name, data or {}, stripped)
    return objects


# --- Unity -> Thistle numbers --------------------------------------------------

BUILTIN_GUID = "0000000000000000e000000000000000"
BUILTIN_MESHES = {10202: "box", 10206: "cylinder", 10207: "sphere", 10208: "capsule", 10209: "plane", 10210: "quad"}
MODEL_EXT = {".glb", ".gltf", ".obj"}
CONVERT_MODEL_EXT = {".fbx", ".dae", ".blend", ".3ds", ".dxf", ".max", ".ma", ".mb"}
COPY_EXT = MODEL_EXT | {".mtl", ".bin", ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".wav", ".ogg", ".mp3", ".flac", ".ttf", ".otf"}
VEC_KEYS = {"position": ("m_LocalPosition", 3), "rotation": ("m_LocalRotation", 4), "scale": ("m_LocalScale", 3)}


def _vec(d, names, default):
    if not isinstance(d, dict):
        return list(default)
    return [float(d.get(n, default[i])) for i, n in enumerate(names)]


def _color(d, default=(1.0, 1.0, 1.0, 1.0)):
    return _vec(d, "rgba", default)


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def _qrot(q, v):
    x, y, z, w = q
    # v' = v + 2w(q x v) + 2 q x (q x v)
    cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
    dx, dy, dz = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return [v[0] + 2 * (w * cx + dx), v[1] + 2 * (w * cy + dy), v[2] + 2 * (w * cz + dz)]


def _mirror_pos(p):
    return [p[0], p[1], -p[2]]


def _mirror_rot(q):
    return [-q[0], -q[1], q[2], q[3]]


def _round(values, digits=6):
    return [round(v, digits) + 0.0 for v in values]


class Node:
    """One thing in the scene, still in Unity's space, before the tree is flattened."""

    def __init__(self, name: str):
        self.name = name
        self.kind = "empty"
        self.pos = [0.0, 0.0, 0.0]
        self.rot = [0.0, 0.0, 0.0, 1.0]
        self.scale = [1.0, 1.0, 1.0]
        self.color = [1.0, 1.0, 1.0, 1.0]
        self.model = ""
        self.intensity = 1.0
        self.range = 10.0
        self.spot_angle = math.radians(30.0)
        self.properties = []
        self.children = []
        self.sun_only = False  # a directional light's object: it only sets the scene's sun


class Importer:
    def __init__(self, project: Path, out: Path, force: bool):
        self.project, self.out, self.force = project, out, force
        self.assets = project / "Assets"
        self.guid_path = {}  # guid -> path relative to Assets/, with forward slashes
        self.warnings = []
        self.counts = {}
        self.skipped_types = {}
        self.to_convert = []  # model files Thistle can't read yet
        self._docs = {}
        self.environment = {}

    # -- bookkeeping --

    def warn(self, message: str) -> None:
        if message not in self.warnings:
            self.warnings.append(message)

    def count(self, what: str, n: int = 1) -> None:
        self.counts[what] = self.counts.get(what, 0) + n

    # -- scanning --

    def scan_guids(self) -> None:
        for meta in self.assets.rglob("*.meta"):
            try:
                head = meta.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            m = re.search(r"^guid: ([0-9a-fA-F]{32})", head, re.MULTILINE)
            if m:
                self.guid_path[m.group(1)] = meta.with_suffix("").relative_to(self.assets).as_posix()

    def load_doc(self, rel: str):
        """{fileID: UnityObject} for a scene or prefab, or None if it isn't text-serialized."""
        if rel not in self._docs:
            text = (self.assets / rel).read_text(encoding="utf-8", errors="replace")
            if not text.startswith("%YAML"):
                self.warn(f"{rel} is binary-serialized. Set Edit > Project Settings > Editor > Asset Serialization to Force Text in Unity, then import again.")
                self._docs[rel] = None
            else:
                self._docs[rel] = parse_unity_yaml(text)
        return self._docs[rel]

    # -- references --

    def asset_of(self, ref):
        if isinstance(ref, dict):
            return self.guid_path.get(str(ref.get("guid", "")))
        return None

    def model_path(self, rel: str) -> str:
        ext = Path(rel).suffix.lower()
        if ext in CONVERT_MODEL_EXT:
            glb = str(Path(rel).with_suffix(".glb").as_posix())
            if rel not in self.to_convert:
                self.to_convert.append(rel)
            return "assets/" + glb
        return "assets/" + rel

    # -- one document -> a tree of nodes --

    def build_nodes(self, objects: dict, depth: int = 0):
        """(root nodes, {transform fileID: Node}) for everything in one scene or prefab."""
        transforms = {fid: o for fid, o in objects.items() if o.class_id in (4, 224) and not o.stripped}
        gameobjects = {fid: o for fid, o in objects.items() if o.class_id == 1}
        anchors = {}   # transform fileID -> Node (also stripped ones, mapped into prefab expansions)
        by_go = {}
        for fid, go in gameobjects.items():
            comps = [c.get("component", {}).get("fileID") for c in (go.data.get("m_Component") or []) if isinstance(c, dict)]
            node = Node(str(go.data.get("m_Name") or "GameObject"))
            if go.data.get("m_IsActive", 1) == 0:
                node.properties.append(("active", "false"))
            tag = go.data.get("m_TagString")
            if tag and tag != "Untagged":
                node.properties.append(("tag", str(tag)))
            if go.data.get("m_Layer", 0):
                node.properties.append(("layer", str(go.data["m_Layer"])))
            self.fill_node(node, [objects[c] for c in comps if c in objects], objects)
            by_go[fid] = node
            self.count("GameObjects")
        for fid, tr in transforms.items():
            node = by_go.get(tr.data.get("m_GameObject", {}).get("fileID"))
            if node is None:
                continue
            node.pos = _vec(tr.data.get("m_LocalPosition"), "xyz", (0, 0, 0))
            node.rot = _vec(tr.data.get("m_LocalRotation"), "xyzw", (0, 0, 0, 1))
            node.scale = _vec(tr.data.get("m_LocalScale"), "xyz", (1, 1, 1))
            if tr.class_id == 224:
                self.skipped_types["UI RectTransform"] = self.skipped_types.get("UI RectTransform", 0) + 1
            anchors[fid] = node
        pending = []  # (node, parent transform fileID, order)
        for fid, tr in transforms.items():
            node = anchors.get(fid)
            if node is not None:
                pending.append((node, tr.data.get("m_Father", {}).get("fileID", 0), tr.data.get("m_RootOrder", 0)))
        for inst in [o for o in objects.values() if o.class_id == 1001]:
            expansion = self.expand_prefab(inst, objects, depth)
            if expansion is None:
                continue
            roots, inner, parent = expansion
            for stripped_fid, s in objects.items():
                if s.stripped and s.class_id in (4, 224) and s.data.get("m_PrefabInstance", {}).get("fileID") == inst.file_id:
                    target = inner.get(s.data.get("m_CorrespondingSourceObject", {}).get("fileID"))
                    if target is not None:
                        anchors[stripped_fid] = target
            for r in roots:
                pending.append((r, parent, 0))
        roots_out = []
        pending.sort(key=lambda p: p[2])
        for node, parent_fid, _order in pending:
            parent = anchors.get(parent_fid) if parent_fid else None
            if parent is None or parent is node:
                roots_out.append(node)
            else:
                parent.children.append(node)
        return roots_out, anchors

    def fill_node(self, node: Node, comps: list, objects: dict) -> None:
        mesh_filter = next((c for c in comps if c.class_id == 33), None)
        renderer = next((c for c in comps if c.class_id == 23), None)
        extras = []
        if mesh_filter is not None:
            self.fill_mesh(node, mesh_filter, renderer)
        elif renderer is not None:
            self.skipped_types["renderer without a mesh"] = self.skipped_types.get("renderer without a mesh", 0) + 1
        for c in comps:
            if c.class_id == 108:
                light = self.light_node(node, c)
                if light is not None:
                    if node.kind == "empty":
                        node.kind, node.color, node.intensity, node.range, node.spot_angle = light.kind, light.color, light.intensity, light.range, light.spot_angle
                    else:
                        light.name = node.name + " (light)"
                        extras.append(light)
            elif c.class_id == 65:
                if c.data.get("m_IsTrigger"):
                    t = Node(node.name + " (trigger)")
                    t.kind = "trigger"
                    t.pos = _vec(c.data.get("m_Center"), "xyz", (0, 0, 0))
                    t.scale = _vec(c.data.get("m_Size"), "xyz", (1, 1, 1))
                    extras.append(t)
                    self.count("trigger volumes")
                else:
                    self.skipped_types["BoxCollider (solid: shapes and models are solid already)"] = self.skipped_types.get("BoxCollider (solid: shapes and models are solid already)", 0) + 1
            elif c.class_id == 20:
                node.properties.append(("camera_fov", str(round(float(c.data.get("field of view", 60)), 3))))
                self.count("cameras (kept as properties on an empty)")
            elif c.class_id == 114:
                self.script_properties(node, c)
            elif c.class_id in (4, 224, 1, 33, 23):
                continue
            else:
                label = c.type_name
                self.skipped_types[label] = self.skipped_types.get(label, 0) + 1
        node.children.extend(extras)

    def fill_mesh(self, node: Node, mesh_filter, renderer) -> None:
        ref = mesh_filter.data.get("m_Mesh") or {}
        guid = str(ref.get("guid", ""))
        file_id = ref.get("fileID", 0)
        if guid == BUILTIN_GUID:
            shape = BUILTIN_MESHES.get(file_id)
            if shape in ("box", "sphere", "cylinder", "plane"):
                node.kind = shape
                if shape == "cylinder":
                    node.properties.append(("unity_scale_note", "cylinder: height doubled to match Unity's 2 m cylinder"))
            elif shape == "capsule":
                node.kind = "cylinder"
                self.warn("A Unity capsule was imported as a cylinder of the same height and width.")
                node.properties.append(("unity_scale_note", "capsule: imported as a cylinder"))
            else:
                self.warn(f"Built-in Unity mesh {shape or file_id} has no Thistle shape; the object was kept as an empty.")
                return
            self.count("built-in shapes")
        else:
            rel = self.asset_of(ref)
            if rel is None:
                self.warn("A mesh reference points at a file that isn't in Assets/ (a package or a missing asset); the object was kept as an empty.")
                return
            node.kind, node.model = "model", self.model_path(rel)
            self.count("models")
            if file_id not in (0, 4300000):
                self.warn(f"{rel}: an object uses one mesh inside this file; Thistle draws the whole model there.")
        if renderer is not None and node.kind != "model":
            mats = renderer.data.get("m_Materials") or []
            mat_rel = self.asset_of(mats[0]) if mats else None
            if mat_rel and mat_rel.endswith(".mat"):
                info = self.read_material(mat_rel)
                if info:
                    node.color = info["color"]

    def light_node(self, node: Node, c) -> "Node | None":
        kind = c.data.get("m_Type", 2)
        color = _color(c.data.get("m_Color"))
        intensity = float(c.data.get("m_Intensity", 1.0))
        if kind == 1:
            self.environment["sun"] = {"color": [round(v, 4) for v in color], "intensity": intensity, "light_node": node,
                                       "shadows": (c.data.get("m_Shadows") or {}).get("m_Type", 0) != 0}
            node.sun_only = True
            self.count("directional lights (became the scene's sun)")
            return None
        light = Node(node.name)
        light.kind = "spot_light" if kind == 0 else "point_light"
        if kind == 3:
            self.warn("A Unity area light was imported as a point light.")
        light.color, light.intensity, light.range = color, intensity, float(c.data.get("m_Range", 10.0))
        light.spot_angle = math.radians(float(c.data.get("m_SpotAngle", 60.0))) / 2.0
        self.count("point and spot lights")
        return light

    def script_properties(self, node: Node, c) -> None:
        script = self.asset_of(c.data.get("m_Script"))
        name = Path(script).stem if script else "MonoBehaviour"
        node.properties.append(("unity_script", name))
        for key, value in c.data.items():
            if key.startswith("m_") or key in ("serializedVersion",) or isinstance(value, (dict, list)) or value is None:
                continue
            node.properties.append((f"{name}.{key}", str(value)))
        self.skipped_types[f"C# script {name}"] = self.skipped_types.get(f"C# script {name}", 0) + 1

    def render_settings(self, c) -> None:
        d = c.data
        env = self.environment
        if d.get("m_Fog"):
            mode = d.get("m_FogMode", 3)
            start, end = float(d.get("m_LinearFogStart", 0)), float(d.get("m_LinearFogEnd", 300))
            if mode != 1:
                density = max(float(d.get("m_FogDensity", 0.01)), 1e-4)
                start, end = 0.0, 4.6 / density
                self.warn("Exponential fog was approximated as linear fog that reaches full strength where Unity's fades to 1%.")
            env["fog"] = {"enabled": True, "start": start, "end": end, "color": [round(v, 4) for v in _color(d.get("m_FogColor"))], "match_sky": False}
        mode = d.get("m_AmbientMode", 0)
        if mode == 1:
            env["sky"] = {"top": _color(d.get("m_AmbientSkyColor")), "horizon": _color(d.get("m_AmbientEquatorColor")), "ground": _color(d.get("m_AmbientGroundColor"))}
            env["ambient"] = float(d.get("m_AmbientIntensity", 1.0))
        elif mode == 3:
            flat = _color(d.get("m_AmbientSkyColor"))
            env["sky"] = {"top": flat, "horizon": flat, "ground": flat}
            env["ambient"] = float(d.get("m_AmbientIntensity", 1.0))
        elif (d.get("m_SkyboxMaterial") or {}).get("fileID"):
            self.warn("The scene uses a skybox material; Thistle's sky colors were left at their defaults (set world.sky.skybox in code).")
        self.count("render settings")

    # -- prefabs --

    def expand_prefab(self, inst, objects: dict, depth: int):
        """(root nodes, {source transform fileID: Node}, parent transform fileID) or None."""
        src = inst.data.get("m_SourcePrefab") or inst.data.get("m_ParentPrefab") or {}
        rel = self.asset_of(src)
        mod = inst.data.get("m_Modification") or {}
        parent = (mod.get("m_TransformParent") or {}).get("fileID", 0)
        if rel is None:
            self.warn("A prefab instance points at a prefab that isn't in Assets/; it was skipped.")
            return None
        overrides = {}
        nested = 0
        for m in mod.get("m_Modifications") or []:
            if isinstance(m, dict):
                overrides.setdefault((m.get("target") or {}).get("fileID"), {})[m.get("propertyPath")] = m.get("value")
        ext = Path(rel).suffix.lower()
        if ext == ".prefab":
            if depth > 8:
                self.warn(f"Prefab nesting deeper than 8 levels at {rel}; the inner part was skipped.")
                return None
            sub = self.load_doc(rel)
            if sub is None:
                return None
            roots, inner = self.build_nodes(sub, depth + 1)
            root_tr = next((fid for fid, o in sub.items() if o.class_id in (4, 224) and not o.stripped and not o.data.get("m_Father", {}).get("fileID")), None)
            root_go = sub[root_tr].data.get("m_GameObject", {}).get("fileID") if root_tr else None
            nodes = [r for r in roots]
            root = inner.get(root_tr)
            for target, props in overrides.items():
                node = root if target == root_tr else None
                if target == root_go:
                    node = root
                if node is None:
                    nested += len(props)
                    continue
                self.apply_overrides(node, props)
            if nested:
                self.warn(f"{nested} property overrides on objects inside {rel} (not its root) were not applied.")
            self.count("prefab instances")
            return nodes, inner, parent
        if ext in MODEL_EXT or ext in CONVERT_MODEL_EXT:
            node = Node(Path(rel).stem)
            node.kind, node.model = "model", self.model_path(rel)
            for _target, props in overrides.items():
                self.apply_overrides(node, props)
            self.count("models")
            return [node], {}, parent
        self.warn(f"A prefab instance of {rel} ({ext}) was skipped.")
        return None

    @staticmethod
    def apply_overrides(node: Node, props: dict) -> None:
        for path, value in props.items():
            if path == "m_Name":
                node.name = str(value)
                continue
            for field, (prefix, size) in VEC_KEYS.items():
                if path.startswith(prefix + "."):
                    axis = path.split(".", 1)[1]
                    idx = "xyzw".find(axis)
                    if 0 <= idx < size:
                        try:
                            getattr(node, "pos" if field == "position" else "rot" if field == "rotation" else "scale")[idx] = float(value)
                        except (TypeError, ValueError):
                            pass

    # -- materials --

    def read_material(self, rel: str):
        doc = self.load_doc(rel)
        if not doc:
            return None
        mat = next((o for o in doc.values() if o.class_id == 21), None)
        if mat is None:
            return None
        saved = mat.data.get("m_SavedProperties") or {}
        colors = {}
        for entry in saved.get("m_Colors") or []:
            if isinstance(entry, dict):
                colors.update(entry)
        floats = {}
        for entry in saved.get("m_Floats") or []:
            if isinstance(entry, dict):
                floats.update(entry)
        textures = {}
        for entry in saved.get("m_TexEnvs") or []:
            if isinstance(entry, dict):
                for key, tex in entry.items():
                    path = self.asset_of((tex or {}).get("m_Texture"))
                    if path:
                        textures[key] = "assets/" + path
        base = colors.get("_BaseColor") or colors.get("_Color")
        smooth = floats.get("_Smoothness", floats.get("_Glossiness", 0.5))
        mode = floats.get("_Surface", 0) if "_Surface" in floats else {0: 0, 1: 1, 2: 2, 3: 2}.get(int(floats.get("_Mode", 0)), 0)
        cutout = int(floats.get("_Mode", 0)) == 1 or floats.get("_AlphaClip", 0) == 1
        return {
            "name": str(mat.data.get("m_Name", Path(rel).stem)),
            "source": rel,
            "shader": (mat.data.get("m_Shader") or {}).get("guid", ""),
            "color": _color(base) if base else [1.0, 1.0, 1.0, 1.0],
            "metallic": float(floats.get("_Metallic", 0.0)),
            "roughness": round(1.0 - float(smooth), 4),
            "emissive": _color(colors.get("_EmissionColor"), (0, 0, 0, 1))[:3],
            "alpha": "cutout" if cutout else ("blend" if mode else "opaque"),
            "alpha_cutoff": float(floats.get("_Cutoff", 0.5)),
            "texture": textures.get("_BaseMap") or textures.get("_MainTex"),
            "normal_texture": textures.get("_BumpMap"),
            "metallic_roughness_texture": textures.get("_MetallicGlossMap"),
            "occlusion_texture": textures.get("_OcclusionMap"),
            "emissive_texture": textures.get("_EmissionMap"),
        }

    # -- flattening and writing --

    def flatten(self, roots: list, base: list = None, parent: int = -1):
        out = [] if base is None else base
        for node in roots:
            if node.sun_only and node.kind == "empty" and not node.children and not node.properties:
                continue
            out.append((node, parent))
            index = len(out) - 1
            self.flatten(node.children, out, index)
        return out

    def entity_json(self, node: Node, parent: int) -> dict:
        scale = list(node.scale)
        if node.kind == "cylinder":
            scale[1] *= 2.0
        elif node.kind == "plane":
            scale[0] *= 10.0
            scale[2] *= 10.0
        e = {"name": node.name, "kind": node.kind, "parent": parent, "position": _round(_mirror_pos(node.pos)),
             "rotation": _round(_mirror_rot(node.rot)), "scale": _round(scale), "color": _round(node.color)}
        if node.kind == "model":
            e["model"] = node.model
        if node.kind in ("point_light", "spot_light"):
            e["intensity"] = round(node.intensity, 6)
            e["range"] = round(node.range, 6)
        if node.kind == "spot_light":
            e["spot_angle"] = round(node.spot_angle, 6)
        props = [list(p) for p in node.properties if p[0] != "unity_scale_note"]
        if props:
            e["properties"] = props
        return e

    def scene_json(self, roots: list, environment: dict, anchors: dict = None) -> dict:
        flat = self.flatten(roots)
        env = {
            "ambient": environment.get("ambient", 0.55),
            "fog": {"enabled": False, "start": 30, "end": 150, "color": [0.7, 0.8, 0.9, 1], "match_sky": True},
            "sky": {"top": [0.35, 0.55, 0.9, 1], "horizon": [0.75, 0.85, 0.95, 1], "ground": [0.35, 0.33, 0.3, 1], "visible": True, "sun_disc": True},
            "sun": {"direction": [-0.35, -1.0, -0.45], "color": [1.0, 0.96, 0.88, 1], "intensity": 1.0, "shadows": True,
                    "shadow_distance": 60.0, "shadow_strength": 1.0},
        }
        if "fog" in environment:
            env["fog"].update(environment["fog"])
        if "sky" in environment:
            env["sky"].update({k: _round(v) for k, v in environment["sky"].items()})
        sun = environment.get("sun")
        if sun:
            node = sun["light_node"]
            rot = node.rot
            parent_rot = getattr(node, "_world_parent_rot", [0.0, 0.0, 0.0, 1.0])
            forward = _qrot(_qmul(parent_rot, rot), [0.0, 0.0, 1.0])
            env["sun"].update({"direction": _round(_mirror_pos(forward)), "color": sun["color"], "intensity": sun["intensity"], "shadows": sun["shadows"]})
        return {"thistle_scene": 1, "environment": env, "entities": [self.entity_json(n, p) for n, p in flat]}

    def write_json(self, path: Path, data) -> bool:
        if path.exists() and not self.force:
            self.warn(f"{path.relative_to(self.out).as_posix()} already exists; kept it (use --force to overwrite).")
            return False
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return True

    def convert_doc(self, rel: str, folder: str, kind: str) -> None:
        objects = self.load_doc(rel)
        if objects is None:
            return
        self.environment = {}
        for o in objects.values():
            if o.class_id == 104:
                self.render_settings(o)
        roots, _anchors = self.build_nodes(objects)
        env = dict(self.environment)
        if "sun" in env:
            self.give_world_rotation(roots, env["sun"]["light_node"])
        name = re.sub(r"[^A-Za-z0-9_.-]+", "_", Path(rel).stem)
        dest = self.out / "assets" / folder / f"{name}.scene.json"
        if self.write_json(dest, self.scene_json(roots, env)):
            self.count(kind)

    def give_world_rotation(self, roots: list, target: Node) -> None:
        def walk(nodes, rot):
            for n in nodes:
                n._world_parent_rot = rot
                if n is target:
                    return True
                if walk(n.children, _qmul(rot, n.rot)):
                    return True
            return False
        walk(roots, [0.0, 0.0, 0.0, 1.0])

    def copy_assets(self) -> None:
        for path in sorted(self.assets.rglob("*")):
            if not path.is_file() or path.suffix == ".meta" or path.suffix.lower() in (".unity", ".prefab", ".mat"):
                continue
            ext = path.suffix.lower()
            rel = path.relative_to(self.assets)
            if ext in COPY_EXT:
                dest = self.out / "assets" / rel
                if dest.exists() and not self.force:
                    continue
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, dest)
                self.count("files copied")
            elif ext in CONVERT_MODEL_EXT:
                if rel.as_posix() not in self.to_convert:
                    self.to_convert.append(rel.as_posix())
            elif ext not in (".cs", ".asmdef", ".asmref", ".meta", ".dll", ".shader", ".cginc", ".hlsl", ".anim", ".controller", ".asset", ".json", ".txt", ".md"):
                self.skipped_types[f"{ext or 'no extension'} files"] = self.skipped_types.get(f"{ext or 'no extension'} files", 0) + 1
            elif ext in (".cs", ".shader", ".anim", ".controller"):
                label = {".cs": "C# scripts (not translated)", ".shader": "shaders", ".anim": "animation clips", ".controller": "animator controllers"}[ext]
                self.skipped_types[label] = self.skipped_types.get(label, 0) + 1

    def run(self) -> int:
        if not self.assets.is_dir():
            print(f"error: {self.project} has no Assets/ folder - is it a Unity project?")
            return 1
        self.scan_guids()
        for scene in sorted(self.assets.rglob("*.unity")):
            self.convert_doc(scene.relative_to(self.assets).as_posix(), "scenes", "scenes")
        for prefab in sorted(self.assets.rglob("*.prefab")):
            self.convert_doc(prefab.relative_to(self.assets).as_posix(), "prefabs", "prefabs")
        for mat in sorted(self.assets.rglob("*.mat")):
            info = self.read_material(mat.relative_to(self.assets).as_posix())
            if info and self.write_json(self.out / "assets" / "materials" / (re.sub(r"[^A-Za-z0-9_.-]+", "_", mat.stem) + ".json"), info):
                self.count("materials")
        self.copy_assets()
        self.write_report()
        return 0

    def write_report(self) -> None:
        lines = ["# Unity import report", "", f"From `{self.project}`. See docs/unity-import.md for what this does and doesn't carry over.", "", "## Converted", ""]
        lines += [f"- {n} {what}" for what, n in sorted(self.counts.items())] or ["- nothing"]
        if self.to_convert:
            lines += ["", "## Models Thistle can't read yet (convert to .glb, same name, same folder under assets/)", ""]
            lines += [f"- `{p}` -> `assets/{Path(p).with_suffix('.glb').as_posix()}`" for p in sorted(set(self.to_convert))]
        if self.skipped_types:
            lines += ["", "## Left behind", ""] + [f"- {n} x {what}" for what, n in sorted(self.skipped_types.items())]
        if self.warnings:
            lines += ["", "## Warnings", ""] + [f"- {w}" for w in self.warnings]
        text = "\n".join(lines) + "\n"
        (self.out / "unity-import-report.md").write_text(text, encoding="utf-8")
        print(text)


def import_unity(project: Path, out: Path, force: bool = False) -> int:
    return Importer(project, out, force).run()
