#pragma once

#include <string>

namespace mitm {

// Which visual is currently on screen for a given window. Left/right
// arrow keys cycle through these live (see curses_util::InputAction);
// --visual=bars|matrix|checkerboard only picks the *starting* mode now.
enum class VisualMode {
    Bars,
    Matrix,
    Checkerboard,
};

constexpr int kVisualModeCount = 3;

inline VisualMode nextVisualMode(VisualMode mode) {
    return static_cast<VisualMode>((static_cast<int>(mode) + 1) % kVisualModeCount);
}

inline VisualMode prevVisualMode(VisualMode mode) {
    return static_cast<VisualMode>((static_cast<int>(mode) - 1 + kVisualModeCount) % kVisualModeCount);
}

// Unrecognized strings default to Bars, matching the AppArgs default.
inline VisualMode parseVisualMode(const std::string& s) {
    if (s == "matrix") return VisualMode::Matrix;
    if (s == "checkerboard") return VisualMode::Checkerboard;
    return VisualMode::Bars;
}

// Inverse of parseVisualMode() -- used to report a window's current mode
// over IPC (see ipc::StateMessage) and to the control server's web UI.
inline std::string visualModeName(VisualMode mode) {
    switch (mode) {
        case VisualMode::Matrix: return "matrix";
        case VisualMode::Checkerboard: return "checkerboard";
        case VisualMode::Bars: return "bars";
    }
    return "bars";
}

} // namespace mitm
