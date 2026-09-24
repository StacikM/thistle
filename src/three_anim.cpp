// Skeletal animation: sampling clips, crossfading, and turning a pose into
// the per-joint matrices skinning uses. The skinning itself (moving the
// vertices) is in three_render.cpp, next to the buffers it fills.
//
// Poses follow glTF: a joint's world matrix is its parent's times its own
// local TRS (roots start from root_offset, the transform of whatever
// non-joint nodes sit above them), and skinning uses world * inverse_bind.
#include "thistle_internal.h"

#include <algorithm>
#include <cmath>

namespace thistle::three {

int Skeleton::find(const std::string& name) const {
    for (size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

namespace {

// Joints in an order where every parent comes before its children.
std::vector<int> parent_first(const Skeleton& s) {
    std::vector<int> order;
    std::vector<uint8_t> placed(s.joints.size(), 0);
    order.reserve(s.joints.size());
    while (order.size() < s.joints.size()) {
        const size_t before = order.size();
        for (size_t i = 0; i < s.joints.size(); ++i) {
            if (placed[i]) continue;
            const int p = s.joints[i].parent;
            if (p < 0 || p >= static_cast<int>(s.joints.size()) || placed[static_cast<size_t>(p)]) {
                order.push_back(static_cast<int>(i));
                placed[i] = 1;
            }
        }
        if (order.size() == before) { // a cycle: bad data. Treat the rest as roots.
            for (size_t i = 0; i < s.joints.size(); ++i) {
                if (!placed[i]) { order.push_back(static_cast<int>(i)); placed[i] = 1; }
            }
        }
    }
    return order;
}

quat to_quat(vec4 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
    return len > 1e-8f ? quat(v.x / len, v.y / len, v.z / len, v.w / len) : quat{};
}

// A channel's value at `time`: the keyframes around it, blended (or the
// earlier one, for step channels). Past either end: the end value.
vec4 sample_channel(const AnimationClip::Channel& c, float time) {
    const size_t n = std::min(c.times.size(), c.values.size());
    if (n == 0) return {};
    if (n == 1 || time <= c.times[0]) return c.values[0];
    if (time >= c.times[n - 1]) return c.values[n - 1];
    const size_t hi = static_cast<size_t>(std::upper_bound(c.times.begin(), c.times.begin() + static_cast<std::ptrdiff_t>(n), time) - c.times.begin());
    const size_t lo = hi - 1;
    if (c.step) return c.values[lo];
    const float span = c.times[hi] - c.times[lo];
    const float t = span > 0.0f ? (time - c.times[lo]) / span : 0.0f;
    const vec4 a = c.values[lo], b = c.values[hi];
    if (c.path == AnimationClip::Channel::Path::Rotation) {
        const quat q = slerp(to_quat(a), to_quat(b), t);
        return {q.x, q.y, q.z, q.w};
    }
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

float wrap(float time, float duration, bool loop) {
    if (duration <= 0.0f) return 0.0f;
    if (!loop) return std::clamp(time, 0.0f, duration);
    const float t = std::fmod(time, duration);
    return t < 0.0f ? t + duration : t;
}

} // namespace

Animator::Animator(Model model) : model_(model) {
    if (const Skeleton* s = model_skeleton(model)) {
        order_ = parent_first(*s);
        pose_.resize(s->joints.size());
        global_.resize(s->joints.size());
        skin_.resize(s->joints.size());
    }
    evaluate();
}

void Animator::play(const std::string& clip, float fade, bool loop) {
    const int index = find_animation(model_, clip);
    if (index < 0) {
        log_warn("Animator::play: the model has no animation called \"" + clip + "\"");
        return;
    }
    play(index, fade, loop);
}

void Animator::play(int clip, float fade, bool loop) {
    if (clip == clip_ && clip >= 0) {
        loop_ = loop;
        return;
    }
    if (clip >= model_animation_count(model_)) return;
    from_clip_ = clip_;
    from_time_ = time_;
    from_loop_ = loop_;
    clip_ = clip;
    time_ = speed < 0.0f && clip >= 0 ? model_animation(model_, clip)->duration : 0.0f;
    loop_ = loop;
    fade_ = 0.0f;
    fade_length_ = std::max(fade, 0.0f);
    evaluate();
}

void Animator::stop(float fade) { play(-1, fade, true); }

void Animator::update(float dt) {
    const float step = dt * speed;
    if (clip_ >= 0) {
        const float duration = model_animation(model_, clip_)->duration;
        time_ = loop_ ? wrap(time_ + step, duration, true) : std::clamp(time_ + step, 0.0f, duration);
    }
    if (from_clip_ >= 0) {
        const float duration = model_animation(model_, from_clip_)->duration;
        from_time_ = wrap(from_time_ + step, duration, from_loop_);
    }
    fade_ += dt;
    evaluate();
}

void Animator::set_time(float seconds) {
    if (clip_ < 0) return;
    time_ = wrap(seconds, model_animation(model_, clip_)->duration, loop_);
    evaluate();
}

std::string Animator::clip_name() const {
    const AnimationClip* c = clip_ >= 0 ? model_animation(model_, clip_) : nullptr;
    return c ? c->name : std::string();
}

bool Animator::finished() const {
    if (clip_ < 0 || loop_) return false;
    const float duration = model_animation(model_, clip_)->duration;
    return speed >= 0.0f ? time_ >= duration : time_ <= 0.0f;
}

int Animator::find_joint(const std::string& name) const {
    const Skeleton* s = model_skeleton(model_);
    return s ? s->find(name) : -1;
}

mat4 Animator::joint_matrix(int joint) const {
    return joint >= 0 && joint < static_cast<int>(global_.size()) ? global_[static_cast<size_t>(joint)] : mat4{};
}

Transform Animator::joint_transform(int joint) const {
    const mat4 m = joint_matrix(joint);
    Transform t;
    detail::decompose(m, t.position, t.rotation, t.scale);
    return t;
}

void Animator::sample(int clip, float time, std::vector<Local>& out) const {
    const Skeleton* s = model_skeleton(model_);
    out.resize(s ? s->joints.size() : 0);
    for (size_t i = 0; i < out.size(); ++i) {
        const Transform& rest = s->joints[i].rest;
        out[i] = Local{rest.position, rest.rotation, rest.scale};
    }
    const AnimationClip* c = clip >= 0 ? model_animation(model_, clip) : nullptr;
    if (!c) return;
    for (const AnimationClip::Channel& ch : c->channels) {
        if (ch.joint < 0 || ch.joint >= static_cast<int>(out.size())) continue;
        const vec4 v = sample_channel(ch, time);
        Local& l = out[static_cast<size_t>(ch.joint)];
        switch (ch.path) {
            case AnimationClip::Channel::Path::Translation: l.t = {v.x, v.y, v.z}; break;
            case AnimationClip::Channel::Path::Rotation: l.r = to_quat(v); break;
            case AnimationClip::Channel::Path::Scale: l.s = {v.x, v.y, v.z}; break;
        }
    }
}

void Animator::evaluate() {
    const Skeleton* s = model_skeleton(model_);
    if (!s || s->joints.empty()) return;
    sample(clip_, time_, pose_);
    if (fade_ < fade_length_) { // crossfading: blend the old clip's pose into the new one's
        sample(from_clip_, from_time_, from_pose_);
        const float w = std::clamp(fade_ / fade_length_, 0.0f, 1.0f);
        for (size_t i = 0; i < pose_.size(); ++i) {
            pose_[i].t = lerp(from_pose_[i].t, pose_[i].t, w);
            pose_[i].r = slerp(from_pose_[i].r, pose_[i].r, w);
            pose_[i].s = lerp(from_pose_[i].s, pose_[i].s, w);
        }
    }
    for (int j : order_) {
        const Skeleton::Joint& joint = s->joints[static_cast<size_t>(j)];
        const Local& l = pose_[static_cast<size_t>(j)];
        const mat4 local = mat4::trs(l.t, l.r, l.s);
        global_[static_cast<size_t>(j)] = (joint.parent >= 0 ? global_[static_cast<size_t>(joint.parent)] : joint.root_offset) * local;
        skin_[static_cast<size_t>(j)] = global_[static_cast<size_t>(j)] * joint.inverse_bind;
    }
}

} // namespace thistle::three

namespace thistle::detail {

void decompose(const three::mat4& m, vec3& translation, three::quat& rotation, vec3& scale) {
    translation = {m(0, 3), m(1, 3), m(2, 3)};
    vec3 cols[3];
    for (int c = 0; c < 3; ++c) cols[c] = {m(0, c), m(1, c), m(2, c)};
    scale = {length(cols[0]), length(cols[1]), length(cols[2])};
    if (dot(cross(cols[0], cols[1]), cols[2]) < 0.0f) scale.x = -scale.x; // mirrored
    for (int c = 0; c < 3; ++c) {
        const float s = c == 0 ? scale.x : c == 1 ? scale.y : scale.z;
        if (std::fabs(s) > 1e-8f) cols[c] = cols[c] / s;
    }
    // Rotation matrix (columns = rotated axes) to quaternion.
    const float r00 = cols[0].x, r11 = cols[1].y, r22 = cols[2].z;
    const float trace = r00 + r11 + r22;
    float x, y, z, w;
    if (trace > 0.0f) {
        const float k = 0.5f / std::sqrt(trace + 1.0f);
        w = 0.25f / k;
        x = (cols[1].z - cols[2].y) * k;
        y = (cols[2].x - cols[0].z) * k;
        z = (cols[0].y - cols[1].x) * k;
    } else if (r00 > r11 && r00 > r22) {
        const float k = 2.0f * std::sqrt(1.0f + r00 - r11 - r22);
        w = (cols[1].z - cols[2].y) / k;
        x = 0.25f * k;
        y = (cols[1].x + cols[0].y) / k;
        z = (cols[2].x + cols[0].z) / k;
    } else if (r11 > r22) {
        const float k = 2.0f * std::sqrt(1.0f + r11 - r00 - r22);
        w = (cols[2].x - cols[0].z) / k;
        x = (cols[1].x + cols[0].y) / k;
        y = 0.25f * k;
        z = (cols[2].y + cols[1].z) / k;
    } else {
        const float k = 2.0f * std::sqrt(1.0f + r22 - r00 - r11);
        w = (cols[0].y - cols[1].x) / k;
        x = (cols[2].x + cols[0].z) / k;
        y = (cols[2].y + cols[1].z) / k;
        z = 0.25f * k;
    }
    rotation = three::quat(x, y, z, w);
    rotation = three::normalize(rotation);
}

void rest_skin_matrices(const three::Skeleton& s, std::vector<three::mat4>& out) {
    const std::vector<int> order = three::parent_first(s);
    std::vector<three::mat4> global(s.joints.size());
    out.assign(s.joints.size(), three::mat4{});
    for (int j : order) {
        const three::Skeleton::Joint& joint = s.joints[static_cast<size_t>(j)];
        const three::mat4 local = three::mat4::trs(joint.rest.position, joint.rest.rotation, joint.rest.scale);
        global[static_cast<size_t>(j)] = (joint.parent >= 0 ? global[static_cast<size_t>(joint.parent)] : joint.root_offset) * local;
        out[static_cast<size_t>(j)] = global[static_cast<size_t>(j)] * joint.inverse_bind;
    }
}

} // namespace thistle::detail
