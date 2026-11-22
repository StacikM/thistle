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
import glob
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# --- Unity's YAML ----------------------------------------------------------
# Unity writes a small, regular subset of YAML: block mappings and sequences
# (a sequence's dashes sit at its key's indent), flow `{a: 1, b: {c: 2}}` and
# `[]`, and plain or quoted scalars. That's all this reads. A general YAML
# library would be another install for every user of the CLI.

_STRING_KEYS = {"guid", "_typelessdata", "m_IndexBuffer"}
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
    if key in _STRING_KEYS:  # hex dumps: "0000000000000000e000000000000000", or 0001000200030004, would read as numbers
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
# A Unity Mesh saved as an .asset (MapForge and other tools bake their geometry this way).
MESH_ASSET_EXT = ".asset"
TEXTURE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".tga"}
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


# --- Mesh .asset files ------------------------------------------------------------

_FORMAT_SIZE = {0: 4, 1: 2, 2: 1, 3: 1, 4: 2, 5: 2, 6: 1, 7: 1, 8: 2, 9: 2, 10: 4, 11: 4}


def _decode(raw: bytes, at: int, fmt: int, dim: int):
    """One vertex attribute, as floats. Only the formats Unity meshes use for positions, normals and UVs."""
    if fmt == 0:
        return list(struct.unpack_from(f"<{dim}f", raw, at))
    if fmt == 1:
        return list(struct.unpack_from(f"<{dim}e", raw, at))
    if fmt == 2:
        return [v / 255.0 for v in struct.unpack_from(f"<{dim}B", raw, at)]
    if fmt == 3:
        return [max(v / 127.0, -1.0) for v in struct.unpack_from(f"<{dim}b", raw, at)]
    if fmt == 4:
        return [v / 65535.0 for v in struct.unpack_from(f"<{dim}H", raw, at)]
    if fmt == 5:
        return [max(v / 32767.0, -1.0) for v in struct.unpack_from(f"<{dim}h", raw, at)]
    raise ValueError(f"vertex format {fmt} isn't supported")


def mesh_asset_to_obj(mesh: dict):
    """(obj text, name) for a Unity Mesh's data, or raises ValueError saying what isn't supported.

    Z is mirrored (and every triangle's winding reversed with it) so the mesh sits in
    Thistle's right-handed space the way the scene around it does."""
    if mesh.get("m_MeshCompression"):
        raise ValueError("it uses Unity's mesh compression")
    if (mesh.get("m_StreamData") or {}).get("size"):
        raise ValueError("its vertex data is stored in a separate .resS file")
    vd = mesh.get("m_VertexData") or {}
    count = int(vd.get("m_VertexCount", 0))
    channels = vd.get("m_Channels") or []
    raw = bytes.fromhex(vd.get("_typelessdata") or "")
    if count <= 0 or not raw:
        raise ValueError("it has no vertices")
    # Each stream holds some channels back to back; streams follow each other, padded to 16 bytes.
    stride = {}
    for ch in channels:
        dim = int(ch.get("dimension", 0)) & 0xF
        if dim:
            stream = int(ch.get("stream", 0))
            stride[stream] = max(stride.get(stream, 0), int(ch.get("offset", 0)) + dim * _FORMAT_SIZE[int(ch.get("format", 0))])
    start, at = {}, 0
    for stream in sorted(stride):
        start[stream] = at
        at += (count * stride[stream] + 15) // 16 * 16

    def attribute(index: int, want: int):
        if index >= len(channels):
            return None
        ch = channels[index]
        dim = int(ch.get("dimension", 0)) & 0xF
        if dim < want:
            return None
        stream, fmt = int(ch.get("stream", 0)), int(ch.get("format", 0))
        base, step = start[stream] + int(ch.get("offset", 0)), stride[stream]
        return [_decode(raw, base + i * step, fmt, dim)[:want] for i in range(count)]

    positions = attribute(0, 3)
    if positions is None:
        raise ValueError("it has no vertex positions")
    normals, uvs = attribute(1, 3), attribute(4, 2)

    index_hex = mesh.get("m_IndexBuffer") or ""
    indices_raw = bytes.fromhex(index_hex)
    wide = int(mesh.get("m_IndexFormat", 0)) == 1
    unit = 4 if wide else 2
    triangles = []
    for sub in mesh.get("m_SubMeshes") or []:
        if int(sub.get("topology", 0)) != 0:
            raise ValueError("it has a submesh that isn't made of triangles")
        first, n, base = int(sub.get("firstByte", 0)), int(sub.get("indexCount", 0)), int(sub.get("baseVertex", 0))
        values = struct.unpack_from(f"<{n}{'I' if wide else 'H'}", indices_raw, first)
        triangles += [(values[i] + base, values[i + 1] + base, values[i + 2] + base) for i in range(0, n - n % 3, 3)]
    if not triangles or max(max(t) for t in triangles) >= count:
        raise ValueError("its indices don't match its vertices")

    name = str(mesh.get("m_Name") or "mesh")
    lines = [f"# from Unity mesh {name} by thistle import-unity (Z mirrored, winding reversed)", f"o {name}"]
    lines += [f"v {x + 0.0:.7g} {y + 0.0:.7g} {-z + 0.0:.7g}" for x, y, z in positions]
    if normals:
        lines += [f"vn {x + 0.0:.7g} {y + 0.0:.7g} {-z + 0.0:.7g}" for x, y, z in normals]
    if uvs:
        lines += [f"vt {u:.7g} {v:.7g}" for u, v in uvs]

    def corner(i: int) -> str:
        n = i + 1
        return f"{n}/{n if uvs else ''}/{n}" if normals else (f"{n}/{n}" if uvs else f"{n}")

    lines += [f"f {corner(a)} {corner(c)} {corner(b)}" for a, b, c in triangles]
    return "\n".join(lines) + "\n", name


