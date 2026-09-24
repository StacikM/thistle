# 3D physics — `three::Physics3D` (Jolt)

Rigid bodies that fall, stack, tumble, bounce and get knocked over, on [Jolt Physics](https://github.com/jrouwe/JoltPhysics) `v5.6.0`. Meters, kilograms, seconds, +Y up, the same as the rest of `thistle::three`.

It's **opt-in**. Jolt is a big library (about a minute of extra first build on a fast machine, more on a laptop), and most games don't need it. A walking character doesn't: `CharacterController` + `CollisionWorld` handle walls, floors, stairs and jumping with no physics library at all. Reach for this when things in your game should move *on their own* once pushed.

## Turning it on

In a project made by `thistle new`:

```bash
thistle enable physics3d     # writes "modules": {"physics3d": true} into thistle.json
thistle build                # reconfigures by itself and builds Jolt the first time
```

`thistle disable physics3d` turns it back off. Projects made before modules existed don't read them from `thistle.json`; `thistle enable` notices and prints the four lines to paste into your `CMakeLists.txt`.

Plain CMake: `-DTHISTLE_PHYSICS3D=ON`.

**Off**, everything below still compiles and links, so a game can treat physics as optional. Nothing simulates: `add()` returns an invalid handle, queries find nothing, and the first `add()` logs once how to turn it on. Code that must differ can check `physics3d_available()` at runtime, or `#if THISTLE_PHYSICS3D` at compile time.

## Thirty seconds of it

```cpp
using namespace thistle::three;

Physics3D physics;
physics.add_box({0, -0.5f, 0}, {40, 1, 40}, BodyType::Static);   // the ground
RigidBody crate = physics.add_box({0, 5, 0}, {1, 1, 1});          // falls onto it

app.update([&](Frame f) {
    physics.step(f.dt);
    world.box(physics.transform(crate, {1, 1, 1}), rgb(0.7f, 0.5f, 0.3f));
    world.render(f, camera);
});
```

`examples/physics_demo.cpp` is the bigger version: a crate pyramid, a plank tower, barrels, convex rocks, 26 dominoes, a ramp into a goal zone, throwing balls, blasts, and collider wireframes. It's only built with physics on.

## Bodies

Three kinds, and picking the right one matters:

- **`Static`**: never moves. Floors, walls, the level. Costs almost nothing, even thousands of them.
- **`Dynamic`**: the simulation moves it. It falls, gets pushed, spins.
- **`Kinematic`**: only *you* move it (with `move_kinematic()`), and it shoves dynamic bodies out of its way without being pushed back. Moving platforms, doors, a crusher.

`add(BodySettings{...})` takes everything; `add_box()`, `add_sphere()`, `add_static(model)` and `add_static(terrain)` are shorthands. The settings worth knowing about:

| field | default | notes |
|---|---|---|
| `density` | `500` (wood) | kg/m³; mass is the collider's volume times this. Water 1000, stone about 2500 |
| `mass` | `0` | set it to override density |
| `friction` | `0.5` | 0 = ice |
| `bounciness` | `0` | 1 = a superball |
| `gravity_scale` | `1` | 0 = floats |
| `trigger` | `false` | detects, doesn't block (see events) |
| `fast` | `false` | continuous collision for small fast things (bullets) so they can't skip through thin walls between steps |
| `can_sleep` | `true` | resting bodies stop costing CPU until touched. Turn it off for bodies you push with forces every frame |
| `layer` | `0` | 0..31, see layers |
| `user` | `0` | yours: an entity id, an index |

### Colliders

| builder | what it is | cost |
|---|---|---|
| `Collider::box(size)`, `sphere(r)`, `capsule(r, h)`, `cylinder(r, h)` | exact primitives | cheapest |
| `Collider::convex(points / MeshData / Model)` | shrink-wrapped hull of the points | cheap. Good for rocks, barrels, props |
| `Collider::mesh(MeshData / Model)` | the exact triangles | static/kinematic only |
| `Collider::compound({...})` | several glued together, each placed with `.at(offset, rotation)` | a sum of its parts |

A **dynamic body given a mesh collider uses its convex hull instead**. Jolt can't collide two triangle meshes with each other, and a moving concave mesh is almost never what you want anyway. Hulls are capped at 256 corners (Jolt's limit) and simplified automatically past that. A flat or degenerate cloud (a single quad) becomes a thin box instead of failing.

