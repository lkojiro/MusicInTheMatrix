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
./build/mitm                              # starts in Bars Left mode, green
./build/mitm --visual=bars-right          # bucket 0 at the right edge instead
./build/mitm --visual=bars-middle         # bucket 0 centered, mirrored outward
./build/mitm --visual=matrix              # starts in Matrix rain mode
./build/mitm --visual=checkerboard        # starts in Checkerboard mode
./build/mitm --visual=oscilloscope        # starts in Oscilloscope (time-domain waveform) mode
./build/mitm --visual=bands               # starts in Bands (low/mid/high VU meters) mode
./build/mitm --color=purple               # any mode, any color (default green)
```

`--color` picks one of red/green/blue/yellow/cyan/magenta(purple)/white --
every renderer's brightness ramp is generated from it (see **Shared
infrastructure** below), rather than each hardcoding its own green. It
only sets *this* window's own color, though -- see the next paragraph for
what a window spawned via `n` gets instead.

Press `q` to quit, the **left/right arrow keys** to cycle live between all
five modes (`--visual` only picks the starting one now), or **`n`** to
spawn a brand new window (same audio device, starts in Bars Left mode). Each
window has its own color rather than inheriting one from whoever spawned
it -- a window's `BaseColor` lives in its own process's `GlobalConfig`
(see **Shared infrastructure** below), so nothing here is actually shared
across windows, and a spawned window gets a different preset from a fixed
rotation (`colorNameForIndex()` in `color_scheme.hpp`) rather than
whatever `--color` the spawning window happened to be started with.
Expect a brief (~0.3-1s) pause in the window you pressed `n` in while the
new one opens.

All of the above -- spawning, closing, mode, color -- is also available
from a browser: the host serves a small control panel at
`http://127.0.0.1:7887` (override with `--web-port=N`). See **Web control
panel** below.

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
- **The host doesn't render a visualizer; every child window does, live
  and simultaneously.** The host is a dedicated orchestrator: it runs the
  one `AudioCapture`/`FftProcessor`/`BeatDetector` pipeline for the whole
  setup and broadcasts its output to every connected child (see **Audio
  pipeline** below), but its own terminal just shows a small status
  screen (`curses_util::drawHostStatus()` -- app name, version, connected
  windows, quit/spawn key prompts), not a visualizer. Every connected
  child is activated the moment it connects and never deactivated -- no
  rotation, no idling, as many windows as are open all show live visuals
  at once.
- **Windows are titled and numbered for human readability, not by pid.**
  Each window's terminal tab title (set via the xterm OSC 0 escape
  sequence, `curses_util::setWindowTitle()` -- consumed by the terminal
  emulator itself, safe to call any time, never touches the character
  grid) shows a small, gap-free display number instead of its real pid:
  children are titled `mitm — CHILD (0)`, `(1)`, etc., in connection
  order (the host doesn't occupy a slot in this numbering -- it isn't a
  visualizer window itself). The host tracks this order (`windowOrder` in
  `host.cpp`) and rebroadcasts the full ordered id list via
  `ipc::ConfigMessage` on every connect/disconnect; each child just finds
  its own real id's position in that list and retitles itself to match.
  Removing a disconnected id from the middle of that list naturally
  closes the gap for everyone after it -- a child never needs an explicit
  "renumber" message, just the same broadcast it already gets. The real
  id (pid, or a spawn-token-correlated window handle) never changes and
  is still what every internal map is keyed by; only the label shown to
  the user does.
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
./build/mitm --controller [--count=N] [--socket=PATH] [--visual=bars-left|bars-right|bars-middle|matrix|checkerboard|oscilloscope|bands] [--color=NAME]
                                           # spawns N Terminal windows up front and rotates
                                           # which one is "active" every 5s. Doesn't run the
                                           # central audio pipeline the default host does (see
                                           # below), so subordinates spawned this way render
                                           # without audio reactivity for now.

./build/mitm --subordinate --id=N --socket=PATH [--visual=...] [--color=NAME]
                                           # what --controller launches in each window.
                                           # --device is meaningless here now --
                                           # subordinates don't open an audio device
                                           # themselves; only the host does.
