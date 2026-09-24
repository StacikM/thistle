// Checks VoxelSync over a real loopback TCP connection (server and client
// in one process, like net_smoketest): a joining client receives the whole
// world (streamed in pieces under a small byte budget) and ends up identical;
// server edits, including emptying a chunk, reach it; a client's edit is
// shown at once and applied on the server; a refused edit is put back.
// Also the networking additions VoxelSync is built on (command sender,
// target RPCs, the client's connection id) and TransformInterpolator.
// No window, no GPU.
#include <thistle.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
using namespace thistle;
using namespace thistle::three;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { std::printf("  ok  %s\n", msg.c_str()); }
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_failures; }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool same(const VoxelWorld& a, const VoxelWorld& b) {
    if (a.block_count() != b.block_count() || a.block_type_count() != b.block_type_count()) return false;
    for (const ivec3& c : a.chunks()) {
        if (a.serialize_chunk(c) != b.serialize_chunk(c)) return false;
    }
    return a.chunks().size() == b.chunks().size();
}

class Probe : public NetObject {
public:
    int pings = 0;
    Probe() { on_client_rpc("ping", [this](const NetArgs&) { ++pings; }); }
    bool ping(int conn) { return call_target_rpc(conn, "ping"); }
};
} // namespace

int main() {
    std::printf("TransformInterpolator\n");
    {
        TransformInterpolator it;
        it.delay = 0.1f;
        check(it.empty() && it.sample(1.0).position == vec3{}, "empty: an identity transform");
        it.add(Transform{{0, 0, 0}}, 0.0);
        it.add(Transform{{1, 0, 0}, quat::axis_angle({0, 1, 0}, 1.0f)}, 0.1);
        it.add(Transform{{2, 0, 0}, quat::axis_angle({0, 1, 0}, 1.0f)}, 0.2);
        check(near(it.sample(0.25).position.x, 1.5f), "halfway between two samples, delay behind");
        check(near(it.sample(0.15).rotation.y, quat::axis_angle({0, 1, 0}, 0.5f).y, 1e-3f), "rotation turns halfway too");
        check(near(it.sample(5.0).position.x, 2.0f) && near(it.sample(0.0).position.x, 0.0f), "holds the ends");
    }

    // The server's world: several chunks, several block types.
    VoxelWorld server_world;
    const BlockId stone = server_world.add_block({.name = "stone", .color = rgb(0.5f, 0.5f, 0.5f)});
    const BlockId grass = server_world.add_block({.name = "grass", .color = rgb(0.3f, 0.6f, 0.2f)});
    const BlockId glass = server_world.add_block({.name = "glass", .alpha = AlphaMode::Blend});
    server_world.voxel_size = 0.5f;
    server_world.fill({0, 0, 0}, {95, 0, 95}, grass);   // 9 chunks of ground
    server_world.fill({10, 1, 10}, {20, 40, 20}, stone); // a tower into the chunk above
    for (int i = 0; i < 200; ++i) server_world.set((i * 37) % 96, 1 + i % 5, (i * 53) % 96, i % 2 ? glass : stone);

    VoxelWorld client_world;
    client_world.add_block({.name = "junk"});
    client_world.fill({-40, -40, -40}, {-35, -35, -35}, 1); // must be gone once the server's world arrives

    VoxelSync server_sync(server_world);
    VoxelSync client_sync(client_world);
    int refused_from = -1;
    server_sync.allow_edit = [&](int conn, ivec3 b, BlockId) {
        if (b.y <= 20) return true;
        refused_from = conn;
        return false;
    };
    server_sync.stream_bytes_per_update = 1024; // small, so the world arrives over several updates
    net_register_class("Probe", [] { return std::make_unique<Probe>(); });

    NetServer server;
    NetClient client;
    const int port = 47931;
    if (!server.listen(port)) {
        std::printf("  FAIL couldn't listen on %d\n", port);
        return 1;
    }
    server_sync.host();
    Probe* probe = static_cast<Probe*>(net_spawn("Probe"));
    check(client.connect("127.0.0.1", port), "client connects over loopback");

    auto pump = [&](int ms, const std::function<bool()>& until = {}) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            server.update();
            server_sync.update();
            client.update();
            client_sync.update();
            if (until && until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    std::printf("joining\n");
    bool saw_partial = false;
    const bool arrived = pump(5000, [&] {
        const float p = client_sync.progress();
        if (p > 0.0f && p < 1.0f) saw_partial = true;
        return client_sync.ready() && client_world.block_count() > 0;
    });
    check(arrived, "the whole world arrives");
    check(saw_partial, "streamed over several updates (progress seen between 0 and 1)");
    check(same(server_world, client_world), "the client's world is identical, chunk by chunk (" + std::to_string(client_world.block_count()) + " blocks)");
    check(client_world.find_block("glass") == glass && client_world.find_block("junk") == 0 && near(client_world.voxel_size, 0.5f),
          "block types and voxel size came with it; what the client had is gone");
    check(client.connection_id() > 0, "the client knows its connection id");

    std::printf("edits\n");
    server_world.set(50, 5, 50, stone);
    server_world.fill({64, 0, 64}, {95, 0, 95}, 0); // empties one ground chunk entirely
    pump(300, [&] { return same(server_world, client_world); });
    check(same(server_world, client_world) && client_world.get(50, 5, 50) == stone && client_world.get(70, 0, 70) == 0,
          "server edits reach the client, including a chunk emptied completely");

    client_sync.request_set({30, 3, 30}, glass);
    check(client_world.get(30, 3, 30) == glass, "a client's edit shows up there at once");
    pump(300, [&] { return server_world.get(30, 3, 30) == glass; });
    check(server_world.get(30, 3, 30) == glass, "and the server applies it");
    // Let the server's echo of that chunk arrive before the next edit in the
    // same chunk: otherwise that (older) echo would overwrite the next
    // optimistic edit, and "put back" below would pass for the wrong reason.
    // (It did, on macOS CI.)
    pump(300, [&] { return same(server_world, client_world); });

    client_sync.request_set({30, 30, 30}, stone);
    check(client_world.get(30, 30, 30) == stone, "a refused edit also shows up at first");
    pump(1000, [&] { return refused_from != -1 && client_world.get(30, 30, 30) == 0; });
    check(server_world.get(30, 30, 30) == 0 && client_world.get(30, 30, 30) == 0, "an edit the server refuses is put back on the client");
    check(refused_from == client.connection_id(), "allow_edit is told which connection asked (net::command_sender)");

    std::printf("target rpc\n");
    pump(100);
    check(probe && probe->ping(client.connection_id()), "call_target_rpc reaches a live connection");
    check(probe && !probe->ping(9999), "and says so when the connection doesn't exist");
    pump(200, [&] {
        auto* mine = static_cast<Probe*>(client.find(probe->net_id()));
        return mine && mine->pings == 1;
    });
    auto* replica = probe ? static_cast<Probe*>(client.find(probe->net_id())) : nullptr;
    check(replica && replica->pings == 1, "the client got exactly the one ping");

    client.disconnect();
    server.stop();
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
