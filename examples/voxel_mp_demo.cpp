// Multiplayer block world (VoxelSync + NetObject players).
//
//   thistle_mp_demo server [port]        a dedicated server: no window, generates the world
//   thistle_mp_demo [host] [port]        a player (default 127.0.0.1:47000)
//
// Players walk around (WASD + mouse, Space jumps; click to capture the
// mouse), break blocks with the left button and place stone with the right.
// Everyone sees the same world and each other: each player is a NetObject
// the server spawns on connect, moved by its owner's "move" commands (the
// server checks the sender) and drawn smoothly by the others through a
// TransformInterpolator. Stats go to the log. Never quits on its own, so CI
// builds it but doesn't run it.
#include <thistle.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
using namespace thistle;
using namespace thistle::three;

namespace {

class Avatar : public NetObject {
public:
    NetVar<int> owner{0};
    NetVar<vec3> position{{0, 0, 0}};
    NetVar<float> yaw{0.0f};
    NetVar<rgba> color{white};

    Avatar() {
        net_sync("owner", owner);
        net_sync("position", position);
        net_sync("yaw", yaw);
        net_sync("color", color);
        on_command("move", [this](const NetArgs& a) {
            if (net::command_sender() != owner.get()) return; // only its own player moves it
            position = a.at("p").get<vec3>();
            yaw = a.at("yaw").get<float>();
        });
    }
    void send_move(vec3 p, float y) { call_command("move", {{"p", p}, {"yaw", y}}); }
};

struct Blocks {
    BlockId grass, dirt, stone, wood, leaves;
};

Blocks add_blocks(VoxelWorld& w) {
    return {w.add_block({.name = "grass", .color = rgb(0.38f, 0.6f, 0.28f)}), w.add_block({.name = "dirt", .color = rgb(0.47f, 0.34f, 0.22f)}),
            w.add_block({.name = "stone", .color = rgb(0.55f, 0.55f, 0.57f)}), w.add_block({.name = "wood", .color = rgb(0.5f, 0.36f, 0.22f)}),
            w.add_block({.name = "leaves", .color = rgb(0.27f, 0.52f, 0.24f)})};
}

void generate(VoxelWorld& w, const Blocks& b) {
    for (int z = -48; z < 48; ++z) {
        for (int x = -48; x < 48; ++x) {
            const int h = 4 + static_cast<int>(std::floor(fbm(x * 0.03f, z * 0.03f, 4, 11) * 8.0f + 4.0f));
            for (int y = 0; y < h; ++y) w.set(x, y, z, y == h - 1 ? b.grass : y > h - 4 ? b.dirt : b.stone);
            if ((x * 7 + z * 13) % 97 == 0 && h > 6) { // a few trees
                for (int y = h; y < h + 5; ++y) w.set(x, y, z, b.wood);
                w.fill_sphere({x + 0.5f, h + 5.5f, z + 0.5f}, 2.6f, b.leaves);
                for (int y = h; y < h + 5; ++y) w.set(x, y, z, b.wood);
            }
        }
    }
}

int run_server(int port) {
    VoxelWorld world;
    const Blocks b = add_blocks(world);
    generate(world, b);
    net_register_class("Avatar", [] { return std::make_unique<Avatar>(); });
    VoxelSync sync(world);
    NetServer server;
    if (!server.listen(port)) {
        log_error("mp demo: can't listen on port " + std::to_string(port));
        return 1;
    }
    sync.host();
    std::map<int, Avatar*> avatars;
    const rgba colors[] = {rgb(0.9f, 0.3f, 0.25f), rgb(0.25f, 0.5f, 0.95f), rgb(0.95f, 0.8f, 0.2f), rgb(0.6f, 0.3f, 0.85f)};
    server.on_connect = [&](int conn) {
        auto* a = static_cast<Avatar*>(net_spawn("Avatar"));
        a->owner = conn;
        a->position = vec3{0.5f, 20.0f, 0.5f};
        a->color = colors[conn % 4];
        avatars[conn] = a;
        log_info("mp demo: player " + std::to_string(conn) + " joined (" + std::to_string(server.connection_count()) + " online)");
    };
    server.on_disconnect = [&](int conn) {
        if (avatars.count(conn)) net_despawn(avatars[conn]);
        avatars.erase(conn);
        log_info("mp demo: player " + std::to_string(conn) + " left");
    };
    log_info("mp demo: serving " + std::to_string(world.block_count()) + " blocks on port " + std::to_string(port));
    for (;;) { // 60 updates a second; a dedicated server needs no window
        server.update();
        sync.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "server") == 0) return run_server(argc >= 3 ? std::atoi(argv[2]) : 47000);
    const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    const int port = argc >= 3 ? std::atoi(argv[2]) : 47000;