```

The (default, auto-discovered) host runs the *only* `AudioCapture` →
`FftProcessor` → `BeatDetector` pipeline in the whole setup, and
broadcasts its output to every connected subordinate once per processing
tick via `ipc::AudioFrameMessage` (see `AudioEventSink` in
`audio_event_sink.hpp`) -- subordinates just render whatever they most
recently received, gated on activate/deactivate messages from the host,
rather than capturing/analyzing audio themselves. Every other message
here stays tiny and event-driven as before (`hello`, `activate`, `bye`,
`shutdown`, `config`, `spawn_request`, `set_mode`, `set_color`, `state`);
`audio_frame` is the one exception, sent at a real cadence regardless of
how many windows are running. (The explicit `--controller` path below
doesn't run this pipeline, so subordinates spawned through it don't get
audio reactivity yet.)

## Web control panel

The host also serves a small web UI, at `http://127.0.0.1:7887` by
default (override with `--web-port=N`) -- see `control_server.hpp`. One
page, no build step, no external requests (everything it does is a
`fetch()` back to this same server): a row per connected subordinate
window (the host itself doesn't appear here -- it doesn't render
anything, so there's no mode/color of its own to control; quitting it is
a `q` in its own terminal, which still cascades to every subordinate),
each with a mode dropdown, a color dropdown, and a Close button, plus a
"+ Spawn Window" button and a "🎲 Randomize All" one. It polls `GET
/api/state` once a second and just
re-renders; every action (`POST /api/spawn`, `/api/close`, `/api/mode`,
`/api/color`, the latter three taking a small JSON body keyed by the
window's *display* number, never its real pid) fires and waits for the
next poll to confirm it, rather than updating optimistically itself --
simple, at the cost of up to ~1s before a change is visibly confirmed in
the browser. Randomize All doesn't add a route of its own -- it's
client-side only, fetching a fresh window list (so a window that just
connected or disconnected, outside this page's own poll cycle, isn't
skipped or 404s) and firing an independent `/api/mode` + `/api/color`
pair per window, each picked separately so windows don't all land on the
same mode/color together.

Two real gotchas this surfaced:

- **A poll landing mid-interaction could yank a dropdown out from under
  you.** `render()` replaces every row's innerHTML wholesale, `<select>`
  elements included -- destroying and recreating one out from under an
  open (or just-changed-but-not-yet-confirmed) dropdown closes it and
  can make the choice look like it silently reverted, since the rebuilt
  element gets its selected option from whatever state was last polled,
  not the pick that hadn't been confirmed back yet. Fixed by skipping a
  poll's render entirely while any `<select>` in the table has focus,
  and catching up immediately (rather than waiting up to 1s) via a
  `focusout` listener the moment focus leaves one.
- **A taken port crashed the whole host, not just the web panel.**
  `ControlServer`'s constructor throws on `bind()` failure (e.g. a
  previous host that didn't shut down cleanly still holding the port),
  and that exception was going uncaught -- turning a missing nice-to-have
  into a dead visualizer. Same fix as audio's best-effort startup: wrap
  the construction in try/catch and keep running without the web panel,
  rather than let an unrelated subsystem's failure take down rendering.

Only the host runs this -- it's the only process with a view of every
window, so it's the natural single control point (same reasoning as `n`
already being relayed to the host from a child, see above). The host
doesn't have a mode/color of its own to control (it doesn't render
anything), so every `/api/mode`/`/api/color` request just means sending
the target child the new `ipc::SetModeMessage`/`ipc::SetColorMessage`.
Each color-carrying renderer (`CheckerboardRenderer`/`MatrixRainRenderer`)
only ever reads `GlobalConfig::baseColor` once, in its constructor -- so
applying a live color change on the *child* that receives it means
explicitly re-running `setupColors()` (public for exactly this) rather
than just mutating the config and expecting it to show up. That re-run
has to happen on the thread that owns the ncurses session, never a
background reader thread (same rule as every other ncurses call in this
codebase), which is why `subordinate.cpp`'s main loop diffs a
locally-tracked "last applied" snapshot once a frame rather than acting
the moment the message arrives.

