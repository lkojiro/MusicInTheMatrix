#pragma once

#include <vector>

namespace mitm {

// Where bucket 0 (the lowest frequency) sits, and which way frequency
// increases from there -- see draw()'s doc comment for the three
// layouts this drives (VisualMode::BarsLeft/BarsRight/BarsMiddle).
enum class BarLayout {
    Left,   // bucket 0 at the left edge, increasing rightward (the original/default layout)
    Right,  // bucket 0 at the right edge, increasing leftward -- a horizontal mirror of Left
    Middle, // bucket 0 centered, increasing outward in both directions at once
};

// Draws a vertical bar chart on the current ncurses screen, one or more
// columns per bucket, stacked upward from the bottom using '#'. Each bar
// also carries a peak-hold cap ('_'): it snaps up instantly whenever a
// new frame's bar exceeds it, otherwise decays back down toward the live
// bar over time. This is stateful across calls to draw() -- decay speed
// is expressed in rows/second, scaled by config::kFrameDeltaSeconds. The
// peak-hold state is keyed by *bucket*, not screen column, so switching
// `layout` between frames doesn't reset or glitch it.
//
// Does NOT own the ncurses session (no initscr()/endwin() here) -- a
// single run can now switch live between this and MatrixRainRenderer via
// arrow keys, so the caller (standalone.cpp / subordinate.cpp) owns
// curses_util::init()/teardown() once, up front, shared by both. The
// constructor sets up its own color pair in a range reserved apart from
// MatrixRainRenderer's (1-8) and CheckerboardRenderer's (20+) so all
// three can coexist.
class TerminalRenderer {
public:
    TerminalRenderer();

    TerminalRenderer(const TerminalRenderer&) = delete;
    TerminalRenderer& operator=(const TerminalRenderer&) = delete;

    // Current terminal size, refreshed on each call to draw().
    int width() const { return width_; }
    int height() const { return height_; }

    // Draws one frame. `magnitudes` values are expected roughly in [0, 1];
    // values outside that range are clamped. The vector's size determines
    // how many bars are drawn (bars share whatever width is available).
    //
    // `layout` picks where bucket 0 sits and which way frequency
    // increases from there:
    //   - Left:   bucket 0 at the left edge, increasing rightward (unchanged
    //             from before this had a layout concept at all).
    //   - Right:  bucket 0 at the right edge, increasing leftward -- the
    //             same bars, just mirrored left-right.
    //   - Middle: bucket 0 in the horizontal center; every other bucket is
    //             drawn twice, mirrored the same distance out from center
    //             on both sides, so the whole chart reads symmetric
    //             around the middle rather than running in one direction.
    //             Needs roughly 2x the columns-per-bucket Left/Right would
    //             for the same bucket count, since it spreads outward in
    //             both directions from one shared center.
    void draw(const std::vector<float>& magnitudes, BarLayout layout = BarLayout::Left);

    // Re-reads GlobalConfig::baseColor and redefines this renderer's
    // color pair from it -- safe to call again after construction, not
    // just from it (see CheckerboardRenderer::setupColors() for why: the
    // web control panel's live color changes need this). Must be called
    // from the thread that owns the ncurses session.
    void setupColors();

private:
    int width_ = 0;
    int height_ = 0;

    // Peak-hold row (in "rows above the bottom" units, float for smooth
    // decay) per bucket, persisted across draw() calls.
    std::vector<float> peakRows_;

    bool colorEnabled_ = false;
    bool extendedColor_ = false; // true if the terminal offers a 256-color palette
};

} // namespace mitm
