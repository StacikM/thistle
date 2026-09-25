// Loopback test for the realtime networking layer: a server and a client in
// one process, talking over 127.0.0.1, no window/App needed at all (this is
// deliberate — NetServer/NetClient don't depend on the render loop, so a
// dedicated server binary needs no graphics). Exercises spawn replication,
// NetVar sync (server -> client), Command (client -> server), and ClientRpc
// (server -> client), and actually inspects the CLIENT's replicated copy of
// the object via net_find(), not just the server's own instance.
//
// One process may only meaningfully run ONE active NetServer and ONE active
// NetClient at a time (see docs/networking.md) — that's why this test uses
// exactly one of each, not two clients.
//
// Build with -DTHISTLE_BUILD_SMOKETEST=ON, run ./thistle_net_smoketest.
#include <thistle.hpp>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
using namespace thistle;

namespace {

class Player : public NetObject {
public:
    NetVar<int> hp{100};
    NetVar<vec2> pos{{0, 0}};
    bool fired = false;      // set by the "fire" Command handler (runs on the server)
    bool healed = false;     // set by the "heal" ClientRpc handler (runs on clients)

    Player() {
        net_sync("hp", hp);
        net_sync("pos", pos);
        on_command("fire", [this](const NetArgs&) { fired = true; });
        on_client_rpc("heal", [this](const NetArgs&) { healed = true; });
    }

