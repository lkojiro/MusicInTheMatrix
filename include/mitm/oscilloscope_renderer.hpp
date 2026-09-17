#pragma once

#include <vector>

namespace mitm {

// Draws a time-domain waveform (an oscilloscope-style line trace) across
// the full terminal, vertically centered -- positive values bow upward
// from center, negative downward. The trace always stretches to span the
// full terminal width: `samples` is linearly interpolated to exactly one
// value per screen column (see sampleAtColumn() in the .cpp), regardless
// of how many points it actually has, rather than laying out one column
// per point and leaving any width difference as padding. A thin line,
// not a solid fill: level segments draw as '-', a rising or falling step
// draws as a '/' or '\' cap on a '|' run for the rows in between (see
// draw()'s doc comment for exactly which row gets the cap) -- picked by
// the slope between each pair of consecutive columns, the same idea as a
// real oscilloscope's traced line rather than a bar chart's filled
// columns.
//
// Does NOT own the ncurses session (no initscr()/endwin() here) -- see
// TerminalRenderer for why (arrow-key mode switching needs all renderers
// to share one session). The constructor sets up its own color pair in a
// range reserved apart from every other renderer's so all can coexist.
class OscilloscopeRenderer {
public:
    OscilloscopeRenderer();

    OscilloscopeRenderer(const OscilloscopeRenderer&) = delete;
    OscilloscopeRenderer& operator=(const OscilloscopeRenderer&) = delete;

    // `samples` values are expected roughly in [-1, 1]; values outside
    // that range are clamped. The vector's size determines how many
    // columns-worth of the trace are drawn (points share whatever width
    // is available, same convention as TerminalRenderer's buckets).
    void draw(const std::vector<float>& samples);

    // Re-reads GlobalConfig::baseColor and redefines this renderer's
    // color pair from it -- safe to call again after construction, not
    // just from it. Must be called from the thread that owns the ncurses
    // session.
    void setupColors();

private:
    bool colorEnabled_ = false;
    bool extendedColor_ = false; // true if the terminal offers a 256-color palette
};

} // namespace mitm
