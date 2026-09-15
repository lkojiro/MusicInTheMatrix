# Music in the Matrix

A terminal-native music visualizer. Two visual modes -- an FFT bar chart
and a Matrix-style digital rain -- both reacting to system audio (level
and detected beats), switchable live with the left/right arrow keys.

## Design decisions

- **Audio source**: system audio output (not the microphone) via
  [PortAudio](https://www.portaudio.com/), captured through a virtual
  loopback device. macOS has no built-in "listen to what's playing" input,
  so `AudioCapture` looks for an input device whose name matches a given
  substring (default: `"BlackHole"`) rather than opening the default mic.
  If no match is found, it fails with a clear error and a list of what
  input devices *are* available, instead of silently falling back to the
  mic and visualizing the wrong source. See **Capturing system audio**
  below for setup, or pass a different device-name substring on the
  command line to target a real microphone instead.
- **FFT**: [KissFFT](https://github.com/mborgerding/kissfft) v131, vendored
  under `third_party/kissfft/` (BSD-3-Clause, see `COPYING` there). Chosen
  over FFTW (GPL/commercial-licensed) to keep this project's licensing
  options open, and over a hand-rolled FFT to avoid owning subtle
  correctness bugs (bit-reversal, twiddle factors) for negligible
  performance benefit at these window sizes (hundreds of microseconds at
  most, nowhere near the bottleneck for a 60fps terminal redraw).
- **Rendering**: ncurses, which ships with macOS/Linux and handles terminal
  clearing/sizing cleanly.
- **Multi-window IPC**: a Unix domain socket carrying tiny JSON messages
  (via vendored [nlohmann/json](https://github.com/nlohmann/json)
  `third_party/json/`, MIT-licensed) between one controller process and any
  number of subordinate visualizer windows. Chosen over WebSocket/ZeroMQ
  since everything here is same-machine IPC with no need for a browser or
  remote client yet -- no new external dependency, lowest latency, easy to
  debug with `nc -U`. If a browser dashboard or remote control becomes a
  real need later, bridge it with a small proxy in front of this same
  protocol rather than replacing the transport.
- **Window spawning**: AppleScript automation of Terminal.app
  (`WindowLauncher` / `AppleScriptWindowLauncher`), chosen over tmux (panes
  aren't separate OS windows) and over a specific terminal emulator's
  native remote-control API (locks the project to that emulator). The
  interface is deliberately generic so an iTerm2- or kitty-native launcher
  can be swapped in later without touching the controller or protocol.
- **Beat/tempo detection**: hand-rolled (spectral flux + adaptive
  threshold, in `BeatDetector`), not a library. The well-established
  real-time-capable options were checked directly against their license
  files, not memory: [aubio](https://github.com/aubio/aubio) and
  [BTrack](https://github.com/adamstark/BTrack) are GPLv3,
  [Essentia](https://github.com/MTG/essentia) is AGPLv3 -- all
  incompatible with this project's permissive-licensing stance (the same
  reason FFTW was ruled out for FFT). Unlike FFT, though, beat detection
  has no single "correct" answer to get subtly wrong -- it's an
  inherently tunable heuristic, so implementing the standard technique
  ourselves is a comfortable choice here, not a compromise. (A genuinely
  permissive DSP library, [cycfi/Q](https://github.com/cycfi/q), Boost
  license, was also checked -- it has the right primitives, `envelope.hpp`
  /`peak_picker.hpp`/`onset_gate.hpp`, but no assembled tempo/beat tracker,
  so it wouldn't have saved the actual beat-detection logic anyway.)
  Stops at discrete beat *pulses*, not a continuous BPM number -- turning
  pulses into a stable tempo estimate is meaningfully harder (the classic
  failure mode is locking onto double or half the real tempo) and isn't
  needed to drive reactive visuals.

## Dependencies

- CMake >= 3.16
- A C++17 compiler
- PortAudio (not vendored — install via Homebrew):
  ```sh
  brew install portaudio
  ```
- ncurses (already present via the macOS SDK / most Linux distros)

## Building

```sh
cmake -S . -B build
cmake --build build
```

## Capturing system audio

macOS input devices are microphones by default -- there's no built-in way
to open "whatever's currently playing" as an input. This project instead
targets a virtual loopback driver: [BlackHole](https://github.com/ExistentialAudio/BlackHole),
free and open-source (unlike Rogue Amoeba's Loopback), and it never
triggers a mic-permission prompt since it isn't a real microphone.

1. Install it: `brew install blackhole-2ch`
2. Open **Audio MIDI Setup** (Spotlight it), click **+** in the bottom
   left, choose **Create Multi-Output Device**, and check both your
   normal output (e.g. "MacBook Air Speakers") and "BlackHole 2ch". This
   way system audio still plays out loud *and* feeds BlackHole.
3. In **System Settings > Sound > Output**, select that new Multi-Output
   Device.
4. Run the visualizer -- it looks for "BlackHole" by default:
   ```sh
   ./build/mitm
   ```

To target a different device instead (e.g. a real microphone), pass a
substring of its name as the first argument:

```sh
./build/mitm "MacBook Air Microphone"
```

If the given name doesn't match anything, the program exits immediately
with an error listing the input devices it did find.

## Running

```sh
./build/mitm                              # starts in bars mode, green
./build/mitm --visual=matrix              # starts in Matrix rain mode
./build/mitm --visual=checkerboard        # starts in Checkerboard mode
./build/mitm --color=purple               # any mode, any color (default green)
```

`--color` picks one of red/green/blue/yellow/cyan/magenta(purple)/white --
every renderer's brightness ramp is generated from it (see **Shared
infrastructure** below), rather than each hardcoding its own green. It
only sets *this* window's own color, though -- see the next paragraph for
what a window spawned via `n` gets instead.

Press `q` to quit, the **left/right arrow keys** to cycle live between all
three modes (`--visual` only picks the starting one now), or **`n`** to
spawn a brand new window (same audio device, starts in bars mode). Each
window has its own color rather than inheriting one from whoever spawned
it -- a window's `BaseColor` lives in its own process's `GlobalConfig`
(see **Shared infrastructure** below), so nothing here is actually shared
across windows, and a spawned window gets a different preset from a fixed
rotation (`colorNameForIndex()` in `color_scheme.hpp`) rather than
whatever `--color` the spawning window happened to be started with.
Expect a brief (~0.3-1s) pause in the window you pressed `n` in while the
new one opens.

## Multi-window: one host, any number of children

Every plain `./build/mitm` invocation auto-discovers whether a host is
already running and either becomes the host or joins as a child of it --
see `main.cpp` and `host_discovery.hpp`. There's no separate "start the
orchestrator" step anymore: run the binary however many times you want,
however you want (a fresh terminal you opened yourself, or `n` from
inside an existing window), and exactly one of them ends up as host.

- **Discovery**: a hidden lock file at `/tmp/.mitm-host` holds the host's
  Unix socket path and pid, written by whichever instance becomes host.
  Every other instance reads it, checks the pid is actually alive
  (`kill(pid, 0)`), and attempts to connect -- if that succeeds, it joins
  as a child (reusing the same `runSubordinate` the explicit
  `--subordinate` path uses, just with an auto-assigned id -- its own pid,
  rather than a controller-assigned sequential number). If the file is
  missing, the pid is dead, or the connection fails, it becomes the host
  itself. `UnixSocketServer` already cleans up a stale socket file at its
  bind path, so a crashed host's leftovers don't need special handling.
- **The host renders too, and every window is live simultaneously.**
  Unlike the old `--controller` mode (a pure background orchestrator that
  rotates which one window is "active" and idles the rest), the host is a
  real, visible window -- standalone's old render loop merged with the
  orchestration logic -- and every connected child is activated the
  moment it connects and never deactivated. No rotation, no idling: as
  many windows as are open all show live visuals at once.
- **Windows are titled and numbered for human readability, not by pid.**
  Each window's terminal tab title (set via the xterm OSC 0 escape
  sequence, `curses_util::setWindowTitle()` -- consumed by the terminal
  emulator itself, safe to call any time, never touches the character
  grid) shows a small, gap-free display number instead of its real pid:
  the host is always `mitm — HOST (0)`, and children are
  `mitm — CHILD (1)`, `(2)`, etc., in connection order. The host tracks
  this order itself (`windowOrder` in `host.cpp`, seeded with its own id
  at index 0 and never removed) and rebroadcasts the full ordered id list
  via `ipc::ConfigMessage` on every connect/disconnect; each child just
  finds its own real id's position in that list and retitles itself to
  match. Removing a disconnected id from the middle of that list
  naturally closes the gap for everyone after it -- a child never needs
  an explicit "renumber" message, just the same broadcast it already
  gets. The real id (pid, or a spawn-token-correlated window handle)
  never changes and is still what every internal map is keyed by; only
  the label shown to the user does.
- **`n` works from any window, host or child.** Only the host actually
  holds a `WindowLauncher` and the spawn-token bookkeeping needed to
  later `close()` what it spawns, so a child doesn't spawn directly --
  pressing `n` there sends the host an `ipc::SpawnRequestMessage` and the
  host spawns on its behalf (see `spawnChild()` in `host.cpp`, called
  both from the host's own `n` and from a child's relayed request). The
  new window always ends up a sibling of whichever window requested it --
  a child of the *host*, not of the requesting child -- since there's
  only ever one orchestrator per process tree. (The explicit
  `--controller` path doesn't listen for this message yet, so `n` in one
  of its subordinates is silently a no-op there.)
- **Shutdown cascades from the host.** Quitting the host (`q` or Ctrl-C)
  sends every child a shutdown message and waits for them to actually
  disconnect before closing their windows -- see the gotcha below for why
  that ordering matters. A child quitting on its own only closes itself.
  One honest limit: the host can only explicitly close() a window it
  spawned itself via `n` (see the correlation mechanism below); a window
  the user launched by just typing `./build/mitm` in their own fresh
  terminal still gets told to shut down and its *process* still exits
  cleanly, but the host has no handle to close that window with.

### Two real gotchas this surfaced

**Closing a window with a live foreground process silently fails.** A
Terminal window whose shell still has a *foreground process* running
(including a child's `mitm` process, or even something as simple as
`cat`) pops Terminal's "this will terminate the running process"
confirmation sheet on `close` -- and AppleScript's `close` command returns
successfully without an error even though the window doesn't actually
close, since nothing is there to answer that dialog headlessly. The fix
is ordering: always wait for the child to actually disconnect (which
happens right after it tears down ncurses, just before the process fully
exits) before calling `close()`. Even that's not quite enough on its own,
though -- disconnecting isn't the same instant as the process actually
exiting (there's still PortAudio teardown, `Pa_Terminate()` in particular,
between the two, which can take a real, non-negligible moment), so
`runHost` adds a flat ~400ms grace delay after detecting disconnect before
calling `close()`, on top of waiting for the disconnect itself. Separately:
Terminal's dictionary doesn't support `window id N` as an element
reference (raises `-1728`, "Can't get window id N") despite `id` being a
normal readable property -- `AppleScriptWindowLauncher::close()` walks the
window list and compares ids explicitly instead.

**Correlating a spawned window's AppleScript handle with its eventual
hello is a genuine two-thread race, not just sequencing.** The natural
first instinct is: call `launcher.spawn()`, get a handle back, remember
it, and match it up once that window's hello arrives. But the *spawned
process itself* starts running concurrently with the host's own thread,
which is still blocked inside `launcher.spawn()` waiting out
AppleScript's ~0.3s+ delay -- easily enough time for the new window to
launch, discover the host via the lock file, connect, and send its hello
*before* the spawning thread has even gotten a handle back to store.
`runHost` handles both arrival orders with two maps: `pendingHandles`
(spawn finished first, token -> handle) and `pendingHellos` (hello arrived
first, token -> child id) -- whichever side loses the race leaves a note
for the other to complete the correlation. Caught by writing debug output
to a separate file (never to stdout/stderr while curses owns the
terminal -- redirecting a curses process's own stdout for logging
silently breaks its actual rendering, a mistake made once already during
this investigation) and simply reading back the order hello/spawn events
actually arrived in.

Requires the same Terminal.app automation permission as any AppleScript
tool -- macOS will prompt for it the first time a host spawns a window;
grant it under **System Settings > Privacy & Security > Automation**.

### Explicit --controller/--subordinate (unchanged, still available)

The original explicit orchestration mode still exists, unaffected by any
of the above, as a manual/scriptable alternative that doesn't rely on
auto-discovery:

```sh
./build/mitm --controller [--count=N] [--socket=PATH] [--visual=bars|matrix|checkerboard] [--color=NAME]
                                           # spawns N Terminal windows up front and rotates
                                           # which one is "active" every 5s, without rendering
                                           # anything itself (unlike the host).

./build/mitm --subordinate --id=N --socket=PATH [--device=NAME] [--visual=...] [--color=NAME]
                                           # what --controller launches in each window.
```

Each subordinate runs its own independent `AudioCapture` → `FftProcessor`
→ `BeatDetector` pipeline and just gates whether it actually renders based
on activate/deactivate messages from the controller -- no audio or frame
data crosses the socket, only tiny control messages (`hello`, `activate`,
`bye`, `shutdown`, `config`), which keeps the protocol trivial regardless
of how many windows are running.

## A real ncurses rendering bug

Checkerboard mode's color rendering hit a genuinely strange bug worth
recording: intermittently, cells that shouldn't be lit (off the current
checkerboard parity) would render fully bright -- indistinguishable from
a real hit, consistently affecting only one row-parity at a time, and
never occurring in the plain `#`/space fallback (no color).

The investigation ruled out the code, not just the terminal, before
landing on a fix:

1. **Cell-selection logic** (which cells get chosen to light) was
   verified correct three separate ways: a full-grid flash, 250 frames of
   the sparse random-selection path with decay disabled so any wrong hit
   would stay visible, and forcing the plain-character fallback so the
   pattern could be read back as text via AppleScript and diffed against
   the expected checkerboard by hand. All three came back clean.
2. Switching the render call from `attron()`/`mvaddch()`/`attroff()` to
   `mvchgat()` (ncurses' purpose-built "recolor this span" primitive) made
   no difference -- ruled out that specific API.
3. Throttling the actual screen refresh rate from ~125Hz to ~31Hz (while
   the simulation kept updating at full rate) made no difference -- ruled
   out redraw throughput/timing.
4. The decisive test: reading back ncurses' *own internal state* via
   `mvinch()`/`PAIR_NUMBER()` -- not the terminal's rendering, ncurses'
   record of what it thinks is on screen -- across multiple snapshots with
   realistic overlapping intensities. Every off-parity cell held pair 20
   (correctly off); every on cell held a valid on-pair. Zero exceptions.
   That meant the bug was downstream of anything callable from this
   codebase: either in ncurses' terminfo-driven translation of correct
   internal state into escape codes, or in Terminal.app's rendering of
   those codes.

**The fix**: call `redrawwin(stdscr)` immediately before `refresh()`,
forcing ncurses to do a full non-differential repaint every frame instead
of computing a minimal diff against what it believes is already on the
terminal. This resolved it. The working theory is that ncurses' internal
change-tracking optimizer was computing an incorrect diff (or Terminal.app
was misinterpreting it) specifically for this renderer's background-color,
high-pair-number, full-screen-every-frame update pattern -- the first
renderer in this project doing all three of those at once (Matrix rain
only colors foreground text, using pairs 1-8, not background colors with
pairs 20+). `redrawwin()` is now load-bearing, not a style choice; see the
comment at its call site in `checkerboard_renderer.cpp`.

## Shared infrastructure

Two small modules exist purely as scaffolding for future effects, not
because anything currently needs their full capability:

- **`color_scheme.hpp`**: `BaseColor` (an xterm-256 color-cube coordinate,
  0-5 per channel) plus `makeBrightnessRamp()`, which generates an N-step
  brightness ramp from one base color by scaling its channels down toward
  (not quite to) zero. Every renderer's palette is now generated from this
  instead of each hardcoding its own ramp -- `CheckerboardRenderer`'s
  ramp is a direct swap; `MatrixRainRenderer` keeps its bespoke near-white
  pulse-head and dark-gray tail steps (neither is really "about" the base
  hue) and only derives its middle 5 levels this way. The default
  (green, `{0,5,0}`) reproduces the exact palette indices that used to be
  hardcoded, so nothing changed visually unless `--color` is passed.
  `parseColorName()` maps `--color`'s named presets (red/green/blue/
  yellow/cyan/magenta/white) to a `BaseColor`; `basicAnsiColorIndex()`
  picks the nearest of the 7 basic ANSI colors for terminals without a
  256-color palette, where a ramp isn't possible at all.
  `colorNameForIndex()` maps the Nth auto-spawned window (0-based, wraps,
  green excluded so window 0's likely default color isn't the first
  repeat) to a preset name -- how `spawnChild()` in `host.cpp` and the
  `--controller` spawn loop in `controller.cpp` give each window its own
  color by default instead of forwarding the spawning window's `--color`.
  One real gotcha this surfaced while testing: both `CheckerboardRenderer`
  and `MatrixRainRenderer` read `GlobalConfig::baseColor` exactly once,
  in their constructor (`setupColors()`), not live per frame -- so a
  screenshot of two overlapping windows can look like one window's color
  bled into another's when really it's just the terminal profile's window
  transparency showing a different-colored window through from behind;
  worth remembering before treating an overlapping-window screenshot as
  evidence of a color bug.
- **`global_config.hpp`**: a thread-safe, process-wide `GlobalConfig`
  (`getGlobalConfig()`/`setGlobalConfig()`/`updateGlobalConfig()`) holding
  `baseColor` (consumed by every renderer's constructor) and
  `windowCount`/`windows`. The latter two aren't read by any renderer
  (every window still renders live simultaneously, so nothing needs "how
  many windows are there" to make a drawing decision), but they're no
  longer pure scaffolding either: `windows`' *order* is exactly what
  drives the human-readable window-title numbering described in
  **Multi-window** above -- a child's reader thread derives its own
  display number by finding its id's position in this same list, not
  just mirroring it for its own sake. Populated differently per mode: the
  host derives it from `windowOrder` (its own id first, then children in
  connection order) and broadcasts an `ipc::ConfigMessage` to every
  connected child whenever the set changes (a connect or disconnect --
  see `broadcastConfig()` in `host.cpp`), which a child's reader thread
  applies via `updateGlobalConfig()`. Since every plain launch now goes
  through host discovery (see **Multi-window** below), this is no longer
  the isolated gap it once was -- the one honest limit left is that a
  window the user launches manually (not via `n`) still isn't something
  the host can explicitly close, only tell to shut down; see that section
  for why.

## Project layout

```
CMakeLists.txt          Top-level build config
cmake/FindPortAudio.cmake   Manual PortAudio lookup (no bundled CMake module, no pkg-config assumed)
include/mitm/            Public headers
src/                      Implementation
third_party/kissfft/      Vendored KissFFT source (pinned to v131)
third_party/json/        Vendored nlohmann/json single header (pinned to v3.11.3)
```

## Current state (MVP)

- `AudioCapture`: opens an input device matched by name (a BlackHole
  loopback device by default, or any device you point it at), keeps a
  ring buffer of the most recent samples, downmixing stereo to mono.
- `FftProcessor`: applies a Hann window, runs a real FFT via KissFFT, and
  groups bins into log-spaced magnitude buckets (linear bucketing would
  leave most bars flat, since musical energy skews toward low frequencies).
- `BeatDetector`: RMS loudness (adaptive running-max normalized) plus
  spectral-flux beat pulses (adaptive mean+stddev threshold, refractory
  period) -- see the license writeup above for why this is hand-rolled.
- `TerminalRenderer` (bars mode): draws one stacked column of `#` per
  bucket, sized to the current terminal, each capped with a peak-hold `_`
  marker that snaps up instantly and decays back down over time; a
  constant `_` baseline along the bottom row keeps quiet audio from
  looking totally blank.
- `MatrixRainRenderer` (Matrix rain mode): falling character strings,
  spawning above the top edge and despawning past the bottom, fixed
  length chosen randomly at spawn, brightest at the head fading toward
  the tail. Reactive to audio: loudness continuously scales spawn density
  and brightens the whole scene; each detected beat spawns a burst of new
  strings and brightness-pulses a loudness-scaled subset of existing ones
  (see the constants at the top of `matrix_rain_renderer.cpp` for current
  tuning). Runs fine with no audio at all -- just non-reactive rain.
- `CheckerboardRenderer` (Checkerboard mode): a fixed grid (4 cols x 8
  rows, sized evenly off the current terminal dimensions -- see
  `evenSpan()`) where cells start off (black) and only cells on the
  classic alternating checkerboard parity are ever eligible to light.
  Purely beat-triggered, not continuously loudness-reactive: each beat
  lights 1-4 cells (count scales with loudness) or, above a "particularly
  loud" threshold, the entire alternating pattern at once. Each lit cell
  independently rolls one of three fade-to-black styles (`FadeStyle` in
  `checkerboard_renderer.hpp`): a constant dim-down (`Linear`), a rapid
  on/off strobe that settles into a slow fade (`Flicker`), or a dim that
  shrinks the lit area inward -- peeling off the outermost ring of
  characters -- as it darkens (`Shrink`). See **A real ncurses rendering
  bug** below -- its color path looks nothing like a naive attron/mvaddch
  loop because of it.
- `curses_util`: shared ncurses lifecycle (`init()`/`teardown()`, owned
  once per run so all three renderers can coexist), `pollInput()` (one
  `getch()` per frame recognizing `q`, the arrow keys, and `n`), and the
  shared idle screen for windows that aren't currently active.
- `visual_mode.hpp`: the `VisualMode` enum and `next`/`prev`/`parse`
  helpers arrow-key switching cycles through.
- `main.cpp` / `args.cpp`: parse CLI args and dispatch to host discovery
  (the default), or the explicit `--controller`/`--subordinate` modes.
- `host_discovery.hpp`: the `/tmp/.mitm-host` lock file read/write/
  liveness-check helpers `main.cpp` uses to decide host-vs-child.
- `host.cpp`: `runHost` -- standalone's old single-window render loop
  merged with orchestration (accept children, activate each on connect,
  broadcast `GlobalConfig`, cascade shutdown). See **Multi-window** above.
- `unix_socket.cpp`, `ipc_protocol.hpp`: the length-prefixed JSON transport
  and its tiny message schema.
- `window_launcher.cpp`: `AppleScriptWindowLauncher`, spawning/closing
  Terminal.app windows, plus `currentExecutablePath()`.
- `controller.cpp` / `subordinate.cpp`: the explicit orchestration mode
  (see **Multi-window** above) -- `runSubordinate` is also what a
  discovered child runs, just with different args.

## Known rough edges / next steps

- Bucket count is fixed at 64 rather than derived from terminal width.
- Magnitude normalization is a hardcoded empirical constant — works
  reasonably for typical music/system-audio input but isn't adaptive
  (no AGC / running max).
- No smoothing/decay between frames on bars themselves (only the peak
  cap decays) — bars can look jittery on percussive audio. A simple
  exponential falloff per bucket would help. Matrix rain's brightness
  pulses already do this kind of decay.
- Single translation unit for the render loop — fine for MVP, but audio
  capture and rendering are already on separate threads implicitly (the
  PortAudio callback vs. the main loop), worth keeping in mind if this
  grows.
- The host/child default flow has no rotation at all -- every window
  renders live simultaneously (see **Multi-window** above). The explicit
  `--controller` mode still has the original fixed 5-second round-robin
  (`kRotateInterval` in `controller.cpp`, one-window-active-at-a-time) if
  that's ever wanted again as a deliberate choice, with no audio-driven
  "jump on a beat/transient" logic and no way to reconfigure the interval
  without editing the constant. Dynamically adding/removing windows
  mid-run works for the default host/child flow (children can connect or
  disconnect at any time via `n` or manual launches) -- it's only the
  explicit `--controller` mode that still spawns a fixed set once at
  startup.
- Every subordinate independently opens the same loopback device -- this
  worked fine in testing (CoreAudio/BlackHole supports multiple
  simultaneous readers), but hasn't been stress-tested with many windows.
- macOS/Terminal.app-only: `AppleScriptWindowLauncher` and
  `currentExecutablePath()` (via `_NSGetExecutablePath`) are both
  Darwin-specific. `WindowLauncher` is the seam for a Linux/other-emulator
  implementation later.
- Spawning is sequential and each `osascript` round-trip has real
  overhead, so `--count=N` startup time grows roughly linearly with N
  (~1-2s per window observed) -- fine for a handful of windows, would want
  parallel spawning before this scales much further.
- Since arrow keys can switch any window into bars mode at any time,
  audio failing to start no longer hard-fails the process (bars mode
  used to) -- it just prints a warning and bars mode shows as an empty
  baseline until/unless audio becomes available.
- The rotation timer, beat-detection constants, and Matrix rain tuning
  constants are all separate named constants with no shared config file
  or CLI overrides yet -- fine for now, but worth consolidating if there
  end up being many more knobs to tune.
- The host lock file path (`/tmp/.mitm-host`) is a single fixed,
  system-wide location, not configurable or scoped per-session -- there
  can only ever be one host (and one connected group of windows) on the
  whole machine at a time. Wanting two independent groups (e.g. for
  testing) currently means explicitly using `--controller`/`--subordinate`
  with a distinct `--socket` for the second group instead.
- The host can only explicitly `close()` a window it spawned itself via
  `n` (see the spawn-token correlation in **Multi-window** above); a
  window the user launches manually in their own fresh terminal still
  gets told to shut down and its process still exits cleanly on host
  shutdown, but the host has no AppleScript handle to close that specific
  window with, so it can be left showing "[Process completed]" instead of
  actually closing.
