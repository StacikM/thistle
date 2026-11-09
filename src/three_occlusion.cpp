// OcclusionBuffer: a small software depth buffer on the CPU. Part of the
// core both libraries build, so it needs no GPU and the tests can use it.
#include "thistle_core.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace thistle::three {

struct OcclusionImpl {
    int width, height;
    std::vector<float> depth; // (width+1) x (height+1) pixel corners: NDC z (-1 near .. 1 far) of the nearest occluder there; none = infinity
    mat4 view_proj;
    int triangles = 0;
};

namespace {

constexpr float no_occluder = std::numeric_limits<float>::infinity();
constexpr float near_epsilon = 1e-4f;

struct ClipVertex {
    float x, y, z, w;
};

ClipVertex lerp_clip(const ClipVertex& a, const ClipVertex& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

struct ScreenVertex {
    float x, y, z;
};

// Records the triangle's depth at every pixel corner (grid point) it covers.
// Corners, not pixels: two triangles that share an edge both claim the
// corners along it, so a quad doesn't leave a seam down its diagonal. A pixel
// then counts as hidden-behind only if all four of its corners are covered.
void raster_triangle(OcclusionImpl& o, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    const float inv_area = 1.0f / area;
    const float dzdx = ((b.z - a.z) * (c.y - a.y) - (c.z - a.z) * (b.y - a.y)) * inv_area;
    const float dzdy = ((c.z - a.z) * (b.x - a.x) - (b.z - a.z) * (c.x - a.x)) * inv_area;

    const float min_x = std::min({a.x, b.x, c.x}), max_x = std::max({a.x, b.x, c.x});
    const float min_y = std::min({a.y, b.y, c.y}), max_y = std::max({a.y, b.y, c.y});
    const int x0 = std::max(0, static_cast<int>(std::ceil(min_x)));
    const int x1 = std::min(o.width, static_cast<int>(std::floor(max_x)));
    const int y0 = std::max(0, static_cast<int>(std::ceil(min_y)));
    const int y1 = std::min(o.height, static_cast<int>(std::floor(max_y)));
    if (x0 > x1 || y0 > y1) return;

    const float sign = area > 0.0f ? 1.0f : -1.0f;
    auto edge = [&](const ScreenVertex& p, const ScreenVertex& q, float x, float y) {
        return ((q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x)) * sign;
    };
    constexpr float on_the_edge = -1e-3f; // a corner exactly on a shared edge belongs to both triangles
    const int stride = o.width + 1;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            if (edge(a, b, fx, fy) < on_the_edge || edge(b, c, fx, fy) < on_the_edge || edge(c, a, fx, fy) < on_the_edge) continue;
            float& d = o.depth[static_cast<size_t>(y) * stride + x];
            d = std::min(d, a.z + dzdx * (fx - a.x) + dzdy * (fy - a.y));
        }
    }
}

ScreenVertex to_screen(const OcclusionImpl& o, const ClipVertex& v) {
    const float inv_w = 1.0f / v.w;
    return {(v.x * inv_w * 0.5f + 0.5f) * static_cast<float>(o.width), (v.y * inv_w * 0.5f + 0.5f) * static_cast<float>(o.height),
            v.z * inv_w};
}

} // namespace

OcclusionBuffer::OcclusionBuffer(int width, int height) : impl_(std::make_unique<OcclusionImpl>()) {
    impl_->width = std::max(width, 8);
    impl_->height = std::max(height, 8);
    impl_->depth.assign(static_cast<size_t>(impl_->width + 1) * (impl_->height + 1), no_occluder);
}
OcclusionBuffer::~OcclusionBuffer() = default;
OcclusionBuffer::OcclusionBuffer(OcclusionBuffer&&) noexcept = default;
OcclusionBuffer& OcclusionBuffer::operator=(OcclusionBuffer&&) noexcept = default;

void OcclusionBuffer::begin(const mat4& view_proj) {
    std::fill(impl_->depth.begin(), impl_->depth.end(), no_occluder);
    impl_->view_proj = view_proj;
    impl_->triangles = 0;
}

int OcclusionBuffer::triangles_drawn() const { return impl_->triangles; }

