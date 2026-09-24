// three::play_sound_at() and friends: positional sound on miniaudio's
// engine (the same one play_sound()/play_music() use), through the
// sound-effects group so set_sfx_volume() covers these too.
//
// miniaudio's spatializer already uses this engine's conventions
// (right-handed, +Y up, a listener with no rotation facing -Z), so
// positions and the camera's orientation go in unconverted.
#include "thistle_internal.h"

#include "miniaudio.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace thistle::three {

namespace {

constexpr int kMaxSounds = 128;

struct Slot {
    ma_sound sound;            // must not move once initialized: slots live on the heap
    uint32_t generation = 1;   // bumped whenever the slot is freed, so old handles go stale
    bool used = false;
    bool looping = false;
    uint64_t started = 0;      // for picking the oldest one to cut off
};

struct AudioState {
    std::vector<std::unique_ptr<Slot>> slots;
    std::unordered_map<std::string, std::unique_ptr<ma_sound>> preloaded; // held so the decoded data stays cached
    std::unordered_set<std::string> failed;                             // warn once per bad file
    bool manual_listener = false;
    uint64_t counter = 0;
};

AudioState& state() {
    static AudioState s;
    return s;
}

uint32_t make_id(size_t index, uint32_t generation) { return (generation << 8) | static_cast<uint32_t>(index + 1); }

Slot* lookup(Sound sound) {
    if (!sound) return nullptr;
    AudioState& a = state();
    const size_t index = (sound.id & 0xFF) - 1;
    if (index >= a.slots.size()) return nullptr;
    Slot* s = a.slots[index].get();
    return s->used && s->generation == (sound.id >> 8) ? s : nullptr;
}

void release(Slot& s) {
    ma_sound_uninit(&s.sound);
    s.used = false;
    s.generation = (s.generation + 1) & 0xFFFFFF;
    if (s.generation == 0) s.generation = 1;
}

void apply_listener(ma_engine* engine, vec3 position, quat rotation) {
    const vec3 forward = rotation.forward(), up = rotation.up();
    ma_engine_listener_set_position(engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(engine, 0, up.x, up.y, up.z);
}

} // namespace

Sound play_sound_at(const std::string& path, vec3 position, const SoundSettings& settings) {
    ma_engine* engine = detail::audio_engine();
    if (!engine) return {};
    AudioState& a = state();
    // A free slot, a new one, or cut off the oldest (preferring one-shots:
    // a stopped explosion is less noticeable than an engine going silent).
    size_t index = a.slots.size();
    for (size_t i = 0; i < a.slots.size(); ++i) {
        if (!a.slots[i]->used) { index = i; break; }
    }
    if (index == a.slots.size()) {
        if (static_cast<int>(a.slots.size()) < kMaxSounds) {
            a.slots.push_back(std::make_unique<Slot>());
        } else {
            size_t oldest = 0;
            for (size_t i = 1; i < a.slots.size(); ++i) {
                const Slot& c = *a.slots[i];
                const Slot& o = *a.slots[oldest];
                if (c.looping != o.looping ? !c.looping : c.started < o.started) oldest = i;
            }
            index = oldest;
            release(*a.slots[index]);
        }
    }
    Slot& slot = *a.slots[index];
    const ma_uint32 flags = settings.stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    if (ma_sound_init_from_file(engine, path.c_str(), flags, detail::sfx_group(), nullptr, &slot.sound) != MA_SUCCESS) {
        if (a.failed.insert(path).second) log_warn("play_sound_at: can't play " + path);
        return {};
    }
    ma_sound* s = &slot.sound;
    ma_sound_set_spatialization_enabled(s, MA_TRUE);
    ma_sound_set_attenuation_model(s, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(s, std::max(settings.min_distance, 0.01f));
    ma_sound_set_max_distance(s, std::max(settings.max_distance, settings.min_distance));
    ma_sound_set_rolloff(s, std::max(settings.rolloff, 0.0f));
    ma_sound_set_doppler_factor(s, 0.0f); // positions jump between frames; doppler would warble
    ma_sound_set_position(s, position.x, position.y, position.z);
    ma_sound_set_volume(s, std::max(settings.volume, 0.0f));
    ma_sound_set_pitch(s, std::max(settings.pitch, 0.01f));
    ma_sound_set_looping(s, settings.loop ? MA_TRUE : MA_FALSE);
    if (ma_sound_start(s) != MA_SUCCESS) {
        ma_sound_uninit(s);
        return {};
    }
    slot.used = true;
    slot.looping = settings.loop;
    slot.started = ++a.counter;
    return Sound{make_id(index, slot.generation)};
}

void set_sound_position(Sound sound, vec3 p) {
    if (Slot* s = lookup(sound)) ma_sound_set_position(&s->sound, p.x, p.y, p.z);
}

void set_sound_volume(Sound sound, float volume) {
    if (Slot* s = lookup(sound)) ma_sound_set_volume(&s->sound, std::max(volume, 0.0f));
}

void set_sound_pitch(Sound sound, float pitch) {
    if (Slot* s = lookup(sound)) ma_sound_set_pitch(&s->sound, std::max(pitch, 0.01f));
}

void stop_sound(Sound sound) {
    if (Slot* s = lookup(sound)) release(*s);
}

bool sound_playing(Sound sound) {
    Slot* s = lookup(sound);
    return s && !ma_sound_at_end(&s->sound);
}

void stop_all_sounds() {
    for (auto& s : state().slots) {
        if (s->used) release(*s);
    }
}

int playing_sound_count() {
    int n = 0;
    for (const auto& s : state().slots) n += s->used;
    return n;
}

void preload_sound(const std::string& path) {
    ma_engine* engine = detail::audio_engine();
    AudioState& a = state();
    if (!engine || a.preloaded.count(path)) return;
    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound.get()) != MA_SUCCESS) {
        if (a.failed.insert(path).second) log_warn("preload_sound: can't load " + path);
        return;
    }
    a.preloaded.emplace(path, std::move(sound));
}

void set_listener(vec3 position, quat rotation) {
    state().manual_listener = true;
    if (ma_engine* engine = detail::audio_engine()) apply_listener(engine, position, rotation);
}

void set_listener(const Camera& camera) { set_listener(camera.position, camera.rotation); }

void set_listener_automatic() { state().manual_listener = false; }

} // namespace thistle::three

namespace thistle::detail {

void three_audio_update() {
    for (auto& s : three::state().slots) {
        if (s->used && !s->looping && ma_sound_at_end(&s->sound)) three::release(*s);
    }
}

void three_audio_shutdown() {
    three::AudioState& a = three::state();
    for (auto& s : a.slots) {
        if (s->used) three::release(*s);
    }
    for (auto& [path, sound] : a.preloaded) ma_sound_uninit(sound.get());
    a.preloaded.clear();
}

void three_audio_camera(vec3 position, three::quat rotation) {
    if (three::state().manual_listener) return;
    if (ma_engine* engine = audio_engine()) three::apply_listener(engine, position, rotation);
}

} // namespace thistle::detail
