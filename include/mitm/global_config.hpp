#pragma once

#include <functional>
#include <vector>

#include "mitm/color_scheme.hpp"

namespace mitm {

// Minimal per-window record -- just enough to know a window exists and
// which id it is. Deliberately small: this is scaffolding for effects
// that need to know things like "how many windows are there right now"
// or "what's this window's id", not a full window-state model.
struct WindowInfo {
    int id = 0;
};

// Process-local snapshot of the current multi-window setup and shared
// color scheme -- infrastructure for future single-window effects (which
// only need baseColor) and multi-window effects (which also need
// windowCount/windows) to build on, without each one having to invent
// its own way to discover "what does the whole setup look like right
// now". Nothing consumes windowCount/windows yet beyond storing them;
// baseColor already drives every renderer's brightness ramp.
//
// Populated differently per mode:
//   - standalone: windowCount=1, windows={{0}} (a synthetic self-entry --
//     standalone processes aren't tracked across each other yet, even
//     ones spawned via 'n', so this is honestly just "this window" for
//     now, not a real cross-process registry).
//   - controller: derived directly from its own subordinates map.
//   - subordinate: kept in sync by the controller, which broadcasts an
//     ipc::ConfigMessage to every connected subordinate whenever the
//     window set changes (a connect or disconnect) -- see runController
//     in controller.cpp and the "config" message handling in
//     runSubordinate in subordinate.cpp.
struct GlobalConfig {
    int windowCount = 1;
    std::vector<WindowInfo> windows;
    BaseColor baseColor;
};

// Thread-safe accessors: a subordinate's background socket-reader thread
// writes windowCount/windows while the main render loop may read this
// (today, only renderer constructors read baseColor, which happens
// before that reader thread is even started -- but future per-frame
// consumers of windowCount/windows would race without this).
GlobalConfig getGlobalConfig();
void setGlobalConfig(const GlobalConfig& config);
void updateGlobalConfig(const std::function<void(GlobalConfig&)>& mutator);

} // namespace mitm
