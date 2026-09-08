# Input and audio

## Input

```cpp
if (f.key_pressed(Key::Space)) jump();     // true ONLY on the frame it went down
if (f.key_down(Key::Left)) x -= speed * f.dt;   // true every frame it's held

vec2 m = f.mouse();
if (f.mouse_pressed(Mouse::Left)) startDrag(m);
```

`*_pressed` is edge-triggered (this frame only), `*_down` is level-triggered (held). Mixing these up is the single most common input bug in every game ever written in any engine — if your action fires every frame instead of once, you used `_down` where you meant `_pressed`.

`Key` values match GLFW/sokol keycodes on purpose, so there's no translation table hiding a bug — what you pass is what the platform reports. It's not a complete keyboard (no function keys, no numpad-specific codes, no punctuation beyond what's listed) because nothing in the games built on this needed them. Add to the enum in `thistle.hpp` and wire the value through if you need more; don't work around a missing key with something hacky, just add it, it's a one-line enum entry.

### Touch

```cpp
if (f.touching()) drag(f.touch_pos());
```

A tap is *also* delivered as a left-mouse click — `mouse_pressed(Mouse::Left)` fires for a finger tap too. `touching()`/`touch_pos()` exist purely for readability when you're writing code you know only runs on a touch device; they're not a separate input path under the hood. Don't write parallel mouse-handling and touch-handling code paths that both try to catch the same tap — you'll double-fire.

### Gamepad

```cpp
if (f.pad_connected()) {
    if (f.pad_pressed(Pad::A)) jump();
    vec2 stick = f.pad_left_stick();     // -1..1 each axis
    const char* label = f.pad_label(Pad::A);   // "A" / "Cross" / "B" depending on brand
}
```

`Pad` names buttons by *physical position*, Xbox-style (`A` is always the bottom face button, on every controller, regardless of brand) — this is deliberate so your gameplay code never has an `if (brand == PlayStation)` branch. If you want to *display* the brand-correct glyph/name (Cross instead of A on a PlayStation pad), that's what `pad_label()` and `pad_kind()` are for — a display concern, not a gameplay concern. Keep that separation; don't let brand-specific logic leak into your input handling just because you wanted the right icon in a tooltip.

Only the first connected controller is polled. No multi-controller support. If you're building local co-op, that's an engine change (`thistle_gamepad.h`/`.mm`, and the Frame API), not a workaround in game code — don't fake it.

Backed by GameController.framework (iOS/macOS), XInput (Windows), and the kernel joystick API (Linux, `/dev/input/js0`) — Android and web still get a no-op. The Linux backend assumes a fairly standard Xbox-shaped button/axis layout (matching the kernel's `xpad` driver, by far the most common case); a controller using a different driver may report the wrong button in the wrong slot. This hasn't been verified against real hardware on Windows or Linux yet — it's checked in by code review and a clean compile/link against the real XInput/joystick APIs, not by an actual button press. Try it on real hardware before trusting it for something that matters.

## Audio

```cpp
play_sound("assets/jump.wav");         // fire-and-forget, non-blocking
play_music("assets/theme.wav", 0.6f);  // one looping track at a time
stop_music();

set_music_volume(0.6f);
set_sfx_volume(1.0f);
set_master_volume(1.0f);
```

`play_sound` is exactly what it sounds like — call it, forget it, it plays and cleans itself up. There's no handle, no "is it still playing," no way to stop one specific sound effect once fired. If you need that (a looping engine hum you can start/stop, positional audio, ducking), you're past what this API gives you and need to go straight to miniaudio yourself — it's already a dependency, nothing stops you from using its full API alongside this one.

`play_music` replaces whatever's currently playing. There is exactly one music slot. Two overlapping tracks is not a supported concept — if you want a crossfade, that's your own two-`ma_sound` setup against miniaudio directly, not something `play_music` will ever grow, because "one background track" is a deliberate simplification, not an oversight waiting to be fixed.

Volumes are independent multipliers, 0..1, and `master` scales both — set them once from a settings screen and persist the values yourself with `save::set_float` (see [platform-and-networking.md](platform-and-networking.md)). The engine doesn't persist audio settings for you; "music: on" is a checkbox in *your* game's save data, not the engine's.