Mode is simpler -- `draw()` already reads the current mode fresh every
frame -- but a window's mode can *also* change locally (the arrow keys
still work), and the host has no way to know that happened unless the
window tells it. Rather than have two senders touch the same child
connection (the reader thread confirming a remote command, the main
loop reporting a local key press -- a real risk of interleaving two
messages' bytes on the wire, since `UnixSocketConnection::send()` isn't
internally synchronized), only the main loop ever sends: every frame it
diffs the window's actual current mode/color against what it last told
the host, and sends `ipc::StateMessage` exactly when that diff is
non-empty. This one mechanism covers "arrow key changed it" and "a
remote command changed it" and "just connected, host doesn't know my
starting state yet" (the diff's initial state is deliberately
unmatchable) without three separate code paths.

`ControlServer` (`control_server.hpp`/`.cpp`) is a from-scratch,
deliberately minimal HTTP/1.1 server -- 127.0.0.1-only, one thread per
connection, no keep-alive, no chunked encoding -- in the same spirit as
`UnixSocketConnection`'s hand-rolled framing (see its doc comment): a
browser-compatible transport instead of the length-prefixed JSON one
everything else speaks, but built the same way rather than reaching for
a dependency.

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

## Semi-transparent terminal windows: opaque squares aren't a drawing bug

A user running mitm in a Terminal.app window with a translucent background
(opacity turned down in the profile) noticed every "empty" cell rendered
as an opaque black square instead of showing the desktop through, like
the terminal's own genuinely-blank areas do. The natural first guess --
and a completely reasonable one -- was that the renderers were
explicitly painting those cells `COLOR_BLACK` (an opaque RGB(0,0,0))
instead of leaving them alone, and that just not drawing there would fix
it.

That guess was half right: `CheckerboardRenderer` did explicitly repaint
every "off" cell `COLOR_BLACK`/`COLOR_BLACK` every frame, and every
`MatrixRainRenderer` color pair used `COLOR_BLACK` as its background --
neither renderer called `use_default_colors()`, so there was no way to
say "leave the terminal's own background alone" in the first place, only
"paint an opaque color" vs. "paint a different opaque color." Both were
switched to `-1` (the default-color sentinel, after `use_default_colors()`)
for exactly the cells/pairs that should show through.

It didn't fix it. Isolating why took two comparison tests, both against
the same translucent Terminal.app window and verified by direct
screenshot (not just "looks right by eye"):

1. **A totally blank, freshly-cleared shell prompt** in an identical
   translucent window showed the desktop through correctly -- confirming
   the translucency setting and the screenshot method both actually work,
   ruling out "the effect doesn't really apply" or "screenshots can't
   capture it."
2. **`vim`** -- a completely unrelated, stock ncurses application with
   zero code in common with this project -- opened in the same
   translucent window and showed the exact same flat opaque black
   background in its main editing area that mitm did.

Since `vim`'s renderer has nothing to do with this codebase, the only
thing it and mitm share is switching the terminal into its *alternate
screen buffer* (the `smcup`/`rmcup` terminfo capability `initscr()` uses
for any full-screen TUI app, so the shell's original scrollback is
restored on exit). That match, plus the blank-shell-prompt control
showing the desktop through correctly, points at the real cause:
**Terminal.app renders the alternate screen buffer opaquely regardless
of the window's transparency setting** -- a renderer-level behavior that
applies to any full-screen terminal app, not something happening in this
codebase's drawing code, and not fixable by changing what gets painted
or skipping paints entirely (both were tried; neither changed anything).

