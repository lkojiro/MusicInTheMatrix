#pragma once

#include <string>
#include <vector>

namespace mitm {

// A base color for renderers to derive brightness ramps from, expressed
// as xterm-256 color-cube coordinates (0-5 per channel; the 6x6x6 cube
// occupies palette indices 16-231). This is what every renderer's ramp
// is generated from now, replacing what used to be a hardcoded green
// ramp duplicated independently in MatrixRainRenderer and
// CheckerboardRenderer.
struct BaseColor {
    int r = 0;
    int g = 5;
    int b = 0; // default: green, matching the original hardcoded look

    // Lets a render loop cheaply notice "did GlobalConfig::baseColor
    // actually change since I last applied it" (see control_server.hpp's
    // live color changes) without needing a separate dirty flag.
    friend bool operator==(const BaseColor& a, const BaseColor& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }
    friend bool operator!=(const BaseColor& a, const BaseColor& b) { return !(a == b); }
};

// xterm-256 color-cube coordinates (each clamped to 0-5) -> palette index.
int cubeToXterm256(int r, int g, int b);

// `levels` brightness steps derived from `base`, brightest first, each
// channel scaled down toward (but never quite reaching) zero -- a zero
// channel in `base` stays zero throughout, so hue is preserved and only
// brightness varies. With the default green BaseColor and levels=5 this
// reproduces the exact ramp {46, 40, 34, 28, 22} every renderer used to
// hardcode independently.
std::vector<int> makeBrightnessRamp(const BaseColor& base, int levels);

// Named presets for the --color CLI flag. Unrecognized names (including
// an empty string) fall back to the default (green) rather than erroring
// -- picking a color is a cosmetic choice, not something worth failing a
// whole run over.
BaseColor parseColorName(const std::string& name);

// A named preset for the Nth auto-spawned window (0-based, wraps), used
// so windows the host/controller spawns default to visually distinct
// colors from each other rather than all inheriting the same --color --
// each window is its own process with its own BaseColor (set once at
// startup and baked into that process's renderers; see
// GlobalConfig::baseColor), so "different windows, different colors"
// just means picking a different name per spawn rather than blindly
// forwarding the spawning window's own --color. Deliberately excludes
// green, the default preset most likely to already be window 0's color,
// so the very first spawned window doesn't usually just duplicate it.
// Picked by index, not randomly, so which window gets which color is at
// least deterministic/reproducible run to run.
std::string colorNameForIndex(int index);

// Cycles through the 7 named presets (red, green, blue, yellow, cyan,
// magenta, white -- same set --color/parseColorName recognize, "purple"
// treated as the "magenta" slot) in that fixed order, wrapping both ways.
// Driven by the up/down arrow keys (see curses_util::InputAction) the
// same way nextVisualMode()/prevVisualMode() drive left/right -- an
// unrecognized `name` is treated as if it were "green", matching
// parseColorName()'s own fallback.
std::string nextColorName(const std::string& name);
std::string prevColorName(const std::string& name);

// Nearest basic ANSI color (1-7: red/green/yellow/blue/magenta/cyan/
// white) for terminals without a 256-color palette, where a brightness
// ramp isn't possible at all -- just picks the color whose channels are
// present in `base`. Deliberately untyped (plain int, not e.g. curses'
// COLOR_RED) so this header doesn't need to depend on <ncurses.h>; the
// values are numerically identical to ncurses' COLOR_* constants (the
// standard ANSI 3-bit RGB bit-pattern), so callers can pass this straight
// through to init_pair() themselves.
int basicAnsiColorIndex(const BaseColor& base);

} // namespace mitm
