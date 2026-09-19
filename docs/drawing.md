# Drawing

Every draw call is a method on `Frame`, the thing your `app.update` lambda gets called with every frame. Pixel coordinates, origin top-left, Y increases downward — like every 2D framework since the dawn of raster displays, and unlike math class. Get over it.

There is no draw list, no batching you control, no "commit" step. Call `f.rect(...)`, it's drawn this frame, in the order you called it. Later calls draw on top of earlier ones. That's the entire mental model. If you want something behind something else, draw it first.

## Basics

```cpp
f.clear(rgb(0.05f, 0.05f, 0.08f));                 // wipes the frame, do this first
f.rect({100, 100}, {200, 80}, rgb(0.3f, 0.6f, 0.9f));
f.line({0, 0}, {200, 200}, white, 3.0f);
f.triangle({0, 0}, {100, 0}, {50, 100}, coral);
f.circle({200, 200}, 40.0f, white);
f.circle_outline({200, 200}, 40.0f, white, 2.0f);
```

Colors are `rgba{r, g, b, a}`, each 0..1, not 0..255 — this isn't CSS. Use `rgb(r, g, b)` for opaque colors so you don't type `1.0f` for alpha every time. Four colors ship as constants: `white`, `black`, `midnight`, `coral`. Everything else, you write yourself. There is no color palette system. Make one in your own game code if you need it; the engine isn't going to guess your art direction.

## Sprites

```cpp
Texture tex = load_texture("assets/hero.png");   // safe to call before app.run()

f.sprite(tex, {100, 100});                        // native size
f.sprite(tex, {100, 100}, { .size = {64, 64}, .tint = coral, .rotation = 0.3f });
```

`SpriteOpts::src` is a sub-rectangle in pixels for spritesheets. `Anim` builds that rectangle for you from a time value and a frame-count/fps, for the common case of "row-major grid of equal cells":

```cpp
Anim run = Anim(spritesheet, 64, 64).frames(0, 8).fps(12);
f.sprite(spritesheet, pos, { .src = run.frame_at(f.time) });
```

If your sheet isn't a uniform grid, don't fight `Anim` — build the `Rect` yourself and pass it as `.src`. `Anim` is a convenience for the 90% case, not a general spritesheet format.

Textures own GPU memory. `unload_texture()` when a level goes away, or you leak GPU images for the life of the process. `reload_texture()` re-reads from disk — useful for live-editing art during development, useless in a shipped build, don't call it in a hot path.

## Text

```cpp
Font font = load_font("assets/font.ttf");   // first font loaded is the default
f.text("score: 100", {20, 20}, { .size = 32, .color = white, .font = font });
```