The `use_default_colors()`/`-1` change was kept anyway -- it's more
correct regardless (an explicit `COLOR_BLACK` and "the terminal's actual
default background" aren't the same thing even when they look the same,
and some other terminal emulator might not share Terminal.app's
alternate-screen-buffer behavior, though that's untested here) -- but it
doesn't fix transparency in Terminal.app specifically. If genuine
translucency behind a running mitm window matters, the terminal emulator
itself is the lever to pull, not this codebase.

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
  hue) and only derives its middle 5 levels this way; `TerminalRenderer`
  (bars mode) doesn't use `makeBrightnessRamp()` at all -- there's no
  "levels" concept for a single solid-colored `#`, so it just goes
  straight to `cubeToXterm256()` (or `basicAnsiColorIndex()`, same
  fallback as the other two) for one full-brightness pair. The default
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
  host derives it directly from `windowOrder` (children in connection
  order -- the host itself doesn't render, so it isn't part of this list)
  and broadcasts an `ipc::ConfigMessage` to every
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
  Bucket magnitudes then get an exponential gain boost that grows with
  frequency (`highFrequencyGain()`, 1x at the lowest bucket up to
  `kHighFrequencyBoost` at the highest) -- without it, bass buckets read
  as almost always maxed out and treble buckets barely register at all,
  since real audio carries far more raw energy at the low end than the
  high end regardless of how loud the treble actually sounds in context.
  This affects every bucket-driven mode (Bars, Bands), not just one,
  since it's applied once at the source.
- `BeatDetector`: RMS loudness (adaptive running-max normalized) plus
  spectral-flux beat pulses (adaptive mean+stddev threshold, refractory
  period) -- see the license writeup above for why this is hand-rolled.
