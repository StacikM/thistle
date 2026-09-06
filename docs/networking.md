# Realtime networking

A small client/server layer shaped like Unity's [Mirror](https://mirror-networking.com/): server-authoritative synced fields, Commands that flow client→server, ClientRpcs that flow server→all-clients. C++ has no attribute-driven code weaver, so it's not `[SyncVar]`/`[Command]` decorating a method — it's explicit registration calls that do the same job. Read the scope before you build anything real on this:

- **TCP only.** No UDP, no unreliable channel. If you want the "TCP for things that must arrive, unreliable UDP for high-frequency stuff like position where a dropped packet doesn't matter" hybrid real games use, that's a real V2, not this.
- **JSON wire format**, length-prefixed over the TCP stream. Not fast, not compact, genuinely simple and easy to debug (log the message, read it). If you're sending hundreds of updates a second to dozens of clients, you'll feel it. For a small game, you won't.
- **No NAT traversal, no relay service.** Same situation raw Mirror is in without its paid relay — you need a port someone can actually reach: a LAN, a VPS you control, port-forwarding. This library will not get two players behind separate home routers talking to each other for free. Nothing does, without a relay.
- **No client-side prediction, no interpolation, no lag compensation.** A client's `NetVar` updates the instant a `sync` message arrives — one tick of network latency, visible as-is. Fine for a turn-based or low-tempo game; a twitch shooter will feel it immediately.
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

If you genuinely need both roles alive in one process — the actual, sanctioned reason to do this is a same-process test harness, which is exactly what `examples/net_smoketest.cpp` is — use `NetServer::find(id)` / `NetClient::find(id)` directly instead of the free functions. Those are unambiguous; they always look at that specific instance's own registry, never the global slot. This is also the reason the test file exists and is worth reading: it's a real spawn/sync/Command/ClientRpc round trip over actual TCP sockets on localhost, with real assertions, not a hand-wave. It found a genuine bug once already — see the comment on the "sync" handler in `thistle.cpp` about nlohmann's `.items()` and dangling temporaries, which only surfaced by actually running the thing, not by reading the code. Trust the test, not just the diff, if you're changing anything in this file.

## Build and run the test yourself

```bash
cmake -S . -B build -DTHISTLE_BUILD_SMOKETEST=ON
cmake --build build --target thistle_net_smoketest
./build/thistle_net_smoketest
```

No window, no `App`, no graphics dependency at all — `NetServer`/`NetClient` don't touch rendering, which is also why a dedicated (headless) server binary for your game needs nothing but `thistle`'s networking pieces linked in, not the whole engine's graphics stack.
