#pragma once
// Internal transport struct for gamepad state, shared between thistle.cpp and
// the Apple backend in ios_support.mm. Plain C so both agree on the layout.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThistleGamepad {
    int connected;
    int kind;          // 0=none 1=Xbox 2=PlayStation 3=Nintendo 4=Generic
    char name[64];     // vendor name, e.g. "Xbox Wireless Controller"
    int a, b, x, y;
    int up, down, left, right;
    int l1, r1, start, back;
    float lx, ly, rx, ry, lt, rt;
} ThistleGamepad;

// Fills *g from the first connected controller (zeros it if none). Each
// platform has its own real backend now — Apple via GameController.framework
// (ios_support.mm), Windows via XInput (win32_support.cpp), Linux via the
// kernel joystick API (linux_gamepad.cpp) — Android and web still get a no-op
// zero-fill (see thistle.cpp's dispatch in the frame-poll code).
void thistle_apple_poll_gamepad(ThistleGamepad* g);
void thistle_win32_poll_gamepad(ThistleGamepad* g);
void thistle_linux_poll_gamepad(ThistleGamepad* g);

#ifdef __cplusplus
}
#endif
