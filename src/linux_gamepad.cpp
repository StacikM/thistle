// Linux gamepad support via the kernel's legacy joystick API (/dev/input/js0)
// rather than raw evdev. It's a much smaller surface — open one device node,
// read a stream of button/axis delta events, no HID report parsing — at the
// cost of assuming a fairly standard controller layout. The axis/button
// numbering below matches how the kernel's "xpad" driver (Xbox-style
// controllers, by a wide margin the most common gamepad on Linux, and the
// same shape XInput assumes on Windows) reports through this interface.
// A controller using a different driver/layout (most non-Xbox-shaped pads)
// may report buttons or axes in the wrong slots — this is a best-effort
// mapping, not a full SDL-style per-device database.
#include "thistle_gamepad.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/joystick.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
int g_fd = -1;
ThistleGamepad g_state{};

void open_device() {
    g_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
    if (g_fd < 0) return;

    g_state = ThistleGamepad{};
    g_state.connected = 1;
    g_state.kind = 1; // assume Xbox-style layout; see the file comment above

    char name[64] = {};
    if (ioctl(g_fd, JSIOCGNAME(sizeof(name)), name) >= 0 && name[0] != '\0') {
        std::strncpy(g_state.name, name, sizeof(g_state.name) - 1);
        const std::string lower = [&] {
            std::string s(name);
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }();
        if (lower.find("xbox") != std::string::npos) {
            g_state.kind = 1;
        } else if (lower.find("sony") != std::string::npos || lower.find("playstation") != std::string::npos ||
                   lower.find("dualshock") != std::string::npos || lower.find("dualsense") != std::string::npos ||
                   lower.find("wireless controller") != std::string::npos) {
            g_state.kind = 2;
        } else if (lower.find("nintendo") != std::string::npos || lower.find("switch") != std::string::npos ||
                   lower.find("joy-con") != std::string::npos) {
            g_state.kind = 3;
        } else {
            g_state.kind = 4;
        }
    } else {
        std::strncpy(g_state.name, "Linux Gamepad", sizeof(g_state.name) - 1);
    }
}

void close_device() {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_state = ThistleGamepad{};
}
} // namespace

void thistle_linux_poll_gamepad(ThistleGamepad* g) {
    if (g_fd < 0) open_device();
    if (g_fd < 0) {
        std::memset(g, 0, sizeof(*g));
        return;
    }

    js_event e;
    ssize_t n;
    while ((n = read(g_fd, &e, sizeof(e))) == static_cast<ssize_t>(sizeof(e))) {
        const int type = e.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON) {
            const int down = e.value != 0;
            switch (e.number) {
                case 0: g_state.a = down; break;
                case 1: g_state.b = down; break;
                case 2: g_state.x = down; break;
                case 3: g_state.y = down; break;
                case 4: g_state.l1 = down; break;
                case 5: g_state.r1 = down; break;
                case 6: g_state.back = down; break;
                case 7: g_state.start = down; break;
                default: break; // stick-click buttons etc. — no Pad enum slot for these yet
            }
        } else if (type == JS_EVENT_AXIS) {
            const float v = e.value / 32767.0f;
            switch (e.number) {
                case 0: g_state.lx = v; break;
                case 1: g_state.ly = -v; break; // js reports down-positive; every other backend is up-positive
                case 2: g_state.lt = (v + 1.0f) * 0.5f; break;
                case 3: g_state.rx = v; break;
                case 4: g_state.ry = -v; break;
                case 5: g_state.rt = (v + 1.0f) * 0.5f; break;
                case 6: g_state.left = e.value < -16000; g_state.right = e.value > 16000; break;
                case 7: g_state.up = e.value < -16000; g_state.down = e.value > 16000; break;
                default: break;
            }
        }
    }
    // EAGAIN just means "no new events this frame" — the normal case every
    // frame once the queue is drained. Anything else means the device is gone.
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close_device();
        std::memset(g, 0, sizeof(*g));
        return;
    }
    *g = g_state;
}