- `TerminalRenderer` (Bars Left/Right/Middle modes): draws one stacked
  column of `#` per bucket, sized to the current terminal, each capped
  with a peak-hold `_` marker that snaps up instantly and decays back
  down over time; a constant `_` baseline along the bottom row keeps
  quiet audio from looking totally blank. `BarLayout` (see
  `terminal_renderer.hpp`) picks where bucket 0 sits and which way
  frequency increases from there -- `Left` (bucket 0 at the left edge,
  increasing rightward, the original layout) and `Right` (the same
  chart, mirrored left-right) are one screen-column slot per bucket;
  `Middle` centers bucket 0 and mirrors every other bucket the same
  distance out on *both* sides at once, so it needs roughly twice the
  slots for the same bucket count. Peak-hold state is keyed by bucket,
  not screen column, so it carries over cleanly across all three
  layouts. The `#` bars are colored from the window's
  `GlobalConfig::baseColor` (see **Shared infrastructure** below), same
  as the other two modes -- the peak-hold and baseline `_` markers stay
  the terminal's plain default foreground on purpose, so the color reads
  as "the bar" and not "the whole chart." Unlike Matrix rain and
  Checkerboard, there's no brightness ramp here -- no notion of
  "levels" to shade between, just the one solid full-brightness hue.
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
- `OscilloscopeRenderer` (Oscilloscope mode): a time-domain waveform
  trace across the full terminal, vertically centered. The trace always
  stretches to span the full terminal width -- the waveform data is
  linearly interpolated to exactly one value per screen column
  regardless of how its point count compares to the terminal's width,
  rather than one column per point and whatever's left over as padding.
  A thin line, not a filled bar chart: level segments between columns
  draw as `-`, and a rising or falling step draws as a `/` or `\` cap on
  a `|` run for the rows in between -- picked from the slope between
  each pair of consecutive columns, the same idea as a real scope's
  traced line; a dim `-` center row marks zero amplitude. Sourced from
  `AudioFrame::waveform` -- a `config::kWaveformPointCount`-point stride
  decimation of the host's raw capture window (see `downsampleWaveform()`
  in `host.cpp`), not the FFT buckets every other mode uses, since a
  waveform trace needs the actual time-domain shape rather than frequency
  magnitudes. Like Bars, just one solid full-brightness hue -- no
  brightness ramp.
- `BandMeterRenderer` (Bands mode): three vertical VU-meter-style
  progress bars side by side, each its own bordered box titled "Low",
  "Mid", or "High". Each bar is a stack of discrete segments (a
  classic LED VU meter look, not one continuous fill), lighting from the
  bottom up one segment at a time as that band gets louder. Segments are
  a fixed height (`kSegmentHeightRows` = 4 rows each, `segmentSpan()`)
  rather than a fixed *count*: how many segments a bar actually shows
  (`segmentCount_`) is worked out from the terminal's current height
  instead (`interiorHeight / kSegmentHeightRows`, floor division, at
  least 1), with any leftover rows collected into a blank margin above
  the topmost segment -- deliberately not the same "spread the remainder
  across the first few pieces" scheme `evenSpan()` uses for the 3 boxes'
  column widths, since a handful of segments ending up visibly taller
  than their neighbors at some terminal sizes read as a layout bug. That
  means a taller terminal shows *more* segments at the same size rather
  than the same segment count stretched taller, and a shorter one shows
  fewer rather than every segment getting squeezed thinner -- recomputed
  alongside the boxes themselves whenever a resize triggers a rebuild.
  `kSegmentHeightRows` = 4 was picked to match what a fixed 16-segment
  layout used to work out to at a 67-row terminal (a 65-row box interior
  divided 16 ways), the height this renderer's spacing originally looked
  best at, so that terminal height still renders pixel-for-pixel
  identically now. A segment gets a 1-row gap below it only when its
  (uniform) height is 2+ rows, so at any given terminal size either every
  segment the bar shows has a gap or none do, never some but not others
  (verified directly at terminal heights 67, 99, and 35: each renders the
  same 4-row-tall segments -- 3 fill rows plus a 1-row gap -- just with
  more or fewer of them as the interior grows or shrinks). Which of the 64
  log-spaced FFT buckets belong to which band (low <70Hz, mid
  70Hz-700Hz, high >700Hz) is worked out once at construction from
  `FftProcessor::bucketFrequencyRange()` -- a bucket's midpoint frequency
  decides which band it falls in -- not recomputed every frame. All 3
  bands fill to their own absolute level alone (`bandAbsoluteLevel()`:
  `bandEnergy()`, each band's raw bucket-magnitude sum, averaged by
  bucket count and clamped to `[0, 1]`) -- no percentage-of-total
  blending for Mid/High, so each bar reflects how loud that part of the
  spectrum actually is, not what share of the total spectrum it happens
  to make up. That fill fraction drives the lit-segment
  count directly, every frame, with no peak-hold smoothing of its own --
  the per-segment fade-out (below) is what keeps the bar from reading as
  flicker, not a second layer of smoothing on the count itself. It isn't
  truncated to a whole segment count before that, though: the one
  segment the level's fractional position actually falls inside targets
  proportional partial brightness -- however far up through that
  segment's own range the level sits, quantized to the nearest of the
  fade ramp's `kFadeLevels` steps -- rather than snapping straight to
  fully lit the instant the level reaches it, so the meter reads with
  `segmentCount_ * kFadeLevels` effective brightness increments instead
  of just `segmentCount_` whole-segment jumps.

  Segment color is a fixed zone by position, not the window's base
  color: the top ~1/8 of segments are always one hue, the next ~1/4
  another, the next ~1/4 another, and the rest (the bottom ~1/2) the last
  (`zoneForSegment()`, thresholds scaled proportionally to the bar's
  current `segmentCount_` rather than hardcoded absolute counts, each
  floored to at least 1 segment so every zone still gets a chance to
  appear even on a short bar) -- a VU meter's 4 zones are a fixed
  convention, not a cosmetic choice
  tied to the rest of the UI's color, so `setupColors()` never reads
  `GlobalConfig::baseColor` and this renderer ignores the web panel's
  color picker entirely. Which 4 hues fill those zones comes from one of
  5 fixed color schemes instead (`kSchemeZoneColors`): Default
  (green/yellow/orange/red bottom-to-top, the original, reading
  calm-to-intense), Warm (gold at the bottom through deep red at the
  top), Cool (bright cyan at the bottom through deep blue at the top),
  Greyscale (white at the bottom through dim grey at the top), and Candy
  (lime, turquoise, pink, and cyan -- fully saturated like Default, just
  a punchier, less natural hue set). Warm/Cool/Greyscale deliberately run
  the opposite direction from Default and Candy, brightest/most
  saturated at the bottom rather than the top. The up/down keys, "change
  color" for every
  other mode, cycle through these schemes here instead of picking a
  single hue (`nextScheme()`/`prevScheme()`, wired up in
  `subordinate.cpp`'s arrow-key handling) -- the one thing about this
  mode's colors that *is* switchable. Lit segments render as reverse
  video (`COLOR_BLACK` on the zone's color) rather than a colored `#`, so
  each reads as a solid block, same idea as Checkerboard's lit cells. A
  segment that stops being lit fades out
  over `kFadeDurationSeconds` (a 3-step brightness ramp per zone via
  `makeBrightnessRamp()`) instead of switching off instantly, tracked
  per segment (`segmentBrightness_` in the header) -- on a
  256-color terminal only; a basic terminal has no ramp to fade through
  and just turns off instantly, same restriction Checkerboard's own ramp
  has.

  Each segment's brightness is one continuous value in `[0, 1]`
  (`segmentBrightness_`), moved incrementally toward 1 (lit, over
  `kFadeInDurationSeconds`) or 0 (unlit, over `kFadeDurationSeconds`)
  each frame -- never reset from a fixed endpoint. That distinction
  mattered in practice: two independent "time since it turned off"/"time
  since it turned on" counters used to reset from their own fixed
  endpoint whenever a segment reversed direction mid-fade (unlit just
  long enough to start dimming, then lit again before finishing), so it
  would jump to "freshly off, fading in from black" instead of
  continuing from wherever it already was -- the actual cause of a
  flicker where a bar draining down and a bar filling up collided on the
  same segment. A single brightness value can't do that: reversing
  direction just means it starts moving the other way from right where
  it is.

  Fade-outs (descending) cascade top-to-bottom rather than firing in
  parallel, but not by making a segment wait for the one above it to
  *completely* finish -- just for it to drop by one brightness step
  (`kStepBrightness = 1 / kFadeLevels`, see the `aboveBrightness` gate in
  `draw()`'s pass 1). Once that one-step gate opens it stays open (the
  gated segment then descends in lockstep with the one above, not
  re-checking every frame), so the two keep a constant one-step
  brightness gap between them for the rest of the drain -- a bar
  draining from full reads as a gradient sweeping down the stack,
  several segments visibly at different brightness levels at once,
  rather than each LED fully blinking out before the next one even
  starts. Fade-ins (ascending) aren't gated by neighbors at all -- every
  segment is always free to rise as soon as it's lit. This is why
  `draw()` computes every segment's state in one pass (top segment to
  bottom, since segment N's descend decision needs segment N-1's
  already-finalized brightness from this same frame) and only then
  *paints* them in a second pass, bottom segment to top -- selling the
  "filling up/emptying out" read by drawing in the same bottom-up order
  the meter actually fills, even though the final pixels are
  order-independent (each segment owns disjoint rows) so this second
  pass's direction is cosmetic where the first pass's is load-bearing.

  Unlike every other renderer, this one keeps its 3 `newwin()` boxes
  alive across `draw()` calls instead of recreating them every frame --
  see the `windows_` member and its doc comment in
  `band_meter_renderer.hpp`. Recreating and re-bordering all 3 boxes on
  every ~8ms tick was the actual cause of a whole-window flicker
  (borders included, not just the fill): redrawing alternate-charset
  box-drawing characters at 125Hz is visibly janky on real terminals
  even though the final content never changes. Boxes are rebuilt --
  border, title, and all -- only if the terminal's size actually
  changes; every other frame only repaints each box's interior fill.
  That rebuild also erases and refreshes `stdscr` once, immediately
  before recreating the 3 boxes: `delwin()` only frees the `WINDOW*`
  structs, it doesn't erase whatever they'd already drawn on screen, so
  a resize that shrinks the terminal (or otherwise reflows the 3 boxes'
  widths) used to leave stale border/fill content lingering outside the
  new, differently-sized boxes' bounds. This clear only runs when a
  rebuild is already happening -- a real resize, or `invalidate()` after
  switching into this mode -- not every frame, so it doesn't reintroduce
  the flicker above.

  A bar's border also goes bold, and its title switches from lowercase
  ("low") to uppercase ("LOW"), whenever any segment in that bar's top
  3/4 is still lit or fading -- i.e. roughly once the bar is a quarter
  full -- back to normal/lowercase once none are -- read off
  `segmentBrightness_` (as just finalized by pass 1, for the same frame)
  rather than the raw level directly, so the border lags the level
  exactly as long as that segment's own fade-out does: the same "sticky"
  trailing feel the fade already gives the segments themselves, instead
  of snapping the instant the level dips under a quarter.
  Same reasoning as the rest of this section otherwise: `box()` (with
  `A_BOLD` on or off) is only re-run on the actual crossing
  (`boldBorder_`), not every frame. Since row 0 is a
  border row, re-running `box()` there erases the title label along with
  the rest of it, so `bandLabel()` regenerates it (cased to match, and
  bolded right along with the border) and redraws it immediately after.

  That "only rebuild on resize" check isn't enough on its own, though:
  switching away to a different mode (or the window going idle) fully
  repaints the whole screen without this renderer knowing, so switching
  back to Bands at the same terminal size used to leave stale characters
  from whatever was on screen before and no border at all, since draw()
  had no reason to think anything needed rebuilding. `invalidate()`
  (declared in the header) is the fix: it resets the "last built size"
  sentinel so the next `draw()` call rebuilds unconditionally, and
  `subordinate.cpp`'s render loop calls it whenever the *previous*
  frame's draw call wasn't this same renderer's (tracked via
  `lastDrawWasBandMeters`), i.e. exactly on the transition into this
  mode.
