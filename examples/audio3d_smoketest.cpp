// Checks that 3D sound is safe without an audio device: before App::run()
// (and on machines without one) there's no audio engine, so every call
// must quietly do nothing rather than crash. What it sounds like can't be
// checked here; see docs/input-and-audio.md for how that was verified
// (recorded output: panning and distance falloff measured).
#include <thistle.hpp>
#include <cstdio>
#include <string>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
} // namespace

int main() {
    App app{{.title = "audio3d smoketest", .width = 64, .height = 64}};
    const Sound s = play_sound_at("does-not-matter.wav", {1, 2, 3}, {.loop = true});
    check(!s && !sound_playing(s) && playing_sound_count() == 0, "no audio engine yet: nothing plays, handle invalid");
    set_sound_position(s, {0, 0, 0});
    set_sound_volume(s, 0.5f);
    set_sound_pitch(s, 2.0f);
    stop_sound(s);
    stop_sound(Sound{12345});
    stop_all_sounds();
    preload_sound("does-not-matter.wav");
    Camera cam;
    set_listener(cam);
    set_listener_automatic();
    check(true, "every call on an invalid or made-up handle is harmless");
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
