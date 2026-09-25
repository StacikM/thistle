# Realtime networking

A small client/server layer shaped like Unity's [Mirror](https://mirror-networking.com/): server-authoritative synced fields, Commands that flow client→server, ClientRpcs that flow server→all-clients. C++ has no attribute-driven code weaver, so it's not `[SyncVar]`/`[Command]` decorating a method — it's explicit registration calls that do the same job. Read the scope before you build anything real on this:

- **TCP only.** No UDP, no unreliable channel. If you want the "TCP for things that must arrive, unreliable UDP for high-frequency stuff like position where a dropped packet doesn't matter" hybrid real games use, that's a real V2, not this.
- **JSON wire format**, length-prefixed over the TCP stream. Not fast, not compact, genuinely simple and easy to debug (log the message, read it). If you're sending hundreds of updates a second to dozens of clients, you'll feel it. For a small game, you won't.
- **No NAT traversal, no relay service.** Same situation raw Mirror is in without its paid relay — you need a port someone can actually reach: a LAN, a VPS you control, port-forwarding. This library will not get two players behind separate home routers talking to each other for free. Nothing does, without a relay.
- **No client-side prediction, no lag compensation.** A client's `NetVar` updates the instant a `sync` message arrives — one tick of network latency, visible as-is. `three::TransformInterpolator` (below) smooths *other* things' movement between those updates; nothing predicts your own. Fine for a co-op builder or a low-tempo game; a twitch shooter will feel it immediately.
- **No "host mode."** A process is a server, or a client. It cannot usefully be both — see the last section of this doc for exactly why, and how to still test locally without two machines.

If any of those are dealbreakers, that's the honest answer to "is this enough for my game," not a bug report.

## The shape of it

```cpp
class Player : public NetObject {
public:
    NetVar<int> hp{100};
    NetVar<vec2> pos{{0, 0}};

    Player() {
        net_sync("hp", hp);
        net_sync("pos", pos);
        on_command("fire", [this](const NetArgs& a) { DoFire(a); });      // runs on the SERVER
        on_client_rpc("heal", [this](const NetArgs&) { flash_heal = true; }); // runs on CLIENTS
    }

    void Fire(vec2 dir) { call_command("fire", {{"dir", dir}}); }   // a client calls this
    void Heal() { call_client_rpc("heal"); }                          // the server calls this

private:
    void DoFire(const NetArgs& a) { /* server-authoritative effect */ }
    bool flash_heal = false;
};
```

Register the class once, on every machine that will ever spawn or receive one — server and every client, same name, before anyone connects:

```cpp
net_register_class("Player", [] { return std::make_unique<Player>(); });
```

Server side: spawn it, mutate `NetVar`s from server logic, they replicate automatically.

```cpp
NetServer server;
server.listen(7777);
Player* p = static_cast<Player*>(net_spawn("Player"));
p->hp = 80;   // every connected (and later-connecting) client's copy updates on the next update()

app.update([&](Frame f) {
    server.update();   // accept, dispatch Commands, flush dirty NetVars — call this every frame/tick
});
```

Client side: connect, then find the objects the server told you about to actually render them.

```cpp
NetClient client;
client.connect("game.example.com", 7777);

app.update([&](Frame f) {
    client.update();   // read/dispatch spawn, sync, rpc, despawn messages
    net_each_object([&](NetObject& obj) {
        if (auto* p = dynamic_cast<Player*>(&obj)) f.circle(p->pos, 16, white);
    });
});
```

That's the whole API surface: `NetVar<T>` for auto-synced fields, `on_command`/`call_command` for client→server, `on_client_rpc`/`call_client_rpc` for server→clients, `net_spawn`/`net_despawn` (server-only) and `net_find`/`net_each_object` (read side, works on either) to actually get at your objects.

## Who sent it, and talking to one client

Three small additions that most real games need sooner or later:

- **`net::command_sender()`** — inside an `on_command` handler on the server, the connection id that sent the command (the same id `NetServer::on_connect` got). That's how a server says "only this player's own client may move this player":
  ```cpp
  on_command("move", [this](const NetArgs& a) {
      if (net::command_sender() != owner.get()) return;
      position = a.at("p").get<vec3>();
  });
  ```
