#pragma once

#include <random>
#include <vector>

namespace mitm {

// A fixed grid of cells (see kGridCols/kGridRows in the .cpp), each
// starting off (black). Only cells that fall on the "bright" square of
// the classic alternating checkerboard parity are ever eligible to be
// lit -- a beat lights a handful of them (or, for a particularly loud
// beat, all of them at once), each independently rolling one of a few
// fade-back-to-black styles (see FadeStyle). Which parity counts as
// "bright" is currently static (see invertedParity_).
//
// Does NOT own the ncurses session -- see TerminalRenderer /
// MatrixRainRenderer for why (arrow-key mode switching needs all
// renderers to share one session). The constructor sets up its own color
// pairs in a range reserved apart from MatrixRainRenderer's so both can
// coexist.
class CheckerboardRenderer {
public:
    CheckerboardRenderer();

    CheckerboardRenderer(const CheckerboardRenderer&) = delete;
    CheckerboardRenderer& operator=(const CheckerboardRenderer&) = delete;

    // `beatPulse` lights up a handful of cells -- more of them the louder
    // `loudness` (roughly [0, 1]) was at that moment, and the classic
    // alternating checkerboard pattern instead for particularly loud
    // beats (see kFullPatternLoudnessThreshold). Each lit cell
    // independently ages and fades back to black according to whichever
    // FadeStyle it was randomly assigned when lit.
    void draw(float loudness, bool beatPulse);

private:
    // How a lit cell fades back to black, chosen randomly each time a
    // cell is lit (see randomFadeStyle()):
    //  - Linear: constant dim-down. The original/default behavior.
    //  - Flicker: rapid on/off flashing, then a slow fade to black.
    //  - Shrink: dims while the lit area shrinks inward -- the outermost
    //    ring of the cell's characters peels off first -- down to
    //    nothing, rather than staying full-size until it's fully dark.
    enum class FadeStyle {
        Linear,
        Flicker,
        Shrink,
    };

    struct CellState {
        // Seconds since this cell was last lit. Starts at a large
        // sentinel (rather than needing a separate "is it lit" bool) so
        // computeFade() naturally reports it as already faded.
        float age = 1e9f;
        FadeStyle style = FadeStyle::Linear;
    };

    struct FadeResult {
        float intensity = 0.0f;      // 0..1, feeds colorPairForIntensity
        float shrinkFraction = 0.0f; // 0 = full-size cell, 1 = shrunk to nothing
        bool alive = false;          // false once fully faded -- renders as off
    };

    void setupColors();
    int colorPairForIntensity(float intensity) const;
    void lightCellsForBeat(float loudness);
    void lightFullCheckerboardPattern();
    void igniteCell(int idx);
    FadeStyle randomFadeStyle();
    static FadeResult computeFade(FadeStyle style, float age);
    // True if (row, col) falls on the currently-"bright" checkerboard
    // parity -- the single source of truth for which cells are eligible
    // to be lit, enforced both when lighting cells and when rendering
    // them, so an off-parity cell can never show color regardless of
    // whatever its stored state happens to be.
    bool isBrightParity(int row, int col) const;

    int width_ = 0;
    int height_ = 0;

    std::vector<CellState> cells_; // one entry per grid cell (kGridCols x kGridRows)
    std::mt19937 rng_;

    // Which checkerboard parity currently counts as "bright". Static for
    // now (always false) -- previously flipped every few beats.
    bool invertedParity_ = false;

    bool colorEnabled_ = false;
    bool extendedColor_ = false; // true if the terminal offers a 256-color palette
};

} // namespace mitm
