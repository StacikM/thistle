// Linux gamepads through evdev (/dev/input/event*), the interface SDL uses:
// it names buttons and axes by what they are (BTN_SOUTH, ABS_RX, ...) rather
// than by the driver's own numbering the legacy joystick API (/dev/input/js*)
// passes through, reports each axis's range, and is the only one with
// force feedback, which rumble needs. Desktop distros give the logged-in
// user access to gamepad event nodes (udev's uaccess tag), so no extra
// permissions are needed there; without write access a pad still works,
// it just doesn't rumble.
//
// Still a best-effort mapping, not SDL's per-device database: it trusts the
// driver to follow the kernel's gamepad layout (Documentation/input/
// gamepad.rst), which xpad, hid-playstation and hid-nintendo do, with one
// historical exception handled below.
#include "thistle_gamepad.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

struct AxisRange { int min = -32768, max = 32767; };

struct Device {
    int fd = -1;
    std::string path;
    bool xpad_face_buttons = false;
    int rumble_id = -1;
    AxisRange lx, ly, rx, ry, lt, rt;
    ThistleGamepad state{};
};

Device g_slots[THISTLE_MAX_PADS];
struct timespec g_dir_mtime{};
bool g_scanned = false;

bool test_bit(const unsigned long* bits, int bit) {
    constexpr int per = static_cast<int>(sizeof(unsigned long) * 8);
    return (bits[bit / per] >> (bit % per)) & 1UL;
}

AxisRange axis_range(int fd, int code) {
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum) return {};
    return {info.minimum, info.maximum};
}

float stick(int v, AxisRange r) {
    const float t = (static_cast<float>(v) - r.min) / static_cast<float>(r.max - r.min);
    return t * 2.0f - 1.0f;
}

float trigger(int v, AxisRange r) {
    return (static_cast<float>(v) - r.min) / static_cast<float>(r.max - r.min);
}

bool open_slot(Device& d, const std::string& path) {
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;

    unsigned long keys[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
    unsigned long abs[(ABS_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0 ||
        !test_bit(keys, BTN_SOUTH) || !test_bit(abs, ABS_X)) {
        close(fd);
        return false;
    }

    d = Device{};
    d.fd = fd;
    d.path = path;
    d.lx = axis_range(fd, ABS_X);
    d.ly = axis_range(fd, ABS_Y);
    d.rx = axis_range(fd, ABS_RX);
    d.ry = axis_range(fd, ABS_RY);
    d.lt = axis_range(fd, ABS_Z);
    d.rt = axis_range(fd, ABS_RZ);
    d.state.connected = 1;

    char name[64] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || name[0] == '\0')
        std::strncpy(name, "Linux Gamepad", sizeof(name) - 1);
    std::strncpy(d.state.name, name, sizeof(d.state.name) - 1);
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](const char* s) { return lower.find(s) != std::string::npos; };
    if (has("xbox") || has("x-box") || has("xinput")) d.state.kind = 1;
    else if (has("sony") || has("playstation") || has("dualshock") || has("dualsense") || has("wireless controller")) d.state.kind = 2;
    else if (has("nintendo") || has("switch") || has("joy-con")) d.state.kind = 3;
    else d.state.kind = 4;

    // xpad reports Xbox X and Y as BTN_X and BTN_Y, which the kernel defines
    // as BTN_NORTH and BTN_WEST: the two top-row buttons swapped compared to
    // the documented layout every other driver follows.
    input_id id{};
    d.xpad_face_buttons = d.state.kind == 1 ||
                          (ioctl(fd, EVIOCGID, &id) >= 0 && id.vendor == 0x045e); // Microsoft
    return true;
}

void close_slot(Device& d) {
    if (d.fd >= 0) close(d.fd);
    d = Device{};
}

bool already_open(const std::string& path) {
    for (const Device& d : g_slots)
        if (d.fd >= 0 && d.path == path) return true;
    return false;
}

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

// Rescans when /dev/input changes (a node added or removed bumps the
// directory's mtime), not every frame: opening every event device to ask
// what it is costs real syscalls, and keyboards and mice are in there too.
// A new node appears a moment before udev grants the user access to it, so
// after a change it keeps rescanning a few times a second for 3 seconds.
void scan_if_changed() {
    static double changed_at = -100.0, last_scan = -100.0;
    const double now = now_seconds();
    struct stat st{};
    if (stat("/dev/input", &st) != 0) return;
    if (!g_scanned || st.st_mtim.tv_sec != g_dir_mtime.tv_sec || st.st_mtim.tv_nsec != g_dir_mtime.tv_nsec) {
        g_dir_mtime = st.st_mtim;
        g_scanned = true;
        changed_at = now;
    } else if (now - changed_at > 3.0 || now - last_scan < 0.25) {
        return;
    }
    last_scan = now;

    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + e->d_name;
        if (already_open(path)) continue;
        for (Device& slot : g_slots) {
            if (slot.fd >= 0) continue;
            open_slot(slot, path);
            break;
        }
    }
    closedir(dir);
}