- `curses_util`: shared ncurses lifecycle (`init()`/`teardown()`, owned
  once per run so all three renderers can coexist), `pollInput()` (one
  `getch()` per frame recognizing `q`, the arrow keys, and `n`), the
  shared idle screen for windows that aren't currently active, and
  `drawHostStatus()` for the host's own (non-visualizing) status screen.
- `visual_mode.hpp`: the `VisualMode` enum and `next`/`prev`/`parse`
  helpers arrow-key switching cycles through.
- `main.cpp` / `args.cpp`: parse CLI args and dispatch to host discovery
  (the default), or the explicit `--controller`/`--subordinate` modes.
- `host_discovery.hpp`: the `/tmp/.mitm-host` lock file read/write/
  liveness-check helpers `main.cpp` uses to decide host-vs-child.
- `host.cpp`: `runHost` -- the orchestrator: accepts children, activates
  each on connect, runs the one audio pipeline for the whole setup and
  broadcasts its output to every child (`ChildBroadcastSink`, see
  `audio_event_sink.hpp`), cascades shutdown, and draws its own status
  screen. Doesn't render a visualizer itself. See **Multi-window** above.
- `audio_event_sink.hpp`: `AudioFrame` and `AudioEventSink`, the
  publisher/subscriber interface the host's audio pipeline fans its
  per-tick output out through -- `ChildBroadcastSink` in `host.cpp` is the
  only implementation today.
