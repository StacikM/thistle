#!/usr/bin/env python3
"""Checks `thistle import-unity` against a small Unity project written out here
by hand, in Unity's text serialization as documented (no Unity was available to
make a real one, so this proves the importer reads that format, not that it
reads every real project). Then, if given the path to thistle_scene_dump, loads
the result with the engine's own three::Scene3D. Run by ctest as
unity_import_smoketest; standard library only."""
import json
import math
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


def near(a, b, eps=1e-4) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


def meta(path: Path, guid: str) -> None:
    Path(str(path) + ".meta").write_text(f"fileFormatVersion: 2\nguid: {guid}\n", encoding="utf-8")


HEADER = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"

G_SHIP, G_ROCK, G_CRATE, G_RED, G_DOOR, G_TEX = ("a" * 32, "b" * 32, "c" * 32, "d" * 32, "e" * 32, "f" * 32)

SCENE = HEADER + """--- !u!104 &2
RenderSettings:
  m_ObjectHideFlags: 0
  serializedVersion: 9
  m_Fog: 1
  m_FogColor: {r: 0.5, g: 0.6, b: 0.7, a: 1}
  m_FogMode: 1
  m_FogDensity: 0.01
  m_LinearFogStart: 20
  m_LinearFogEnd: 120
  m_AmbientMode: 3
  m_AmbientSkyColor: {r: 0.2, g: 0.3, b: 0.4, a: 1}
  m_AmbientIntensity: 0.8
  m_SkyboxMaterial: {fileID: 0}
--- !u!1 &10
GameObject:
  m_ObjectHideFlags: 0
  serializedVersion: 6
  m_Component:
  - component: {fileID: 11}
  - component: {fileID: 12}
  m_Layer: 0
  m_Name: Sun
  m_TagString: Untagged
  m_IsActive: 1
--- !u!4 &11
Transform:
  m_ObjectHideFlags: 0
  m_GameObject: {fileID: 10}
  m_LocalRotation: {x: 0.5, y: 0, z: 0, w: 0.8660254}
  m_LocalPosition: {x: 0, y: 10, z: 0}
  m_LocalScale: {x: 1, y: 1, z: 1}
  m_Children: []
  m_Father: {fileID: 0}
  m_RootOrder: 0
--- !u!108 &12
Light:
  m_ObjectHideFlags: 0
  m_GameObject: {fileID: 10}
  m_Type: 1
  m_Color: {r: 1, g: 0.9, b: 0.8, a: 1}
  m_Intensity: 1.5
  m_Range: 10
  m_Shadows:
    m_Type: 2
--- !u!1 &20
GameObject:
  m_Component:
  - component: {fileID: 21}
  - component: {fileID: 22}
  - component: {fileID: 23}
  m_Layer: 0
  m_Name: Floor Cube
  m_TagString: Ground
  m_IsActive: 1
--- !u!4 &21
Transform:
  m_GameObject: {fileID: 20}
  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}
  m_LocalPosition: {x: 1, y: 2, z: 3}
  m_LocalScale: {x: 4, y: 0.5, z: 6}
  m_Children:
  - {fileID: 31}
  m_Father: {fileID: 0}
  m_RootOrder: 1
--- !u!33 &22
MeshFilter:
  m_GameObject: {fileID: 20}
  m_Mesh: {fileID: 10202, guid: 0000000000000000e000000000000000, type: 0}
--- !u!23 &23
MeshRenderer:
  m_GameObject: {fileID: 20}
  m_Materials:
  - {fileID: 2100000, guid: dddddddddddddddddddddddddddddddd, type: 2}
--- !u!1 &30
GameObject:
  m_Component:
  - component: {fileID: 31}
  - component: {fileID: 32}
  - component: {fileID: 34}
  m_Layer: 0
  m_Name: Child Ball
  m_TagString: Untagged
  m_IsActive: 0
--- !u!4 &31
Transform:
  m_GameObject: {fileID: 30}
  m_LocalRotation: {x: 0, y: 0.7071068, z: 0, w: 0.7071068}
  m_LocalPosition: {x: 0, y: 1, z: 2}
  m_LocalScale: {x: 1, y: 1, z: 1}
  m_Children: []
  m_Father: {fileID: 21}
  m_RootOrder: 0
--- !u!33 &32
MeshFilter:
  m_GameObject: {fileID: 30}
  m_Mesh: {fileID: 10207, guid: 0000000000000000e000000000000000, type: 0}
--- !u!114 &34
MonoBehaviour:
  m_GameObject: {fileID: 30}
  m_Script: {fileID: 11500000, guid: eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee, type: 3}
  health: 20
  label: Bouncy
  target: {fileID: 0}
--- !u!1 &40
GameObject:
  m_Component:
  - component: {fileID: 41}
  - component: {fileID: 42}
  m_Layer: 0
  m_Name: Exit
  m_TagString: Untagged
  m_IsActive: 1
--- !u!4 &41
Transform:
  m_GameObject: {fileID: 40}
  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}
  m_LocalPosition: {x: 0, y: 0, z: 10}
  m_LocalScale: {x: 1, y: 1, z: 1}
  m_Children: []
  m_Father: {fileID: 0}
  m_RootOrder: 2
--- !u!65 &42
BoxCollider:
  m_GameObject: {fileID: 40}
  m_IsTrigger: 1
  m_Size: {x: 2, y: 3, z: 4}
  m_Center: {x: 0, y: 1.5, z: 0}
--- !u!1001 &100
PrefabInstance:
  m_ObjectHideFlags: 0
  serializedVersion: 2
  m_Modification:
    m_TransformParent: {fileID: 0}
    m_Modifications:
    - target: {fileID: 9001, guid: cccccccccccccccccccccccccccccccc, type: 3}
      propertyPath: m_LocalPosition.x
      value: 5
      objectReference: {fileID: 0}
    - target: {fileID: 9001, guid: cccccccccccccccccccccccccccccccc, type: 3}
      propertyPath: m_LocalPosition.z
      value: -7
      objectReference: {fileID: 0}
    - target: {fileID: 9000, guid: cccccccccccccccccccccccccccccccc, type: 3}
      propertyPath: m_Name
      value: Crate A
      objectReference: {fileID: 0}
    - target: {fileID: 9003, guid: cccccccccccccccccccccccccccccccc, type: 3}
      propertyPath: m_LocalPosition.y
      value: 99
      objectReference: {fileID: 0}
    m_RemovedComponents: []
  m_SourcePrefab: {fileID: 100100000, guid: cccccccccccccccccccccccccccccccc, type: 3}
--- !u!1001 &101
PrefabInstance:
  m_Modification:
    m_TransformParent: {fileID: 0}
    m_Modifications:
    - target: {fileID: 400000, guid: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa, type: 3}
      propertyPath: m_LocalPosition.y
      value: 4
      objectReference: {fileID: 0}
    m_RemovedComponents: []
  m_SourcePrefab: {fileID: 100100000, guid: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa, type: 3}
"""

