# 3D levels (`three::Scene3D`)

A level made in the [Thistle Editor](../tools/thistle-editor/README.md), or in code: named things, each a model, a built-in shape, a light, a trigger volume, a spawn point or an empty (a group), with a transform relative to its parent. Plus the environment: sun, sky, fog, ambient light. The editor saves one as `assets/scenes/<name>.scene.json`, and your game loads it.

```cpp
using namespace thistle::three;

Scene3D level;
level.load("assets/scenes/level1.scene.json");

const int start = level.find("player_start");
if (start >= 0) player.position = level.world_transform(start).position;

app.update([&](Frame f) {
    level.draw(world);          // models, shapes and lights, plus sun/sky/fog
    world.render(f, camera);
});
```

`draw()` goes every frame, like every other `World` call. It draws the models and shapes, adds the lights, and (unless you pass `environment = false`) sets `world.sun`, `world.sky`, `world.fog` and `world.ambient` from the file. A cube-map skybox isn't part of the file, so one you put on `world.sky.skybox` stays.

Triggers, spawn points and empties don't draw. They're there for your game to read.

## Reading a level

`level.entities` is a plain vector of `SceneEntity`: `name`, `kind`, `parent` (an index, -1 at the top), `transform` (relative to the parent), `model` (a path), `color`, light settings, and `properties`.

- **Where something is**: `world_transform(i)` goes through its parents. `transform` alone is relative to the parent, and it's only the same thing for entities at the top.
- **Finding things**: `find("name")` (the first one with that name, or -1), `children(i)` (`children(-1)` gives the top-level ones), or just loop over `entities` and look at `kind`.
- **Properties** are free-form string pairs set in the editor's Inspector: `e.property("health", "100")` returns the value, or the fallback if it isn't set. Parse numbers yourself (`std::stof`). The editor attaches no meaning to them. They're how a level says "this door goes to level 2" or "this crate has 20 hp".
- **Triggers**: `level.inside(i, point)` says whether a point is inside the entity's box, rotation and parents' scale included. That's the whole trigger API. Check your player against the triggers every frame, and track whether they were already inside if you want "on enter".

```cpp
bool was_in_exit = false;
// every frame:
const int exit = level.find("exit");
const bool in_exit = exit >= 0 && level.inside(exit, player.position);
if (in_exit && !was_in_exit) load_next_level(level.entities[exit].property("goes_to"));
was_in_exit = in_exit;
```

- **Physics**: a level doesn't make physics bodies by itself, because only your game knows what should be solid, what should fall and what's decoration. With `Physics3D` (see [physics3d.md](physics3d.md)), the usual start is to make the shapes solid:

```cpp
for (int i = 0; i < (int)level.entities.size(); ++i) {
    const SceneEntity& e = level.entities[i];
    const Transform t = level.world_transform(i);
    if (e.kind == SceneEntity::Kind::Box || e.kind == SceneEntity::Kind::Plane) {
        BodySettings b;
        b.collider = Collider::box(e.kind == SceneEntity::Kind::Plane ? vec3{t.scale.x, 0.02f, t.scale.z} : t.scale);
        b.type = BodyType::Static;
        b.position = t.position;
        b.rotation = t.rotation;
        physics.add(b);
    } else if (e.kind == SceneEntity::Kind::Model) {
        physics.add_static(level.model(i), t);   // its triangles, exactly
    }
}
```

`model(i)` is the loaded model of a `Kind::Model` entity. `local_bounds(i)` is its unrotated box before the transform: the model's own for models, a unit box for shapes and triggers, a person-sized box for spawns.

## Building or changing one in code

It's all plain data, so games can make levels too, or change one after loading it: `add(entity)` returns the new index, `remove(i)` also removes everything under it (and indices after it shift down), `set_parent(child, parent)` keeps it where it is in the world (and refuses to put something under its own child), and `set_world_transform(i, t)` places something in world space whatever its parent. `save(path)` writes the file.

## Shapes and sizes

Shapes are unit-sized and scaled by the transform, the same as `World::box()` and friends: a box is 1×1×1, a sphere is 1 across, a cylinder and a cone are 1 across and 1 tall, and a plane is 1×1. All are centered on their position. So a `scale` of (4, 1, 2) on a box makes a 4×1×2 box, and on a plane a 4×2 floor.

## The file

JSON, written with sorted keys and two-space indents so it diffs well in git:

```json
{
  "thistle_scene": 1,
  "environment": {
    "ambient": 0.55,
    "sun": {"direction": [..], "color": [..], "intensity": 1, "shadows": true, "shadow_distance": .., "shadow_strength": ..},
    "sky": {"top": [..], "horizon": [..], "ground": [..], "sun_disc": true, "visible": true},
    "fog": {"enabled": false, "start": 30, "end": 150, "color": [..], "match_sky": true}
  },
  "entities": [
    {"name": "crate", "kind": "box", "parent": -1,
     "position": [0, 0.5, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1], "color": [0.75, 0.6, 0.45, 1],
     "properties": [["hp", "20"]]}
  ]
}
```

- `kind` is one of `empty`, `model`, `box`, `sphere`, `cylinder`, `cone`, `plane`, `point_light`, `spot_light`, `trigger`, `spawn`. Anything else reads as `empty`.
- `rotation` is a quaternion (x, y, z, w). `spot_angle` is in radians, half the cone's opening.
- Models add `"model": "assets/ship.glb"`, lights add `intensity` and `range`, spot lights add `spot_angle`. Properties are a list of pairs, so their order is kept.
- `parent` is an index into `entities`. On load, a parent that's missing, or that would make a loop, is set to -1 instead of trusted, so a hand-edited file can't hang anything.
- Loading and re-saving without changes gives the identical file.

## Things to know

- **Paths are relative to the working directory**, like every other file in Thistle: the editor writes `assets/...` and the game runs with its `assets/` folder beside it.
- **Models are loaded once per path and shared** by every scene in the program. They're never unloaded (the editor reloads the scene on every undo, so re-loading them would be slow). For a game with a few levels, that's fine. With hundreds of distinct models across levels, they all stay in memory.
- **`world_transform` walks up the parents each call.** Levels have hundreds of things, not millions, so that's cheap, but cache it for anything that doesn't move.

## What's been verified

- **`scene3d_smoketest`** (ctest, headless) checks:
  - world transforms through a rotated, scaled parent, against hand-computed positions
  - `set_world_transform` and `set_parent` keeping things in place
  - refusing a parent loop
  - `remove()` taking children along and renumbering the rest
  - properties
  - the JSON round trip being byte-identical
  - repairing a file with bad parents
  - `inside()` on a turned trigger under a scaled parent, with points just inside and just outside each face
  - `draw()` applying the environment
  - `to_euler()`, which the editor uses to show rotations as angles
- The editor was used to make, save and reopen scenes under Xvfb. The saved files were checked value by value (see the editor's README for exactly what was clicked).
- No game has loaded an editor-made level yet. The templates coming next are the first real users.
