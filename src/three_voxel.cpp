#include "thistle_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

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
    bool modified = false; // changed after generation: streaming must not drop it
    uint64_t revision = 0; // VoxelWorldImpl::revision at its last change (physics colliders follow it)
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

    VoxelWorld::Generator generator;
    std::unordered_set<int64_t> generated; // chunks the generator has filled (even if it left them empty)
    bool generating = false;
    ivec3 focus{0, 0, 0}; // block the last stream_around() centered on; re-meshing goes nearest-first
    bool has_focus = false;
    uint64_t revision = 0; // bumped by every change to any chunk
    Bounds grid_box;       // grid_bounds() in block units, as of grid_box_revision
    uint64_t grid_box_revision = ~uint64_t{0};

    // Consecutive set()/get() calls almost always hit the same chunk (a
    // generator filling one, a fill() box), so remember the last one.
    // unordered_map nodes don't move on rehash; only erase invalidates this.
    int64_t last_key = 0;
    Chunk* last = nullptr;

    Chunk* find(int cx, int cy, int cz) {
        const int64_t key = chunk_key(cx, cy, cz);
        if (last && key == last_key) return last;
        auto it = chunks.find(key);
        if (it == chunks.end()) return nullptr;
        last_key = key;
        last = &it->second;
        return last;
    }
    void erase(int64_t key) {
        if (last && key == last_key) last = nullptr;
        chunks.erase(key);
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
        rotation = other.rotation;
        ambient_occlusion = other.ambient_occlusion;
        max_remesh_per_frame = other.max_remesh_per_frame;
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

void VoxelWorld::set_block_type(BlockId id, const BlockType& type) {
    if (id == 0 || id >= impl_->types.size()) return;
    impl_->types[id] = type;
    for (auto& [key, chunk] : impl_->chunks) chunk.dirty = true;
}

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
    c->revision = ++impl_->revision;
    if (!impl_->generating) c->modified = true;
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
    ++impl_->revision;
    impl_->last = nullptr;
    impl_->generated.clear();
}

ivec3 VoxelWorld::to_block(vec3 p) const {
    const vec3 b = (rotation.inverse() * (p - origin)) / voxel_size;
    return {static_cast<int>(std::floor(b.x)), static_cast<int>(std::floor(b.y)), static_cast<int>(std::floor(b.z))};
}

vec3 VoxelWorld::block_center(ivec3 b) const {
    return origin + rotation * (vec3{b.x + 0.5f, b.y + 0.5f, b.z + 0.5f} * voxel_size);
}

Bounds VoxelWorld::block_bounds(ivec3 b) const {
    const vec3 lo = vec3{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)} * voxel_size;
    const Bounds local{lo, lo + vec3{voxel_size, voxel_size, voxel_size}};
    return local.transformed(Transform{origin, rotation}.matrix());
}

Bounds VoxelWorld::grid_bounds() const {
    VoxelWorldImpl& w = *impl_;
    if (w.grid_box_revision != w.revision) {
        Bounds box;
        for (const auto& [key, chunk] : w.chunks) {
            if (chunk.non_air == 0) continue;
            const ivec3 c = key_chunk(key);
            for (int i = 0; i < CS3; ++i) {
                if (chunk.blocks[i] == 0) continue;
                const ivec3 b{c.x * CS + i % CS, c.y * CS + (i / CS) % CS, c.z * CS + i / (CS * CS)};
                box.add(vec3{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)});
                box.add(vec3{b.x + 1.0f, b.y + 1.0f, b.z + 1.0f});
            }
        }
        w.grid_box = box;
        w.grid_box_revision = w.revision;
    }
    if (!w.grid_box.valid()) return {};
    return {w.grid_box.min * voxel_size, w.grid_box.max * voxel_size};
}

Bounds VoxelWorld::bounds() const {
    const Bounds local = grid_bounds();
    return local.valid() ? local.transformed(Transform{origin, rotation}.matrix()) : Bounds{};
}

int VoxelWorld::chunk_count() const { return static_cast<int>(impl_->chunks.size()); }

int VoxelWorld::block_count() const {
    int n = 0;
    for (const auto& [key, chunk] : impl_->chunks) n += chunk.non_air;
    return n;
}