    void Fire() { call_command("fire"); }      // client calls this
    void Heal() { call_client_rpc("heal"); }   // server calls this
};

// A lot of data to send: see "a client that stops reading" below.
class Blob : public NetObject {
public:
    NetVar<std::string> data;
    Blob() { net_sync("data", data); }
};

int fails = 0;
void check(bool cond, const char* what) {
    std::printf("%s %s\n", cond ? "  ok " : "FAIL ", what);
    if (!cond) ++fails;
}

void pump(NetServer& server, NetClient& client, int ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        server.update();
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

int main() {
    net_register_class("Player", [] { return std::make_unique<Player>(); });
    net_register_class("Blob", [] { return std::make_unique<Blob>(); });

    NetServer server;
    check(server.listen(47100), "server.listen(47100)");

    NetClient client;
    bool connected_cb = false;
    client.on_connect = [&] { connected_cb = true; };
    check(client.connect("127.0.0.1", 47100), "client.connect(127.0.0.1, 47100)");

    server.update(); client.update();   // let the accept()/on_connect handshake settle
    check(connected_cb, "client on_connect fired");
    check(server.connection_count() == 1, "server sees 1 connection");

    // --- spawn: does the client actually get a usable replica? -----------
    auto* server_player = static_cast<Player*>(net_spawn("Player"));
    check(server_player != nullptr, "net_spawn returned an object");
    server_player->hp = 80;
    server_player->pos = vec2{12.0f, 34.0f};

    pump(server, client, 200);

    // Use client.find(), not the free net_find() — this test process has
    // BOTH an active NetServer and NetClient (for the loopback itself), and
    // net_find() can't disambiguate that (documented: no "host mode" in V1).
    // A real client-only process would just use net_find().
    NetObject* client_obj = client.find(server_player->net_id());
    check(client_obj != nullptr, "client replicated the spawned object");
    check(client_obj != static_cast<NetObject*>(server_player), "it's a distinct instance, not the same pointer");
    auto* client_player = static_cast<Player*>(client_obj);
    check(client_player && (int)client_player->hp == 80, "spawn snapshot carried hp=80");
    check(client_player && vec2(client_player->pos).x == 12.0f && vec2(client_player->pos).y == 34.0f, "spawn snapshot carried pos");

    // --- NetVar sync after a post-spawn change ----------------------------
    server_player->hp = 55;
    pump(server, client, 200);
    check((int)client_player->hp == 55, "sync delivered the post-spawn hp change");

    // --- Command: client -> server -----------------------------------
    check(!server_player->fired, "not fired yet");
    client_player->Fire();   // this is the client's replica calling it — correct direction
    pump(server, client, 200);
    check(server_player->fired, "server received the Command from the client");

    // NOTE: "call_command from server-side code should no-op" isn't
    // testable from THIS harness — call_command() checks "is there an
    // active NetClient in this process," and this process has one (the
    // loopback client above), so it would route through it regardless of
    // which object called it. That check is only meaningful across two
    // separate processes, which is the only configuration this API assumes.

    // --- ClientRpc: server -> all clients --------------------------------
    check(!client_player->healed, "not healed yet");
    server_player->Heal();
    pump(server, client, 200);
    check(client_player->healed, "client received the ClientRpc from the server");

    server.stop();
    client.disconnect();
    check(!client.connected(), "client reports disconnected after disconnect()");

    // --- joining and leaving: passwords, a full server, kicks ------------
    {
        NetServer s;
        s.password = "hunter2";
        s.max_players = 1;
        std::vector<int> left;
        s.on_disconnect = [&](int id) { left.push_back(id); };
        check(s.listen(47101), "a second server listens (password, 1 player at most)");

        NetClient a;
        check(!a.connect("127.0.0.1", 47101, "nope"), "a wrong password is refused");
        check(a.disconnect_reason() == "Wrong password", "... and says \"Wrong password\"");
        check(s.connection_count() == 0, "... and never counted as a player");
        check(a.connect("127.0.0.1", 47101, "hunter2"), "the right password gets in");
        const std::vector<NetConnection> list = s.connections();
        check(list.size() == 1 && list[0].id == a.connection_id() && list[0].address.rfind("127.0.0.1:", 0) == 0,
              "connections() lists them with their id and address");

        NetClient b;
        check(!b.connect("127.0.0.1", 47101, "hunter2"), "a second player is turned away");
        check(b.disconnect_reason() == "The server is full", "... with \"The server is full\"");

        std::string reason_in_callback;
        a.on_disconnect = [&] { reason_in_callback = a.disconnect_reason(); };
        check(s.kick(a.connection_id(), "griefing"), "kick() finds the player");
        check(left.size() == 1 && s.connection_count() == 0, "the server's on_disconnect ran for them");
        pump(s, a, 300);
        check(!a.connected(), "the kicked client is disconnected");
        check(a.disconnect_reason() == "Kicked: griefing", "... and knows why: \"Kicked: griefing\"");
        check(reason_in_callback == "Kicked: griefing", "... already inside its on_disconnect");
        check(!s.kick(999), "kick() of nobody is false");

        check(a.connect("127.0.0.1", 47101, "hunter2"), "there's room again after the kick");
        s.stop();
        pump(s, a, 300);
        check(!a.connected() && a.disconnect_reason() == "The server stopped", "stopping the server tells the client why");
        NetClient c;
        check(!c.connect("127.0.0.1", 47101) && c.disconnect_reason() == "Couldn't reach 127.0.0.1:47101",
              "connecting to nothing fails with \"Couldn't reach 127.0.0.1:47101\"");
    }

    // --- a client that stops reading can't stall the server -------------
    // (A frozen game, or someone doing it on purpose. Sending used to wait
    // for the socket, so the whole server hung on them.)
    {
        NetServer s;
        int dropped = 0;
        s.on_disconnect = [&](int) { ++dropped; };
        check(s.listen(47102), "a third server listens");
        NetClient lazy;
        check(lazy.connect("127.0.0.1", 47102), "a client connects, then never reads again");
        auto* blob = static_cast<Blob*>(net_spawn("Blob"));
        double worst = 0.0;
        for (int i = 0; i < 100 && dropped == 0; ++i) {
            blob->data = std::string(1 << 20, static_cast<char>('a' + i % 26)); // 1 MB to send every update
            const auto t0 = std::chrono::steady_clock::now();
            s.update();
            worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        check(dropped == 1, "the server drops it once too much is waiting for it");
        check(worst < 0.25, "... and no update() waited on it");
    }

    std::printf(fails == 0 ? "\nALL PASS\n" : "\n%d CHECK(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