PREFAB = HEADER + """--- !u!1 &9000
GameObject:
  m_Component:
  - component: {fileID: 9001}
  - component: {fileID: 9010}
  m_Layer: 0
  m_Name: Crate
  m_TagString: Untagged
  m_IsActive: 1
--- !u!4 &9001
Transform:
  m_GameObject: {fileID: 9000}
  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}
  m_LocalPosition: {x: 0, y: 0, z: 0}
  m_LocalScale: {x: 1, y: 1, z: 1}
  m_Children:
  - {fileID: 9003}
  m_Father: {fileID: 0}
  m_RootOrder: 0
--- !u!33 &9010
MeshFilter:
  m_GameObject: {fileID: 9000}
  m_Mesh: {fileID: 10202, guid: 0000000000000000e000000000000000, type: 0}
--- !u!1 &9002
GameObject:
  m_Component:
  - component: {fileID: 9003}
  - component: {fileID: 9011}
  m_Layer: 0
  m_Name: Lid Lamp
  m_TagString: Untagged
  m_IsActive: 1
--- !u!4 &9003
Transform:
  m_GameObject: {fileID: 9002}
  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}
  m_LocalPosition: {x: 0, y: 1, z: 0}
  m_LocalScale: {x: 1, y: 1, z: 1}
  m_Children: []
  m_Father: {fileID: 9001}
  m_RootOrder: 0
--- !u!108 &9011
Light:
  m_GameObject: {fileID: 9002}
  m_Type: 0
  m_Color: {r: 1, g: 1, b: 1, a: 1}
  m_Intensity: 2
  m_Range: 8
  m_SpotAngle: 60
"""

