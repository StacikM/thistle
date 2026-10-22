#pragma once
// Internal transport struct for gamepad state, shared between thistle.cpp and
// the Apple backend in ios_support.mm. Plain C so both agree on the layout.
#ifdef __cplusplus
extern "C" {
#endif

#define THISTLE_MAX_PADS 4

typedef struct ThistleGamepad {
    int connected;
    int kind;          // 0=none 1=Xbox 2=PlayStation 3=Nintendo 4=Generic
    char name[64];     // vendor name, e.g. "Xbox Wireless Controller"
    int a, b, x, y;
    int up, down, left, right;
    int l1, r1, start, back;
    float lx, ly, rx, ry, lt, rt;
} ThistleGamepad;

// Fills pads[0..THISTLE_MAX_PADS) — one slot per player, zeroed when empty.
// A controller keeps its slot while it stays connected, so player 2 doesn't
// become player 1 when player 1 unplugs. Apple via GameController.framework
// (ios_support.mm), Windows via XInput (win32_support.cpp), Linux via evdev
// (linux_gamepad.cpp); Android and web get a zero-fill (thistle.cpp).
void thistle_apple_poll_gamepads(ThistleGamepad* pads);
void thistle_win32_poll_gamepads(ThistleGamepad* pads);
void thistle_linux_poll_gamepads(ThistleGamepad* pads);

// Both motors at `strength` (0..1; 0 stops) for up to `seconds`. thistle.cpp
// also stops it itself when the time is up, so a backend whose API has no
// duration (XInput) can ignore `seconds`.
void thistle_apple_rumble(int slot, float strength, float seconds);
void thistle_win32_rumble(int slot, float strength, float seconds);
void thistle_linux_rumble(int slot, float strength, float seconds);

#ifdef __cplusplus
}
#endif
