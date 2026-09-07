# Crash reporting

Not Unreal's crash reporter — there's no server, no upload, nothing ever leaves the device. When the game crashes (segfault, abort/assert, a floating-point exception, an uncaught C++ exception), this writes a plain text file explaining what happened, and — on by default — shows a native "the game crashed" dialog. Both are automatic the moment you construct an `App`; there's nothing to opt into.

```cpp
App app{{.title = "MyGame", .width = 1280, .height = 720}};   // crash handling is already live after this line

set_crash_context("level", "3");            // shows up in a report if one happens
set_crash_context("player_hp", "42");

// crash_log_dir() if you want to tell the player where to find reports,
// or point them at it from a settings/support screen
log_info("crash reports save to: " + crash_log_dir());
```

## What a report actually contains

```
Thistle crash report
time: 20260907-224302
platform: macOS
device: Stanislas' MacBook Air
reason: Segmentation fault: 11

context:
  level: 3
  player_hp: 42

backtrace:
  0   MyGame    0x0000000104137960 _ZN...crash_backtrace... + 80
  ...

last log lines:
  [info] loaded level 3
  [warn] enemy count exceeded pool size, reusing oldest
  ...
```

Timestamp, platform, device, what actually happened, whatever `set_crash_context()` calls you made, a best-effort stack trace, and the last ~50 lines from the engine's own log buffer (the same one `THISTLE_DEBUG`'s in-app overlay reads) — free context about what was happening right before things went wrong, since you were probably already calling `log_info`/`log_warn` for other reasons.

## The popup

On by default; turn it off with `set_crash_popup(false)` if you want to build your own crash UI, or you're running automated tests where a blocking system dialog would just hang the run.

**Why this never touches Thistle's own renderer:** the crash might have *been* your rendering code. Trying to draw a new UI from inside the already-crashed graphics context is exactly the kind of extra risk a crash handler should avoid, so this doesn't attempt it. Instead:

- **Windows** calls the OS's native `MessageBoxA` directly — a separate system surface (`user32.dll`), not dependent on your app's window or graphics state at all.
- **macOS** spawns a fresh, fully independent `osascript` process to show a native alert. This is the same principle real crash reporters (Crashpad, Breakpad) use: a genuinely separate, uncorrupted process instead of trying to do UI work inside the dying one.
- **Linux** tries `zenity` if it's installed; if it isn't, this silently does nothing — the report file is unaffected either way, this only touches the popup.
- **iOS never shows one.** By the time any of your code could run after a crash, iOS has already killed the app and returned to the home screen — there is no hook for a crashed app to display anything. The report file still gets written to the sandbox for you to pull off the device later (Xcode's device file browser, or wherever your app exposes its Documents folder); there's just no in-the-moment dialog possible on that platform. This is a genuine OS limitation, not something left unimplemented.

### The bug that shipped once during this feature's own testing, and the fix

The macOS/Linux popup path forks a child process and `execl()`s into `osascript`/`zenity`. The very first working version of this had a real, intermittent failure: right after `fork()`, the child still shared the crashing parent's process group, and the parent re-raises its signal (to let the OS's own crash handling continue) almost immediately afterward — often fast enough that the shell/terminal's process-group cleanup on the parent's abnormal death would kill the child before it finished `execl()`, so the popup silently never appeared. It looked like it "sometimes worked" (it did, when something coincidentally slowed the parent down — adding debug logging around the fork masked the bug at first by accident, which is exactly the kind of false signal to distrust). The fix is `setsid()` in the child immediately after `fork()`, before `execl()` — it moves the child into its own new session and process group, fully detached from the parent's, so it survives the parent's death regardless of timing. Verified with and without the fix, side by side, before trusting it.

## Reading this honestly: async-signal-safety

Signal handlers (and Windows' `SetUnhandledExceptionFilter`, and `std::set_terminate`) run in a genuinely restricted context — most of the C++ standard library, including `malloc` itself, is not guaranteed safe to call there, because the crash could have interrupted the very same code mid-operation (e.g. mid-allocation), and calling it again can deadlock. This implementation does the same pragmatic thing every real-world game crash handler does: use the restricted-but-practical subset (plain C file I/O, `backtrace()`/`CaptureStackBackTrace()`, a `fork()+execl()` that's `setsid()`'d for full independence) and accept that on a sufficiently unlucky crash, the handler can occasionally fail to produce a report. It fails **open** — worst case is a missing report, never a hang, and never a second crash that erases the first one's information (see the re-entrancy guard in `thistle.cpp`, which makes sure a crash *during* crash handling gets out of the way instead of looping or double-writing).

## Testing it yourself

```bash
cmake -S . -B build -DTHISTLE_BUILD_SMOKETEST=ON
cmake --build build --target thistle_crash_smoketest
./build/thistle_crash_smoketest segv     # or: abort | throw
./build/thistle_crash_smoketest segv popup   # also shows the real popup
```

Not registered as a `ctest` — it's supposed to crash, and "did this pass" here means "did a report file appear with the right content," not "did the process exit 0." CI still *builds* it on all three desktop platforms (same `-DTHISTLE_BUILD_SMOKETEST=ON` + build-everything pattern the other example programs use), which catches compile/link regressions on Windows and Linux automatically — it just never executes it, for the same reason `thistle_smoketest` (the windowed one) never gets executed there either.