- **`call_target_rpc(conn_id, name, args)`** — a ClientRpc to one connection only (Mirror's TargetRpc): a newcomer's catch-up data, one player's private hand of cards. Returns false if that connection is gone.
- **`NetClient::connection_id()`** — the client's own id as the server knows it, e.g. to find which player object is yours (`avatar->owner == client.connection_id()`). It's set once `connect()` has succeeded.

## Joining and leaving

A client says hello first; only then does the server let it in (and send it every object) or turn it away. `connect()` waits for that answer, so it tells you there and then whether you got in, and why not:

```cpp
NetClient client;
if (!client.connect(host, port, password)) {
    show(client.disconnect_reason());   // "Wrong password", "The server is full", "Couldn't reach 1.2.3.4:47000", ...
}
client.on_disconnect = [&] {
    show(client.disconnect_reason());   // "Kicked: being rude", "The server stopped", "Lost the connection to the server"
};
```

It blocks briefly (one round trip; at most a few seconds if something answers the connection but never says anything). `disconnect_reason()` is already set when `on_disconnect` runs, and is `""` after the game's own `disconnect()`.

On the server:

- **`password`** and **`max_players`** (set before `listen()`): a client with the wrong password, or one past the limit, is turned away with the reason and never counts as a player. `on_connect` only runs for players who got in.
- **`kick(conn_id, reason)`**: disconnects a player and tells their game why ("Kicked: reason", or "Kicked by the server"). `on_disconnect` runs for them. False if there's no such player.
- **`connections()`**: every player's id, address (`"203.0.113.7:51544"`) and time connected.
- **`stop()`** tells every player "The server stopped" before closing.

A kick or a refusal closes the connection gently (it stops sending, reads what's left, then closes), because closing a socket with unread data resets it, and a reset can throw away the message just sent, the reason with it.

[dedicated-servers.md](dedicated-servers.md) has a whole server built around these: a console to type `kick 3` into, settings in a file, a log.

### A player that stops reading can't stall the server

Sending never waits. What the network won't take right away waits in that connection's own queue and goes out on later `update()`s; a connection with 32 MB waiting is dropped (and logged). Before, sending busy-waited for the socket, so one player whose game froze, or who stopped reading on purpose, hung the whole server. A message whose length is past 16 MB (garbage, or someone probing the port) closes that connection instead of making the server try to buffer it. And sending to a player who just vanished no longer raises SIGPIPE, which on Linux and macOS used to end the server process.

## Multiplayer block worlds: `three::VoxelSync`

Keeps a `VoxelWorld` identical on the server and every client:

```cpp
VoxelSync sync(world);            // server and every client, before listen()/connect()
sync.host();                      // server, after listen()
sync.update();                    // every frame, after server.update() / client.update()
sync.request_set(block, stone);   // client: shown at once, then the server decides
sync.allow_edit = [](int conn, ivec3 b, BlockId id) { return b.y > 0; }; // server: no digging through bedrock
```

- **Joining**: the client gets the block types, then every chunk, streamed a byte budget per update (`stream_bytes_per_update`, 64 KB) so one player joining never stalls the server for everyone else. `ready()` / `progress()` drive a loading bar. Whatever the client's world held before is replaced.
- **Changes**: anything that changes the server's world reaches every client as the changed chunks: `set()`, `fill()`, a generator, a `VoxelDestruction` blast. It's detected through per-chunk revisions, so there's no special edit API to route through. A chunk is a few hundred bytes of run-length data for a typical edit.
- **Client edits** go through `request_set()`. They show immediately (no waiting a round trip to see your own block), and the server's copy of that chunk replaces it if the server disagrees (`allow_edit`). One visible consequence of that simplicity: if an update to the same chunk was already on its way when you edited it, your block can blink off for a moment until the server's version with it arrives. It always settles to the server's world.
- **Not synced**: debris from a `VoxelDestruction` on the server is physics, not blocks, and doesn't replicate. Clients see the holes, not the pieces flying. Streaming worlds (`stream_around`) aren't synced as such either; generate on the server and let the chunks flow.

`examples/voxel_mp_demo.cpp` is the whole thing in about 200 lines: `thistle_mp_demo server` runs a dedicated server with no window, and `thistle_mp_demo [host]` joins as a player who can walk, break and place blocks, and see the others (NetObject avatars moved by their owners' commands, drawn through a `TransformInterpolator`).

### Smoothing other players: `three::TransformInterpolator`

Network updates arrive in steps (the demo sends 20 a second). Drawing a remote player at its latest position looks like teleporting. Instead, `add()` each new transform as it arrives and `sample()` one `delay` (0.1 s) in the past: position and scale are blended, rotation is slerped, and past the newest sample it holds still rather than guessing.

## Why it's this verbose instead of `[Command]`-style magic

Mirror's ergonomics come from Unity's IL weaver rewriting your assembly after compilation — a `[Command] void CmdFire()` method's body gets moved into a generated handler, and the original method becomes a network-send call, invisibly. C++ has no equivalent compile step here. What you see (`on_command` to register what runs, `call_command` to trigger it) is the honest, unmagical version of the same idea: two explicit halves instead of one method that secretly becomes two things. It's more typing. It's also something you can read top-to-bottom and know exactly what happens, which matters more once something's wrong at 2am.

## `T` has to be JSON-convertible

Every built-in numeric type, `std::string`, and `vec2`/`vec3`/`rgba` already work — `NetVar<vec2>` just works, because `thistle.hpp` gives those three ADL `to_json`/`from_json` overloads. For your own struct, do the same thing:

```cpp
struct Inventory { int gold; std::vector<std::string> items; };
void to_json(nlohmann::json& j, const Inventory& v) { j = {{"gold", v.gold}, {"items", v.items}}; }
void from_json(const nlohmann::json& j, Inventory& v) { v.gold = j.at("gold"); v.items = j.at("items").get<std::vector<std::string>>(); }
```

Then `NetVar<Inventory>` works exactly like `NetVar<int>`. This is a real, if verbose, extension point — not a limitation you have to route around.

## `net_find` / `net_each_object` and why "host mode" isn't a thing here

`call_command`, `call_client_rpc`, `net_spawn`, `net_find` — none of them take a `NetServer&`/`NetClient&` argument. They reach for **the** active server or **the** active client in this process via a global pointer set by `listen()`/`connect()`. That's what makes `player->Fire()` work without threading a transport handle through every object — and it's also exactly why a single process can't usefully run both a server and a client at once: whichever one connected/listened *last* wins the global slot, and `net_find`/`net_each_object` always check the server slot before the client slot, so in a mixed-role process you may silently get the server's own object back when you meant to inspect a client's replica.

If you genuinely need both roles alive in one process — the actual, sanctioned reason to do this is a same-process test harness, which is exactly what `examples/net_smoketest.cpp` is — use `NetServer::find(id)` / `NetClient::find(id)` directly instead of the free functions. Those are unambiguous; they always look at that specific instance's own registry, never the global slot. This is also the reason the test file exists and is worth reading: it's a real spawn/sync/Command/ClientRpc round trip over actual TCP sockets on localhost, with real assertions, not a hand-wave. It found a genuine bug once already — see the comment on the "sync" handler in `src/net.cpp` about nlohmann's `.items()` and dangling temporaries, which only surfaced by actually running the thing, not by reading the code. Trust the test, not just the diff, if you're changing anything in this file.

## Build and run the test yourself

```bash
cmake -S . -B build -DTHISTLE_BUILD_SMOKETEST=ON
cmake --build build --target thistle_net_smoketest
./build/thistle_net_smoketest
```

No window, no `App`, no graphics dependency at all — `NetServer`/`NetClient` don't touch rendering. A dedicated server links `thistle_server`, the engine without the graphics stack, and needs no graphics libraries even to build ([dedicated-servers.md](dedicated-servers.md)); CI runs this test against that library too.

Besides the round trip above, it checks joining and leaving: a wrong password refused with its reason, the right one let in, `connections()`, a full server, a kick's reason reaching the client (already inside its `on_disconnect`), the server stopping, and connecting to nothing. And a client that stops reading: the server has to drop it without any `update()` waiting on it. With the old busy-waiting send put back, that check hangs until killed.

`thistle_voxelnet_smoketest` does the same kind of loopback round trip for `VoxelSync`. It checks:
- a joining client ends up with an identical world, chunk for chunk, streamed over several updates under a tiny budget
- server edits reach it, including a chunk emptied completely
- a client edit shows up at once and is applied by the server, and a refused one is put back
- `allow_edit` sees the sender's connection id
- `call_target_rpc` reaches exactly one client and reports a gone connection
- `TransformInterpolator` blends

The demo was also run as a dedicated server plus two player windows under Xvfb. Both players received the 74,690-block world. Player A's walk showed up at the right spot for B, and A's placed blocks appeared in B's world. B, walking off a tree, saw A's avatar standing next to them. That was one machine over loopback: not a real network with latency and packet loss.
