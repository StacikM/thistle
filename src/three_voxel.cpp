#include "thistle_internal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace thistle::three {

namespace {

constexpr int CS = VoxelWorld::chunk_size;
constexpr int CS3 = CS * CS * CS;
static_assert((CS & (CS - 1)) == 0, "chunk_size must be a power of two (coordinates split with shifts/masks)");
constexpr int kChunkShift = 5;
static_assert((1 << kChunkShift) == CS, "kChunkShift must match chunk_size");

int64_t chunk_key(int cx, int cy, int cz) {
    constexpr int64_t bias = 1 << 20;
    return ((static_cast<int64_t>(cx) + bias) << 42) | ((static_cast<int64_t>(cy) + bias) << 21) | (static_cast<int64_t>(cz) + bias);
}
ivec3 key_chunk(int64_t key) {
    constexpr int64_t bias = 1 << 20, mask = (1 << 21) - 1;
    return {static_cast<int>(((key >> 42) & mask) - bias), static_cast<int>(((key >> 21) & mask) - bias),
            static_cast<int>((key & mask) - bias)};
}
int local_index(int lx, int ly, int lz) { return lx + CS * (ly + CS * lz); }

struct Chunk {
    std::vector<BlockId> blocks = std::vector<BlockId>(CS3, 0);
    int non_air = 0;
    bool dirty = true;
    Model model;
};

const BlockType& air_type() {
    static const BlockType air = [] {
        BlockType t;
        t.name = "air";
        t.alpha = AlphaMode::Blend;
        t.color = rgba{0, 0, 0, 0};
        t.solid = false;
        return t;
    }();
    return air;
}

} // namespace

struct VoxelWorldImpl {
    std::vector<BlockType> types{air_type()};
    std::unordered_map<int64_t, Chunk> chunks;
    Texture atlas;
    int tile_size = 16;
    TextureFilter atlas_filter = TextureFilter::Nearest;

    Chunk* find(int cx, int cy, int cz) {
        auto it = chunks.find(chunk_key(cx, cy, cz));
        return it == chunks.end() ? nullptr : &it->second;
    }
    const Chunk* find(int cx, int cy, int cz) const {
        auto it = chunks.find(chunk_key(cx, cy, cz));
        return it == chunks.end() ? nullptr : &it->second;
    }
    const BlockType& type(BlockId id) const { return id < types.size() ? types[id] : types[0]; }
};

VoxelWorld::VoxelWorld() : impl_(std::make_unique<VoxelWorldImpl>()) {}
VoxelWorld::~VoxelWorld() {
    if (impl_) {
        for (auto& [key, chunk] : impl_->chunks) unload_model(chunk.model);
    }
}
VoxelWorld::VoxelWorld(VoxelWorld&&) noexcept = default;
VoxelWorld& VoxelWorld::operator=(VoxelWorld&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            for (auto& [key, chunk] : impl_->chunks) unload_model(chunk.model);
        }
        impl_ = std::move(other.impl_);
        voxel_size = other.voxel_size;
        origin = other.origin;
        ambient_occlusion = other.ambient_occlusion;
    }
    return *this;
}

BlockId VoxelWorld::add_block(const BlockType& type) {
    if (impl_->types.size() >= 0xFFFF) {
        log_warn("VoxelWorld::add_block: out of block ids (65535 types)");
        return 0;
    }
    impl_->types.push_back(type);
    return static_cast<BlockId>(impl_->types.size() - 1);
}

const BlockType& VoxelWorld::block_type(BlockId id) const { return impl_->type(id); }

BlockId VoxelWorld::find_block(const std::string& name) const {
    for (size_t i = 1; i < impl_->types.size(); ++i) {
        if (impl_->types[i].name == name) return static_cast<BlockId>(i);
    }
    return 0;
}

int VoxelWorld::block_type_count() const { return static_cast<int>(impl_->types.size()) - 1; }

void VoxelWorld::set_atlas(Texture atlas, int tile_size, TextureFilter filter) {
    impl_->atlas = atlas;
    impl_->tile_size = std::max(1, tile_size);
    impl_->atlas_filter = filter;
    for (auto& [key, chunk] : impl_->chunks) chunk.dirty = true;
}