`capsule()` and `cylinder()` stand upright, and `h` includes the caps, the same as `capsule_mesh()`. `.at()` moves a collider inside its body: `Collider::box({1, 2, 1}).at({0, 1, 0})` puts the box's bottom at the body's origin, handy when your model's origin is at its feet.

## Drawing what it simulated

`physics.transform(body)` is a `Transform` (position + rotation, scale 1) ready for `world.draw(model, ...)`. The optional second argument goes into the result's scale, which is what the unit shapes want:

```cpp
world.draw(barrel_model, physics.transform(barrel));
world.box(physics.transform(crate, crate_size), brown);     // 1 m cube, scaled
world.sphere(physics.transform(ball, {d, d, d}), red);      // 1 m sphere, scaled
```

`World::box/sphere/cylinder/cone(const Transform&, color)` were added for this. They're the rotated versions of the center-and-size ones.

## Moving things yourself

- `set_position()` / `set_rotation()`: **teleport**. Things in the way get shoved out violently next step. Use them for spawning and respawning, not for movement.
- `move_kinematic(body, pos, rot, f.dt)`: the way to move a `Kinematic` body. It arrives there by the next step and pushes things along properly. Call it before `step()`, with the same `dt`.
- `apply_impulse()`: an instant kick. The units are momentum (kg·m/s), so `mass(body) * change_in_velocity`. Pass a point to set it spinning too.
- `apply_force()` / `apply_torque()`: last **one step**. Call them every frame for a steady push.
- `set_velocity()`: when you just want it going that fast, now.
- `set_type()`: Dynamic ↔ Kinematic is how "pick it up" (kinematic, carried by `move_kinematic`) and "drop it" work. A body *added* as `Static` can't be made to move; add it as `Kinematic` if it ever will.

## Queries

- `raycast(ray, max_distance, ignore)`: nearest body, with point, normal, distance and which body. Triggers are skipped, and so is `ignore` (whoever fired).
- `overlap_sphere()` / `overlap_box()`: every body actually touching the volume. Exact shapes, not just bounding boxes; triggers skipped.
- `explode(center, radius, speed)`: every dynamic body in range flies away from the center at up to `speed` m/s, full strength at the center and nothing at the edge. **The speed is the same for a pebble and a car**, which is much easier to tune than a force. The kick lands on each body's near side, so off-center pieces spin. It returns how many bodies it moved.

## Events: contacts and triggers

After each `step()`:

- `contacts()` lists pairs of bodies that **started touching**, with a point, a normal (from `a` toward `b`), and `speed`, the closing speed at impact. Use `speed` for impact sounds, dust (the demo does), and damage. A pair touching through several sub-shapes (a compound) is reported once.
- `trigger_events()` lists bodies **entering and leaving** triggers.

Or register `on_contact()` / `on_trigger()`: they're called at the end of `step()` on your thread, so adding and removing bodies from inside them is fine.

Behavior worth knowing, all covered by the test:

- **Sleep.** When two resting bodies fall asleep, Jolt forgets their contact. When something wakes them later, they're reported as touching *again*, with a `speed` near 0. That's why you filter sounds on `speed`.
- **Triggers don't lose sleeping bodies.** Jolt normally drops a sleeping body from a sensor. A crate resting inside a goal zone would "leave" the moment it fell asleep. Triggers here are kept awake as kinematic sensors, which see sleeping bodies. On top of that, a contact that's dropped and found again within one step isn't reported as leave-and-re-enter. The cost: a real exit is reported **one step (1/60 s) late**.
- **`remove()`-ing a body that's inside a trigger** reports it leaving on the next step.

## Layers

32 layers; every pair collides until you say otherwise:

```cpp
enum { WORLD = 0, DEBRIS = 1, PLAYER_SHOTS = 2 };
physics.set_layers_collide(DEBRIS, DEBRIS, false);         // rubble doesn't pile up on itself
physics.set_layers_collide(PLAYER_SHOTS, PLAYER_SHOTS, false);
```

Triggers go by layers too: a trigger only notices bodies on layers it collides with.

## Debug view

