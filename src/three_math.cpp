#include <thistle.hpp>

#include <cmath>

namespace thistle {

float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(vec3 v) { return std::sqrt(dot(v, v)); }
float distance(vec3 a, vec3 b) { return length(a - b); }
vec3 normalize(vec3 v) {
    const float len = length(v);
    return len > 1e-12f ? v / len : vec3{0.0f, 0.0f, 0.0f};
}

} // namespace thistle

namespace thistle::three {

namespace {

// Rotation-matrix-to-quaternion (Shepperd's method): picks the numerically
// largest of w/x/y/z to divide by, so it never divides by ~0.
quat quat_from_basis(vec3 x, vec3 y, vec3 z) {
    const float trace = x.x + y.y + z.z;
    quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y.z - z.y) / s;
        q.y = (z.x - x.z) / s;
        q.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q.w = (y.z - z.y) / s;
        q.x = 0.25f * s;
        q.y = (y.x + x.y) / s;
        q.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q.w = (z.x - x.z) / s;
        q.x = (y.x + x.y) / s;
        q.y = 0.25f * s;
        q.z = (z.y + y.z) / s;
    } else {
        const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q.w = (x.y - y.x) / s;
        q.x = (z.x + x.z) / s;
        q.y = (z.y + y.z) / s;
        q.z = 0.25f * s;
    }
    return normalize(q);
}

vec3 any_perpendicular(vec3 v) {
    const vec3 other = std::fabs(v.x) < 0.9f ? vec3{1.0f, 0.0f, 0.0f} : vec3{0.0f, 1.0f, 0.0f};
    return normalize(cross(v, other));
}

} // namespace

quat quat::axis_angle(vec3 axis, float radians) {
    const vec3 a = thistle::normalize(axis);
    const float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}

quat quat::euler(float pitch, float yaw, float roll) {
    return axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
           axis_angle({1.0f, 0.0f, 0.0f}, pitch) *
           axis_angle({0.0f, 0.0f, 1.0f}, roll);
}

quat quat::look_rotation(vec3 forward, vec3 up) {
    const vec3 f = thistle::normalize(forward);
    if (f == vec3{0.0f, 0.0f, 0.0f}) return {};
    const vec3 z = -f;
    vec3 x = thistle::normalize(cross(up, z));
    if (x == vec3{0.0f, 0.0f, 0.0f}) x = any_perpendicular(z); // looking straight along `up`
    const vec3 y = cross(z, x);
    return quat_from_basis(x, y, z);
}

quat quat::from_to(vec3 from, vec3 to) {
    const vec3 a = thistle::normalize(from);
    const vec3 b = thistle::normalize(to);
    const float d = dot(a, b);
    if (d < -0.999999f) return axis_angle(any_perpendicular(a), pi); // opposite: any 180° turn works
    const vec3 c = cross(a, b);
    return normalize(quat{c.x, c.y, c.z, 1.0f + d});
}

vec3 quat::forward() const { return *this * vec3{0.0f, 0.0f, -1.0f}; }
vec3 quat::right() const { return *this * vec3{1.0f, 0.0f, 0.0f}; }
vec3 quat::up() const { return *this * vec3{0.0f, 1.0f, 0.0f}; }

