# Save data, networking, and everything else platform-shaped

## Save data

```cpp
save::set_int("best_score", 4200);
int best = save::get_int("best_score", 0);   // fallback if the key doesn't exist yet
save::set("player_name", "stacik");
save::has("player_name");
save::remove("player_name");
save::clear();          // wipes everything — you almost never want this in a shipped feature
save::path();            // where it actually lives, for debugging
```

A flat key/value store, one file, written to the correct writable directory per platform automatically:

| Platform | Location |
|---|---|
| iOS | app's Documents directory |
| macOS | `~/Library/Application Support/<sanitized app title>` |
| Windows | `%APPDATA%\Thistle\<sanitized app title>` |
| Linux/other | `$XDG_DATA_HOME/thistle/<sanitized app title>`, or `~/.local/share/thistle/...` |

The folder name comes from your `AppConfig::title` — set it before you rely on save data existing, and don't change it later expecting old saves to carry over, because they won't; a title change is a folder change is a fresh save file. This has nothing to do with your bundle ID or product name anywhere else, it's specifically the string you passed to `App{{.title = "..."}}`.

That's the entire feature set: flat keys, four value types (string/int/float, plus existence/removal). No nested structures, no schema, no migration system. If your save data is complex enough to need structure, serialize your own JSON with nlohmann (already vendored, `third_party/nlohmann/json.hpp`) into a single string value and store *that* under one key — don't try to bolt a document database onto four functions that were never meant to be one. This is exactly what community-level upload in Fling does for level geometry; it does not use `save::` for that at all, and neither should you for anything beyond simple flags/scores/settings.

**Critical, and it has actually gone wrong before:** if you deploy over an existing install by *uninstalling first*, you wipe the entire app sandbox — including this save file — on every single deploy. Install *over* the existing app instead; only uninstall-and-retry as a fallback when the install is outright rejected. This is exactly the bug that shipped once in Fling's deploy script and cost real player progress before it was caught. Check your own deploy tooling for this if you're iterating fast on a real device.

## Networking

```cpp
Http req = Http::get("https://api.example.com/levels");
// ... every frame:
if (req.done()) {
    if (req.ok()) parse(req.body());        // ok() = done() && status in 200..299
    else if (req.status() == 0) show("can't reach server");
    else show("server error " + std::to_string(req.status()));
}
```

Async, poll-based, on purpose — there's no callback, no promise/future, because this engine has one thread running your `app.update` lambda and a callback firing from a background networking thread into your game state would be a data race waiting to happen. Polling `done()` from inside the frame you already control sidesteps that entirely. Don't "fix" this by adding a callback — you'd be reintroducing the exact bug this design avoids.

This is request/response, not a persistent connection — for realtime client/server play (spawn an object, sync a field, call an RPC), see [networking.md](networking.md) instead; that's a different, TCP-socket-based layer, not built on `Http` at all.

`Http::post(url, json_body)` sends the body as `application/json`. There is no way to set custom headers, no streaming, no file upload, no cookies. It exists specifically for "send/receive a small JSON blob," which is what a community-content API needs — Fling's entire account/level/comment system runs on exactly this and nothing more. If you need a real HTTP client, use one; don't extend this into something it was never designed to be.

**Platform support, actually verified, not assumed:** Apple (NSURLSession) and Windows (WinHTTP) both really work — Apple's been used in production for months; Windows was verified by cross-compiling, running it, confirming a real TLS handshake against a live server, and then actually confirmed again on real Windows hardware, not just a cross-compiler and a translation layer. Every other platform: `Http::get`/`post` compile, do nothing, and `done()` returns `true` with `status() == 0` on the very next poll. That's not a bug, it's the documented fallback — `ok()` will correctly be `false`, your error-handling path will run, your game won't crash. It just won't fetch anything. If you're shipping on Linux and need this, someone has to write the `curl`/libcurl equivalent of `win32_support.cpp` — it's a contained, well-scoped piece of work, not a rewrite, follow the exact pattern already there.

## Clipboard, haptics, text input, logging

```cpp
set_clipboard("share this");                        // platform clipboard, where supported
haptic(Haptic::Light);                                // iOS only, silent no-op elsewhere — don't gate gameplay logic on it firing

begin_text_input("current value");                    // shows the soft keyboard on mobile, starts capturing
std::string typed = text_input();                     // read every frame while capturing
end_text_input();                                      // hides the keyboard, stops capturing

log_info("loaded level " + id);
log_warn("falling back to default asset");
log_error("could not open " + path);
```

Text input is the mechanism behind every on-screen text field in Fling (level names, login username/password, comments) — it's deliberately primitive: printable ASCII, Backspace, that's the whole alphabet of editing operations. No cursor positioning, no selection, no cut/copy/paste within the field, no IME/composed-character support (so no CJK input). If your game needs a real text field, that's a real limitation to plan around, not an oversight to work around with a hack — there isn't a clever workaround, the underlying capture is genuinely that simple.

Logs print to console *and* get captured into an in-memory ring buffer (trimmed at 1000 lines) that the `THISTLE_DEBUG` overlay reads (see [building.md](building.md)). Use `log_error` for things that are actually wrong, not for routine flow — every log line is a line someone has to read while debugging a real problem later, and a log spammed with "frame 4821 ok" is a log nobody reads at all.

## Platform queries

```cpp
if (is_mobile()) { /* touch-sized hit targets */ }
platform();          // Platform::iOS, ::MacOS, ::Windows, ::Linux, ::Android, ::Web, ::Unknown
platform_name();     // "iOS", "macOS", ...
device_name();       // actual device model on mobile ("iPhone14,4"), else same as platform_name()
```

Use these to branch on *capability* (`is_mobile()` for touch-sized UI, `platform() == Platform::Windows` if you genuinely need a Windows-only code path like the HTTP note above), not to reimplement feature detection that already exists elsewhere in this API. If you're checking `platform()` to decide whether to call `haptic()`, stop — `haptic()` already no-ops safely everywhere it's not supported, the check is redundant and it's one more place your logic can drift out of sync with the engine's actual behavior.