`TextOpts::max_width > 0` word-wraps. `align` positions text within that wrap width (or the measured block width if you're not wrapping). `measure_text()` gives you the pixel size before you draw it, for centering — there is no other layout system, you compute your own positions:

```cpp
vec2 m = f.measure_text("GAME OVER", {.size = 64});
f.text("GAME OVER", {W * 0.5f - m.x * 0.5f, 100}, {.size = 64});
```

This is tedious for anything beyond a HUD. It is also completely predictable and debuggable, which a "real" layout engine at this scale would not be. Don't build a flexbox clone on top of this; if you need real UI layout, that's a different, much bigger project.

## Camera

`f.camera(offset)` translates everything drawn after it by `offset`. That's the entire camera system:

```cpp
f.camera(worldCamPos);      // draw world-space stuff
for (auto& platform : level.platforms) f.rect(platform.pos, platform.size, gray);

f.camera({0, 0});           // reset — draw screen-space UI on top, unaffected by scroll
f.text("score: 100", {20, 20});
```

No zoom, no rotation, no viewport rectangle. If you want zoom, scale your world-space sizes and positions yourself before drawing them — there's nothing stopping you, the engine just isn't going to do the math for you. `camera()` also resets the projection matrix and pipeline to 2D/orthographic, which matters if you called `camera3d()` earlier this frame (see below) — it's the one function that guarantees "back to normal 2D drawing," use it after any 3D pass.

For local transforms — rotate/scale/translate a subtree without hand-computing every child's world position — use `push_transform()`/`pop_transform()`, or just use `Node` (see [ui-and-scenes.md](ui-and-scenes.md)) if you have more than a couple of these:

```cpp
f.push_transform(pos, rotation, scale);
f.rect({-16, -16}, {32, 32}, white);   // drawn relative to pos/rotation/scale
f.pop_transform();
```

Push what you pop. There's no leak detection or assertion if you don't — you'll just get a permanently wrong transform for the rest of the frame, and it'll look like a completely unrelated bug three draw calls later. This is the classic immediate-mode-GL footgun and it's still a footgun here.

## Blend modes

```cpp
f.blend(Blend::Additive);   // glow, fire, hit-flash
f.circle(pos, 20, white);
f.blend(Blend::Alpha);      // back to normal — do this, don't assume it resets itself mid-frame
```

Resets to `Alpha` automatically at the start of the *next* frame, not immediately after your draw call. If you're drawing a mix of additive and normal elements in the same frame, set the mode you want before each group. Don't assume "I set it once, it's fine" — it's exactly as bad an assumption as forgetting to reset the color in an old immediate-mode GL app, because that's literally what this is.

## Post-processing

```cpp
set_post_effect(PostEffect::Vignette, 0.6f);
```

Full-screen effects (`Grayscale`, `Vignette`, `Chromatic`, `Flash`, `Fade`) applied after your frame is rendered, via an offscreen pass. Every backend has a real shader now — Metal, D3D11 (Windows), GLCore (Linux), GLES3 (Android/Web) — instead of Metal being the only one that did anything.

Verification differs by backend: Metal is unchanged from the version that's been running in Fling, so that one's genuinely proven. The three new ones (D3D11/GLCore/GLES3) are verified by independently compiling their actual shader source through `glslangValidator`'s real GLSL and HLSL frontends — real syntax/semantic checking, not a guess — plus a clean `sg_make_shader`/`sg_make_pipeline` at runtime with no validation errors. None of the three have been confirmed by an actual rendered vignette on a screen yet; `examples/smoketest.cpp` turns one on (`Vignette`, full strength) specifically so the first person to run it on Windows/Linux/Android sees immediately whether it's right.

## Minor 3D — read this before you get excited

`camera3d()`, `cube()`, `plane3d()`, `line3d()`, `sphere3d()`, `cylinder3d()`, `cone3d()`, `mesh3d()` exist because sokol_gl (the library the 2D renderer sits on) is secretly a full legacy-OpenGL-style immediate-mode API with its own matrix stack, perspective projection, and depth testing already built in. Exposing it took an afternoon; the extra primitives, texture support, and `.obj` mesh loading below took a bit more. It is still not a 3D renderer. Do not confuse the two.

```cpp
Camera3D cam;
cam.eye = {0, 2, 8};
cam.target = {0, 0, 0};
f.camera3d(cam);

f.plane3d({0, -1, 0}, 20, 20, rgb(0.3f, 0.5f, 0.3f));   // ground
f.cube({0, 0, 0}, {1, 1, 1}, coral);
f.sphere3d({-2, 0, 0}, 0.6f, rgb(0.9f, 0.8f, 0.2f));
f.cylinder3d({2, 0, 0}, 0.5f, 1.2f, rgb(0.4f, 0.7f, 0.9f));
f.cone3d({4, 0, 0}, 0.6f, 1.2f, rgb(0.8f, 0.4f, 0.8f));
f.line3d({0, -1, 0}, {0, 2, 0}, white);                  // an axis, a gizmo, whatever

f.camera({0, 0});   // MANDATORY before drawing 2D UI again
```

What you get: a perspective camera (`eye`/`target`/`up`/`fov_deg`), real depth testing against the swapchain's own depth buffer (no offscreen pass needed, sokol_app gives you one by default), and shaded primitives under one fixed key light baked into the vertex color on the CPU. `cube()`/`plane3d()` are flat-shaded per face (one normal per face); `sphere3d()`/`cylinder3d()`/`cone3d()` shade per vertex instead, so they read as smoothly round rather than faceted once `rings`/`segments` are reasonably high. That's it — same one fixed light for all of them, no per-object lights.

```cpp
Texture skin = load_texture("assets/crate.png");
f.cube({0, 0, 0}, {1, 1, 1}, skin);                  // whole texture per face, 6x
f.plane3d({0, -1, 0}, 20, 20, skin);                 // whole texture across the quad, no tiling
f.sphere3d({-2, 0, 0}, 0.6f, skin);                  // equirectangular wrap
f.cylinder3d({2, 0, 0}, 0.5f, 1.2f, skin);           // wraps around the side, caps get a circular UV
f.cone3d({4, 0, 0}, 0.6f, 1.2f, skin, coral);        // optional tint multiplies the sampled color
```

Every primitive has a textured overload taking a `Texture` instead of (or in addition to, via the trailing `tint`) an `rgba`. There's no tiling control and no per-face UV customization — `plane3d()` stretches the whole texture across its full width/depth (scale the mesh or pre-tile the image yourself if you want repetition), and `cube()` puts the whole texture on each of its six faces independently rather than unwrapping one texture across the box. An invalid/unloaded `Texture` silently falls back to the flat-color draw instead of drawing garbage.

### Real meshes: `load_mesh()`/`mesh3d()`

```cpp
Mesh crate = load_mesh("assets/crate.obj");   // safe to call before app.run()

f.mesh3d(crate, {0, 0, 0});                                       // flat white, no rotation/scale
f.mesh3d(crate, {2, 0, 0}, {0, f.time, 0}, {1, 1, 1}, coral);      // spinning on Y, flat-colored
f.mesh3d(crate, {4, 0, 0}, {0, 0, 0}, {1, 1, 1}, skin);            // textured
```

This loads an actual Wavefront `.obj` — real authored geometry, not generated primitives — as a flat triangle list. It's still drawn through the exact same immediate-mode path as `cube()`/`sphere3d()`/etc: no persistent GPU vertex buffer, no `sg_pipeline` of its own, the whole mesh gets re-walked and re-shaded (same one fixed key light, same `shade_face` math) every single frame via sokol_gl's matrix stack for the position/rotation/scale transform. `rotation_rad` is Euler angles in radians, applied X then Y then Z. That per-frame CPU walk is the real cost here — fine for a handful of props, not something to do for a level's worth of static geometry every frame without measuring first.

What `load_mesh()` actually parses: `v`/`vn`/`vt`, `f` faces (fan-triangulated if they're quads/n-gons, since only triangles get drawn). If a vertex's face entry doesn't reference a `vn`, its normal is computed as the flat face normal of that triangle instead — so a `.obj` with real per-vertex normals shades smooth, one with none shades faceted, same as it would in any other engine. Everything else in the file — `o`/`g`/`s`, `usemtl`/`mtllib`, multiple objects — is ignored; every face in the file becomes one flat triangle soup, whatever objects/groups it was organized into in the source file.

If you don't have a `.obj` handy, `make_cube_mesh()`/`make_sphere_mesh()`/`make_cylinder_mesh()`/`make_cone_mesh()`/`make_plane_mesh()` return the same kind of `Mesh` handle, generated instead of loaded — unit-sized, scale them via `mesh3d()`'s own `scale` parameter. The actual reason these exist even though `cube()`/`sphere3d()`/etc. already draw the same shapes: those raw calls have no rotation parameter at all (nowhere to put one — each is a single immediate-mode draw call), while a generated `Mesh` drawn through `mesh3d()` gets full rotation support, correctly-rotated shading included, for free.

What you do **not** get, and what it would actually take to get it:

- **Lighting.** There's no shader stage here at all — sokol_gl's pipeline is fixed-function. A real lighting model (even flat Phong, forget PBR) means writing actual vertex/fragment shaders and a real `sg_pipeline`-based renderer that bypasses sokol_gl entirely for anything lit. That's a from-scratch mini-renderer, one set of shaders per backend (Metal MSL / D3D11 HLSL / GL GLSL, or one GLSL source cross-compiled with sokol-shdc). Weeks, not an afternoon.
- **Materials, skeletal animation, normal maps, tiling UVs.** `load_mesh()` reads geometry and nothing else — no `.mtl`, no rigging, no per-face materials. Loading a `.gltf`/`.glb` instead would get you PBR materials and skinning in the *file format*, but none of it would render any differently here — there's still no shader stage to use a material or a skeleton with, so it wasn't worth the much bigger parser for zero rendering payoff.
- **3D physics.** `Physics` below is Box2D. Box2D is 2D. There is zero relationship between the minor-3D drawing calls (primitives or meshes) and any physics simulation — no forces, no velocities, no rigid bodies, nothing resolves a collision by pushing anything anywhere. What you *do* get is basic 3D collision **detection** — see below — which is a much smaller thing than physics and doesn't change this.

## 3D collision — overlap tests, not physics

```cpp
Box3D box{node.world_mesh_transform().pos, {0.5f, 0.5f, 0.5f}};
Sphere3D player_bounds{player_pos, 0.4f};
if (box3d_sphere3d_overlap(box, player_bounds)) { /* ... */ }
```

`Box3D` (center + half-extent) and `Sphere3D` (center + radius), plus `box3d_overlap()`, `box3d_contains_point()`, `sphere3d_overlap()`, `box3d_sphere3d_overlap()`, and `ray_box3d()` — real geometry math (verified with a standalone test: overlapping/separated/touching cases for every pair, ray-hit distance checked against a hand-computed value), not a shortcut. This is the actual overlap-test layer a `Prim::Trigger` node needs to be useful (see docs/ui-and-scenes.md) — `Node::world_mesh_transform()` gives you a node's real world-space position/rotation/scale (composed through its whole parent chain) to build a `Box3D`/`Sphere3D` from.

What this is **not**: there's no broad-phase (checking N objects against each other is your own O(n²) loop, or your own spatial partitioning if N gets large), no continuous collision detection (a fast-moving object can tunnel through a thin box between frames, same as any discrete check), and `Box3D` is always axis-aligned — it ignores rotation entirely, even though `Node::world_mesh_transform()` gives you one. A rotated-box-vs-box test (oriented bounding boxes, via the separating axis theorem) is real extra math for a case most trigger volumes don't actually need; keep triggers unrotated if you rely on these tests. `examples/smoketest.cpp` exercises `box3d_sphere3d_overlap()` for real every frame (an orbiting sphere against a static box, both changing color on overlap) — not just called once and assumed correct.

Use this for: trigger volumes, "is the player near this thing," simple pickup radii — checks, not simulation. Do not expect an object to stop or bounce off anything; that's what "no forces, no resolution" above means. If you need real 3D physics (rigid bodies that actually push each other apart, friction, restitution), that's a genuinely different, much bigger project — integrating a real 3D physics library (Jolt, Bullet, PhysX), not something to grow out of these overlap tests.

Use minor 3D drawing itself for: a spinning icon on a menu, a background scene behind 2D gameplay (this is what Fling does — see its `draw_background3d`), a debug visualization, a title-screen flourish. Do not use this as the foundation for an actual 3D game and then be surprised when "add lighting" turns into a multi-week project. You were warned in this file.