void OcclusionBuffer::add_occluder(const std::vector<vec3>& positions, const std::vector<uint32_t>& triangles, const mat4& world) {
    OcclusionImpl& o = *impl_;
    const mat4 m = o.view_proj * world;
    std::vector<ClipVertex> clip(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        const vec3& p = positions[i];
        clip[i] = {m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2) * p.z + m(0, 3), m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2) * p.z + m(1, 3),
                   m(2, 0) * p.x + m(2, 1) * p.y + m(2, 2) * p.z + m(2, 3), m(3, 0) * p.x + m(3, 1) * p.y + m(3, 2) * p.z + m(3, 3)};
    }
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        ClipVertex in[3] = {clip[triangles[t]], clip[triangles[t + 1]], clip[triangles[t + 2]]};
        // Clip against the near plane (z >= -w, with a hair of margin) so a
        // floor under the camera still occludes. The other planes are the
        // pixel loop's bounds.
        ClipVertex poly[4];
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            const ClipVertex& cur = in[i];
            const ClipVertex& nxt = in[(i + 1) % 3];
            const float dc = cur.z + cur.w - near_epsilon, dn = nxt.z + nxt.w - near_epsilon;
            if (dc >= 0.0f) poly[n++] = cur;
            if ((dc >= 0.0f) != (dn >= 0.0f)) poly[n++] = lerp_clip(cur, nxt, dc / (dc - dn));
        }
        if (n < 3) continue;
        bool usable = true;
        for (int i = 0; i < n; ++i) usable = usable && poly[i].w > near_epsilon;
        if (!usable) continue;
        ScreenVertex s[4];
        for (int i = 0; i < n; ++i) s[i] = to_screen(o, poly[i]);
        raster_triangle(o, s[0], s[1], s[2]);
        if (n == 4) raster_triangle(o, s[0], s[2], s[3]);
        ++o.triangles;
    }
}

bool OcclusionBuffer::is_visible(const Bounds& box) const {
    const OcclusionImpl& o = *impl_;
    if (o.triangles == 0 || !box.valid()) return true;
    float min_x = no_occluder, max_x = -no_occluder, min_y = no_occluder, max_y = -no_occluder, min_z = no_occluder;
    for (int i = 0; i < 8; ++i) {
        const vec3 p{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z};
        const mat4& m = o.view_proj;
        const float w = m(3, 0) * p.x + m(3, 1) * p.y + m(3, 2) * p.z + m(3, 3);
        const float z = m(2, 0) * p.x + m(2, 1) * p.y + m(2, 2) * p.z + m(2, 3);
        if (w <= near_epsilon || z + w <= near_epsilon) return true; // touches the near plane: can't judge
        const float x = (m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2) * p.z + m(0, 3)) / w;
        const float y = (m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2) * p.z + m(1, 3)) / w;
        min_x = std::min(min_x, x), max_x = std::max(max_x, x);
        min_y = std::min(min_y, y), max_y = std::max(max_y, y);
        min_z = std::min(min_z, z / w);
    }
    const float fw = static_cast<float>(o.width), fh = static_cast<float>(o.height);
    const int x0 = std::max(0, static_cast<int>(std::floor((min_x * 0.5f + 0.5f) * fw)));
    const int x1 = std::min(o.width - 1, static_cast<int>(std::floor((max_x * 0.5f + 0.5f) * fw)));
    const int y0 = std::max(0, static_cast<int>(std::floor((min_y * 0.5f + 0.5f) * fh)));
    const int y1 = std::min(o.height - 1, static_cast<int>(std::floor((max_y * 0.5f + 0.5f) * fh)));
    // Entirely off screen: not this test's call (the frustum's).
    if (x0 > x1 || y0 > y1 || max_x < -1.0f || min_x > 1.0f || max_y < -1.0f || min_y > 1.0f) return true;
    // A box that sticks out past the screen's edge hides behind occluders
    // only as far as the screen goes, which is all the buffer knows.
    const int stride = o.width + 1;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float* top = &o.depth[static_cast<size_t>(y) * stride + x];
            // The pixel's far side: the farthest of its four corners (a
            // missing corner is infinity, so a pixel with a gap never hides).
            const float far_side = std::max(std::max(top[0], top[1]), std::max(top[stride], top[stride + 1]));
            if (!(min_z > far_side)) return true;
        }
    }
    return false;
}

} // namespace thistle::three