`physics.draw_debug(world)` draws every collider as wireframe: **green** awake, **gray** asleep, **blue** static, **yellow** triggers. Boxes, spheres, capsules, cylinders and hulls are drawn as their real shape; triangle meshes just as their bounds (all of a level's triangles would bury everything). `F` in the demo.

## Stepping

`step(f.dt)` once per frame. Frames longer than 1/60 s are split into 1/60 s collision steps, up to 4. Past that (a 200 ms hitch) the rest is dropped: the game runs slow for a moment instead of freezing up while physics catches up. There's no interpolation between steps. At high frame rates each frame is simply one shorter step, which is smooth.

That also means results depend on your frame rate's `dt` sequence, so **it's not deterministic across machines**. Two players running the same simulation locally will drift apart. Networked physics has to come from one authority (the server) sending positions. Jolt has a cross-platform-deterministic mode; it isn't turned on.

## Choices made for you (and how to undo them)

- **CPU baseline: SSE4.2, not AVX2.** Jolt's own default compiles for AVX2/FMA (Intel Haswell, 2013, and later). A game built that way crashes at startup on CPUs without it: some Pentiums/Celerons still in use, some VMs. Jolt's flags also reach every engine file that links it. SSE4.2 runs everywhere that matters. If you know your players' hardware: `-DUSE_AVX2=ON` (plus `USE_AVX`, `USE_FMADD`, …), since those defaults aren't forced.
- **Resting penetration: 5 mm instead of Jolt's 2 cm.** At 2 cm, a crate on the floor visibly sank into it (the test caught it: 0.480 instead of 0.5). 5 mm reads as "on the floor" and still stacks steadily (a 6-box tower drifts about 1 cm in 5 s).
- **Worker threads: cores − 1, at most 8**, shared by every `Physics3D`. Your own thread also runs physics jobs while it waits in `step()`.
- **Limits:** `Physics3D(max_bodies)` defaults to 65,536 bodies, with room for a quarter that many contact pairs at once. Past that, contacts get skipped (things sink into each other) and it logs once. Pass a bigger number if you see that.
- **Build settings Jolt ships with that were off-by-default'd for game builds:** its tests and samples, `-Werror`, trapping floating-point exceptions, its profiler and debug renderer in release builds, the GPU compute backends (which would need the DX12/Vulkan/Metal SDKs installed), a static MSVC runtime that won't link with the engine's, link-time optimization and AVX-512.

## What isn't here

Joints and constraints (hinges, ropes, ragdolls), vehicles, soft bodies, and saving/loading physics state are all things Jolt can do, but they aren't wrapped yet. **`CharacterController` doesn't collide with `Physics3D` bodies**: a walking character goes straight through a crate. The two systems aren't connected yet. For now, give things the character should bump into a `CollisionWorld` box too.

## What's actually been verified

- **`physics3d_smoketest`** (a ctest, built both ways) simulates at 60 Hz and checks against hand-worked numbers:
  - a dropped box's resting height, its impact speed in the contact event (9.41 m/s measured vs 9.4 expected), and it falling asleep
  - a 6-box stack staying up
  - raycasts (distance, normal, `ignore`, `max_distance`) and overlaps
  - triggers: one enter, **no false exit while the body inside sleeps**, one exit on leaving, and one on removal
  - layers, `explode()` (in range vs out of range, direction, waking)
  - a kinematic pusher moving exactly and shoving a crate
  - `set_type()` both ways
  - every collider kind resting at the right height, and `add_static()` with a model and a terrain
  - a `fast` bullet at 300 m/s not tunneling through a 10 cm wall
  - stale and made-up handles, `clear()`, and a second `Physics3D` alongside the first

  Without Jolt, the same test checks the stand-in does nothing, safely. It passed repeatedly (Jolt is multithreaded, so flakiness was the worry) on Linux, GCC, Release.
- **`physics_demo`** was run and played on Linux under Xvfb with Mesa's llvmpipe software renderer, driven by xdotool and screenshotted. Throwing balls, the domino chain, the goal counting 5 balls in, a blast collapsing the pyramid, and the debug colors all behaved. That's a software GL driver, not a real GPU: it proves the simulation and drawing, not driver compatibility.
- **The CLI flow** was run end to end in a fresh `thistle new` project: enable → build → `THISTLE_PHYSICS3D` set and a box landing at 0.495. Disable → the next build reconfigures by itself → the stand-in and its log line. And the warning for an old project's `CMakeLists.txt`.
- **Windows**: cross-compiled and linked with MinGW-w64 (GCC) with physics on. **Not run**: there was no Windows machine or Wine here. MSVC, macOS (Clang, Apple Silicon) and the stand-in build are covered by CI's `physics3d: [OFF, ON]` matrix, which builds everything and runs the ctests there. That proves compiling and headless simulation, not the demo on screen.
- **iOS: not built yet** with physics on. Jolt supports it (ARM64, NEON), but nobody has tried it here, and it's listed as a gap until someone does.
- **Web: not supported.** Jolt's thread pool needs pthreads under Emscripten, and nobody has tried it.