BlockId VoxelWorld::get(int x, int y, int z) const {
    const Chunk* c = impl_->find(x >> kChunkShift, y >> kChunkShift, z >> kChunkShift);
    return c ? c->blocks[local_index(x & (CS - 1), y & (CS - 1), z & (CS - 1))] : BlockId{0};
}

void VoxelWorld::set(int x, int y, int z, BlockId id) {
    const int cx = x >> kChunkShift, cy = y >> kChunkShift, cz = z >> kChunkShift;
    const int lx = x & (CS - 1), ly = y & (CS - 1), lz = z & (CS - 1);
    Chunk* c = impl_->find(cx, cy, cz);
    if (!c) {
        if (id == 0) return;
        c = &impl_->chunks[chunk_key(cx, cy, cz)];
    }
    BlockId& slot = c->blocks[local_index(lx, ly, lz)];
    if (slot == id) return;
    c->non_air += (id != 0) - (slot != 0);
    slot = id;
    c->dirty = true;
    // Faces and corner shading of blocks in neighboring chunks depend on
    // this block too (up to all 26 neighbors, for a corner block).
    const int rx[2] = {lx == 0 ? -1 : 0, lx == CS - 1 ? 1 : 0};
    const int ry[2] = {ly == 0 ? -1 : 0, ly == CS - 1 ? 1 : 0};
    const int rz[2] = {lz == 0 ? -1 : 0, lz == CS - 1 ? 1 : 0};
    for (int dz = rz[0]; dz <= rz[1]; ++dz) {
        for (int dy = ry[0]; dy <= ry[1]; ++dy) {
            for (int dx = rx[0]; dx <= rx[1]; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                if (Chunk* n = impl_->find(cx + dx, cy + dy, cz + dz)) n->dirty = true;
            }
        }
    }
}

void VoxelWorld::fill(ivec3 a, ivec3 b, BlockId id) {
    const ivec3 lo{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    const ivec3 hi{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x) set(x, y, z, id);
}

void VoxelWorld::fill_sphere(vec3 center, float radius, BlockId id) {
    const float r2 = radius * radius;
    const ivec3 lo{static_cast<int>(std::floor(center.x - radius)), static_cast<int>(std::floor(center.y - radius)),
                   static_cast<int>(std::floor(center.z - radius))};
    const ivec3 hi{static_cast<int>(std::floor(center.x + radius)), static_cast<int>(std::floor(center.y + radius)),
                   static_cast<int>(std::floor(center.z + radius))};
    for (int z = lo.z; z <= hi.z; ++z) {
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const vec3 d = vec3{x + 0.5f, y + 0.5f, z + 0.5f} - center;
                if (dot(d, d) <= r2) set(x, y, z, id);
            }
        }
    }
}

void VoxelWorld::clear() {
    for (auto& [key, chunk] : impl_->chunks) unload_model(chunk.model);
    impl_->chunks.clear();
}

ivec3 VoxelWorld::to_block(vec3 p) const {
    const vec3 b = (p - origin) / voxel_size;
    return {static_cast<int>(std::floor(b.x)), static_cast<int>(std::floor(b.y)), static_cast<int>(std::floor(b.z))};
}

vec3 VoxelWorld::block_center(ivec3 b) const {
    return origin + vec3{b.x + 0.5f, b.y + 0.5f, b.z + 0.5f} * voxel_size;
}

Bounds VoxelWorld::block_bounds(ivec3 b) const {
    const vec3 lo = origin + vec3{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)} * voxel_size;
    return Bounds{lo, lo + vec3{voxel_size, voxel_size, voxel_size}};
}

Bounds VoxelWorld::bounds() const {
    Bounds out;
    for (const auto& [key, chunk] : impl_->chunks) {
        if (chunk.non_air == 0) continue;
        const ivec3 c = key_chunk(key);
        for (int i = 0; i < CS3; ++i) {
            if (chunk.blocks[i] == 0) continue;
            const ivec3 b{c.x * CS + i % CS, c.y * CS + (i / CS) % CS, c.z * CS + i / (CS * CS)};
            const Bounds bb = block_bounds(b);
            out.add(bb.min);
            out.add(bb.max);
        }
    }
    return out;
}

int VoxelWorld::chunk_count() const { return static_cast<int>(impl_->chunks.size()); }

