#include "thistle_core.h"

#include <algorithm>
#include <cmath>

namespace thistle::three {

namespace {
constexpr int kTileCells = 32; // cells per tile side: one Model (and one frustum test) per tile
}

struct TerrainImpl {
    int cx = 0, cz = 0;
    float cell = 1.0f;
    std::vector<float> heights; // (cx + 1) * (cz + 1), row-major in z
    bool dirty = true;
    std::vector<Model> tiles;
    vec3 built_origin;     // tiles have world positions baked in, so moving
    bool built_flat = true; // the terrain or switching shading means a rebuild

    float& at(int x, int z) { return heights[static_cast<size_t>(z) * (cx + 1) + x]; }
    float at(int x, int z) const {
        x = std::clamp(x, 0, cx);
        z = std::clamp(z, 0, cz);
        return heights[static_cast<size_t>(z) * (cx + 1) + x];
    }
    void drop_tiles() {
        for (Model& m : tiles) unload_model(m);
        tiles.clear();
    }
};

Terrain::Terrain(int cells_x, int cells_z, float cell_size) : impl_(std::make_unique<TerrainImpl>()) {
    impl_->cx = std::max(1, cells_x);
    impl_->cz = std::max(1, cells_z);
    impl_->cell = cell_size > 0.0f ? cell_size : 1.0f;
    impl_->heights.assign(static_cast<size_t>(impl_->cx + 1) * (impl_->cz + 1), 0.0f);
    material.specular = 0.05f;
}

Terrain::~Terrain() {
    if (impl_) impl_->drop_tiles();
}
Terrain::Terrain(Terrain&&) noexcept = default;
Terrain& Terrain::operator=(Terrain&& other) noexcept {
    if (this != &other) {
        if (impl_) impl_->drop_tiles();
        impl_ = std::move(other.impl_);
        origin = other.origin;
        flat_shaded = other.flat_shaded;
        material = other.material;
        colorize = std::move(other.colorize);
    }
    return *this;
}

int Terrain::cells_x() const { return impl_->cx; }
int Terrain::cells_z() const { return impl_->cz; }
float Terrain::cell_size() const { return impl_->cell; }
float Terrain::height(int x, int z) const { return impl_->at(x, z); }

void Terrain::set_height(int x, int z, float h) {
    if (x < 0 || z < 0 || x > impl_->cx || z > impl_->cz) return;
    impl_->at(x, z) = h;
    impl_->dirty = true;
}

void Terrain::generate(const std::function<float(float, float)>& height_fn) {
    for (int z = 0; z <= impl_->cz; ++z)
        for (int x = 0; x <= impl_->cx; ++x)
            impl_->at(x, z) = height_fn(origin.x + x * impl_->cell, origin.z + z * impl_->cell);
    impl_->dirty = true;
}

float Terrain::height_at(float wx, float wz) const {
    const float gx = std::clamp((wx - origin.x) / impl_->cell, 0.0f, static_cast<float>(impl_->cx));
    const float gz = std::clamp((wz - origin.z) / impl_->cell, 0.0f, static_cast<float>(impl_->cz));
    const int ix = std::min(static_cast<int>(gx), impl_->cx - 1);
    const int iz = std::min(static_cast<int>(gz), impl_->cz - 1);
    const float fx = gx - ix, fz = gz - iz;
    const float h00 = impl_->at(ix, iz), h10 = impl_->at(ix + 1, iz);
    const float h01 = impl_->at(ix, iz + 1), h11 = impl_->at(ix + 1, iz + 1);
    // Same split as the drawn triangles (diagonal from (0,0) to (1,1)), so
    // things placed with this sit exactly on the visible surface.
    const float h = fz >= fx ? h00 + (h11 - h01) * fx + (h01 - h00) * fz
                             : h00 + (h10 - h00) * fx + (h11 - h10) * fz;
    return origin.y + h;
}

vec3 Terrain::normal_at(float wx, float wz) const {
    const float gx = std::clamp((wx - origin.x) / impl_->cell, 0.0f, static_cast<float>(impl_->cx));
    const float gz = std::clamp((wz - origin.z) / impl_->cell, 0.0f, static_cast<float>(impl_->cz));
    const int ix = std::min(static_cast<int>(gx), impl_->cx - 1);
    const int iz = std::min(static_cast<int>(gz), impl_->cz - 1);
    const float c = impl_->cell;
    const float h00 = impl_->at(ix, iz), h10 = impl_->at(ix + 1, iz);
    const float h01 = impl_->at(ix, iz + 1), h11 = impl_->at(ix + 1, iz + 1);
    const bool upper = (gz - iz) >= (gx - ix);
    // The plane h = h00 + a*fx + b*fz has normal (-a/c, 1, -b/c).
    const float a = upper ? h11 - h01 : h10 - h00;
    const float b = upper ? h01 - h00 : h11 - h10;
    return normalize(vec3{-a / c, 1.0f, -b / c});
}

Bounds Terrain::bounds() const {
    const auto [lo, hi] = std::minmax_element(impl_->heights.begin(), impl_->heights.end());
    return Bounds{origin + vec3{0.0f, *lo, 0.0f}, origin + vec3{impl_->cx * impl_->cell, *hi, impl_->cz * impl_->cell}};
}

namespace {

// Tiny per-vertex brightness jitter so big flat areas don't look plastic.
float jitter(int x, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return 0.96f + 0.08f * static_cast<float>(h & 0xFFFF) / 65535.0f;
}

rgba default_color(float h, float slope, float lo, float hi) {
    const float t = hi > lo ? (h - lo) / (hi - lo) : 0.5f;
    const rgba sand = rgb(0.84f, 0.78f, 0.56f), grass = rgb(0.40f, 0.62f, 0.30f), dark = rgb(0.26f, 0.46f, 0.24f);
    const rgba rock = rgb(0.50f, 0.48f, 0.46f), snow = rgb(0.94f, 0.95f, 0.98f);
    if (t > 0.82f && slope < 0.55f) return snow;
    if (slope > 0.42f) return rock;
    if (t < 0.07f) return sand;
    return lerp(grass, dark, std::clamp((t - 0.07f) / 0.75f, 0.0f, 1.0f));
}

} // namespace

void Terrain::rebuild() {
    TerrainImpl& t = *impl_;
    t.drop_tiles();
    const auto [lo_it, hi_it] = std::minmax_element(t.heights.begin(), t.heights.end());
    const float lo = *lo_it, hi = *hi_it;
    auto grid_normal = [&](int x, int z) {
        // Central differences: smooth normals that agree across tile seams.
        const float dx = (t.at(x + 1, z) - t.at(x - 1, z)) / (2.0f * t.cell);
        const float dz = (t.at(x, z + 1) - t.at(x, z - 1)) / (2.0f * t.cell);
        return normalize(vec3{-dx, 1.0f, -dz});
    };
    for (int tz = 0; tz < t.cz; tz += kTileCells) {
        for (int tx = 0; tx < t.cx; tx += kTileCells) {
            const int x1 = std::min(tx + kTileCells, t.cx), z1 = std::min(tz + kTileCells, t.cz);
            MeshData mesh;
            const int w = x1 - tx + 1;
            for (int z = tz; z <= z1; ++z) {
                for (int x = tx; x <= x1; ++x) {
                    Vertex v;
                    v.position = origin + vec3{x * t.cell, t.at(x, z), z * t.cell};
                    v.normal = grid_normal(x, z);
                    v.uv = {x * t.cell, z * t.cell};
                    const float slope = 1.0f - v.normal.y;
                    rgba c = colorize ? colorize(t.at(x, z), slope) : default_color(t.at(x, z), slope, lo, hi);
                    const float j = jitter(x, z);
                    v.color = rgba{c.r * j, c.g * j, c.b * j, c.a};
                    mesh.vertices.push_back(v);
                }
            }
            for (int z = 0; z < z1 - tz; ++z) {
                for (int x = 0; x < x1 - tx; ++x) {
                    const uint32_t i00 = z * w + x, i10 = i00 + 1, i01 = i00 + w, i11 = i01 + 1;
                    mesh.indices.insert(mesh.indices.end(), {i00, i01, i11, i00, i11, i10});
                }
            }
            if (flat_shaded) {
                mesh.make_flat();
                // One color per facet too: the low-poly look is flat faces,
                // not gradients across them.
                for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
                    rgba avg{0, 0, 0, 0};
                    for (int k = 0; k < 3; ++k) {
                        const rgba& c = mesh.vertices[i + k].color;
                        avg = rgba{avg.r + c.r / 3, avg.g + c.g / 3, avg.b + c.b / 3, avg.a + c.a / 3};
                    }
                    for (int k = 0; k < 3; ++k) mesh.vertices[i + k].color = avg;
                }
            }
            t.tiles.push_back(make_model(mesh, material));
        }
    }
    t.dirty = false;
    t.built_origin = origin;
    t.built_flat = flat_shaded;
}

#if !defined(THISTLE_SERVER) // drawing: not in the server library
void World::draw(Terrain& terrain) {
    const TerrainImpl& t = *terrain.impl_;
    if (t.dirty || t.built_origin != terrain.origin || t.built_flat != terrain.flat_shaded) terrain.rebuild();
    for (const Model& tile : terrain.impl_->tiles) {
        set_model_material(tile, terrain.material); // picks up material edits without a rebuild
        draw(tile);
    }
}
#endif

} // namespace thistle::three

namespace thistle::detail {

void terrain_triangles(const three::Terrain& terrain, std::vector<vec3>& out) {
    const int cx = terrain.cells_x(), cz = terrain.cells_z();
    const float c = terrain.cell_size();
    out.reserve(out.size() + static_cast<size_t>(cx) * cz * 6);
    auto p = [&](int x, int z) { return terrain.origin + vec3{x * c, terrain.height(x, z), z * c}; };
    for (int z = 0; z < cz; ++z) {
        for (int x = 0; x < cx; ++x) {
            out.insert(out.end(), {p(x, z), p(x, z + 1), p(x + 1, z + 1), p(x, z), p(x + 1, z + 1), p(x + 1, z)});
        }
    }
}

} // namespace thistle::detail
