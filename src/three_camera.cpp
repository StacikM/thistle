#include <thistle.hpp>

#include <algorithm>
#include <cmath>

namespace thistle::three {

mat4 Camera::view() const {
    return mat4::rotate(rotation.inverse()) * mat4::translate(-position);
}

mat4 Camera::projection(float aspect) const {
    if (orthographic) {
        const float h = ortho_height * 0.5f;
        return mat4::ortho(-h * aspect, h * aspect, -h, h, near_z, far_z);
    }
    return mat4::perspective(fov, aspect, near_z, far_z);
}

Frustum Camera::frustum(float aspect) const {
    return Frustum::from_matrix(projection(aspect) * view());
}

namespace {
Rect full_frame(const Frame& f) {
    return Rect{{0.0f, 0.0f}, {static_cast<float>(f.width), static_cast<float>(f.height)}};
}
float aspect_of(Rect r) { return r.size.y > 0.0f ? r.size.x / r.size.y : 1.0f; }
} // namespace

Ray Camera::screen_ray(vec2 pixel, const Frame& f) const { return screen_ray(pixel, full_frame(f)); }

Ray Camera::screen_ray(vec2 pixel, Rect viewport) const {
    const float nx = viewport.size.x > 0.0f ? (pixel.x - viewport.pos.x) / viewport.size.x * 2.0f - 1.0f : 0.0f;
    const float ny = viewport.size.y > 0.0f ? 1.0f - (pixel.y - viewport.pos.y) / viewport.size.y * 2.0f : 0.0f;
    const mat4 inv = inverse(projection(aspect_of(viewport)) * view());
    const vec3 near_pt = inv.transform_point({nx, ny, -1.0f});
    const vec3 far_pt = inv.transform_point({nx, ny, 1.0f});
    return Ray{near_pt, normalize(far_pt - near_pt)};
}

bool Camera::world_to_screen(vec3 point, const Frame& f, vec2& out) const {
    return world_to_screen(point, full_frame(f), out);
}

bool Camera::world_to_screen(vec3 point, Rect viewport, vec2& out) const {
    const vec4 clip = (projection(aspect_of(viewport)) * view()) * vec4{point.x, point.y, point.z, 1.0f};
    if (clip.w <= 1e-6f) return false;
    const float nx = clip.x / clip.w, ny = clip.y / clip.w;
    out = {viewport.pos.x + (nx * 0.5f + 0.5f) * viewport.size.x,
           viewport.pos.y + (0.5f - ny * 0.5f) * viewport.size.y};
    return true;
}

namespace {
constexpr float kStickDeadzone = 0.15f;

vec2 deadzone(vec2 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len < kStickDeadzone) return {0.0f, 0.0f};
    const float scaled = std::min(1.0f, (len - kStickDeadzone) / (1.0f - kStickDeadzone));
    return {v.x / len * scaled, v.y / len * scaled};
}

// Click-to-capture / Escape-to-release, shared by every controller that has
// a capture_mouse option. Returns whether mouse movement should turn the view.
bool mouse_turns_view(bool capture_mouse, Mouse drag_button, const Frame& f) {
    if (!capture_mouse) return f.mouse_down(drag_button);
    if (!mouse_locked() && f.mouse_pressed(Mouse::Left)) lock_mouse(true);
    if (mouse_locked() && f.key_pressed(Key::Escape)) lock_mouse(false);
    return mouse_locked();
}
} // namespace

void MouseLook::update(Camera& camera, const Frame& f) {
    if (mouse_turns_view(capture_mouse, Mouse::Right, f)) {
        const vec2 d = f.mouse_delta();
        yaw -= d.x * sensitivity;
        pitch -= d.y * sensitivity * (invert_y ? -1.0f : 1.0f);
    }
    if (f.pad_connected()) {
        const vec2 stick = deadzone(f.pad_right_stick());
        yaw -= stick.x * stick_speed * f.dt;
        pitch += stick.y * stick_speed * f.dt * (invert_y ? -1.0f : 1.0f);
    }
    pitch = std::clamp(pitch, -max_pitch, max_pitch);
    yaw = std::remainder(yaw, 2.0f * pi);
    camera.rotation = quat::euler(pitch, yaw);
}

vec3 MouseLook::forward_flat() const { return {-std::sin(yaw), 0.0f, -std::cos(yaw)}; }

vec3 MouseLook::move_input(const Frame& f) const {
    vec2 in{0.0f, 0.0f}; // x = right, y = forward
    if (f.key_down(Key::W) || f.key_down(Key::Up)) in.y += 1.0f;
    if (f.key_down(Key::S) || f.key_down(Key::Down)) in.y -= 1.0f;
    if (f.key_down(Key::D) || f.key_down(Key::Right)) in.x += 1.0f;
    if (f.key_down(Key::A) || f.key_down(Key::Left)) in.x -= 1.0f;
    if (f.pad_connected()) in += deadzone(f.pad_left_stick());
    const float len = std::sqrt(in.x * in.x + in.y * in.y);
    if (len > 1.0f) in = in * (1.0f / len); // diagonals aren't faster
    const vec3 fwd = forward_flat();
    const vec3 right{-fwd.z, 0.0f, fwd.x};
    return fwd * in.y + right * in.x;
}

void FlyCamera::update(Camera& camera, const Frame& f) {
    look.update(camera, f);
    vec3 move = look.move_input(f);
    // Fly along the full look direction (not just the ground plane), so W
    // goes where you're looking, up or down.
    const vec3 flat_fwd = look.forward_flat();
    const float along = dot(move, flat_fwd);
    move = move - flat_fwd * along + camera.forward() * along;
    if (f.key_down(Key::E) || f.key_down(Key::Space)) move.y += 1.0f;
    if (f.key_down(Key::Q) || f.key_down(Key::LeftControl)) move.y -= 1.0f;
    const float boost_now = (f.key_down(Key::LeftShift) || f.key_down(Key::RightShift)) ? boost : 1.0f;
    camera.position += move * (speed * boost_now * f.dt);
}

void OrbitCamera::apply(Camera& camera) const {
    const vec3 offset{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    camera.position = target + offset * distance;
    camera.look_at(target);
}

void OrbitCamera::update(Camera& camera, const Frame& f) {
    const bool shift = f.key_down(Key::LeftShift) || f.key_down(Key::RightShift);
    const vec2 d = f.mouse_delta();
    const bool panning = allow_pan && !capture_mouse &&
                         (f.mouse_down(Mouse::Middle) || (shift && f.mouse_down(Mouse::Right)));
    if (panning) {
        // Scaled by distance so the point under the cursor roughly stays under it.
        const float k = distance * 0.0015f;
        target += camera.right() * (-d.x * k) + camera.up() * (d.y * k);
    } else if (mouse_turns_view(capture_mouse, Mouse::Right, f)) {
        yaw -= d.x * sensitivity;
        pitch += d.y * sensitivity;
    }
    if (f.pad_connected()) {
        const vec2 stick = deadzone(f.pad_right_stick());
        yaw -= stick.x * 2.5f * f.dt;
        pitch -= stick.y * 2.5f * f.dt;
    }
    const float scroll = f.mouse_scroll();
    if (scroll != 0.0f) distance *= std::pow(1.0f - zoom_speed, scroll);
    distance = std::clamp(distance, min_distance, max_distance);
    pitch = std::clamp(pitch, min_pitch, max_pitch);
    yaw = std::remainder(yaw, 2.0f * pi);
    apply(camera);
}

} // namespace thistle::three
