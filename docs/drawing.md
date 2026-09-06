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

Full-screen effects (`Grayscale`, `Vignette`, `Chromatic`, `Flash`, `Fade`) applied after your frame is rendered, via an offscreen pass. **Metal only right now** — a no-op everywhere else, silently. If you build a damage-flash effect around this and then wonder why it doesn't show up on your Windows build, this is why: check `platform()` if it matters, or drive the same visual effect a different way (a full-screen `f.rect` with additive blend gets you 80% of `Flash` and works everywhere).

## Minor 3D — read this before you get excited

`camera3d()`, `cube()`, `plane3d()`, `line3d()` exist because sokol_gl (the library the 2D renderer sits on) is secretly a full legacy-OpenGL-style immediate-mode API with its own matrix stack, perspective projection, and depth testing already built in. Exposing it took an afternoon. It is not a 3D renderer. Do not confuse the two.

```cpp
Camera3D cam;
cam.eye = {0, 2, 8};
cam.target = {0, 0, 0};
f.camera3d(cam);

f.plane3d({0, -1, 0}, 20, 20, rgb(0.3f, 0.5f, 0.3f));   // ground
f.cube({0, 0, 0}, {1, 1, 1}, coral);
f.line3d({0, -1, 0}, {0, 2, 0}, white);                  // an axis, a gizmo, whatever

f.camera({0, 0});   // MANDATORY before drawing 2D UI again
```

What you get: a perspective camera (`eye`/`target`/`up`/`fov_deg`), real depth testing against the swapchain's own depth buffer (no offscreen pass needed, sokol_app gives you one by default), and flat-shaded boxes/planes/lines under one fixed key light baked into the vertex color on the CPU. That's it.

What you do **not** get, and what it would actually take to get it:

- **Lighting.** There's no shader stage here at all — sokol_gl's pipeline is fixed-function. A real lighting model (even flat Phong, forget PBR) means writing actual vertex/fragment shaders and a real `sg_pipeline`-based renderer that bypasses sokol_gl entirely for anything lit. That's a from-scratch mini-renderer, one set of shaders per backend (Metal MSL / D3D11 HLSL / GL GLSL, or one GLSL source cross-compiled with sokol-shdc). Weeks, not an afternoon.
- **Meshes.** `cube()` and `plane3d()` are hardcoded generated geometry. There is no model loader. Loading real assets means a glTF parser and a vertex/index buffer pipeline — model loading, not model *drawing*, is the actual work.
- **Textures on 3D geometry, materials, skeletal animation.** None of it exists. Don't go looking.
- **3D physics.** `Physics` below is Box2D. Box2D is 2D. There is zero relationship between the minor-3D drawing calls and any physics simulation — if a cube "falls," you're moving its position yourself, there's no gravity or collision for it.

Use this for: a spinning icon on a menu, a background scene behind 2D gameplay (this is what Fling does — see its `draw_background3d`), a debug visualization, a title-screen flourish. Do not use this as the foundation for an actual 3D game and then be surprised when "add lighting" turns into a multi-week project. You were warned in this file.
