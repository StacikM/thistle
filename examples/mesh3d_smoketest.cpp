// Checks thistle::three's CPU-side geometry: every built-in shape's
// triangles wind counter-clockwise as seen from outside (the renderer culls
// back faces, so a single reversed triangle would render as a hole), their
// normals are unit length and point outward, bounds match the requested
// size, and the MeshData editing helpers do what they say. No GPU needed.
#include <thistle.hpp>
#include <cmath>
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
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near(vec3 a, vec3 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

// Winding vs. stored normals, per triangle, for a closed convex-ish shape
// centered at the origin: the geometric normal from the vertex order must
// agree with the vertex normals and point away from the center.
void check_shape(const char* name, const MeshData& m, vec3 expected_size) {
    int bad_winding = 0, bad_normal = 0, degenerate = 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vertex& a = m.vertices[m.indices[i]];
        const Vertex& b = m.vertices[m.indices[i + 1]];
        const Vertex& c = m.vertices[m.indices[i + 2]];
        const vec3 face = cross(b.position - a.position, c.position - a.position);
        if (length(face) < 1e-7f) { ++degenerate; continue; }
        const vec3 avg_normal = a.normal + b.normal + c.normal;
        const vec3 centroid = (a.position + b.position + c.position) / 3.0f;
        if (dot(face, avg_normal) <= 0.0f) ++bad_winding;
        if (dot(face, centroid) <= 0.0f) ++bad_winding;
        for (const Vertex* v : {&a, &b, &c}) if (!near(length(v->normal), 1.0f)) ++bad_normal;
    }
    check(bad_winding == 0, std::string(name) + ": every triangle is counter-clockwise from outside (" + std::to_string(bad_winding) + " bad)");
    check(bad_normal == 0, std::string(name) + ": every normal is unit length");
    check(degenerate == 0, std::string(name) + ": no zero-area triangles");
    check(m.indices.size() % 3 == 0 && !m.indices.empty(), std::string(name) + ": index count is a whole number of triangles");
    bool in_range = true;
    for (uint32_t idx : m.indices) if (idx >= m.vertices.size()) in_range = false;
    check(in_range, std::string(name) + ": every index points at a real vertex");
    check(near(m.bounds().size(), expected_size, 2e-3f), std::string(name) + ": bounds match the requested size");
}
} // namespace

int main() {
    check_shape("box", box_mesh({2, 1, 3}), {2, 1, 3});
    // 16 segments put vertices exactly on the +-X/+-Z axes, so the bounds are the full diameter.
    check_shape("sphere", sphere_mesh(1.5f, 12, 16), {3, 3, 3});
    // 3 rings at 0/60/120/180 degrees: the widest ring is at 60, so 2*sin(60) across.
    check_shape("low-poly sphere", sphere_mesh(1.0f, 3, 4), {2.0f * std::sin(pi / 3), 2, 2.0f * std::sin(pi / 3)});
    check_shape("cylinder", cylinder_mesh(0.5f, 2.0f, 16), {1, 2, 1});
    check_shape("cone", cone_mesh(1.0f, 2.0f, 32), {2, 2, 2});
    check_shape("capsule", capsule_mesh(0.5f, 2.0f, 16), {1, 2, 1});

    {
        const MeshData p = plane_mesh(4, 2, 3);
        check(p.vertices.size() == 4 && p.indices.size() == 6, "plane is one quad");
        const vec3 face = cross(p.vertices[p.indices[1]].position - p.vertices[p.indices[0]].position,
                                p.vertices[p.indices[2]].position - p.vertices[p.indices[0]].position);
        check(face.y > 0.0f, "plane faces +Y");
        float max_uv = 0.0f;
        for (const Vertex& v : p.vertices) max_uv = std::fmax(max_uv, v.uv.x);
        check(near(max_uv, 3.0f), "plane uv_repeat tiles the texture 3 times");
    }
    {
        MeshData m = sphere_mesh(1.0f, 4, 6);
        const size_t tris = m.indices.size() / 3;
        m.make_flat();
        check(m.vertices.size() == tris * 3, "make_flat gives every triangle its own 3 vertices");
        bool flat = true;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const vec3 n = m.vertices[m.indices[i]].normal;
            if (!near(n, m.vertices[m.indices[i + 1]].normal) || !near(n, m.vertices[m.indices[i + 2]].normal)) flat = false;
        }
        check(flat, "make_flat: all 3 vertices of a triangle share the face normal");
    }
    {
        MeshData m = box_mesh();
        for (Vertex& v : m.vertices) v.normal = {0, 0, 1}; // garbage
        m.recalculate_normals();
        // A box's corners are shared by 3 faces, so smooth normals point diagonally out.
        const float d = 1.0f / std::sqrt(3.0f);
        check(near(m.vertices[0].normal, normalize(m.vertices[0].position)) && near(std::fabs(m.vertices[0].normal.x), d),
              "recalculate_normals averages across faces that share a corner");
    }
    {
        MeshData combined;
        combined.append(box_mesh());
        combined.append(box_mesh(), Transform{{5, 0, 0}, {}, {2, 2, 2}});
        check(combined.vertices.size() == 48 && combined.indices.size() == 72, "append adds all vertices and indices");
        check(near(combined.bounds().max, {6, 1, 1}), "append applies the transform (moved + scaled)");
        check(combined.indices[36] == 24, "appended indices are offset past the existing vertices");
    }
    {
        MeshData m = box_mesh();
        m.set_color(coral);
        check(m.vertices[7].color.r == coral.r && m.vertices[7].color.g == coral.g, "set_color paints every vertex");
    }
    {
        const Bounds b = box_mesh({2, 2, 2}).bounds();
        const Bounds moved = b.transformed(mat4::trs({10, 0, 0}, quat::axis_angle({0, 1, 0}, radians(45)), {1, 1, 1}));
        check(near(moved.center(), {10, 0, 0}) && near(moved.size().x, 2.0f * std::sqrt(2.0f)),
              "Bounds::transformed grows to fit a rotated box");
    }

    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all mesh3d checks passed\n");
    return 0;
}
