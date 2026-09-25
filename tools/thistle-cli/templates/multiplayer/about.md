
A multiplayer starter: players run around an arena and see each other, through a dedicated server that has no window at all.

## Playing

Start the server, and leave it running:

```bash
thistle run --server
```

Then a game, once per player (on this machine, or others that can reach it):

```bash
thistle run
```

The game opens on a connect screen: the server's address (`127.0.0.1` is this machine), its port, and a password if it has one. It remembers them for next time. Click the window to capture the mouse (Esc lets it go, and shows Leave). The mouse turns the camera and the wheel zooms. WASD to run, Shift to walk, Space to jump.

## The server

`src/server.cpp` is a `DedicatedServer` (see the engine's `docs/dedicated-servers.md`). Type into it while it runs: `help` lists the commands. `list` shows who's on with their ids, `kick 2 being rude` sends player 2 away with that reason (their game shows it), `status` shows whether it keeps up, `stop` stops it. `respawn <id>` is this template's own, in `src/server.cpp`: `server.command()` is all it takes to add one.

Its settings are in `server.json` next to it, made on the first run: the port, `tick_rate`, `max_players`, a `password`, and `max_speed`, a setting of this template's own. Command-line options override them for one run: `thistle run --server -- --port 47001 --max-players 8`. `logs/` has everything it printed.

To run it on a VPS or in Docker, where there are no graphics libraries, `thistle build --server` builds only the server, in `build-server/`. Copy the program and its `assets/` folder over, and run it there.

## How it works

- **`src/shared.hpp`** is what both sides compile: the networked `Player`. The server makes one for each player who joins; everyone gets a copy.
- **Each game moves its own player** with a `CharacterController` against the level, and sends where it is 20 times a second.
- **The server checks each move** (not faster than `max_speed`) and passes it on. A move it refuses puts that player back where they were; falling out of the world sends them to a spawn point.
- **Everyone else** is drawn through a `TransformInterpolator`: a little in the past, but smooth.

## Changing the level

The level is `assets/scenes/level.scene.json`, made with the Thistle Editor. Open it with:

```bash
thistle editor run
```

The game collides with every shape and model in it (except ones with the property `solid` = `false`). The server reads the **spawn points** (`spawn`): players join at them in turn, facing the way each faces. Add as many as you like.

## Where to go from here

- **More to sync**: health, a score, a name. Add a `NetVar` to `Player` in `src/shared.hpp`; the server sets it, everyone sees it (the engine's `docs/networking.md`).
- **Things that happen**: a `Command` for "I shot", checked on the server, then a `ClientRpc` to everyone for the effect.
- **A shared world**: `VoxelSync` streams a block world to everyone and applies their edits (the engine's `docs/voxels.md`).
- **Saving**: whatever the server should keep goes in its `stop` function in `src/server.cpp`; `stop`, Ctrl+C and the system shutting it down all run it.