    App app{{.title = "thistle mp demo", .width = 960, .height = 540}};
    VoxelWorld world;
    VoxelSync sync(world);
    net_register_class("Avatar", [] { return std::make_unique<Avatar>(); });
    NetClient client;
    if (!client.connect(host, port)) {
        log_error("mp demo: nothing at " + host + ":" + std::to_string(port) + " (start one with: thistle_mp_demo server)");
        return 1;
    }

    CollisionWorld level;
    level.add(world);
    CharacterController player;
    player.position = {0.5f, 25.0f, 0.5f};
    MouseLook look;
    World scene;
    scene.sun.direction = {-0.4f, -0.8f, -0.3f};
    scene.fog.enabled = true;
    scene.fog.start = 30.0f;
    scene.fog.end = 60.0f;
    Camera camera;
    std::map<uint32_t, TransformInterpolator> others;
    std::map<uint32_t, vec3> last_seen;
    float send_timer = 0.0f;

    app.update([&](Frame f) {
        client.update();
        sync.update();
        if (!client.connected()) {
            f.clear(rgb(0.1f, 0.05f, 0.05f));
            return;
        }
        if (!sync.ready()) { // a loading bar while the world streams in
            f.clear(rgb(0.08f, 0.09f, 0.12f));
            f.rect({f.width * 0.25f, f.height * 0.5f - 6}, {f.width * 0.5f, 12}, rgba{1, 1, 1, 0.15f});
            f.rect({f.width * 0.25f, f.height * 0.5f - 6}, {f.width * 0.5f * sync.progress(), 12}, rgb(0.4f, 0.8f, 0.5f));
            return;
        }

        look.update(camera, f);
        player.update(level, look.move_input(f), f.key_pressed(Key::Space), f.dt);
        camera.position = player.eye();
        if (mouse_locked()) {
            if (const auto hit = world.raycast(Ray{camera.position, camera.rotation.forward()}, 8.0f)) {
                if (f.mouse_pressed(Mouse::Left)) sync.request_set(hit.block, 0);
                if (f.mouse_pressed(Mouse::Right)) {
                    const ivec3 at = hit.block + hit.normal;
                    if (!player.bounds().overlaps(world.block_bounds(at))) sync.request_set(at, world.find_block("stone"));
                }
                scene.wire_box(world.block_bounds(hit.block), rgba{0, 0, 0, 0.6f});
            }
        }

        // Ours: tell the server where we are, 20 times a second. Others: smooth.
        Avatar* mine = nullptr;
        net_each_object([&](NetObject& o) {
            auto* a = dynamic_cast<Avatar*>(&o);
            if (!a) return;
            if (a->owner.get() == client.connection_id()) { mine = a; return; }
            const vec3 p = a->position.get();
            auto seen = last_seen.find(a->net_id());
            if (seen == last_seen.end() || !(seen->second == p)) {
                last_seen[a->net_id()] = p;
                others[a->net_id()].add(Transform{p, quat::euler(0, a->yaw.get())}, f.time);
            }
            const Transform t = others[a->net_id()].sample(f.time);
            scene.box(Transform{t.position + vec3{0, 0.7f, 0}, t.rotation, {0.6f, 1.4f, 0.35f}}, a->color.get());
            scene.box(Transform{t.position + vec3{0, 1.65f, 0}, t.rotation, {0.45f, 0.45f, 0.45f}}, rgb(0.95f, 0.8f, 0.65f));
        });
        send_timer += f.dt;
        if (mine && send_timer >= 0.05f) {
            send_timer = 0.0f;
            mine->send_move(player.position, look.yaw);
        }

        scene.draw(world);
        scene.render(f, camera);
        const float cx = f.width * 0.5f, cy = f.height * 0.5f;
        f.rect({cx - 1, cy - 7}, {2, 14}, rgba{1, 1, 1, 0.8f});
        f.rect({cx - 7, cy - 1}, {14, 2}, rgba{1, 1, 1, 0.8f});
        static int frames = 0;
        if (++frames % 180 == 0) {
            std::string where;
            for (const auto& [id, it] : others) {
                const vec3 p = it.sample(f.time).position;
                where += " [" + std::to_string(static_cast<int>(p.x)) + "," + std::to_string(static_cast<int>(p.y)) + "," +
                         std::to_string(static_cast<int>(p.z)) + "]";
            }
            log_info("mp demo: me=" + std::to_string(client.connection_id()) + " at " + std::to_string(static_cast<int>(player.position.x)) +
                     "," + std::to_string(static_cast<int>(player.position.y)) + "," + std::to_string(static_cast<int>(player.position.z)) +
                     " others:" + where + " blocks=" + std::to_string(world.block_count()));
        }
    });
    return app.run();
}
