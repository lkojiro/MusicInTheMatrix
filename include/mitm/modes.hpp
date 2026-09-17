#pragma once

#include <string>

namespace mitm {

// Parsed command-line invocation. Modes share one binary:
//
//   mitm [device-name-substring] [--visual=bars-left|bars-right|bars-middle|matrix|checkerboard|oscilloscope|bands] [--color=NAME]
//       Default: auto-discovers whether a host is already running (see
//       host_discovery.hpp) and either becomes the host (runHost) or
//       connects to the existing one as a child (runSubordinate) -- see
//       main.cpp. Exactly one host exists at a time; every other
//       instance, however it was started (via 'n' in an existing window,
//       or just run directly in a fresh terminal), becomes a child of it.
//
//   mitm --controller [--count=N] [--socket=PATH] [--visual=...] [--color=NAME]
//       Explicit, non-auto-discovered orchestrator: spawns N subordinate
//       windows up front (forwarding --visual to each, but not --color --
//       each subordinate gets its own auto-rotated color instead, see
//       colorNameForIndex() in color_scheme.hpp) and rotates which one is
//       "active" over time, without rendering anything itself. Kept as a
//       manual/scriptable alternative to the default auto-discovery flow,
//       not used by it. This --color applies only to the controller's own
//       process, which doesn't render anything, so it's effectively unused
//       here.
//
//   mitm --subordinate --id=N --socket=PATH [--device=NAME] [--visual=...] [--color=NAME]
//       What --controller launches in each window. The default
//       auto-discovery flow also ends up here (with id set to the
//       connecting process's own pid instead of a controller-assigned
//       sequential id) when it finds an existing host. Not normally run
//       by hand.
//
// --visual=bars-left (default, "bars" also accepted as an alias) is the
// FFT bar chart, bucket 0 at the left edge increasing rightward;
// --visual=bars-right is the same chart mirrored (bucket 0 at the right
// edge increasing leftward); --visual=bars-middle centers bucket 0 and
// mirrors every other bucket outward to both sides at once (see
// BarLayout in terminal_renderer.hpp for what each one actually draws).
// --visual=matrix is the Matrix-style digital rain; --visual=checkerboard
// is the beat-flash grid; --visual=oscilloscope is the time-domain
// waveform trace (see OscilloscopeRenderer); --visual=bands is the three
// low/mid/high vertical VU-meter bars (see BandMeterRenderer). This only
// picks the *starting* mode -- every window can switch live between all
// seven with the left/right arrow keys (see curses_util::InputAction).
//
// --color=NAME (default "green") picks the base hue every renderer's
// brightness ramp is derived from -- see color_scheme.hpp. One of red,
// green, blue, yellow, cyan, magenta/purple, white; unrecognized names
// fall back to green. Applies only to the window on whose command line it
// appears -- a window spawned via 'n' (host.cpp's spawnChild()) or by
// --controller gets its own auto-rotated color instead of inheriting the
// spawning window's, so windows are visually distinguishable by default;
// see colorNameForIndex() in color_scheme.hpp. Meaningless on the host's
// own command line now: the host doesn't render anything itself (see
// runHost below), so it has no color of its own to set.
//
// --device likewise no longer applies to a subordinate: subordinates
// don't open their own audio device anymore, only the host does (see
// AudioEventSink in audio_event_sink.hpp) -- --device on the host's own
// command line is what picks its capture device.
struct AppArgs {
    bool controller = false;
    bool subordinate = false;
    int count = 3;
    std::string socketPath = "/tmp/mitm-controller.sock";
    int id = 0;
    std::string deviceNameHint = "BlackHole";
    std::string visualMode = "bars";
    std::string colorName = "green";
    // Non-empty only when this process was launched via 'n' from within
    // an existing window -- see HelloMessage's doc comment in
    // ipc_protocol.hpp for what it's for.
    std::string spawnToken;
    // Only meaningful for the host: the port its web control panel
    // listens on (127.0.0.1 only -- see control_server.hpp). Override
    // with --web-port=N if that port's already taken.
    int webPort = 7887;
};

AppArgs parseArgs(int argc, char** argv);

// The host: a dedicated, non-visualizing orchestrator. Runs the one
// AudioCapture/FftProcessor/BeatDetector pipeline for the whole setup and
// broadcasts its output to every connected child once per tick (see
// AudioEventSink in audio_event_sink.hpp) -- accepts child connections
// and activates each immediately (every window renders live
// simultaneously, no rotation), spawns new windows on 'n' (its own or
// relayed from a child's), and cascades shutdown to every child when it
// quits. Its own terminal just shows a status screen (app name, version,
// connected windows) via curses_util::drawHostStatus -- all actual
// visualization happens in subordinate windows. See src/host.cpp.
int runHost(const AppArgs& args);

int runSubordinate(const AppArgs& args);
int runController(const AppArgs& args);

} // namespace mitm
