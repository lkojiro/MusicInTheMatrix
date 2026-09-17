#pragma once

#include <string>

namespace mitm {

// Which visual is currently on screen for a given window. Left/right
// arrow keys cycle through these live (see curses_util::InputAction);
// --visual=bars-left|bars-right|bars-middle|matrix|checkerboard|oscilloscope|bands
// only picks the *starting* mode now. The three Bars* modes all draw
// through TerminalRenderer -- see BarLayout in terminal_renderer.hpp for
// what each one actually looks like; this enum only decides *which*
// layout, not how it's drawn.
enum class VisualMode {
    BarsLeft,
    BarsRight,
    BarsMiddle,
    Matrix,
    Checkerboard,
    Oscilloscope,
    BandMeters,
};

constexpr int kVisualModeCount = 7;

inline VisualMode nextVisualMode(VisualMode mode) {
    return static_cast<VisualMode>((static_cast<int>(mode) + 1) % kVisualModeCount);
}

inline VisualMode prevVisualMode(VisualMode mode) {
    return static_cast<VisualMode>((static_cast<int>(mode) - 1 + kVisualModeCount) % kVisualModeCount);
}

// Unrecognized strings default to BarsLeft, matching the AppArgs
// default. "bars" (no suffix) is kept as a backward-compatible alias for
// "bars-left", matching what --visual=bars meant before BarsRight/
// BarsMiddle existed.
inline VisualMode parseVisualMode(const std::string& s) {
    if (s == "bars" || s == "bars-left") return VisualMode::BarsLeft;
    if (s == "bars-right") return VisualMode::BarsRight;
    if (s == "bars-middle") return VisualMode::BarsMiddle;
    if (s == "matrix") return VisualMode::Matrix;
    if (s == "checkerboard") return VisualMode::Checkerboard;
    if (s == "oscilloscope") return VisualMode::Oscilloscope;
    if (s == "bands") return VisualMode::BandMeters;
    return VisualMode::BarsLeft;
}

// Inverse of parseVisualMode() -- used to report a window's current mode
// over IPC (see ipc::StateMessage) and to the control server's web UI.
// Always the canonical "bars-left" form, never the "bars" alias.
inline std::string visualModeName(VisualMode mode) {
    switch (mode) {
        case VisualMode::BarsLeft: return "bars-left";
        case VisualMode::BarsRight: return "bars-right";
        case VisualMode::BarsMiddle: return "bars-middle";
        case VisualMode::Matrix: return "matrix";
        case VisualMode::Checkerboard: return "checkerboard";
        case VisualMode::Oscilloscope: return "oscilloscope";
        case VisualMode::BandMeters: return "bands";
    }
    return "bars-left";
}

} // namespace mitm
