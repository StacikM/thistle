# Skeletal animation (`thistle::three`)

Animated characters from glTF files: a skeleton, animation clips (walk, run, idle...), and an `Animator` per character that plays them.

```cpp
using namespace thistle::three;

Model fox = load_model("assets/Fox.glb");
Animator anim(fox);
anim.play("Walk");

app.update([&](Frame f) {
    anim.play(moving ? "Run" : "Walk");   // fine every frame: only a change does anything
    anim.update(f.dt);
    world.draw(fox, fox_transform, anim);
    world.render(f, camera);
});
```

## What's loaded

`load_model()` / `load_model_data()` read, from a `.gltf`/`.glb`:

- **The skeleton** (`ModelData::skeleton`, `model_skeleton(model)`): joint names, parents, rest poses, inverse bind matrices. Transforms of non-joint nodes above the skeleton (an "Armature" node, often rotated by Blender's exporter) are kept as each root's `root_offset`.
- **Per-vertex skinning**: up to 4 joints and weights (`Vertex::joints` / `weights`).
- **Every animation** (`ModelData::animations`, `model_animation()`, `find_animation()`): translation, rotation and scale keyframes per joint. `LINEAR` blends, `STEP` holds. `CUBICSPLINE` keys are read as their values and blended linearly, so they're close, not exact.

A skinned model drawn **without** an Animator stands in its rest pose, placed by its joints as glTF requires. The mesh node's own transform doesn't apply to a skinned mesh. Before this, the loader applied it, and `Fox.glb` showed up in the wrong place. That's fixed.

Not loaded: morph targets (blend shapes, facial animation), animations of non-joint nodes (a door opening via its node), and more than one skin per file (meshes on the other skins load unanimated, with a warning).

## `Animator`

One per character. They're cheap, and many can share one `Model`.

- `play(name_or_index, fade = 0.2, loop = true)` switches clips, **crossfading** from the current pose over `fade` seconds. The clip being faded out keeps playing while it fades, like every engine does it. Playing the clip that's already on changes nothing (except `loop`), so `play(moving ? "Run" : "Idle")` every frame is fine.
- `update(dt)` advances the clip. `speed` scales it: 2 = double, negative = backwards. `set_time()` jumps.
- `finished()` is true when a non-looping clip has reached its end: for "attack, then back to idle".
- `stop(fade)` blends back to the rest pose.
- **Attaching things**: `joint_matrix(find_joint("hand_r"))` is that joint's current placement in the model's space. Multiply by the character's transform for the world: a sword in the hand, a hat on the head.

## Building your own

The data types are plain structs, so skeletons and clips can come from code as well as files. `examples/animation_demo.cpp` builds a low-poly robot from boxes (9 joints, each box moved by one joint), with a walk cycle and an idle clip, all in about 80 lines. It then runs 24 of them around in circles.

## How it's drawn

**Skinning is done on the CPU.** Each `world.draw(model, t, animator)` moves the vertices by the pose (standard linear blend skinning: each vertex moved by the weighted sum of its joints' matrices) into a buffer that's streamed to the GPU once a frame. They then draw through the same lit and shadow shaders as everything else, so animated characters get lights, shadows and fog with no new shader code to go wrong on Metal or D3D11. The cost is CPU time per vertex: 24 robots is about 7,000 vertices a frame, and Fox is 1,728. Tens of characters is comfortable; hundreds of detailed ones would want GPU skinning, which isn't there yet. Normals are moved by the same matrices, which is right for the rotations and uniform scales skeletons use.

## What's actually been verified

- **`animation_smoketest`** (ctest) writes its own skinned glTF (a two-bone arm under a rotated armature, with a mesh node transform that must be ignored, a bend clip with a stepped channel, and a one-key clip) and checks it against hand-worked positions:
  - the load itself (joints, parents, root offset, byte-sized joint indices, clip durations)
  - the rest pose ignoring the mesh node's transform
  - 90° and 45° bends, including the stepped channel's jump at 0.5 s
  - non-looping clips stopping at the end and looping ones wrapping
  - a crossfade at its midpoint, and `stop()`
- **`animation_demo`** was run under Xvfb + llvmpipe with the Khronos sample models:
  - `Fox.glb` (24 joints, textured) switched to "Run", and two frames 0.17 s apart show a real gallop, with legs gathered and then extended and the shadow following.
  - `CesiumMan.glb` (a rotated armature node) walks upright, arms and legs swinging.
  - The code-built robot crowd walks at about 50 fps on that software renderer.

  The sample models aren't in the repo; pass any animated `.glb` on the command line.
- Windows (Windows 11 with an NVIDIA RTX 4070 Super (D3D11)): the owner ran `animation_demo` and reported it working. Metal: not seen yet. Skinned models use the same shaders as everything else, which is the point of skinning on the CPU.