# Run inside Blender: FBX in, GLB out. An FBX's takes come in as separate actions but only the active one would be
# exported, so every action is attached to the armature as its own track and the tracks are what's exported: one
# clip each, named without the "Armature|" prefix Blender's FBX importer gives them.
BLENDER_FBX_TO_GLB = """
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
"""


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
        self.tintable = False  # a shape or a converted mesh: Unity's material color can be its color (a glTF model has its own)
        self.look = {}         # texture, normal_texture, metallic, roughness, uv_scale: what a tintable node's material adds to its color


class Importer:
    def __init__(self, project: Path, out: Path, force: bool, scenes=None, ambient_scale: float = 0.4, light_scale: float = 0.28, sun_scale: float = 1.0,
                 blender: str = "", convert_fbx: bool = True):
        self.project, self.out, self.force = project, out, force
        self.blender_arg, self.convert_fbx = blender, convert_fbx
        self.only_scenes = list(scenes or [])
        # Unity's lights and ambient are far hotter than Thistle's for the same numbers: a scene lit
        # "1.0 ambient, point lights at 6" comes out white. These are the factors one game's scenes
        # were hand-tuned to (0.4 and 0.28), not derived: expect to retune.
        self.ambient_scale, self.light_scale, self.sun_scale = ambient_scale, light_scale, sun_scale
        self.assets = project / "Assets"
        self.guid_path = {}  # guid -> path relative to Assets/, with forward slashes
        self.warnings = []
        self.counts = {}
        self.skipped_types = {}
        self.to_convert = []  # model files Thistle can't read yet
        self.used = set()     # Assets-relative files the converted scenes need
        self.names = {}       # (kind, rel) -> a name that no other file of that kind has
        self.embedded = {}    # (doc rel, fileID) -> (output path relative to assets/, mesh data), meshes saved inside a scene
        self.unresolved = {}  # "Type.path" -> count: overrides whose target isn't in the prefab
        self.ignored = {}     # "Type.path" -> count: overrides on things Thistle has no equivalent for
        self._docs = {}
        self.environment = {}

    # -- bookkeeping --

    def warn(self, message: str) -> None:
        if message not in self.warnings:
            self.warnings.append(message)

    def count(self, what: str, n: int = 1) -> None:
        self.counts[what] = self.counts.get(what, 0) + n

    def use(self, rel: str) -> None:
        self.used.add(rel)

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

    def assign_names(self) -> None:
        """Scenes, prefabs and materials are written as <name>.json in one folder per kind, so two files
        with the same name in different folders would overwrite each other (or be skipped). Decided over
        every file up front, so the names don't depend on which scenes you asked for."""
        for kind, ext in (("scene", ".unity"), ("prefab", ".prefab"), ("material", ".mat")):
            taken = set()
            for path in sorted(self.assets.rglob("*" + ext)):
                rel = path.relative_to(self.assets).as_posix()
                stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", path.stem)
                folder = re.sub(r"[^A-Za-z0-9_.-]+", "_", path.parent.name or "Assets")
                name, n = stem, 1
                if name in taken:
                    name = f"{stem}_{folder}"
                while name in taken:
                    n += 1
                    name = f"{stem}_{folder}_{n}"
                taken.add(name)
                self.names[(kind, rel)] = name

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
        self.use(rel)
        if ext == MESH_ASSET_EXT:
            return "assets/" + str(Path(rel).with_suffix(".obj").as_posix())
        if ext in CONVERT_MODEL_EXT:
            glb = str(Path(rel).with_suffix(".glb").as_posix())
            if rel not in self.to_convert:
                self.to_convert.append(rel)
            return "assets/" + glb
        return "assets/" + rel

    def embedded_mesh(self, doc_rel: str, objects: dict, file_id: int):
        """The model path for a Mesh saved inside the scene or prefab file itself, or None."""
        obj = objects.get(file_id)
        if obj is None or obj.class_id != 43:
            return None
        key = (doc_rel, file_id)
        if key not in self.embedded:
            name = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(obj.data.get("m_Name") or "mesh"))
            doc = re.sub(r"[^A-Za-z0-9_.-]+", "_", Path(doc_rel).stem)
            self.embedded[key] = (f"meshes/{doc}/{name}_{file_id}.obj", obj.data)
        return "assets/" + self.embedded[key][0]

    # -- one document -> a tree of nodes --

    def build_nodes(self, objects: dict, doc_rel: str, depth: int = 0):
        """(root nodes, {transform fileID: Node}, {any fileID: (Node, UnityObject)}) for one scene or prefab."""
        transforms = {fid: o for fid, o in objects.items() if o.class_id in (4, 224) and not o.stripped}
        gameobjects = {fid: o for fid, o in objects.items() if o.class_id == 1}
        anchors = {}   # transform fileID -> Node (also stripped ones, mapped into prefab expansions)
        index = {}     # fileID of a GameObject or any of its components -> (Node, that object)
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
            comp_objs = [objects[c] for c in comps if c in objects]
            self.fill_node(node, comp_objs, objects, doc_rel)
            by_go[fid] = node
            index[fid] = (node, go)
            for c in comp_objs:
                index[c.file_id] = (node, c)
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
            expansion = self.expand_prefab(inst, objects, doc_rel, depth)
            if expansion is None:
                continue
            roots, inner_anchors, parent, inner_index = expansion
            # The scene's "stripped" stand-ins for the prefab's own objects: anything that points at
            # one (a child's m_Father, an override) means the object inside the expansion.
            for stripped_fid, st in objects.items():
                if st.stripped and st.data.get("m_PrefabInstance", {}).get("fileID") == inst.file_id:
                    source_fid = st.data.get("m_CorrespondingSourceObject", {}).get("fileID")
                    hit = inner_index.get(source_fid)
                    if hit is not None:
                        index[stripped_fid] = hit
                        if st.class_id in (4, 224):
                            anchors[stripped_fid] = hit[0]
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
        return roots_out, anchors, index

    def fill_node(self, node: Node, comps: list, objects: dict, doc_rel: str) -> None:
        mesh_filter = next((c for c in comps if c.class_id == 33), None)
        renderer = next((c for c in comps if c.class_id == 23), None)
        extras = []
        if mesh_filter is not None:
            self.set_mesh(node, mesh_filter.data.get("m_Mesh") or {}, doc_rel, objects)
            if renderer is not None:
                mats = renderer.data.get("m_Materials") or []
                if mats:
                    self.set_material(node, mats[0])
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
                elif node.kind == "model":
                    # A model is solid by its triangles; a dense one is slow to walk on. Unity's collider is the
                    # usual answer to that, so ask for one box around the model (its size, not the collider's own).
                    self.set_property(node, "collider", "box")
                    self.count("solid BoxColliders on models (collider=box: one box around the whole model)")
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

    def set_mesh(self, node: Node, ref: dict, doc_rel: str, objects: dict) -> None:
        """Make `node` the shape or model a MeshFilter's m_Mesh reference points at."""
        guid = str(ref.get("guid", ""))
        file_id = ref.get("fileID", 0)
        if guid == BUILTIN_GUID:
            shape = BUILTIN_MESHES.get(file_id)
            if shape in ("box", "sphere", "cylinder", "plane"):
                node.kind, node.model, node.tintable = shape, "", True
                if shape == "cylinder":
                    node.properties.append(("unity_scale_note", "cylinder: height doubled to match Unity's 2 m cylinder"))
            elif shape == "capsule":
                node.kind, node.model, node.tintable = "cylinder", "", True
                self.warn("A Unity capsule was imported as a cylinder of the same height and width.")
                node.properties.append(("unity_scale_note", "capsule: imported as a cylinder"))
            else:
                self.warn(f"Built-in Unity mesh {shape or file_id} has no Thistle shape; the object was kept as an empty.")
                return
            self.count("built-in shapes")
            return
        if not guid and file_id:
            path = self.embedded_mesh(doc_rel, objects, file_id)
            if path is None:
                self.warn(f"{doc_rel}: a mesh reference to object {file_id} that isn't a Mesh saved in the same file; the object was kept as an empty.")
                return
            node.kind, node.model, node.tintable = "model", path, True
            self.count("models")
            return
        rel = self.asset_of(ref)
        if rel is None:
            self.warn("A mesh reference points at a file that isn't in Assets/ (a package or a missing asset); the object was kept as an empty.")
            return
        node.kind, node.model = "model", self.model_path(rel)
        node.tintable = rel.lower().endswith(MESH_ASSET_EXT)  # a converted mesh has no material of its own; a glTF/FBX model does
        self.count("models")
        if file_id not in (0, 4300000):
            self.warn(f"{rel}: an object uses one mesh inside this file; Thistle draws the whole model there.")

    def set_material(self, node: Node, ref: dict) -> None:
        rel = self.asset_of(ref)
        if rel and rel.endswith(".mat"):
            self.use(rel)
            info = self.read_material(rel)
            if info and node.tintable:
                node.color = info["color"]
                node.look = {k: info[k] for k in ("texture", "normal_texture", "metallic", "roughness", "uv_scale", "metallic_set") if info.get(k) is not None}

    def light_node(self, node: Node, c) -> "Node | None":
        kind = c.data.get("m_Type", 2)
        color = _color(c.data.get("m_Color"))
        intensity = float(c.data.get("m_Intensity", 1.0))
        if kind == 1:
            self.environment["sun"] = {"color": [round(v, 4) for v in color], "intensity": intensity * self.sun_scale, "light_node": node,
                                       "shadows": (c.data.get("m_Shadows") or {}).get("m_Type", 0) != 0}
            node.sun_only = True
            self.count("directional lights (became the scene's sun)")
            return None
        light = Node(node.name)
        light.kind = "spot_light" if kind == 0 else "point_light"
        if kind == 3:
            self.warn("A Unity area light was imported as a point light.")
        light.color, light.intensity, light.range = color, intensity * self.light_scale, float(c.data.get("m_Range", 10.0))
        light.spot_angle = math.radians(float(c.data.get("m_SpotAngle", 60.0))) / 2.0
        self.count("point and spot lights")
        return light

    def script_name(self, c) -> str:
        script = self.asset_of(c.data.get("m_Script"))
        return Path(script).stem if script else "MonoBehaviour"

    @staticmethod
    def set_property(node: Node, key: str, value) -> None:
        node.properties = [p for p in node.properties if p[0] != key] + [(key, str(value))]

    def script_properties(self, node: Node, c) -> None:
        name = self.script_name(c)
        if not any(p[0] == "unity_script" and p[1] == name for p in node.properties):
            node.properties.append(("unity_script", name))
        for key, value in c.data.items():
            if key.startswith("m_") or key in ("serializedVersion",) or isinstance(value, (dict, list)) or value is None:
                continue
            self.set_property(node, f"{name}.{key}", value)
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
        ambient = float(d.get("m_AmbientIntensity", 1.0)) * self.ambient_scale
        if mode == 1:
            env["sky"] = {"top": _color(d.get("m_AmbientSkyColor")), "horizon": _color(d.get("m_AmbientEquatorColor")), "ground": _color(d.get("m_AmbientGroundColor"))}
            env["ambient"] = ambient
        elif mode == 3:
            flat = _color(d.get("m_AmbientSkyColor"))
            env["sky"] = {"top": flat, "horizon": flat, "ground": flat}
            env["ambient"] = ambient
        elif (d.get("m_SkyboxMaterial") or {}).get("fileID"):
            self.warn("The scene uses a skybox material; Thistle's sky colors were left at their defaults (set world.sky.skybox in code).")
        self.count("render settings")

    # -- prefabs --

    def expand_prefab(self, inst, objects: dict, doc_rel: str, depth: int):
        """(root nodes, {source transform fileID: Node}, parent transform fileID, {source fileID: (Node, object)}) or None."""
        src = inst.data.get("m_SourcePrefab") or inst.data.get("m_ParentPrefab") or {}
        rel = self.asset_of(src)
        mod = inst.data.get("m_Modification") or {}
        parent = (mod.get("m_TransformParent") or {}).get("fileID", 0)
        if rel is None:
            self.warn("A prefab instance points at a prefab that isn't in Assets/; it was skipped.")
            return None
        self.use(rel)
        overrides = {}
        for m in mod.get("m_Modifications") or []:
            if isinstance(m, dict):
                overrides.setdefault((m.get("target") or {}).get("fileID"), {})[m.get("propertyPath")] = (m.get("value"), m.get("objectReference"))
        ext = Path(rel).suffix.lower()
        if ext == ".prefab":
            if depth > 8:
                self.warn(f"Prefab nesting deeper than 8 levels at {rel}; the inner part was skipped.")
                return None
            sub = self.load_doc(rel)
            if sub is None:
                return None
            roots, inner_anchors, inner_index = self.build_nodes(sub, rel, depth + 1)
            self.apply_overrides(overrides, inner_index, rel, objects, doc_rel)
            self.count("prefab instances")
            return list(roots), inner_anchors, parent, inner_index
        if ext in MODEL_EXT or ext in CONVERT_MODEL_EXT:
            node = Node(Path(rel).stem)
            node.kind, node.model = "model", self.model_path(rel)
            # An imported model's own objects have fileIDs we can't see; its root is 100000 (object) and 400000 (transform).
            index = {100000: (node, UnityObject(1, 100000, "GameObject", {}, False)), 400000: (node, UnityObject(4, 400000, "Transform", {}, False))}
            self.apply_overrides(overrides, index, rel, objects, doc_rel)
            self.count("models")
            return [node], {400000: node}, parent, index
        self.warn(f"A prefab instance of {rel} ({ext}) was skipped.")
        return None

    def apply_overrides(self, overrides: dict, index: dict, rel: str, objects: dict, doc_rel: str) -> None:
        """Apply a prefab instance's property overrides to whatever object each one names, wherever it is in the prefab."""
        for target, props in overrides.items():
            hit = index.get(target)
            if hit is None:
                for path in props:
                    key = f"{Path(rel).stem}.{path}"
                    self.unresolved[key] = self.unresolved.get(key, 0) + 1
                continue
            node, obj = hit
            for path, (value, ref) in props.items():
                if not self.apply_override(node, obj, path, value, ref, objects, doc_rel):
                    key = f"{obj.type_name}.{path}"
                    self.ignored[key] = self.ignored.get(key, 0) + 1

    def apply_override(self, node: Node, obj, path: str, value, ref, objects: dict, doc_rel: str) -> bool:
        cls = obj.class_id
        if cls == 1 and path == "m_Name":
            node.name = str(value)
            return True
        if cls == 1 and path == "m_IsActive":
            if value in (0, "0"):
                self.set_property(node, "active", "false")
            else:
                node.properties = [p for p in node.properties if p[0] != "active"]
            return True
        if cls in (4, 224):
            for field, (prefix, size) in VEC_KEYS.items():
                if path.startswith(prefix + "."):
                    idx = "xyzw".find(path.split(".", 1)[1])
                    if 0 <= idx < size:
                        try:
                            getattr(node, "pos" if field == "position" else "rot" if field == "rotation" else "scale")[idx] = float(value)
                        except (TypeError, ValueError):
                            return False
                        return True
            return False
        if cls == 33 and path == "m_Mesh" and isinstance(ref, dict):
            self.set_mesh(node, ref, doc_rel, objects)
            return True
        if cls == 23 and path.startswith("m_Materials.Array.data[") and isinstance(ref, dict):
            if path == "m_Materials.Array.data[0]":
                self.set_material(node, ref)
            return True
        if cls == 114 and not path.startswith("m_") and not path.startswith("Array"):
            self.set_property(node, f"{self.script_name(obj)}.{path}", value)
            return True
        return False

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
        scales = {}
        for entry in saved.get("m_TexEnvs") or []:
            if isinstance(entry, dict):
                for key, tex in entry.items():
                    path = self.asset_of((tex or {}).get("m_Texture"))
                    if path:
                        if Path(path).suffix.lower() not in TEXTURE_EXT:
                            self.warn(f"{rel}: texture {path} is a {Path(path).suffix} file, which Thistle can't load; convert it to PNG.")
                            continue
                        textures[key] = "assets/" + path
                        scales[key] = _vec((tex or {}).get("m_Scale"), "xy", (1, 1))
                        self.use(path)
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
            # What an entity can carry: a base texture, a bump map, tiling, and metallic/roughness
            # (only when the material is actually metallic or bumpy, so plain materials keep the classic look).
            "uv_scale": (scales.get("_BaseMap") or scales.get("_MainTex") or [1.0, 1.0]) if (textures.get("_BaseMap") or textures.get("_MainTex")) else None,
            **({"metallic_set": True} if float(floats.get("_Metallic", 0.0)) >= 0.05 or textures.get("_BumpMap") else {}),
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
        look = node.look
        if look.get("texture"):
            e["texture"] = look["texture"]
        if look.get("normal_texture"):
            e["normal_texture"] = look["normal_texture"]
        if look.get("metallic_set"):
            e["metallic"] = round(float(look["metallic"]), 4)
            e["roughness"] = round(float(look["roughness"]), 4)
        if look.get("uv_scale") and look["uv_scale"] != [1.0, 1.0]:
            e["uv_scale"] = _round(look["uv_scale"], 4)
        if node.kind in ("point_light", "spot_light"):
            e["intensity"] = round(node.intensity, 6)
            e["range"] = round(node.range, 6)
        if node.kind == "spot_light":
            e["spot_angle"] = round(node.spot_angle, 6)
        props = [list(p) for p in node.properties if p[0] != "unity_scale_note"]
        if props:
            e["properties"] = props
        return e

    def scene_json(self, roots: list, environment: dict) -> dict:
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
            parent_rot = getattr(node, "_world_parent_rot", [0.0, 0.0, 0.0, 1.0])
            forward = _qrot(_qmul(parent_rot, node.rot), [0.0, 0.0, 1.0])
            env["sun"].update({"direction": _round(_mirror_pos(forward)), "color": sun["color"], "intensity": sun["intensity"], "shadows": sun["shadows"]})
        return {"thistle_scene": 1, "environment": env, "entities": [self.entity_json(n, p) for n, p in flat]}

    def write_json(self, path: Path, data) -> bool:
        if path.exists() and not self.force:
            self.warn(f"{path.relative_to(self.out).as_posix()} already exists; kept it (use --force to overwrite).")
            return False
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return True

    def convert_doc(self, rel: str, folder: str, kind: str, kind_key: str) -> None:
        objects = self.load_doc(rel)
        if objects is None:
            return
        self.environment = {}
        for o in objects.values():
            if o.class_id == 104:
                self.render_settings(o)
        roots, _anchors, _index = self.build_nodes(objects, rel)
        env = dict(self.environment)
        if "sun" in env:
            self.give_world_rotation(roots, env["sun"]["light_node"])
        dest = self.out / "assets" / folder / f"{self.names[(kind_key, rel)]}.scene.json"
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

    def write_mesh(self, text: str, dest: Path, label: str) -> None:
        if dest.exists() and not self.force:
            self.warn(f"{dest.relative_to(self.out).as_posix()} already exists; kept it (use --force to overwrite).")
            return
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(text, encoding="utf-8")
        self.count("meshes converted to .obj")

    def convert_mesh_assets(self) -> None:
        for path in sorted(self.assets.rglob("*" + MESH_ASSET_EXT)):
            rel = path.relative_to(self.assets)
            if self.only_scenes and rel.as_posix() not in self.used:
                continue
            text = path.read_text(encoding="utf-8", errors="replace") if path.stat().st_size < 200_000_000 else ""
            if not text.startswith("%YAML") or "--- !u!43 " not in text:
                continue
            for obj in parse_unity_yaml(text).values():
                if obj.class_id != 43:
                    continue
                try:
                    obj_text, _name = mesh_asset_to_obj(obj.data)
                except (ValueError, KeyError, struct.error) as why:
                    self.warn(f"Mesh {rel.as_posix()} was not converted: {why}.")
                    continue
                self.write_mesh(obj_text, self.out / "assets" / rel.with_suffix(".obj"), rel.as_posix())
        for (doc_rel, file_id), (out_rel, data) in sorted(self.embedded.items()):
            try:
                obj_text, _name = mesh_asset_to_obj(data)
            except (ValueError, KeyError, struct.error) as why:
                self.warn(f"A mesh saved inside {doc_rel} (object {file_id}) was not converted: {why}.")
                continue
            self.write_mesh(obj_text, self.out / "assets" / out_rel, out_rel)

    def dependencies(self, rel: str) -> list:
        """Files a glTF or OBJ model reads next to itself (buffers, textures, .mtl), Assets-relative."""
        path = self.assets / rel
        base = Path(rel).parent
        found = []
        try:
            if rel.lower().endswith(".gltf"):
                doc = json.loads(path.read_text(encoding="utf-8", errors="replace"))
                for entry in (doc.get("buffers") or []) + (doc.get("images") or []):
                    uri = entry.get("uri", "")
                    if uri and not uri.startswith("data:"):
                        found.append((base / uri).as_posix())
            elif rel.lower().endswith(".obj"):
                for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                    if line.startswith("mtllib "):
                        mtl = (base / line.split(None, 1)[1].strip()).as_posix()
                        found.append(mtl)
                        mtl_path = self.assets / mtl
                        if mtl_path.exists():
                            for l in mtl_path.read_text(encoding="utf-8", errors="replace").splitlines():
                                if l.strip().lower().startswith(("map_kd", "map_bump", "map_ke", "bump")):
                                    found.append((Path(mtl).parent / l.split()[-1]).as_posix())
        except (OSError, ValueError):
            pass
        return [f for f in found if (self.assets / f).exists()]

    def copy_assets(self) -> None:
        wanted = None
        if self.only_scenes:
            wanted = set(self.used)
            for rel in list(wanted):
                wanted.update(self.dependencies(rel))
        for path in sorted(self.assets.rglob("*")):
            if not path.is_file() or path.suffix == ".meta" or path.suffix.lower() in (".unity", ".prefab", ".mat", MESH_ASSET_EXT):
                continue
            ext = path.suffix.lower()
            rel = path.relative_to(self.assets)
            if wanted is not None and rel.as_posix() not in wanted:
                continue
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
            elif wanted is None and ext in (".cs", ".shader", ".anim", ".controller"):
                label = {".cs": "C# scripts (not translated)", ".shader": "shaders", ".anim": "animation clips", ".controller": "animator controllers"}[ext]
                self.skipped_types[label] = self.skipped_types.get(label, 0) + 1
            elif wanted is None and ext not in (".asmdef", ".asmref", ".dll", ".cginc", ".hlsl", ".asset", ".json", ".txt", ".md"):
                self.skipped_types[f"{ext or 'no extension'} files"] = self.skipped_types.get(f"{ext or 'no extension'} files", 0) + 1

    def find_converter(self):
        """('fbx2gltf' | 'blender', path) for something that can turn an .fbx into a .glb, or None."""
        if self.blender_arg:
            exe = shutil.which(self.blender_arg) or (self.blender_arg if Path(self.blender_arg).exists() else None)
            if exe is None:
                self.warn(f"--blender {self.blender_arg!r} isn't a program or a file.")
                return None
            return ("fbx2gltf" if "fbx2gltf" in Path(exe).name.lower() else "blender", exe)
        found = shutil.which("FBX2glTF") or shutil.which("fbx2gltf")
        if found:
            return ("fbx2gltf", found)
        found = os.environ.get("BLENDER") or shutil.which("blender")
        if not found:
            candidates = sorted(glob.glob(r"C:\Program Files\Blender Foundation\Blender *\blender.exe")) + ["/Applications/Blender.app/Contents/MacOS/Blender"]
            found = next((c for c in reversed(candidates) if Path(c).exists()), None)
        return ("blender", found) if found else None

    def convert_models(self) -> None:
        """Run each needed .fbx through Blender (or FBX2glTF) into the .glb the scenes already point at."""
        todo = sorted({rel for rel in self.to_convert if rel.lower().endswith(".fbx")})
        if not todo or not self.convert_fbx:
            return
        tool = self.find_converter()
        if tool is None:
            self.warn("Blender or FBX2glTF wasn't found, so the .fbx models below were not converted (install Blender, or pass --blender PATH).")
            return
        kind, exe = tool
        script = BLENDER_FBX_TO_GLB
        for rel in todo:
            src = self.assets / rel
            dest = self.out / "assets" / Path(rel).with_suffix(".glb")
            if dest.exists() and not self.force:
                self.to_convert.remove(rel)
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            if kind == "fbx2gltf":
                cmd = [exe, "--binary", "--input", str(src), "--output", str(dest.with_suffix(""))]
            else:
                cmd = [exe, "--background", "--factory-startup", "--python-expr", script, "--", str(src), str(dest)]
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
            except (OSError, subprocess.TimeoutExpired) as why:
                self.warn(f"{rel}: couldn't run {Path(exe).name}: {why}")
                continue
            if r.returncode != 0 or not dest.exists():
                tail = " | ".join((r.stdout + r.stderr).strip().splitlines()[-3:])
                self.warn(f"{rel}: {Path(exe).name} didn't produce a .glb ({tail[:300]})")
                continue
            self.to_convert.remove(rel)
            self.count(f"FBX models converted to .glb with {'FBX2glTF' if kind == 'fbx2gltf' else 'Blender'} (animations kept)")

    def pick_scenes(self, scenes: list) -> list:
        if not self.only_scenes:
            return scenes
        chosen = []
        for want in self.only_scenes:
            w = want.lower().removesuffix(".unity").replace("\\", "/")
            hits = [s for s in scenes if Path(s).stem.lower() == w or s.lower().removesuffix(".unity") == w or s.lower().removesuffix(".unity").endswith("/" + w)]
            if not hits:
                print(f"error: no scene matches --scene {want!r}. Scenes in this project:")
                for s in scenes[:60]:
                    print("  " + Path(s).stem)
                if len(scenes) > 60:
                    print(f"  ... and {len(scenes) - 60} more")
                return []
            chosen += [h for h in hits if h not in chosen]
        return chosen

    def run(self) -> int:
        if not self.assets.is_dir():
            print(f"error: {self.project} has no Assets/ folder - is it a Unity project?")
            return 1
        self.scan_guids()
        self.assign_names()
        all_scenes = sorted(p.relative_to(self.assets).as_posix() for p in self.assets.rglob("*.unity"))
        scenes = self.pick_scenes(all_scenes)
        if self.only_scenes and not scenes:
            return 1
        for scene in scenes:
            self.convert_doc(scene, "scenes", "scenes", "scene")
        for prefab in sorted(self.assets.rglob("*.prefab")):
            rel = prefab.relative_to(self.assets).as_posix()
            if not self.only_scenes or rel in self.used:
                self.convert_doc(rel, "prefabs", "prefabs", "prefab")
        for mat in sorted(self.assets.rglob("*.mat")):
            rel = mat.relative_to(self.assets).as_posix()
            if self.only_scenes and rel not in self.used:
                continue
            info = self.read_material(rel)
            if info and self.write_json(self.out / "assets" / "materials" / f"{self.names[('material', rel)]}.json", info):
                self.count("materials")
        self.convert_mesh_assets()
        self.copy_assets()
        self.convert_models()
        self.write_report()
        return 0

    def write_report(self) -> None:
        lines = ["# Unity import report", "", f"From `{self.project}`. See docs/unity-import.md for what this does and doesn't carry over.", ""]
        if self.only_scenes:
            lines += [f"Only the scene(s) {', '.join(self.only_scenes)} and the files they use were converted.", ""]
        lines += ["## Converted", ""]
        lines += [f"- {n} {what}" for what, n in sorted(self.counts.items())] or ["- nothing"]
        if self.to_convert:
            lines += ["", "## Models Thistle can't read yet (convert to .glb, same name, same folder under assets/)", ""]
            lines += [f"- `{p}` -> `assets/{Path(p).with_suffix('.glb').as_posix()}`" for p in sorted(set(self.to_convert))]
        if self.skipped_types:
            lines += ["", "## Left behind", ""] + [f"- {n} x {what}" for what, n in sorted(self.skipped_types.items())]
        if self.unresolved:
            lines += ["", "## Prefab overrides that name an object this tool couldn't find in the prefab (not applied)", ""]
            lines += [f"- {n} x `{key}`" for key, n in sorted(self.unresolved.items(), key=lambda kv: -kv[1])[:40]]
        if self.ignored:
            lines += ["", "## Prefab overrides with no Thistle equivalent (ignored)", ""]
            lines += [f"- {n} x `{key}`" for key, n in sorted(self.ignored.items(), key=lambda kv: -kv[1])[:25]]
        if self.warnings:
            lines += ["", "## Warnings", ""] + [f"- {w}" for w in self.warnings]
        text = "\n".join(lines) + "\n"
        (self.out / "unity-import-report.md").write_text(text, encoding="utf-8")
        print(text)


def import_unity(project: Path, out: Path, force: bool = False, **options) -> int:
    return Importer(project, out, force, **options).run()