void VoxelWorld::each_block(const std::function<void(ivec3, BlockId)>& fn) const {
    for (const auto& [key, chunk] : impl_->chunks) {
        if (chunk.non_air == 0) continue;
        const ivec3 base = key_chunk(key) * CS;
        for (int i = 0; i < CS3; ++i) {
            if (chunk.blocks[i] != 0) fn(base + ivec3{i % CS, (i / CS) % CS, i / (CS * CS)}, chunk.blocks[i]);
        }
    }
}

void VoxelWorld::copy_block_types(const VoxelWorld& from) {
    if (&from == this) return;
    impl_->types = from.impl_->types;
    impl_->atlas = from.impl_->atlas;
    impl_->tile_size = from.impl_->tile_size;
    impl_->atlas_filter = from.impl_->atlas_filter;
    for (auto& [key, chunk] : impl_->chunks) chunk.dirty = true;
}

VoxelWorld VoxelWorld::copy() const {
    VoxelWorld out;
    const std::vector<uint8_t> bytes = serialize();
    out.deserialize(bytes.data(), bytes.size());
    out.copy_block_types(*this); // the atlas (serialize() has the types, not the texture)
    out.voxel_size = voxel_size;
    out.origin = origin;
    out.rotation = rotation;
    out.ambient_occlusion = ambient_occlusion;
    out.max_remesh_per_frame = max_remesh_per_frame;
    return out;
}

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

namespace {
void remesh(VoxelWorld& world, VoxelWorldImpl& impl, int budget) {
    std::vector<int64_t> empty, dirty;
    for (auto& [key, chunk] : impl.chunks) {
        if (chunk.non_air == 0 && !chunk.modified) empty.push_back(key);
        else if (chunk.dirty) dirty.push_back(key);
    }
    if (budget > 0 && static_cast<int>(dirty.size()) > budget) {
        const ivec3 f{impl.focus.x >> kChunkShift, impl.focus.y >> kChunkShift, impl.focus.z >> kChunkShift};
        auto d2 = [&](int64_t k) { const ivec3 c = key_chunk(k) - f; return c.x * c.x + c.y * c.y + c.z * c.z; };
        std::partial_sort(dirty.begin(), dirty.begin() + budget, dirty.end(), [&](int64_t a, int64_t b) { return d2(a) < d2(b); });
        dirty.resize(budget);
    }
    for (int64_t key : dirty) {
        Chunk& chunk = impl.chunks[key];
        detail::replace_model(chunk.model, world.mesh_chunk(key_chunk(key)));
        chunk.dirty = false;
    }
    for (int64_t key : empty) {
        unload_model(impl.chunks[key].model);
        impl.erase(key);
    }
}
} // namespace

void VoxelWorld::remesh_all() { remesh(*this, *impl_, 0); }

void VoxelWorld::set_generator(Generator generator) { impl_->generator = std::move(generator); }

void VoxelWorld::stream_around(vec3 world_position, float radius, int budget) {
    VoxelWorldImpl& w = *impl_;
    w.focus = to_block(world_position);
    w.has_focus = true;
    const ivec3 center{w.focus.x >> kChunkShift, w.focus.y >> kChunkShift, w.focus.z >> kChunkShift};
    const int r = std::max(1, static_cast<int>(std::ceil(radius / (voxel_size * CS))));
    if (w.generator) {
        std::vector<std::pair<int, ivec3>> wanted;
        for (int z = -r; z <= r; ++z)
            for (int y = -r; y <= r; ++y)
                for (int x = -r; x <= r; ++x) {
                    const int d2 = x * x + y * y + z * z;
                    if (d2 > r * r) continue;
                    const ivec3 c = center + ivec3{x, y, z};
                    if (!w.generated.count(chunk_key(c.x, c.y, c.z))) wanted.push_back({d2, c});
                }
        std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (int i = 0; i < static_cast<int>(wanted.size()) && i < budget; ++i) {
            const ivec3 c = wanted[i].second;
            w.generating = true;
            w.generator(*this, c);
            w.generating = false;
            w.generated.insert(chunk_key(c.x, c.y, c.z));
        }
    }
    // Drop what's well outside the radius (the margin stops chunks at the
    // edge flickering in and out as you step back and forth), unless the
    // player changed it. The neighbors left behind are re-meshed, since
    // their faces toward the dropped chunk are now open to the air.
    const int drop = r + 2;
    std::vector<int64_t> gone;
    for (const int64_t key : w.generated) {
        const ivec3 d = key_chunk(key) - center;
        if (d.x * d.x + d.y * d.y + d.z * d.z <= drop * drop) continue;
        auto it = w.chunks.find(key);
        if (it != w.chunks.end() && it->second.modified) continue;
        gone.push_back(key);
    }
    for (const int64_t key : gone) {
        w.generated.erase(key);
        auto it = w.chunks.find(key);
        if (it == w.chunks.end()) continue;
        unload_model(it->second.model);
        w.erase(key);
        ++w.revision;
        const ivec3 c = key_chunk(key);
        for (const ivec3 n : {ivec3{1, 0, 0}, ivec3{-1, 0, 0}, ivec3{0, 1, 0}, ivec3{0, -1, 0}, ivec3{0, 0, 1}, ivec3{0, 0, -1}}) {
            if (Chunk* nc = w.find(c.x + n.x, c.y + n.y, c.z + n.z)) nc->dirty = true;
        }
    }
}

