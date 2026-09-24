#include <thistle.hpp>

#include <cmath>
#include <unordered_map>

namespace thistle::three {

void Bounds::add(vec3 p) {
    min = {std::fmin(min.x, p.x), std::fmin(min.y, p.y), std::fmin(min.z, p.z)};
    max = {std::fmax(max.x, p.x), std::fmax(max.y, p.y), std::fmax(max.z, p.z)};
}

Bounds Bounds::transformed(const mat4& m) const {
    Bounds out;
    if (!valid()) return out;
    for (int i = 0; i < 8; ++i) {
        const vec3 corner{(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z};
        out.add(m.transform_point(corner));
    }
    return out;
}

void MeshData::add_triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    indices.insert(indices.end(), {base, base + 1, base + 2});
}

void MeshData::add_quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), {a, b, c, d});
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void MeshData::append(const MeshData& other, const Transform& t) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const mat4 m = t.matrix();
    const mat4 nm = transpose(inverse(m));
    for (Vertex v : other.vertices) {
        v.position = m.transform_point(v.position);
        v.normal = normalize(nm.transform_direction(v.normal));
        vertices.push_back(v);
    }
    for (uint32_t i : other.indices) indices.push_back(base + i);
}

void MeshData::recalculate_normals() {
    // Vertices at the same position share one averaged normal even when they
    // were split (e.g. at a UV seam), so seams don't show as lighting creases.
    struct Key {
        int32_t x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return (static_cast<size_t>(k.x) * 73856093u) ^ (static_cast<size_t>(k.y) * 19349663u) ^ (static_cast<size_t>(k.z) * 83492791u);
        }
    };
    auto key_of = [](vec3 p) {
        return Key{static_cast<int32_t>(std::lround(p.x * 10000.0f)), static_cast<int32_t>(std::lround(p.y * 10000.0f)),
                   static_cast<int32_t>(std::lround(p.z * 10000.0f))};
    };
    // Each face's normal is weighted by the triangle's angle at the vertex.
    // Area weighting (the obvious choice) makes the result depend on how a
    // quad happened to be split into triangles — a box corner came out
    // lopsided instead of pointing straight out diagonally.
    std::unordered_map<Key, vec3, KeyHash> sums;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const vec3 p[3] = {vertices[indices[i]].position, vertices[indices[i + 1]].position,
                           vertices[indices[i + 2]].position};
        const vec3 face = normalize(cross(p[1] - p[0], p[2] - p[0]));
        for (int k = 0; k < 3; ++k) {
            const vec3 e1 = normalize(p[(k + 1) % 3] - p[k]);
            const vec3 e2 = normalize(p[(k + 2) % 3] - p[k]);
            const float angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, dot(e1, e2))));
            sums[key_of(p[k])] += face * angle;
        }
    }
    for (Vertex& v : vertices) {
        auto it = sums.find(key_of(v.position));
        if (it != sums.end()) v.normal = normalize(it->second);
    }
}

void MeshData::make_flat() {
    std::vector<Vertex> flat;
    std::vector<uint32_t> flat_indices;
    flat.reserve(indices.size());
    flat_indices.reserve(indices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vertex a = vertices[indices[i]], b = vertices[indices[i + 1]], c = vertices[indices[i + 2]];
        const vec3 n = normalize(cross(b.position - a.position, c.position - a.position));
        a.normal = b.normal = c.normal = n;
        const uint32_t base = static_cast<uint32_t>(flat.size());
        flat.insert(flat.end(), {a, b, c});
        flat_indices.insert(flat_indices.end(), {base, base + 1, base + 2});
    }
    vertices = std::move(flat);
    indices = std::move(flat_indices);
}

void MeshData::set_color(rgba color) {
    for (Vertex& v : vertices) v.color = color;
}

Bounds MeshData::bounds() const {
    Bounds b;
    for (const Vertex& v : vertices) b.add(v.position);
    return b;
}

MeshData box_mesh(vec3 size) {
    const vec3 h = size * 0.5f;
    MeshData m;
    // Each face: outward normal, then the face's right and up axes as seen
    // from outside, so corners go bottom-left, bottom-right, top-right,
    // top-left = counter-clockwise from the front.
    struct Face { vec3 n, right, up; };
    const Face faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +Z
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, // -Z
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  // +X
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -X
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  // +Y
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // -Y
    };
    for (const Face& f : faces) {
        const vec3 c = f.n * h;
        const vec3 r = f.right * h;
        const vec3 u = f.up * h;
        m.add_quad({c - r - u, f.n, {0, 1}}, {c + r - u, f.n, {1, 1}},
                   {c + r + u, f.n, {1, 0}}, {c - r + u, f.n, {0, 0}});
    }
    return m;
}

MeshData sphere_mesh(float radius, int rings, int segments) {
    rings = rings < 2 ? 2 : rings;
    segments = segments < 3 ? 3 : segments;
    MeshData m;
    for (int r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / rings;
        const float phi = v * pi; // 0 at the top pole
        for (int s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / segments;
            const float theta = u * 2.0f * pi;
            const vec3 n{std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
            m.vertices.push_back({n * radius, n, {u, v}});
        }
    }
    const uint32_t row = static_cast<uint32_t>(segments + 1);
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const uint32_t a = r * row + s, b = a + row, c = b + 1, d = a + 1;
            if (r != 0) m.indices.insert(m.indices.end(), {a, b, d});
            if (r != rings - 1) m.indices.insert(m.indices.end(), {d, b, c});
        }
    }
    return m;
}