MAT = HEADER + """--- !u!21 &2100000
Material:
  serializedVersion: 6
  m_Name: Red
  m_Shader: {fileID: 46, guid: 0000000000000000f000000000000000, type: 0}
  m_SavedProperties:
    serializedVersion: 3
    m_TexEnvs:
    - _MainTex:
        m_Texture: {fileID: 2800000, guid: ffffffffffffffffffffffffffffffff, type: 3}
        m_Scale: {x: 1, y: 1}
        m_Offset: {x: 0, y: 0}
    m_Floats:
    - _Glossiness: 0.25
    - _Metallic: 0.9
    - _Mode: 0
    m_Colors:
    - _Color: {r: 0.8, g: 0.1, b: 0.1, a: 1}
    - _EmissionColor: {r: 0, g: 0, b: 0, a: 1}
"""


def main() -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from unity_import import parse_unity_yaml, _flow

    print("the YAML subset")
    check(_flow("{fileID: 10202, guid: 0000000000000000e000000000000000, type: 0}") == {"fileID": 10202, "guid": "0000000000000000e000000000000000", "type": 0},
          "a guid made only of digits and one 'e' stays a string")
    check(_flow("{x: -1.5e-05, y: 2, z: 0}") == {"x": -1.5e-05, "y": 2, "z": 0}, "flow mappings with floats and ints")
    check(_flow("[]") == [] and _flow("{}") == {}, "empty flow collections")
    objs = parse_unity_yaml(SCENE)
    check({1, 4, 33, 23, 108, 114, 65, 1001, 104} <= {o.class_id for o in objs.values()}, "every document of the sample scene is found")
    check(objs[12].data["m_Shadows"]["m_Type"] == 2, "a nested block mapping")
    check(objs[10].data["m_Component"] == [{"component": {"fileID": 11}}, {"component": {"fileID": 12}}], "a sequence at its key's own indent")
    check(objs[100].data["m_Modification"]["m_Modifications"][2]["value"] == "Crate A", "a block sequence of mappings, with a string value with a space")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        unity, out = root / "UnityGame", root / "ThistleGame"
        a = unity / "Assets"
        for d in ("Scenes", "Prefabs", "Materials", "Models", "Textures", "Scripts"):
            (a / d).mkdir(parents=True)
        out.mkdir()
        (a / "Scenes" / "Main.unity").write_text(SCENE, encoding="utf-8")
        (a / "Prefabs" / "Crate.prefab").write_text(PREFAB, encoding="utf-8")
        (a / "Materials" / "Red.mat").write_text(MAT, encoding="utf-8")
        (a / "Models" / "ship.glb").write_bytes(b"glTF")
        (a / "Models" / "rock.fbx").write_bytes(b"Kaydara FBX Binary")
        (a / "Textures" / "t.png").write_bytes(b"\x89PNG")
        (a / "Scripts" / "Door.cs").write_text("public class Door : MonoBehaviour {}", encoding="utf-8")
        meta(a / "Models" / "ship.glb", G_SHIP)
        meta(a / "Models" / "rock.fbx", G_ROCK)
        meta(a / "Prefabs" / "Crate.prefab", G_CRATE)
        meta(a / "Materials" / "Red.mat", G_RED)
        meta(a / "Scripts" / "Door.cs", G_DOOR)
        meta(a / "Textures" / "t.png", G_TEX)

        print("importing")
        r = subprocess.run([sys.executable, str(CLI), "import-unity", str(unity), "--out", str(out)], capture_output=True, text=True, encoding="utf-8", errors="replace")
        check(r.returncode == 0, "thistle import-unity exits 0")
        if r.returncode != 0:
            print(r.stdout + r.stderr)
        scene_path = out / "assets" / "scenes" / "Main.scene.json"
        check(scene_path.exists(), "assets/scenes/Main.scene.json is written")
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        ents = {e["name"]: (i, e) for i, e in enumerate(scene["entities"])}

        print("the scene")
        check(scene["thistle_scene"] == 1, "format version")
        floor_i, floor = ents["Floor Cube"]
        check(floor["kind"] == "box" and floor["parent"] == -1, "a built-in cube is a box at the top")
        check(near(floor["position"], [1, 2, -3]), "position is mirrored across Z (Unity left-handed -> Thistle right-handed)")
        check(near(floor["scale"], [4, 0.5, 6]), "scale carries over")
        check(near(floor["color"], [0.8, 0.1, 0.1, 1]), "the material's color becomes the shape's color")
        check(["tag", "Ground"] in floor["properties"], "a non-default tag is kept as a property")
        ball_i, ball = ents["Child Ball"]
        check(ball["parent"] == floor_i and ball["kind"] == "sphere", "a child object is parented to the right entity")
        check(near(ball["position"], [0, 1, -2]), "child position mirrored")
        check(near(ball["rotation"], [0, -0.7071068, 0, 0.7071068]), "a rotation about Y flips direction when Z is mirrored")
        check(["active", "false"] in ball["properties"], "an inactive object is marked")
        check(["unity_script", "Door"] in ball["properties"] and ["Door.health", "20"] in ball["properties"] and ["Door.label", "Bouncy"] in ball["properties"],
              "a script's simple fields become properties")
        check(all(not k.startswith("Door.target") for k, _v in ball["properties"]), "a script's object references don't")
        exit_i, exit_ = ents["Exit"]
        trig = ents["Exit (trigger)"][1]
        check(exit_["kind"] == "empty" and trig["kind"] == "trigger" and trig["parent"] == exit_i, "a trigger collider becomes a trigger child")
        check(near(trig["scale"], [2, 3, 4]) and near(trig["position"], [0, 1.5, 0]), "the trigger takes the collider's size and center")
        crate = ents["Crate A"][1]
        check(crate["kind"] == "box" and near(crate["position"], [5, 0, 7]), "a prefab instance is expanded, with its name and position overrides applied")
        lamp = ents["Lid Lamp"][1]
        check(lamp["kind"] == "spot_light" and lamp["parent"] == ents["Crate A"][0], "the prefab's child light came along, under the crate")
        check(abs(lamp["spot_angle"] - math.radians(30)) < 1e-5 and lamp["range"] == 8, "Unity's full spot angle becomes Thistle's half angle")
        check(near(lamp["position"], [0, 1, 0]), "an override aimed at a nested object (fileID 9003) did not move the lamp")
        ship = next(e for e in scene["entities"] if e["kind"] == "model")
        check(ship["model"] == "assets/Models/ship.glb" and near(ship["position"], [0, 4, 0]), "a prefab instance of a .glb is a model entity, with its override")
        check("Directional Light" not in ents and "Sun" not in ents, "a directional light is the sun, not an entity")
        sun = scene["environment"]["sun"]
        check(sun["intensity"] == 1.5 and sun["shadows"] is True, "sun intensity and shadows")
        # light rotated 60 degrees about X: forward (0,0,1) -> (0, -sin 60, cos 60) = (0, -0.866, 0.5) in Unity; Z mirrored in Thistle
        check(near(sun["direction"], [0.0, -0.8660254, -0.5]), "the sun's direction comes from the light's rotation, mirrored")
        fog = scene["environment"]["fog"]
        check(fog["enabled"] and fog["start"] == 20 and fog["end"] == 120 and fog["match_sky"] is False, "linear fog settings")
        check(near(scene["environment"]["sky"]["top"], [0.2, 0.3, 0.4, 1.0]) and scene["environment"]["ambient"] == 0.8, "flat ambient color and intensity")

        print("prefabs, materials, assets, report")
        check((out / "assets" / "prefabs" / "Crate.scene.json").exists(), "the prefab itself is written as a scene fragment")
        pre = json.loads((out / "assets" / "prefabs" / "Crate.scene.json").read_text(encoding="utf-8"))
        check([e["name"] for e in pre["entities"]] == ["Crate", "Lid Lamp"] and pre["entities"][1]["parent"] == 0, "the fragment has its hierarchy")
        mat = json.loads((out / "assets" / "materials" / "Red.json").read_text(encoding="utf-8"))
        check(mat["metallic"] == 0.9 and abs(mat["roughness"] - 0.75) < 1e-6 and mat["texture"] == "assets/Textures/t.png", "a Standard material: metallic, roughness = 1 - smoothness, texture path")
        check((out / "assets" / "Models" / "ship.glb").exists() and (out / "assets" / "Textures" / "t.png").exists(), "models and textures are copied")
        check(not (out / "assets" / "Models" / "rock.fbx").exists(), "an .fbx isn't copied (Thistle can't read it)")
        report = (out / "unity-import-report.md").read_text(encoding="utf-8")
        check("C# script Door" in report, "the report lists the script it didn't translate")
        check("rock.fbx" in report, "the report lists the .fbx to convert")
        check("overrides on objects inside" in report, "the report mentions the override it couldn't apply")

        print("re-running")
        before = scene_path.read_text(encoding="utf-8")
        scene_path.write_text("edited", encoding="utf-8")
        subprocess.run([sys.executable, str(CLI), "import-unity", str(unity), "--out", str(out)], capture_output=True)
        check(scene_path.read_text(encoding="utf-8") == "edited", "an existing file is kept without --force")
        subprocess.run([sys.executable, str(CLI), "import-unity", str(unity), "--out", str(out), "--force"], capture_output=True)
        check(scene_path.read_text(encoding="utf-8") == before, "--force overwrites it, and the output is the same as the first time")

        print("not a Unity project")
        r = subprocess.run([sys.executable, str(CLI), "import-unity", str(root), "--out", str(out)], capture_output=True, text=True)
        check(r.returncode != 0 and "Assets" in r.stdout, "a folder without Assets/ is refused, clearly")

        if len(sys.argv) > 1:
            print("the engine loads it")
            r = subprocess.run([sys.argv[1], str(scene_path)], capture_output=True, text=True)
            check(r.returncode == 0, "three::Scene3D::load accepts the imported scene")
            lines = r.stdout.strip().splitlines()
            check(any(l.startswith("Floor Cube|box|-1|1.000 2.000 -3.000") for l in lines), "...and puts the floor where the importer says")
            check(any(l.startswith("Child Ball|sphere|") for l in lines) and len(lines) == len(scene["entities"]) + 1, "...with every entity")
            r = subprocess.run([sys.argv[1], str(out / "assets" / "prefabs" / "Crate.scene.json")], capture_output=True, text=True)
            check(r.returncode == 0 and "Lid Lamp|spot_light|0|" in r.stdout, "...and the prefab fragment too")

    print("all passed" if failures == 0 else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
