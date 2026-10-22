# Physics

A thin wrapper over [Box2D](https://box2d.org/) `v2.4.1`, in pixels instead of meters, with +Y down to match the screen instead of up like every physics textbook. Both of those choices exist so you never have to convert units or flip a sign when going from `Physics::position()` to `f.rect()`. That's the whole point of the wrapper — if you find yourself converting units anyway, something's wrong.

**This is 2D.** The "minor 3D" drawing calls in [drawing.md](drawing.md) have zero connection to it: a `cube()` doesn't fall, collide, or exist to Box2D at all. Real 3D physics is a separate, opt-in module on Jolt, `three::Physics3D`; see [physics3d.md](physics3d.md).

## Setup

```cpp
Physics phys({0, 980});   // gravity in px/s^2, +Y down — positive means "falls"

Body ground = phys.add_static_box({640, 700}, {1280, 40});
Body player = phys.add_dynamic_circle({640, 100}, 24, 1.0f, 0.3f, 0.2f);
phys.set_fixed_rotation(player, true);   // stops a circle from spinning like a wheel on every bump
```

`Physics` owns a Box2D world for its whole lifetime and is non-copyable on purpose — copying a physics world is almost never what you meant, so it's deleted rather than left to silently do the wrong thing (a shallow copy sharing Box2D internals, or worse, ODR issues). If you need to reload a level, destroy the `Physics` and construct a new one; don't try to "reset" one in place, that's not a thing Box2D supports cleanly and this wrapper doesn't pretend otherwise.

## Body kinds — pick the right one, not just the one that compiles

- `add_static_box` — never moves. Floors, walls, level geometry. Cheapest kind, use it for anything that doesn't move.
- `add_kinematic_box` — you drive its velocity (`set_velocity`), nothing else can push it, but dynamic bodies resting on it get carried along. This is the moving-platform kind. If you make a moving platform `dynamic` instead, gravity and collisions will fight you the whole way and it will not behave like a platform — this mistake has already been made and fixed once in Fling, don't repeat it.
- `add_dynamic_box` / `add_dynamic_circle` — full simulation: gravity, collision response, the works. Density/friction/restitution are Box2D's usual knobs; if you don't know what they do, `1.0f, 0.3f, 0.0f`-ish defaults are fine starting points, tune by feel, not by reading the Box2D manual cover to cover.
- `add_sensor_box` — detects overlap, applies zero physical force. Use for triggers (a goal zone, a level-end line) with `on_collision`, not for anything you want to actually push against.

## The loop

```cpp
phys.step(f.dt);   // once per frame, that's it — no fixed-timestep accumulator here

vec2 p = phys.position(player);
f.rect({p.x - 24, p.y - 24}, {48, 48}, white);
```

`step()` takes whatever `dt` you give it. Called from `update` with the frame's `dt`, an unstable frame rate makes an unstable simulation, exactly like Box2D always behaves when you feed it a variable `dt`. That usually doesn't matter for a small game on modern hardware. It usually does for a physics puzzle game where the same shot should land in the same place every time. For that, step it from `app.fixed_update` instead, which always gets the same `dt`:

```cpp
app.fixed_update([&](Frame f) { phys.step(f.dt); });   // 60 steps a second, whatever the frame rate
```

See "Fixed timestep" in [ui-and-scenes.md](ui-and-scenes.md) for how that runs and how to draw smoothly between steps.

## Collisions and queries

```cpp
phys.on_collision([&](Body a, Body b) {
    if ((a == player && b == spikes) || (a == spikes && b == player)) die();
});

RayHit hit = phys.raycast(from, to);
if (hit.hit) { /* hit.body, hit.point, hit.normal, hit.fraction */ }
```

`on_collision` fires once per pair when they *start* touching, in unspecified order — don't assume `a` is always "your" body, check both orderings, as the example above does. This is not a full contact-listener API (no separate "still touching" or "stopped touching" events) — if you need those, `touching(a, b)` polls the current state every frame, which covers "still touching" without a dedicated callback.

**Box2D ignores a raycast that starts inside a shape.** This bit Fling for real — a raycast from a player's own center to check "am I grounded" silently missed every time because the ray originated inside the player's own circle. Cast from *outside* the shape you're standing on top of (offset the ray's start point outward first), or cast from the body's edge, not its center. This is a Box2D behavior, not a bug in this wrapper, and it will bite you exactly once until you internalize it.

## Bodies as handles, not pointers

`Body` is `{int id}` — copyable, comparable with `==`, and cheap to store in your own game structs (a `MoverRT` that pairs a `Body` with the two endpoints it patrols between, say). There's no lifetime tracking: destroy the `Physics` world and every `Body` you were holding becomes meaningless, but nothing will tell you that — calling `phys.position()` on a stale handle after the world's gone is undefined, same as any other dangling-handle bug. Don't hold `Body` values past the lifetime of the `Physics` that created them.

## Removing bodies

```cpp
phys.on_collision([&](Body a, Body b) {
    if (a == coin || b == coin) { score++; phys.remove(coin); }
});
```

`remove(body)` takes it out of the world: a bullet that hit, a coin picked up, an enemy that died. The `Body` you were holding goes stale: `position()` returns `{0, 0}`, `touching()` returns false, the setters do nothing, and its id is never given to a new body, so a stale handle can't silently start pointing at something else. Removing an already-removed body does nothing.

It's safe inside `on_collision`. Box2D locks its world during `step()`, so a body removed there is freed when `step()` returns. No more collisions are reported for it after the call, but it's still in the world for the rest of that one step, so it can still push things for one step.

Verified headlessly (`examples/physics2d_smoketest.cpp`, a ctest): removing outside a step, removing a sensor from inside `on_collision`, stale handles, and a raycast passing where a removed body was.