namespace {

// Ring of vertices at height y, normals from `normal_at(theta)`.
void add_ring(MeshData& m, float radius, float y, float v, int segments, vec3 (*normal_at)(float, float), float slope) {
    for (int s = 0; s <= segments; ++s) {
        const float u = static_cast<float>(s) / segments;
        const float theta = u * 2.0f * pi;
        m.vertices.push_back({{std::sin(theta) * radius, y, std::cos(theta) * radius}, normal_at(theta, slope), {u, v}});
    }
}

vec3 side_normal(float theta, float slope) { return normalize(vec3{std::sin(theta), slope, std::cos(theta)}); }

void add_cap(MeshData& m, float radius, float y, bool up, int segments) {
    const vec3 n{0.0f, up ? 1.0f : -1.0f, 0.0f};
    const uint32_t center = static_cast<uint32_t>(m.vertices.size());
    m.vertices.push_back({{0.0f, y, 0.0f}, n, {0.5f, 0.5f}});
    for (int s = 0; s <= segments; ++s) {
        const float theta = static_cast<float>(s) / segments * 2.0f * pi;
        const float x = std::sin(theta), z = std::cos(theta);
        m.vertices.push_back({{x * radius, y, z * radius}, n, {0.5f + x * 0.5f, 0.5f - z * 0.5f}});
    }
    for (int s = 0; s < segments; ++s) {
        const uint32_t a = center + 1 + s, b = a + 1;
        if (up) m.indices.insert(m.indices.end(), {center, a, b});
        else m.indices.insert(m.indices.end(), {center, b, a});
    }
}

void connect_rings(MeshData& m, uint32_t lower, uint32_t upper, int segments) {
    for (int s = 0; s < segments; ++s) {
        const uint32_t a = lower + s, b = a + 1, c = upper + s + 1, d = upper + s;
        m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
    }
}

} // namespace

MeshData cylinder_mesh(float radius, float height, int segments) {
    segments = segments < 3 ? 3 : segments;
    MeshData m;
    const float h = height * 0.5f;
    const uint32_t lower = 0;
    add_ring(m, radius, -h, 1.0f, segments, side_normal, 0.0f);
    const uint32_t upper = static_cast<uint32_t>(m.vertices.size());
    add_ring(m, radius, h, 0.0f, segments, side_normal, 0.0f);
    connect_rings(m, lower, upper, segments);
    add_cap(m, radius, h, true, segments);
    add_cap(m, radius, -h, false, segments);
    return m;
}

MeshData cone_mesh(float radius, float height, int segments) {
    segments = segments < 3 ? 3 : segments;
    MeshData m;
    const float h = height * 0.5f;
    const float slope = height > 0.0f ? radius / height : 0.0f;
    add_ring(m, radius, -h, 1.0f, segments, side_normal, slope);
    // The apex is one vertex per segment (not one shared vertex), so each
    // side facet keeps its own normal and the tip doesn't shade as a spike.
    for (int s = 0; s < segments; ++s) {
        const float theta = (static_cast<float>(s) + 0.5f) / segments * 2.0f * pi;
        m.vertices.push_back({{0.0f, h, 0.0f}, side_normal(theta, slope), {(s + 0.5f) / segments, 0.0f}});
    }
    const uint32_t apex = static_cast<uint32_t>(segments + 1);
    for (int s = 0; s < segments; ++s) {
        m.indices.insert(m.indices.end(), {static_cast<uint32_t>(s), static_cast<uint32_t>(s + 1), apex + s});
    }
    add_cap(m, radius, -h, false, segments);
    return m;
}

MeshData capsule_mesh(float radius, float height, int segments) {
    segments = segments < 3 ? 3 : segments;
    const int half_rings = segments / 2 < 2 ? 2 : segments / 2;
    const float body = std::fmax(height - 2.0f * radius, 0.0f) * 0.5f;
    MeshData m;
    // Rings from the top pole down: the top hemisphere offset up by `body`,
    // then the bottom one offset down, so the middle band is the cylinder.
    const int total = half_rings * 2 + 1;
    for (int r = 0; r <= total; ++r) {
        const bool top = r <= half_rings;
        const float phi = top ? (static_cast<float>(r) / half_rings) * pi * 0.5f
                              : pi * 0.5f + (static_cast<float>(r - half_rings - 1) / half_rings) * pi * 0.5f;
        const float y_off = top ? body : -body;
        const float v = static_cast<float>(r) / total;
        for (int s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / segments;
            const float theta = u * 2.0f * pi;
            const vec3 n{std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
            m.vertices.push_back({n * radius + vec3{0.0f, y_off, 0.0f}, n, {u, v}});
        }
    }
    const uint32_t row = static_cast<uint32_t>(segments + 1);
    for (int r = 0; r < total; ++r) {
        for (int s = 0; s < segments; ++s) {
            const uint32_t a = r * row + s, b = a + row, c = b + 1, d = a + 1;
            if (r != 0) m.indices.insert(m.indices.end(), {a, b, d});
            if (r != total - 1) m.indices.insert(m.indices.end(), {d, b, c});
        }
    }
    return m;
}

MeshData plane_mesh(float width, float depth, float uv_repeat) {
    const float w = width * 0.5f, d = depth * 0.5f;
    const vec3 n{0.0f, 1.0f, 0.0f};
    MeshData m;
    m.add_quad({{-w, 0, d}, n, {0, uv_repeat}}, {{w, 0, d}, n, {uv_repeat, uv_repeat}},
               {{w, 0, -d}, n, {uv_repeat, 0}}, {{-w, 0, -d}, n, {0, 0}});
    return m;
}

} // namespace thistle::three
