#pragma once

#include <vector>

namespace mitm {

// Draws a vertical bar chart on the current ncurses screen, one or more
// columns per bucket, stacked upward from the bottom using '#'. Each bar
// also carries a peak-hold cap ('_'): it snaps up instantly whenever a
// new frame's bar exceeds it, otherwise decays back down toward the live
// bar over time. This is stateful across calls to draw() -- decay speed
// is expressed in rows/second, scaled by config::kFrameDeltaSeconds.
//
// Does NOT own the ncurses session (no initscr()/endwin() here) -- a
// single run can now switch live between this and MatrixRainRenderer via
// arrow keys, so the caller (standalone.cpp / subordinate.cpp) owns
// curses_util::init()/teardown() once, up front, shared by both.
class TerminalRenderer {
public:
    // Current terminal size, refreshed on each call to draw().
    int width() const { return width_; }
    int height() const { return height_; }

    // Draws one frame. `magnitudes` values are expected roughly in [0, 1];
    // values outside that range are clamped. The vector's size determines
    // how many bars are drawn (bars share whatever width is available).
    void draw(const std::vector<float>& magnitudes);

private:
    int width_ = 0;
    int height_ = 0;

    // Peak-hold row (in "rows above the bottom" units, float for smooth
    // decay) per bucket, persisted across draw() calls.
    std::vector<float> peakRows_;
};

} // namespace mitm