void World::draw(VoxelWorld& voxels) {
    remesh(voxels, *voxels.impl_, voxels.max_remesh_per_frame);
    const float s = voxels.voxel_size;
    for (const auto& [key, chunk] : voxels.impl_->chunks) {
        if (!chunk.model.valid()) continue;
        const ivec3 c = key_chunk(key);
        const vec3 offset = vec3{static_cast<float>(c.x * CS), static_cast<float>(c.y * CS), static_cast<float>(c.z * CS)} * s;
        draw(chunk.model, Transform{voxels.origin + voxels.rotation * offset, voxels.rotation, {s, s, s}});
    }
}

} // namespace thistle::three

// --- save / load ----------------------------------------------------------------------

namespace thistle::three {

namespace {

constexpr uint32_t kVoxelMagic = 0x31585654; // "TVX1" little-endian
constexpr uint32_t kVoxelVersion = 1;

struct Writer {
    std::vector<uint8_t> out;
    void bytes(const void* p, size_t n) { const auto* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); }
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void color(rgba c) { f32(c.r); f32(c.g); f32(c.b); f32(c.a); }
    void str(const std::string& s) { u16(static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFF))); bytes(s.data(), std::min<size_t>(s.size(), 0xFFFF)); }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    bool need(size_t n) { if (static_cast<size_t>(end - p) < n) ok = false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return *p++; }
    uint16_t u16() { const uint16_t a = u8(); return static_cast<uint16_t>(a | (u8() << 8)); }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i); return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() { const uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
    rgba color() { rgba c; c.r = f32(); c.g = f32(); c.b = f32(); c.a = f32(); return c; }
    std::string str() { const uint16_t n = u16(); if (!need(n)) return {}; std::string s(reinterpret_cast<const char*>(p), n); p += n; return s; }
};

} // namespace

std::vector<uint8_t> VoxelWorld::serialize() const {
    Writer w;
    w.u32(kVoxelMagic);
    w.u32(kVoxelVersion);
    w.f32(voxel_size);
    w.f32(origin.x); w.f32(origin.y); w.f32(origin.z);
    w.f32(rotation.x); w.f32(rotation.y); w.f32(rotation.z); w.f32(rotation.w);
    w.u32(static_cast<uint32_t>(impl_->types.size() - 1));
    for (size_t i = 1; i < impl_->types.size(); ++i) {
        const BlockType& t = impl_->types[i];
        w.str(t.name);
        w.color(t.color);
        w.i32(t.tile_top); w.i32(t.tile_side); w.i32(t.tile_bottom);
        w.u8(static_cast<uint8_t>(t.alpha));
        w.color(t.emissive);
        w.u8(t.solid ? 1 : 0);
    }
    uint32_t chunks = 0;
    for (const auto& [key, chunk] : impl_->chunks) chunks += chunk.non_air > 0;
    w.u32(chunks);
    for (const auto& [key, chunk] : impl_->chunks) {
        if (chunk.non_air == 0) continue;
        const ivec3 c = key_chunk(key);
        w.i32(c.x); w.i32(c.y); w.i32(c.z);
        // Runs of equal ids: a mostly-solid or mostly-empty chunk (which is
        // nearly every chunk) comes down to a handful of runs.
        std::vector<std::pair<BlockId, uint16_t>> runs;
        for (int i = 0; i < CS3; ++i) {
            const BlockId id = chunk.blocks[i];
            if (!runs.empty() && runs.back().first == id && runs.back().second < 0xFFFF) ++runs.back().second;
            else runs.push_back({id, 1});
        }
        w.u32(static_cast<uint32_t>(runs.size()));
        for (const auto& [id, n] : runs) { w.u16(id); w.u16(n); }
    }
    return std::move(w.out);
}

