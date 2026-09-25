// Checks three::Scene3D (the Thistle Editor's level format) and to_euler():
// world transforms through a rotated, scaled parent (hand-computed),
// set_world_transform/set_parent keeping things where they are, refusing
// to parent something under itself, remove() taking children along and
// renumbering the rest, properties, a JSON round trip (and repairing a file
// with bad parents), inside() on a turned, scaled trigger, block objects
// (voxels: saved in the file, placed through their parents, copy(),
// set_block_type()), and draw() running. Headless.
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
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near3(vec3 a, vec3 b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
bool same_rotation(quat a, quat b) { return std::fabs(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1.0f) < 1e-4f; }
std::string str(vec3 v) { char b[64]; std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z); return b; }
} // namespace

int main() {
    App app{{.title = "scene3d smoketest", .width = 64, .height = 64}};

    std::printf("to_euler\n");
    {
        bool all = true;
        for (float p : {-1.2f, -0.3f, 0.0f, 0.7f, 1.4f}) {
            for (float y : {-2.5f, -0.4f, 0.0f, 1.1f, 3.0f}) {
                for (float r : {-1.0f, 0.0f, 0.6f}) {
                    const quat q = quat::euler(p, y, r);
                    const vec3 e = to_euler(q);
                    all = all && same_rotation(quat::euler(e.x, e.y, e.z), q);
                }
            }
        }
        check(all, "quat::euler(to_euler(q)) gives q back (75 combinations)");
        const vec3 e = to_euler(quat::euler(0.5f, 1.0f, -0.25f));
        check(near3(e, {0.5f, 1.0f, -0.25f}), "and the same angles when they're in range");
        const quat up = quat::euler(radians(90.0f), 0.8f, 0.3f);
        const vec3 g = to_euler(up);
        check(same_rotation(quat::euler(g.x, g.y, g.z), up), "looking straight up still round-trips");
    }

    std::printf("transforms\n");
    Scene3D s;
    SceneEntity group;
    group.name = "group";
    group.transform = Transform{{10, 0, 0}, quat::euler(0, radians(90.0f)), {2, 2, 2}};
    const int g = s.add(group);
    SceneEntity crate;
    crate.name = "crate";
    crate.kind = SceneEntity::Kind::Box;
    crate.parent = g;
    crate.transform.position = {1, 0, 0};
    crate.color = rgb(0.6f, 0.4f, 0.2f);
    crate.set_property("loot", "gold");
    crate.set_property("hp", "20");
    crate.set_property("loot", "silver");
    const int c = s.add(crate);
    // Yaw +90 turns +X to -Z; scale 2 doubles the offset: (10,0,0) + (0,0,-2).
    const Transform w = s.world_transform(c);
    check(near3(w.position, {10, 0, -2}) && near3(w.scale, {2, 2, 2}) && same_rotation(w.rotation, group.transform.rotation),
          "a child's world transform goes through its parent's position, rotation and scale " + str(w.position));
    s.set_world_transform(c, Transform{{3, 4, 5}, quat::euler(0.3f, 0.2f, 0.1f), {1, 1, 1}});
    const Transform w2 = s.world_transform(c);
    check(near3(w2.position, {3, 4, 5}) && same_rotation(w2.rotation, quat::euler(0.3f, 0.2f, 0.1f)) && near3(w2.scale, {1, 1, 1}),
          "set_world_transform lands exactly where asked, under a parent");
    check(near3(s.entities[1].transform.scale, {0.5f, 0.5f, 0.5f}), "(its local scale compensates for the parent's)");

    check(s.set_parent(c, -1) && s.entities[1].parent == -1 && near3(s.world_transform(c).position, {3, 4, 5}),
          "set_parent(-1) takes it to the top without moving it");
    check(s.set_parent(c, g) && s.entities[1].parent == g && near3(s.world_transform(c).position, {3, 4, 5}),
          "and back under the group, still not moving");
    check(!s.set_parent(g, c) && s.entities[0].parent == -1, "putting the group under its own child is refused");
    check(s.children(g) == std::vector<int>{c} && s.children(-1) == std::vector<int>{g}, "children()");
    check(s.find("crate") == c && s.find("nope") == -1, "find()");
    check(s.entities[1].property("loot") == "silver" && s.entities[1].property("hp") == "20" && s.entities[1].properties.size() == 2 &&
              s.entities[1].property("missing", "x") == "x",
          "properties: set, overwrite, read, fallback");

    SceneEntity lamp;
    lamp.name = "lamp";
    lamp.kind = SceneEntity::Kind::SpotLight;
    lamp.transform = Transform{{0, 5, 0}, quat::euler(radians(-90.0f), 0)};
    lamp.intensity = 2.5f;
    lamp.range = 12.0f;
    lamp.spot_angle = radians(20.0f);
    s.add(lamp);
    SceneEntity spawn;
    spawn.name = "player_start";
    spawn.kind = SceneEntity::Kind::Spawn;
    spawn.transform.position = {-4, 0, 7};
    s.add(spawn);
    SceneEntity ship;
    ship.name = "ship";
    ship.kind = SceneEntity::Kind::Model;
    ship.model = "assets/ship.glb";
    s.add(ship);
    s.fog.enabled = true;
    s.fog.end = 88.0f;
    s.sun.direction = {1, -2, 0.5f};
    s.sky.top = rgb(0.1f, 0.2f, 0.3f);
    s.ambient = 0.4f;

    std::printf("files\n");
    const std::string json = s.to_json();
    Scene3D back;
    check(back.from_json(json), "to_json -> from_json");
    bool same_entities = back.entities.size() == s.entities.size();
    for (size_t i = 0; same_entities && i < s.entities.size(); ++i) {
        const SceneEntity &a = s.entities[i], &b = back.entities[i];
        same_entities = a.name == b.name && a.kind == b.kind && a.parent == b.parent && near3(a.transform.position, b.transform.position) &&
                        same_rotation(a.transform.rotation, b.transform.rotation) && near3(a.transform.scale, b.transform.scale) &&
                        a.model == b.model && near(a.color.r, b.color.r) && a.properties == b.properties &&
                        (a.kind != SceneEntity::Kind::SpotLight || (near(a.intensity, b.intensity) && near(a.range, b.range) && near(a.spot_angle, b.spot_angle)));
    }
    check(same_entities, "every entity comes back the same (kinds, parents, transforms, model paths, colors, light settings, properties in order)");
    check(back.fog.enabled && near(back.fog.end, 88.0f) && near3(back.sun.direction, {1, -2, 0.5f}) && near(back.sky.top.b, 0.3f) &&
              near(back.ambient, 0.4f),
          "and so does the environment");
    check(back.to_json() == json, "saving what was loaded gives the identical file");
    const std::string path = "thistle_scene3d_smoketest.scene.json";
    Scene3D from_disk;
    check(s.save(path) && from_disk.load(path) && from_disk.to_json() == json, "save() / load() through a file");
    std::remove(path.c_str());
    Scene3D bad;
    check(!bad.from_json("{not json") && bad.entities.empty() && !bad.load("does/not/exist.scene.json"), "garbage and missing files: false, empty");
    check(bad.from_json(R"({"thistle_scene":1,"entities":[{"name":"a","parent":1},{"name":"b","parent":0},{"name":"c","parent":7}]})") &&
              bad.entities[0].parent == -1 && bad.entities[2].parent == -1,
          "a file with a parent loop or a missing parent is repaired, not trusted");

    std::printf("editing\n");
    const Bounds lb = s.local_bounds(c);
    check(near3(lb.min, {-0.5f, -0.5f, -0.5f}) && near3(s.local_bounds(s.find("player_start")).max, {0.25f, 1.8f, 0.25f}),
          "local_bounds: a unit box for shapes, a person-sized one for spawns");
    {
        // A trigger 4 x 2 x 1, turned 45 degrees, under a parent that doubles
        // everything: in the world it's 8 x 4 x 2, its long side along
        // (1, 0, -1) (yaw +45 turns +X toward -Z).
        Scene3D t;
        SceneEntity big;
        big.transform = Transform{{0, 0, 0}, quat{}, {2, 2, 2}};
        const int b = t.add(big);
        SceneEntity zone;
        zone.kind = SceneEntity::Kind::Trigger;
        zone.parent = b;
        zone.transform = Transform{{5, 1, 0}, quat::euler(0, radians(45.0f)), {4, 2, 1}};
        const int z = t.add(zone);
        const vec3 center{10, 2, 0};
        const vec3 along = normalize(vec3{1, 0, -1}), across = normalize(vec3{1, 0, 1});
        check(t.inside(z, center) && t.inside(z, center + along * 3.9f) && t.inside(z, center + across * 0.9f) &&
                  t.inside(z, center + vec3{0, 1.9f, 0}),
              "inside(): points just within a turned, scaled trigger are in");
        check(!t.inside(z, center + along * 4.1f) && !t.inside(z, center + across * 1.1f) && !t.inside(z, center + vec3{0, 2.1f, 0}) &&
                  !t.inside(z, center + vec3{4, 0, 0}),
              "and points just past each face are out (including one an unturned box would contain)");
        check(!t.inside(99, center) && !t.inside(-1, center), "a missing index is never inside");
    }
    std::printf("voxels\n");
    {
        Scene3D v;
        SceneEntity turned;
        turned.name = "turned";
        turned.transform = Transform{{5, 0, 0}, quat::euler(0, radians(90.0f))};
        const int t = v.add(turned);
        SceneEntity house;
        house.name = "house";
        house.kind = SceneEntity::Kind::Voxels;
        house.parent = t;
        house.transform.position = {1, 0, 0};
        house.voxels = std::make_shared<VoxelWorld>();
        VoxelWorld& w = *house.voxels;
        w.voxel_size = 0.25f;
        const BlockId stone = w.add_block({.name = "stone", .color = rgb(0.5f, 0.5f, 0.5f)});
        const BlockId glass = w.add_block({.name = "glass", .color = rgba{0.6f, 0.8f, 1.0f, 0.3f}, .alpha = AlphaMode::Blend});
        w.fill({0, 0, 0}, {3, 2, 1}, stone);
        w.set(1, 1, 0, glass);
        house.atlas = "assets/blocks.png";
        house.atlas_tile = 8;
        const int h = v.add(house);

        const Bounds gb = v.local_bounds(h);
        check(near3(gb.min, {0, 0, 0}) && near3(gb.max, {1.0f, 0.75f, 0.5f}), "local_bounds of blocks: the blocks' box times the block size");
        w.set(20, 0, 0, stone);
        check(near(v.local_bounds(h).max.x, 5.25f), "and it follows edits (grid_bounds is cached until one)");
        w.set(20, 0, 0, 0);

        v.place_voxels();
        const Transform hw = v.world_transform(h);
        check(near3(w.origin, hw.position) && near3(w.origin, {5, 0, -1}) && same_rotation(w.rotation, hw.rotation),
              "place_voxels(): the grid sits at the entity's world position and turns with it, through its parent");
        check(v.inside(h, w.block_center({1, 1, 1})) && !v.inside(h, w.block_center({1, 1, 3})), "inside() uses the blocks' box");

        const std::string json = v.to_json();
        Scene3D back;
        check(back.from_json(json) && back.entities[1].voxels != nullptr, "voxels come back from JSON");
        const SceneEntity& bh = back.entities[1];
        check(bh.voxels && bh.voxels->serialize() == w.serialize() && bh.voxels->get(1, 1, 0) == glass &&
                  bh.voxels->block_type(glass).alpha == AlphaMode::Blend && near(bh.voxels->voxel_size, 0.25f),
              "with every block, the block types and the block size (" + std::to_string(bh.voxels->block_count()) + " blocks)");
        check(bh.atlas == "assets/blocks.png" && bh.atlas_tile == 8, "and the atlas path and tile size");
        check(bh.voxels && near3(bh.voxels->origin, {5, 0, -1}), "load places them (no draw needed first)");
        check(back.to_json() == json, "saving what was loaded gives the identical file, blocks and all");
        const std::string light = v.to_json(false);
        check(light.find("\"blocks\"") == std::string::npos && json.find("\"blocks\"") != std::string::npos && light.size() < json.size(),
              "to_json(false) leaves the blocks out");

        std::string damaged = json;
        const size_t at = damaged.find("\"blocks\": \"") + 11;
        damaged.replace(at, 8, "AAAAAAAA");
        Scene3D hurt;
        check(hurt.from_json(damaged) && hurt.entities.size() == 2 && hurt.entities[1].voxels && hurt.entities[1].voxels->block_count() == 0,
              "damaged block data: the scene still loads, that object is empty");

        VoxelWorld twin = w.copy();
        twin.set(0, 0, 0, 0);
        check(w.get(0, 0, 0) == stone && twin.get(0, 0, 0) == 0 && twin.block_count() == w.block_count() - 1 &&
                  twin.find_block("glass") == glass && near3(twin.origin, w.origin),
              "VoxelWorld::copy() is a separate world with the same blocks, types and placement");
        BlockType st = w.block_type(stone);
        st.color = rgb(0.9f, 0.1f, 0.1f);
        w.set_block_type(stone, st);
        w.set_block_type(0, st);
        w.set_block_type(99, st);
        check(near(w.block_type(stone).color.r, 0.9f) && w.block_type(0).name == "air", "set_block_type() changes a type (and ignores air and unknown ids)");
        World vw;
        v.draw(vw);
        check(true, "draw() with a block object runs");
    }

    {
        // A parent listed after its own child, plus an unrelated one after
        // both: removing them one at a time would shift the indices under it.
        Scene3D r;
        for (const char* n : {"a", "child", "b", "parent", "c", "d"}) {
            SceneEntity e;
            e.name = n;
            r.add(e);
        }
        r.entities[1].parent = 3;
        r.remove(std::vector<int>{3, 4, 1});
        check(r.entities.size() == 3 && r.find("a") == 0 && r.find("b") == 1 && r.find("d") == 2,
              "remove(several): a parent after its child and another after both, all gone and only them");
    }
    s.remove(g); // the group and the crate under it
    check(s.entities.size() == 3 && s.find("crate") == -1 && s.find("lamp") == 0 && s.find("player_start") == 1,
          "remove() takes the children too and renumbers what's left");
    World world;
    s.draw(world);
    check(world.fog.enabled && near(world.ambient, 0.4f), "draw() applies the environment (and draws without trouble)");

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