namespace {

// The chunk plus a 1-block border from its neighbors, so the mesher never
// has to ask the hash map anything in its inner loops.
constexpr int PAD = CS + 2;

struct Padded {
    std::vector<BlockId> ids = std::vector<BlockId>(PAD * PAD * PAD, 0);
    BlockId at(int x, int y, int z) const { return ids[(x + 1) + PAD * ((y + 1) + PAD * (z + 1))]; }
};

Padded gather(const VoxelWorld& world, const Chunk& chunk, ivec3 c) {
    Padded p;
    const ivec3 base{c.x * CS, c.y * CS, c.z * CS};
    for (int z = -1; z <= CS; ++z) {
        for (int y = -1; y <= CS; ++y) {
            for (int x = -1; x <= CS; ++x) {
                const bool inside = x >= 0 && x < CS && y >= 0 && y < CS && z >= 0 && z < CS;
                p.ids[(x + 1) + PAD * ((y + 1) + PAD * (z + 1))] =
                    inside ? chunk.blocks[local_index(x, y, z)] : world.get(base.x + x, base.y + y, base.z + z);
            }
        }
    }
    return p;
}

// Corner darkness levels 0 (fully tucked into a corner) .. 3 (open).
constexpr float kAoLight[4] = {0.45f, 0.65f, 0.83f, 1.0f};

struct PartKey {
    bool textured;
    AlphaMode alpha;
    uint32_t emissive;
    bool operator<(const PartKey& o) const {
        if (textured != o.textured) return textured < o.textured;
        if (alpha != o.alpha) return alpha < o.alpha;
        return emissive < o.emissive;
    }
};

uint32_t pack_rgb(rgba c) {
    auto b = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16);
}

} // namespace

