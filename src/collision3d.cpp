// The simple Box3D/Sphere3D overlap tests, shared by the full engine and the
// headless server library.
#include <thistle.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace thistle {

bool box3d_overlap(const Box3D& a, const Box3D& b) {
    return std::fabs(a.center.x - b.center.x) <= (a.half_extent.x + b.half_extent.x) &&
           std::fabs(a.center.y - b.center.y) <= (a.half_extent.y + b.half_extent.y) &&
           std::fabs(a.center.z - b.center.z) <= (a.half_extent.z + b.half_extent.z);
}

bool box3d_contains_point(const Box3D& box, vec3 point) {
    return std::fabs(point.x - box.center.x) <= box.half_extent.x &&
           std::fabs(point.y - box.center.y) <= box.half_extent.y &&
           std::fabs(point.z - box.center.z) <= box.half_extent.z;
}

bool sphere3d_overlap(const Sphere3D& a, const Sphere3D& b) {
    const float dx = a.center.x - b.center.x, dy = a.center.y - b.center.y, dz = a.center.z - b.center.z;
    const float r = a.radius + b.radius;
    return (dx * dx + dy * dy + dz * dz) <= r * r;
}

bool box3d_sphere3d_overlap(const Box3D& box, const Sphere3D& sphere) {
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const float cx = clampf(sphere.center.x, box.center.x - box.half_extent.x, box.center.x + box.half_extent.x);
    const float cy = clampf(sphere.center.y, box.center.y - box.half_extent.y, box.center.y + box.half_extent.y);
    const float cz = clampf(sphere.center.z, box.center.z - box.half_extent.z, box.center.z + box.half_extent.z);
    const float dx = sphere.center.x - cx, dy = sphere.center.y - cy, dz = sphere.center.z - cz;
    return (dx * dx + dy * dy + dz * dz) <= sphere.radius * sphere.radius;
}

bool ray_box3d(vec3 ray_origin, vec3 ray_dir, const Box3D& box, float& out_t) {
    const float box_min[3] = {box.center.x - box.half_extent.x, box.center.y - box.half_extent.y, box.center.z - box.half_extent.z};
    const float box_max[3] = {box.center.x + box.half_extent.x, box.center.y + box.half_extent.y, box.center.z + box.half_extent.z};
    const float origin[3] = {ray_origin.x, ray_origin.y, ray_origin.z};
    const float dir[3] = {ray_dir.x, ray_dir.y, ray_dir.z};
    float t_min = -std::numeric_limits<float>::infinity();
    float t_max = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-8f) {
            if (origin[i] < box_min[i] || origin[i] > box_max[i]) return false;
            continue;
        }
        float t1 = (box_min[i] - origin[i]) / dir[i];
        float t2 = (box_max[i] - origin[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) return false;
    }
    if (t_max < 0.0f) return false; // box is entirely behind the ray
    out_t = t_min >= 0.0f ? t_min : t_max; // ray origin starts inside the box
    return true;
}

} // namespace thistle
