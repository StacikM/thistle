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

// Fills *g from the first connected controller (zeros it if none). Implemented
// on Apple; a no-op stub elsewhere.
void thistle_apple_poll_gamepad(ThistleGamepad* g);

#ifdef __cplusplus
}
#endif