- `unix_socket.cpp`, `ipc_protocol.hpp`: the length-prefixed JSON transport
  and its tiny message schema.
- `window_launcher.cpp`: `AppleScriptWindowLauncher`, spawning/closing
  Terminal.app windows, plus `currentExecutablePath()`.
- `control_server.cpp`: `ControlServer`, the host's embedded web control
  panel -- a from-scratch HTTP/1.1 server plus the single HTML/CSS/JS page
  it serves. See **Web control panel** above.
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
- Only the host opens the loopback device now (see **Audio pipeline**
  above) -- a `runSubordinate` process never captures audio itself
  regardless of how it was spawned, which means the explicit
  `--controller` path (which doesn't run the host's audio pipeline or
  send `audio_frame`) currently leaves its subordinates with no audio
  reactivity at all, not degraded/independent reactivity as before.
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
- The web control panel (see **Web control panel** above) has no
  authentication at all -- fine since it's bound to 127.0.0.1 only, but
  that means any other local process/user on the same machine can hit
  it too, not just a browser you opened yourself. It's also host/child-
  only: the explicit `--controller` mode doesn't run one, and a
  subordinate that isn't a host/child doesn't listen for
  `set_mode`/`set_color` either (only `runSubordinate`'s reader thread,
  shared by both paths, does -- but nothing on the `--controller` side
  ever sends them).
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