ModelData VoxelWorld::mesh_chunk(ivec3 c) const {
    ModelData out;
    const Chunk* chunk = impl_->find(c.x, c.y, c.z);
    if (!chunk || chunk->non_air == 0) return out;
    const Padded pad = gather(*this, *chunk, c);
    const VoxelWorldImpl& w = *impl_;
    auto opaque = [&](BlockId id) { return id != 0 && w.type(id).alpha == AlphaMode::Opaque; };
    auto face_visible = [&](BlockId self, BlockId neighbor) {
        if (neighbor == 0) return true;
        if (w.type(neighbor).alpha == AlphaMode::Opaque) return false;
        return neighbor != self; // glass against glass, water against water: no inner walls
    };

    std::map<PartKey, MeshData> parts;
    const int atlas_cols = (w.atlas.valid() && w.tile_size > 0) ? std::max(1, w.atlas.width / w.tile_size) : 1;

    // For each of the 6 face directions, sweep slices along its axis and
    // greedily merge equal faces (same block, same corner shading) into
    // big rectangles. Textured faces aren't merged: a merged quad would
    // stretch one tile across it, and an atlas can't repeat a sub-rectangle.
    std::vector<uint64_t> mask(CS * CS);
    for (int axis = 0; axis < 3; ++axis) {
        const int ua = (axis + 1) % 3, va = (axis + 2) % 3;
        for (int sign = -1; sign <= 1; sign += 2) {
            int dir[3] = {0, 0, 0};
            dir[axis] = sign;
            const vec3 normal{static_cast<float>(dir[0]), static_cast<float>(dir[1]), static_cast<float>(dir[2])};
            for (int k = 0; k < CS; ++k) {
                std::fill(mask.begin(), mask.end(), 0);
                for (int j = 0; j < CS; ++j) {
                    for (int i = 0; i < CS; ++i) {
                        int p[3];
                        p[axis] = k; p[ua] = i; p[va] = j;
                        const BlockId self = pad.at(p[0], p[1], p[2]);
                        if (self == 0) continue;
                        const BlockId next = pad.at(p[0] + dir[0], p[1] + dir[1], p[2] + dir[2]);
                        if (!face_visible(self, next)) continue;
                        // Corner order around the face: (u0,v0) (u1,v0) (u1,v1) (u0,v1).
                        uint64_t ao_bits = 0xFF; // all four corners at level 3
                        if (ambient_occlusion) {
                            ao_bits = 0;
                            const int cu[4] = {-1, 1, 1, -1}, cv[4] = {-1, -1, 1, 1};
                            for (int corner = 0; corner < 4; ++corner) {
                                int s1[3] = {p[0] + dir[0], p[1] + dir[1], p[2] + dir[2]};
                                int s2[3] = {s1[0], s1[1], s1[2]};
                                int cc[3] = {s1[0], s1[1], s1[2]};
                                s1[ua] += cu[corner];
                                s2[va] += cv[corner];
                                cc[ua] += cu[corner];
                                cc[va] += cv[corner];
                                const int a = opaque(pad.at(s1[0], s1[1], s1[2]));
                                const int b = opaque(pad.at(s2[0], s2[1], s2[2]));
                                const int d = opaque(pad.at(cc[0], cc[1], cc[2]));
                                const int level = (a && b) ? 0 : 3 - (a + b + d);
                                ao_bits |= static_cast<uint64_t>(level) << (corner * 2);
                            }
                        }
                        const bool textured = w.type(self).textured();
                        mask[i + j * CS] = static_cast<uint64_t>(self) | (ao_bits << 16) | (textured ? (1ull << 24) : 0) | (1ull << 25);
                    }
                }
                for (int j = 0; j < CS; ++j) {
                    for (int i = 0; i < CS;) {
                        const uint64_t key = mask[i + j * CS];
                        if (key == 0) { ++i; continue; }
                        const bool no_merge = (key >> 24) & 1;
                        int wdt = 1;
                        while (!no_merge && i + wdt < CS && mask[i + wdt + j * CS] == key) ++wdt;
                        int hgt = 1;
                        while (!no_merge && j + hgt < CS) {
                            bool row = true;
                            for (int x = 0; x < wdt; ++x) {
                                if (mask[i + x + (j + hgt) * CS] != key) { row = false; break; }
                            }
                            if (!row) break;
                            ++hgt;
                        }
                        for (int y = 0; y < hgt; ++y)
                            for (int x = 0; x < wdt; ++x) mask[i + x + (j + y) * CS] = 0;

                        const BlockId id = static_cast<BlockId>(key & 0xFFFF);
                        const BlockType& bt = w.type(id);
                        const PartKey pk{bt.textured(), bt.alpha, pack_rgb(bt.emissive)};
                        MeshData& mesh = parts[pk];

                        const float plane = static_cast<float>(k + (sign > 0 ? 1 : 0));
                        const float u0 = static_cast<float>(i), u1 = static_cast<float>(i + wdt);
                        const float v0 = static_cast<float>(j), v1 = static_cast<float>(j + hgt);
                        auto corner_pos = [&](float u, float v) {
                            float q[3];
                            q[axis] = plane; q[ua] = u; q[va] = v;
                            return vec3{q[0], q[1], q[2]};
                        };
                        const vec3 corners[4] = {corner_pos(u0, v0), corner_pos(u1, v0), corner_pos(u1, v1), corner_pos(u0, v1)};
                        int ao[4];
                        for (int n = 0; n < 4; ++n) ao[n] = static_cast<int>((key >> (16 + n * 2)) & 3);

                        // Texture: pick the tile for this face, map each corner to
                        // it so images stand upright on side faces.
                        vec2 uv[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
                        if (bt.textured() && w.atlas.valid()) {
                            int tile = axis == 1 ? (sign > 0 ? bt.tile_top : bt.tile_bottom) : bt.tile_side;
                            if (tile < 0) tile = bt.tile_side >= 0 ? bt.tile_side : (bt.tile_top >= 0 ? bt.tile_top : bt.tile_bottom);
                            const float tw = static_cast<float>(w.tile_size) / w.atlas.width;
                            const float th = static_cast<float>(w.tile_size) / w.atlas.height;
                            // Half a texel in from the tile's edge, so neighbouring tiles don't bleed in.
                            const float iu = 0.5f / w.atlas.width, iv = 0.5f / w.atlas.height;
                            const float tu0 = (tile % atlas_cols) * tw + iu, tu1 = (tile % atlas_cols + 1) * tw - iu;
                            const float tv0 = (tile / atlas_cols) * th + iv, tv1 = (tile / atlas_cols + 1) * th - iv;
                            const vec3 block_min{static_cast<float>(axis == 0 ? k : (ua == 0 ? i : j)),
                                                 static_cast<float>(axis == 1 ? k : (ua == 1 ? i : j)),
                                                 static_cast<float>(axis == 2 ? k : (ua == 2 ? i : j))};
                            // Horizontal texture axis as seen from outside the face.
                            const vec3 right = axis == 1 ? vec3{1, 0, 0} : cross(vec3{0, 1, 0}, normal);
                            for (int n = 0; n < 4; ++n) {
                                const vec3 local = corners[n] - block_min; // 0/1 per axis
                                const float r = dot(local, right);
                                const float s = r < 0.0f ? 1.0f + r : r;
                                const float t = axis == 1 ? (sign > 0 ? local.z : 1.0f - local.z) : 1.0f - local.y;
                                uv[n] = {tu0 + (tu1 - tu0) * s, tv0 + (tv1 - tv0) * t};
                            }
                        }

                        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
                        for (int n = 0; n < 4; ++n) {
                            const float light = kAoLight[ao[n]];
                            mesh.vertices.push_back({corners[n], normal, uv[n],
                                                     rgba{bt.color.r * light, bt.color.g * light, bt.color.b * light, bt.color.a}});
                        }
                        // Winding: (u, v) runs counter-clockwise when seen from the
                        // side cross(U, V) points to; flip for the other side.
                        vec3 uaxis{0, 0, 0}, vaxis{0, 0, 0};
                        (ua == 0 ? uaxis.x : ua == 1 ? uaxis.y : uaxis.z) = 1.0f;
                        (va == 0 ? vaxis.x : va == 1 ? vaxis.y : vaxis.z) = 1.0f;
                        const bool ccw = dot(cross(uaxis, vaxis), normal) > 0.0f;
                        // Split the quad along the diagonal whose corners are more
                        // alike, or the shading shows a visible crease.
                        const bool flip = ao[0] + ao[2] < ao[1] + ao[3];
                        uint32_t q[4] = {base, base + 1, base + 2, base + 3};
                        if (flip) { const uint32_t t0 = q[0]; q[0] = q[1]; q[1] = q[2]; q[2] = q[3]; q[3] = t0; }
                        if (ccw) mesh.indices.insert(mesh.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
                        else mesh.indices.insert(mesh.indices.end(), {q[0], q[2], q[1], q[0], q[3], q[2]});
                        i += wdt;
                    }
                }
            }
        }
    }

    for (auto& [key, mesh] : parts) {
        ModelData::Part part;
        part.name = key.textured ? "textured" : "flat";
        part.mesh = std::move(mesh);
        part.material.color = white;
        part.material.specular = 0.05f;
        part.material.alpha = key.alpha;
        part.material.double_sided = key.alpha == AlphaMode::Blend; // water from below, glass from inside
        part.material.emissive = rgba{(key.emissive & 0xFF) / 255.0f, ((key.emissive >> 8) & 0xFF) / 255.0f,
                                      ((key.emissive >> 16) & 0xFF) / 255.0f, 1.0f};
        if (key.textured) {
            part.material.texture = w.atlas;
            part.material.filter = w.atlas_filter;
        }
        out.parts.push_back(std::move(part));
    }
    return out;
}

void VoxelWorld::remesh_all() {
    std::vector<int64_t> empty;
    for (auto& [key, chunk] : impl_->chunks) {
        if (chunk.non_air == 0) {
            empty.push_back(key);
            continue;
        }
        if (!chunk.dirty) continue;
        detail::replace_model(chunk.model, mesh_chunk(key_chunk(key)));
        chunk.dirty = false;
    }
    for (int64_t key : empty) {
        unload_model(impl_->chunks[key].model);
        impl_->chunks.erase(key);
    }
}

void World::draw(VoxelWorld& voxels) {
    voxels.remesh_all();
    const float s = voxels.voxel_size;
    for (const auto& [key, chunk] : voxels.impl_->chunks) {
        if (!chunk.model.valid()) continue;
        const ivec3 c = key_chunk(key);
        const vec3 at = voxels.origin + vec3{static_cast<float>(c.x * CS), static_cast<float>(c.y * CS), static_cast<float>(c.z * CS)} * s;
        draw(chunk.model, Transform{at, {}, {s, s, s}});
    }
}

} // namespace thistle::three