quat operator*(quat a, quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

vec3 operator*(quat q, vec3 v) {
    const vec3 u{q.x, q.y, q.z};
    const vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

float dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

quat normalize(quat q) {
    const float len = std::sqrt(dot(q, q));
    if (len < 1e-12f) return {};
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

quat slerp(quat a, quat b, float t) {
    float d = dot(a, b);
    if (d < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) {
        return normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                          a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    const float theta = std::acos(d);
    const float sa = std::sin((1.0f - t) * theta) / std::sin(theta);
    const float sb = std::sin(t * theta) / std::sin(theta);
    return {a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb, a.w * sa + b.w * sb};
}

mat4 mat4::translate(vec3 t) {
    mat4 r;
    r(0, 3) = t.x; r(1, 3) = t.y; r(2, 3) = t.z;
    return r;
}

mat4 mat4::scale(vec3 s) {
    mat4 r;
    r(0, 0) = s.x; r(1, 1) = s.y; r(2, 2) = s.z;
    return r;
}

mat4 mat4::rotate(quat q) {
    return trs({0.0f, 0.0f, 0.0f}, q, {1.0f, 1.0f, 1.0f});
}

mat4 mat4::trs(vec3 t, quat q, vec3 s) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    mat4 r;
    r(0, 0) = (1.0f - 2.0f * (yy + zz)) * s.x;
    r(1, 0) = (2.0f * (xy + wz)) * s.x;
    r(2, 0) = (2.0f * (xz - wy)) * s.x;
    r(0, 1) = (2.0f * (xy - wz)) * s.y;
    r(1, 1) = (1.0f - 2.0f * (xx + zz)) * s.y;
    r(2, 1) = (2.0f * (yz + wx)) * s.y;
    r(0, 2) = (2.0f * (xz + wy)) * s.z;
    r(1, 2) = (2.0f * (yz - wx)) * s.z;
    r(2, 2) = (1.0f - 2.0f * (xx + yy)) * s.z;
    r(0, 3) = t.x; r(1, 3) = t.y; r(2, 3) = t.z;
    return r;
}

mat4 mat4::perspective(float fovy, float aspect, float n, float f) {
    const float t = 1.0f / std::tan(fovy * 0.5f);
    mat4 r;
    r(0, 0) = t / aspect;
    r(1, 1) = t;
    r(2, 2) = (f + n) / (n - f);
    r(2, 3) = 2.0f * f * n / (n - f);
    r(3, 2) = -1.0f;
    r(3, 3) = 0.0f;
    return r;
}

mat4 mat4::ortho(float l, float rt, float b, float t, float n, float f) {
    mat4 r;
    r(0, 0) = 2.0f / (rt - l);
    r(1, 1) = 2.0f / (t - b);
    r(2, 2) = -2.0f / (f - n);
    r(0, 3) = -(rt + l) / (rt - l);
    r(1, 3) = -(t + b) / (t - b);
    r(2, 3) = -(f + n) / (f - n);
    return r;
}

mat4 mat4::look_at(vec3 eye, vec3 target, vec3 up) {
    const vec3 f = thistle::normalize(target - eye);
    vec3 s = thistle::normalize(cross(f, up));
    if (s == vec3{0.0f, 0.0f, 0.0f}) s = any_perpendicular(f);
    const vec3 u = cross(s, f);
    mat4 r;
    r(0, 0) = s.x;  r(0, 1) = s.y;  r(0, 2) = s.z;  r(0, 3) = -dot(s, eye);
    r(1, 0) = u.x;  r(1, 1) = u.y;  r(1, 2) = u.z;  r(1, 3) = -dot(u, eye);
    r(2, 0) = -f.x; r(2, 1) = -f.y; r(2, 2) = -f.z; r(2, 3) = dot(f, eye);
    return r;
}

vec3 mat4::transform_point(vec3 p) const {
    const vec4 v = *this * vec4{p.x, p.y, p.z, 1.0f};
    if (std::fabs(v.w) < 1e-12f) return {v.x, v.y, v.z};
    return {v.x / v.w, v.y / v.w, v.z / v.w};
}

vec3 mat4::transform_direction(vec3 d) const {
    const vec4 v = *this * vec4{d.x, d.y, d.z, 0.0f};
    return {v.x, v.y, v.z};
}

mat4 operator*(const mat4& a, const mat4& b) {
    mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r(row, col) = a(row, 0) * b(0, col) + a(row, 1) * b(1, col) +
                          a(row, 2) * b(2, col) + a(row, 3) * b(3, col);
        }
    }
    return r;
}

vec4 operator*(const mat4& a, vec4 v) {
    return {
        a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z + a(0, 3) * v.w,
        a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z + a(1, 3) * v.w,
        a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z + a(2, 3) * v.w,
        a(3, 0) * v.x + a(3, 1) * v.y + a(3, 2) * v.z + a(3, 3) * v.w,
    };
}

mat4 transpose(const mat4& a) {
    mat4 r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) r(row, col) = a(col, row);
    return r;
}

mat4 inverse(const mat4& a) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return {};
    mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
    return r;
}

Transform operator*(const Transform& parent, const Transform& child) {
    Transform r;
    r.position = parent.apply(child.position);
    r.rotation = normalize(parent.rotation * child.rotation);
    r.scale = parent.scale * child.scale;
    return r;
}

} // namespace thistle::three
