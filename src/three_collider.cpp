// three::Collider's builders. Plain data, so they're built either way:
// games that make colliders compile and run the same with Physics3D off.
#include "thistle_internal.h"

namespace thistle::three {

Collider Collider::box(vec3 size) {
    Collider c;
    c.kind = Kind::Box;
    c.size = size;
    return c;
}

Collider Collider::sphere(float radius) {
    Collider c;
    c.kind = Kind::Sphere;
    c.size = {radius, radius * 2.0f, radius};
    return c;
}

Collider Collider::capsule(float radius, float height) {
    Collider c;
    c.kind = Kind::Capsule;
    c.size = {radius, height, radius};
    return c;
}

Collider Collider::cylinder(float radius, float height) {
    Collider c;
    c.kind = Kind::Cylinder;
    c.size = {radius, height, radius};
    return c;
}

Collider Collider::convex(std::vector<vec3> points) {
    Collider c;
    c.kind = Kind::Convex;
    c.points = std::move(points);
    return c;
}

Collider Collider::convex(const MeshData& mesh) {
    std::vector<vec3> points;
    points.reserve(mesh.vertices.size());
    for (const Vertex& v : mesh.vertices) points.push_back(v.position);
    return convex(std::move(points));
}

Collider Collider::convex(Model model, vec3 scale) {
    Collider c;
    c.kind = Kind::Convex;
    detail::model_world_triangles(model, mat4::scale(scale), c.points); // duplicates are fine for a hull
    return c;
}

Collider Collider::mesh(const MeshData& mesh) {
    Collider c;
    c.kind = Kind::Mesh;
    c.points.reserve(mesh.indices.size());
    for (uint32_t i : mesh.indices) {
        if (i < mesh.vertices.size()) c.points.push_back(mesh.vertices[i].position);
    }
    c.points.resize(c.points.size() / 3 * 3);
    return c;
}

Collider Collider::mesh(Model model, vec3 scale) {
    Collider c;
    c.kind = Kind::Mesh;
    detail::model_world_triangles(model, mat4::scale(scale), c.points);
    return c;
}

Collider Collider::compound(std::vector<Collider> parts) {
    Collider c;
    c.kind = Kind::Compound;
    c.parts = std::move(parts);
    return c;
}

Collider Collider::at(vec3 offset_, quat rotation_) const {
    Collider c = *this;
    c.offset = offset_;
    c.rotation = rotation_;
    return c;
}

} // namespace thistle::three