void apply(Device& d, const input_event& e) {
    ThistleGamepad& g = d.state;
    if (e.type == EV_KEY) {
        const int down = e.value != 0;
        switch (e.code) {
            case BTN_SOUTH: g.a = down; break;
            case BTN_EAST: g.b = down; break;
            case BTN_NORTH: (d.xpad_face_buttons ? g.x : g.y) = down; break;
            case BTN_WEST: (d.xpad_face_buttons ? g.y : g.x) = down; break;
            case BTN_TL: g.l1 = down; break;
            case BTN_TR: g.r1 = down; break;
            case BTN_SELECT: g.back = down; break;
            case BTN_START: g.start = down; break;
            case BTN_DPAD_UP: g.up = down; break;
            case BTN_DPAD_DOWN: g.down = down; break;
            case BTN_DPAD_LEFT: g.left = down; break;
            case BTN_DPAD_RIGHT: g.right = down; break;
            default: break; // stick clicks, the guide button: no Pad slot for these yet
        }
    } else if (e.type == EV_ABS) {
        switch (e.code) {
            case ABS_X: g.lx = stick(e.value, d.lx); break;
            case ABS_Y: g.ly = -stick(e.value, d.ly); break; // evdev is down-positive; every other backend is up-positive
            case ABS_RX: g.rx = stick(e.value, d.rx); break;
            case ABS_RY: g.ry = -stick(e.value, d.ry); break;
            case ABS_Z: g.lt = trigger(e.value, d.lt); break;
            case ABS_RZ: g.rt = trigger(e.value, d.rt); break;
            case ABS_HAT0X: g.left = e.value < 0; g.right = e.value > 0; break;
            case ABS_HAT0Y: g.up = e.value < 0; g.down = e.value > 0; break;
            default: break;
        }
    }
}

} // namespace

void thistle_linux_poll_gamepads(ThistleGamepad* pads) {
    scan_if_changed();
    for (int i = 0; i < THISTLE_MAX_PADS; ++i) {
        Device& d = g_slots[i];
        if (d.fd >= 0) {
            input_event e;
            ssize_t n;
            while ((n = read(d.fd, &e, sizeof(e))) == static_cast<ssize_t>(sizeof(e))) apply(d, e);
            // EAGAIN just means no new events this frame. ENODEV (or anything
            // else) means it was unplugged.
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) close_slot(d);
        }
        pads[i] = d.fd >= 0 ? d.state : ThistleGamepad{};
    }
}

void thistle_linux_rumble(int slot, float strength, float seconds) {
    if (slot < 0 || slot >= THISTLE_MAX_PADS) return;
    Device& d = g_slots[slot];
    if (d.fd < 0) return;
    const float s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    if (s <= 0.0f) {
        if (d.rumble_id >= 0) {
            input_event stop{};
            stop.type = EV_FF;
            stop.code = static_cast<__u16>(d.rumble_id);
            stop.value = 0;
            (void)!write(d.fd, &stop, sizeof(stop));
        }
        return;
    }
    ff_effect effect{};
    effect.type = FF_RUMBLE;
    effect.id = static_cast<__s16>(d.rumble_id); // -1 asks for a new one; an existing id updates it in place
    effect.u.rumble.strong_magnitude = static_cast<__u16>(s * 65535.0f);
    effect.u.rumble.weak_magnitude = static_cast<__u16>(s * 65535.0f);
    const float ms = seconds * 1000.0f;
    effect.replay.length = static_cast<__u16>(ms < 1.0f ? 1.0f : (ms > 65535.0f ? 65535.0f : ms));
    if (ioctl(d.fd, EVIOCSFF, &effect) < 0) return; // read-only node, or no force feedback
    d.rumble_id = effect.id;
    input_event play{};
    play.type = EV_FF;
    play.code = static_cast<__u16>(d.rumble_id);
    play.value = 1;
    (void)!write(d.fd, &play, sizeof(play));
}