bool VoxelWorld::deserialize(const uint8_t* data, size_t size) {
    clear();
    impl_->types.resize(1);
    Reader r{data, data + size};
    if (r.u32() != kVoxelMagic || r.u32() != kVoxelVersion) return false;
    const float vs = r.f32();
    const vec3 org{r.f32(), r.f32(), r.f32()};
    quat rot;
    rot.x = r.f32(); rot.y = r.f32(); rot.z = r.f32(); rot.w = r.f32();
    const uint32_t type_count = r.u32();
    if (!r.ok || type_count > 0xFFFE) return false;
    for (uint32_t i = 0; i < type_count && r.ok; ++i) {
        BlockType t;
        t.name = r.str();
        t.color = r.color();
        t.tile_top = r.i32(); t.tile_side = r.i32(); t.tile_bottom = r.i32();
        const uint8_t alpha = r.u8();
        t.alpha = alpha <= static_cast<uint8_t>(AlphaMode::Blend) ? static_cast<AlphaMode>(alpha) : AlphaMode::Opaque;
        t.emissive = r.color();
        t.solid = r.u8() != 0;
        impl_->types.push_back(t);
    }
    const uint32_t chunk_count = r.u32();
    for (uint32_t c = 0; c < chunk_count && r.ok; ++c) {
        const int cx = r.i32(), cy = r.i32(), cz = r.i32();
        const uint32_t runs = r.u32();
        if (!r.ok || runs > static_cast<uint32_t>(CS3)) { r.ok = false; break; }
        Chunk& chunk = impl_->chunks[chunk_key(cx, cy, cz)];
        chunk.revision = ++impl_->revision;
        // Loaded chunks count as generated *and* player-made: a generator
        // must never overwrite them, and streaming must never drop them.
        chunk.modified = true;
        impl_->generated.insert(chunk_key(cx, cy, cz));
        int at = 0;
        for (uint32_t i = 0; i < runs && r.ok; ++i) {
            const BlockId id = r.u16();
            const uint16_t n = r.u16();
            if (at + n > CS3 || id > type_count) { r.ok = false; break; }
            std::fill_n(chunk.blocks.begin() + at, n, id);
            if (id != 0) chunk.non_air += n;
            at += n;
        }
        if (at != CS3) r.ok = false;
    }
    if (!r.ok) {
        clear();
        impl_->types.resize(1);
        return false;
    }
    voxel_size = vs > 0.0f ? vs : 1.0f;
    origin = org;
    rotation = normalize(rot);
    return true;
}

std::vector<uint8_t> VoxelWorld::serialize_chunk(ivec3 c) const {
    Writer w;
    const Chunk* chunk = impl_->find(c.x, c.y, c.z);
    if (!chunk || chunk->non_air == 0) return {};
    std::vector<std::pair<BlockId, uint16_t>> runs;
    for (int i = 0; i < CS3; ++i) {
        const BlockId id = chunk->blocks[i];
        if (!runs.empty() && runs.back().first == id && runs.back().second < 0xFFFF) ++runs.back().second;
        else runs.push_back({id, 1});
    }
    w.u32(static_cast<uint32_t>(runs.size()));
    for (const auto& [id, n] : runs) { w.u16(id); w.u16(n); }
    return std::move(w.out);
}

bool VoxelWorld::deserialize_chunk(ivec3 c, const uint8_t* data, size_t size) {
    std::vector<BlockId> blocks(CS3, 0);
    if (size > 0) {
        Reader r{data, data + size};
        const uint32_t runs = r.u32();
        if (!r.ok || runs > static_cast<uint32_t>(CS3)) return false;
        int at = 0;
        for (uint32_t i = 0; i < runs && r.ok; ++i) {
            const BlockId id = r.u16();
            const uint16_t n = r.u16();
            if (!r.ok || at + n > CS3) return false;
            std::fill_n(blocks.begin() + at, n, id);
            at += n;
        }
        if (!r.ok || at != CS3) return false;
    }
    // Through set(), which keeps counts, dirty flags (this chunk's and its
    // neighbors' faces) and revisions right. Only changed blocks cost anything.
    const ivec3 base = c * CS;
    for (int i = 0; i < CS3; ++i) {
        const ivec3 p = base + ivec3{i % CS, (i / CS) % CS, i / (CS * CS)};
        if (get(p) != blocks[static_cast<size_t>(i)]) set(p, blocks[static_cast<size_t>(i)]);
    }
    return true;
}

