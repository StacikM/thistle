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

```cpp
const float scroll = f.mouse_scroll();   // vertical wheel/trackpad delta this frame, 0 most frames
if (scroll != 0.0f) zoom -= scroll * 0.1f;
```

`mouse_scroll()` is the accumulated vertical scroll delta for the current frame only (like `*_pressed`, it resets every frame) — there's no horizontal scroll exposed, and which way is "positive" varies by OS/device the same way it does in every cross-platform scroll API, so treat it as "some scrolling happened," not a guaranteed sign.

### Text input

```cpp
begin_text_input(current_name);            // shows the soft keyboard on mobile, starts capturing
// every frame while active:
std::string typed = text_input();          // live buffer — Backspace already applied
if (f.key_pressed(Key::Enter)) { current_name = typed; end_text_input(); }
```

Real keystroke capture, not a fake — `begin_text_input()` starts accumulating printable ASCII characters (Backspace edits the buffer) into an internal string you read back with `text_input()` every frame, and `end_text_input()` stops it (hides the soft keyboard on mobile too). There's no visual text-box widget anywhere in the engine — no cursor, no selection, no click-to-position — you draw whatever box/highlight you want around the live `text_input()` value yourself with `f.rect()`/`f.text()`, the same "you compute your own positions" philosophy as everything else in `docs/ui-and-scenes.md`. `tools/thistle-editor`'s `text_box` and `number` widgets are a real example: click (or double-click a number) to `begin_text_input()`, draw `text_input() + "|"` as a stand-in cursor while active, Enter or a click elsewhere commits, Escape cancels.

While capturing, your own keyboard shortcuts still fire from the same physical keys — `begin_text_input()` doesn't suppress `key_pressed()`/`key_down()` for you. If a shortcut and typing share a key (WASD movement and someone typing the letter "s" into a name, say), gate your shortcut handling behind whatever "am I currently capturing text" flag your own code is tracking; the engine has no such flag itself, since it doesn't know which of your fields (if any) is "focused."

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

`play_sound` is exactly what it sounds like — call it, forget it, it plays and cleans itself up. There's no handle, no "is it still playing," no way to stop one specific sound effect once fired. If you need that, or sound with a position in a 3D world, see 3D sound below. For anything past both (ducking, effects chains) go straight to miniaudio yourself; it's already a dependency.

`play_music` replaces whatever's currently playing. There is exactly one music slot. Two overlapping tracks is not a supported concept — if you want a crossfade, that's your own two-`ma_sound` setup against miniaudio directly, not something `play_music` will ever grow, because "one background track" is a deliberate simplification, not an oversight waiting to be fixed.

Volumes are independent multipliers, 0..1, and `master` scales both — set them once from a settings screen and persist the values yourself with `save::set_float` (see [platform-and-networking.md](platform-and-networking.md)). The engine doesn't persist audio settings for you; "music: on" is a checkbox in *your* game's save data, not the engine's.

### 3D sound (`thistle::three`)

Sounds with a place in the world: quieter the farther they are from the listener, and panned toward the side they're on.

```cpp
using namespace thistle::three;

play_sound_at("assets/boom.wav", blast_point);                        // a one-shot
Sound engine = play_sound_at("assets/engine.ogg", car_pos, {.loop = true, .min_distance = 3});
set_sound_position(engine, car_pos);                                  // every frame, to follow it
stop_sound(engine);
```

- **It returns a handle**, unlike `play_sound`. You can move a sound, change its volume or pitch, or stop it. Handles go invalid by themselves when the sound finishes, and every call on a stale handle does nothing, so there's nothing to clean up.
- **The listener is the camera** of the last `World::render()`, automatically. `set_listener(position, rotation)` puts the ears somewhere else (a third-person game might want the character's head) and switches to manual; `set_listener_automatic()` switches back.
- **Distance**: full volume inside `min_distance`, then fading as `min / (min + rolloff × (distance − min))` (miniaudio's inverse model), and no quieter past `max_distance`. `rolloff` 1 is roughly real life; lower it for sounds that should carry.
- **Up to 128 at once.** Past that the oldest one-shot is cut off (a looping sound only if nothing else can go). Files are decoded once and shared; `preload_sound()` does the decoding up front so the first play doesn't stall a frame. `stream = true` decodes while playing, for long ambience loops.
- They go through the same effects group as `play_sound`, so `set_sfx_volume` covers them. There's no doppler (positions jump frame to frame, and it warbles) and no occlusion (walls don't muffle anything).

**How it was verified.** CI has no sound card, so `audio3d_smoketest` only checks that everything is safe without an audio device (before `App::run()`, or on a machine without one). The sound itself was checked by recording the real output: PulseAudio with a virtual sink in the build container, the engine playing a 440 Hz tone around a fixed camera, and the recording measured per channel.
- 6 m to the right: left 282, right 1414 (RMS).
- 6 m to the left: exactly mirrored.
- 2 m ahead: 2121 on both.
- 30 m ahead: 141. That's 1/15 of the 2 m level, which is what the formula above gives.
- 6 m behind: 707, 1/3 of the 2 m level, as predicted.

In the same run, finished sounds freed their handles, a looping sound kept playing past its length until stopped, and the 129th and later sounds took over old slots. The destruction demo's generated blast sound was recorded the same way. Metal (macOS/iOS) and Windows audio backends weren't run; it's miniaudio's own code there, the same as for `play_sound`.
