#pragma once

#include <string>

namespace mitm {

// Parsed command-line invocation. Modes share one binary:
//
//   mitm [device-name-substring] [--visual=bars|matrix|checkerboard] [--color=NAME]
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
// --visual=bars (default) is the FFT bar chart; --visual=matrix is the
// Matrix-style digital rain; --visual=checkerboard is the beat-flash grid.
// This only picks the *starting* mode -- every window can switch live
// between them with the left/right arrow keys (see curses_util::InputAction).
//
// --color=NAME (default "green") picks the base hue every renderer's
// brightness ramp is derived from -- see color_scheme.hpp. One of red,
// green, blue, yellow, cyan, magenta/purple, white; unrecognized names
// fall back to green. Applies only to the window on whose command line it
// appears -- a window spawned via 'n' (host.cpp's spawnChild()) or by
// --controller gets its own auto-rotated color instead of inheriting the
// spawning window's, so windows are visually distinguishable by default;
// see colorNameForIndex() in color_scheme.hpp.
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
};

AppArgs parseArgs(int argc, char** argv);

// The host: renders its own window (like the old standalone mode) AND
// orchestrates -- accepts child connections and activates each
// immediately (every window renders live simultaneously, no rotation),
// broadcasts GlobalConfig updates, spawns new windows on 'n' (its own or
// relayed from a child's), and cascades shutdown to every child when it
// quits. See src/host.cpp.
int runHost(const AppArgs& args);

int runSubordinate(const AppArgs& args);
int runController(const AppArgs& args);

} // namespace mitm