std::vector<ivec3> VoxelWorld::chunks() const {
    std::vector<ivec3> out;
    for (const auto& [key, chunk] : impl_->chunks) {
        if (chunk.non_air > 0) out.push_back(key_chunk(key));
    }
    return out;
}

bool VoxelWorld::save(const std::string& path) const {
    const std::vector<uint8_t> bytes = serialize();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        log_warn("VoxelWorld::save: can't write " + path);
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

bool VoxelWorld::load(const std::string& path) {
    std::vector<unsigned char> bytes;
    if (!detail::read_file_bytes(path, bytes)) {
        log_warn("VoxelWorld::load: can't read " + path);
        return false;
    }
    if (!deserialize(bytes.data(), bytes.size())) {
        log_warn("VoxelWorld::load: " + path + " isn't a valid voxel world file");
        return false;
    }
    return true;
}

// --- MagicaVoxel .vox --------------------------------------------------------------------

namespace {

struct VoxModel {
    int sx = 0, sy = 0, sz = 0;
    std::vector<std::array<uint8_t, 4>> voxels; // x, y, z, palette index
};

struct VoxNode {
    enum Kind { Transform, Group, Shape } kind = Transform;
    int child = -1;                 // Transform
    std::vector<int> children;      // Group
    std::vector<int> models;        // Shape
    int t[3] = {0, 0, 0};           // Transform translation (MagicaVoxel space)
    int r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

using VoxDict = std::vector<std::pair<std::string, std::string>>;

VoxDict read_dict(Reader& rd) {
    VoxDict d;
    const int32_t n = rd.i32();
    for (int32_t i = 0; i < n && rd.ok; ++i) {
        auto read_str = [&] {
            const int32_t len = rd.i32();
            if (len < 0 || !rd.need(static_cast<size_t>(len))) { rd.ok = false; return std::string(); }
            std::string s(reinterpret_cast<const char*>(rd.p), static_cast<size_t>(len));
            rd.p += len;
            return s;
        };
        std::string k = read_str();
        std::string v = read_str();
        d.push_back({std::move(k), std::move(v)});
    }
    return d;
}

std::string dict_get(const VoxDict& d, const std::string& key) {
    for (const auto& [k, v] : d) if (k == key) return v;
    return {};
}

// MagicaVoxel packs a rotation (a signed permutation matrix) into one byte:
// bits 0-1 = which column row 0's nonzero is in, bits 2-3 = row 1's, row 2
// takes the remaining column; bits 4/5/6 = the sign of each row.
void decode_rotation(uint8_t bits, int out[3][3]) {
    const int c0 = bits & 3, c1 = (bits >> 2) & 3;
    const int c2 = 3 - c0 - c1;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) out[i][j] = 0;
    if (c0 > 2 || c1 > 2 || c2 < 0 || c2 > 2) { out[0][0] = out[1][1] = out[2][2] = 1; return; }
    out[0][c0] = (bits >> 4) & 1 ? -1 : 1;
    out[1][c1] = (bits >> 5) & 1 ? -1 : 1;
    out[2][c2] = (bits >> 6) & 1 ? -1 : 1;
}

} // namespace

bool VoxelWorld::load_vox(const std::string& path, ivec3 at) {
    std::vector<unsigned char> bytes;
    if (!detail::read_file_bytes(path, bytes)) {
        log_warn("load_vox: can't read " + path);
        return false;
    }
    Reader rd{bytes.data(), bytes.data() + bytes.size()};
    auto tag = [&] { if (!rd.need(4)) return std::string(); std::string t(reinterpret_cast<const char*>(rd.p), 4); rd.p += 4; return t; };
    if (tag() != "VOX ") { log_warn("load_vox: " + path + " is not a MagicaVoxel file"); return false; }
    rd.i32(); // version
    if (tag() != "MAIN") { log_warn("load_vox: " + path + " has no MAIN chunk"); return false; }
    rd.i32(); rd.i32(); // MAIN content / children sizes

    std::vector<VoxModel> models;
    std::array<rgba, 256> palette;
    bool has_palette = false;
    struct Mat { bool glass = false; float alpha = 1.0f; float emit = 0.0f; };
    std::array<Mat, 256> mats{};
    std::unordered_map<int, VoxNode> nodes;

    while (rd.ok && rd.p + 12 <= rd.end) {
        const std::string id = tag();
        const int32_t content = rd.i32();
        rd.i32(); // children size (children follow inline, handled by the loop)
        if (content < 0 || !rd.need(static_cast<size_t>(content))) break;
        Reader c{rd.p, rd.p + content};
        rd.p += content;
        if (id == "SIZE") {
            VoxModel m;
            m.sx = c.i32(); m.sy = c.i32(); m.sz = c.i32();
            models.push_back(m);
        } else if (id == "XYZI" && !models.empty()) {
            const int32_t n = c.i32();
            for (int32_t i = 0; i < n && c.need(4); ++i) {
                models.back().voxels.push_back({c.p[0], c.p[1], c.p[2], c.p[3]});
                c.p += 4;
            }
        } else if (id == "RGBA") {
            has_palette = true;
            // Palette slot i (1..255) is stored at entry i - 1.
            for (int i = 0; i < 256 && c.need(4); ++i) {
                palette[(i + 1) & 0xFF] = rgba{c.p[0] / 255.0f, c.p[1] / 255.0f, c.p[2] / 255.0f, c.p[3] / 255.0f};
                c.p += 4;
            }
        } else if (id == "MATL") {
            const int32_t mid = c.i32();
            const VoxDict d = read_dict(c);
            if (mid > 0 && mid < 256) {
                Mat& m = mats[mid];
                const std::string type = dict_get(d, "_type");
                if (type == "_glass" || type == "_blend") {
                    m.glass = true;
                    const std::string trans = dict_get(d, "_trans");
                    m.alpha = trans.empty() ? 0.5f : std::clamp(1.0f - static_cast<float>(std::atof(trans.c_str())), 0.05f, 1.0f);
                } else if (type == "_emit") {
                    const std::string e = dict_get(d, "_emit");
                    m.emit = e.empty() ? 1.0f : static_cast<float>(std::atof(e.c_str()));
                }
            }
        } else if (id == "nTRN") {
            const int32_t nid = c.i32();
            read_dict(c);
            VoxNode n;
            n.kind = VoxNode::Transform;
            n.child = c.i32();
            c.i32(); c.i32(); // reserved, layer
            const int32_t frames = c.i32();
            for (int32_t f = 0; f < frames && c.ok; ++f) {
                const VoxDict fd = read_dict(c);
                if (f != 0) continue;
                const std::string t = dict_get(fd, "_t");
                if (!t.empty()) std::sscanf(t.c_str(), "%d %d %d", &n.t[0], &n.t[1], &n.t[2]);
                const std::string r = dict_get(fd, "_r");
                if (!r.empty()) decode_rotation(static_cast<uint8_t>(std::atoi(r.c_str())), n.r);
            }
            nodes[nid] = n;
        } else if (id == "nGRP") {
            const int32_t nid = c.i32();
            read_dict(c);
            VoxNode n;
            n.kind = VoxNode::Group;
            const int32_t k = c.i32();
            for (int32_t i = 0; i < k && c.ok; ++i) n.children.push_back(c.i32());
            nodes[nid] = n;
        } else if (id == "nSHP") {
            const int32_t nid = c.i32();
            read_dict(c);
            VoxNode n;
            n.kind = VoxNode::Shape;
            const int32_t k = c.i32();
            for (int32_t i = 0; i < k && c.ok; ++i) { n.models.push_back(c.i32()); read_dict(c); }
            nodes[nid] = n;
        }
    }
    if (models.empty()) { log_warn("load_vox: no models in " + path); return false; }
    if (!has_palette) {
        // Files without an RGBA chunk use MagicaVoxel's built-in palette,
        // which isn't reproduced here; a gray ramp at least keeps the shape.
        log_warn("load_vox: " + path + " has no palette; using grays");
        for (int i = 0; i < 256; ++i) palette[i] = rgba{i / 255.0f, i / 255.0f, i / 255.0f, 1.0f};
    }

    // Place every model instance in MagicaVoxel space (walking the scene
    // graph when there is one), then turn Z-up into Y-up: (x, y, z) -> (x, z, -y).
    std::vector<std::array<int, 4>> placed; // x, y, z (ours), palette index
    auto place_model = [&](const VoxModel& m, const int t[3], const int r[3][3]) {
        const int pivot[3] = {m.sx / 2, m.sy / 2, m.sz / 2};
        for (const auto& v : m.voxels) {
            const int local[3] = {v[0] - pivot[0], v[1] - pivot[1], v[2] - pivot[2]};
            int w[3];
            for (int i = 0; i < 3; ++i) w[i] = t[i] + r[i][0] * local[0] + r[i][1] * local[1] + r[i][2] * local[2];
            placed.push_back({w[0], w[2], -w[1], v[3]});
        }
    };
    if (nodes.count(0)) {
        std::function<void(int, const int*, const int (*)[3], int)> walk = [&](int nid, const int* t, const int (*r)[3], int depth) {
            auto it = nodes.find(nid);
            if (it == nodes.end() || depth > 64) return;
            const VoxNode& n = it->second;
            if (n.kind == VoxNode::Transform) {
                int nt[3], nr[3][3];
                for (int i = 0; i < 3; ++i) {
                    nt[i] = t[i] + r[i][0] * n.t[0] + r[i][1] * n.t[1] + r[i][2] * n.t[2];
                    for (int j = 0; j < 3; ++j) nr[i][j] = r[i][0] * n.r[0][j] + r[i][1] * n.r[1][j] + r[i][2] * n.r[2][j];
                }
                walk(n.child, nt, nr, depth + 1);
            } else if (n.kind == VoxNode::Group) {
                for (int ch : n.children) walk(ch, t, r, depth + 1);
            } else {
                for (int mid : n.models) if (mid >= 0 && mid < static_cast<int>(models.size())) place_model(models[mid], t, r);
            }
        };
        const int t0[3] = {0, 0, 0};
        const int r0[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        walk(0, t0, r0, 0);
    } else {
        const int t0[3] = {models[0].sx / 2, models[0].sy / 2, models[0].sz / 2};
        const int r0[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        place_model(models[0], t0, r0);
    }
    if (placed.empty()) { log_warn("load_vox: " + path + " contains no voxels"); return true; }

    ivec3 lo{placed[0][0], placed[0][1], placed[0][2]};
    for (const auto& p : placed) lo = {std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2])};

    // One block type per palette slot actually used, found by name so
    // several imports share types instead of piling up duplicates.
    std::array<BlockId, 256> ids{};
    for (const auto& p : placed) {
        const int slot = p[3];
        if (ids[slot] != 0) continue;
        const rgba c = palette[slot];
        const Mat& m = mats[slot];
        char name[48];
        std::snprintf(name, sizeof(name), "vox:%02x%02x%02x%s%s", static_cast<int>(c.r * 255 + 0.5f), static_cast<int>(c.g * 255 + 0.5f),
                      static_cast<int>(c.b * 255 + 0.5f), m.glass ? ":glass" : "", m.emit > 0.0f ? ":emit" : "");
        BlockId id = find_block(name);
        if (id == 0) {
            BlockType t;
            t.name = name;
            t.color = rgba{c.r, c.g, c.b, m.glass ? m.alpha : 1.0f};
            if (m.glass) t.alpha = AlphaMode::Blend;
            if (m.emit > 0.0f) t.emissive = rgba{c.r * std::min(m.emit, 1.0f), c.g * std::min(m.emit, 1.0f), c.b * std::min(m.emit, 1.0f), 1.0f};
            id = add_block(t);
        }
        ids[slot] = id;
    }
    for (const auto& p : placed) set(at.x + p[0] - lo.x, at.y + p[1] - lo.y, at.z + p[2] - lo.z, ids[p[3]]);
    return true;
}

} // namespace thistle::three

namespace thistle::detail {

uint64_t VoxelWorldAccess::revision(const three::VoxelWorld& world) { return world.impl_->revision; }

void VoxelWorldAccess::chunks(const three::VoxelWorld& world, std::vector<std::pair<three::ivec3, uint64_t>>& out) {
    out.clear();
    for (const auto& [key, chunk] : world.impl_->chunks) {
        if (chunk.non_air > 0) out.push_back({three::key_chunk(key), chunk.revision});
    }
}

} // namespace thistle::detail
