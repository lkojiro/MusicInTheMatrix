#include "mitm/checkerboard_renderer.hpp"

#include "mitm/color_scheme.hpp"
#include "mitm/global_config.hpp"
#include "mitm/visualizer_config.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <ncurses.h>

namespace mitm {

namespace {

// Grid size in cells -- the whole terminal is always divided into exactly
// this many columns/rows, so cell size (in characters) is derived from
// the current terminal dimensions each frame rather than fixed.
constexpr int kGridCols = 4;
constexpr int kGridRows = 8;
constexpr int kCellCount = kGridCols * kGridRows;

// Color pair ids reserved for this renderer, kept clear of
// MatrixRainRenderer's (which uses 1..8) so both can coexist -- all
// visual-mode renderers are now constructed up front for arrow-key mode
// switching, so their color pairs share one global namespace.
constexpr short kPairBase = 20;
constexpr short kOffPair = kPairBase; // never-lit / fully-faded cells

// How many shades a lit cell fades through, brightest to dimmest. The
// actual palette indices are computed in setupColors() from the shared
// GlobalConfig::baseColor (see color_scheme.hpp) rather than hardcoded,
// so this mode's color follows whatever base color the run picked
// instead of always being green.
constexpr int kIntensityLevels = 5;

// FadeStyle::Linear: total time for a fully-lit cell to reach black.
// Matches the original ~1.5/sec decay rate.
constexpr float kLinearFadeDurationSeconds = 0.667f;

// FadeStyle::Flicker: a rapid on/off strobe phase, then a slower fade.
constexpr float kFlickerPhaseSeconds = 0.25f;      // strobe phase length
constexpr float kFlickerToggleSeconds = 0.04f;     // on/off toggle period during the strobe
constexpr float kFlickerFadePhaseSeconds = 0.9f;   // slow fade phase length, after the strobe

// FadeStyle::Shrink: total time for a fully-lit cell to both dim to black
// and shrink to nothing. Longer than the linear duration so the shrinking
// motion itself is actually perceptible rather than instant.
constexpr float kShrinkDurationSeconds = 0.8f;

// Cells lit per (non-special) beat scales linearly with loudness across
// this range.
constexpr int kMinCellsPerBeat = 1;
constexpr int kMaxCellsPerBeat = 4;

// Loudness at/above which a beat is "particularly loud" enough to light
// the full alternating checkerboard pattern instead of a handful of
// random cells.
constexpr float kFullPatternLoudnessThreshold = 0.85f;

// Returns the [start, end) span of the `index`-th of `count` cells that
// evenly divide `total` characters. Any remainder is spread one unit at
// a time across the first `remainder` cells (so sizes differ by at most
// one character) rather than dumping it all into the last cell, which
// would otherwise end up noticeably larger than the rest whenever the
// terminal size isn't a clean multiple of the grid size.
std::pair<int, int> evenSpan(int total, int count, int index) {
    int base = total / count;
    int remainder = total % count;
    int start = index * base + std::min(index, remainder);
    int end = start + base + (index < remainder ? 1 : 0);
    return {start, end};
}

} // namespace

CheckerboardRenderer::CheckerboardRenderer()
    : cells_(kCellCount), rng_(std::random_device{}()) {
    setupColors(); // requires curses_util::init() to have already run -- see header
}

void CheckerboardRenderer::setupColors() {
    if (!has_colors()) {
        colorEnabled_ = false;
        return;
    }
    start_color();
    // Lets -1 mean "the terminal's own default background" in init_pair()
    // below, instead of every color needing to be a real, opaque palette
    // entry. Without this, COLOR_BLACK was the only way to say "dark" --
    // but COLOR_BLACK is a real, opaque RGB(0,0,0), not a sentinel for
    // "leave it alone", so every "off" cell was painting over a
    // semi-transparent terminal's own background instead of letting it
    // show through. use_default_colors() is what makes that distinction
    // expressible at all.
    use_default_colors();
    colorEnabled_ = true;
    extendedColor_ = (COLORS >= 256);

    // -1/-1 (not COLOR_BLACK/COLOR_BLACK): an "off" cell should look like
    // nothing was drawn there at all -- matching a semi-transparent
    // terminal's own background -- not an opaque black square. Still
    // painted explicitly every frame (not skipped) rather than just
    // relying on erase() to have already blanked it, so this keeps the
    // same "every cell gets a fresh, explicit paint" structure that
    // redrawwin() below depends on -- see its own comment for why.
    init_pair(kOffPair, -1, -1);

    BaseColor base = getGlobalConfig().baseColor;

    if (extendedColor_) {
        std::vector<int> ramp = makeBrightnessRamp(base, kIntensityLevels);
        for (int level = 0; level < kIntensityLevels; ++level) {
            init_pair(static_cast<short>(kPairBase + 1 + level), COLOR_BLACK, static_cast<short>(ramp[level]));
        }
    } else {
        // Basic terminal fallback: one lit shade, no fade gradient.
        init_pair(kPairBase + 1, COLOR_BLACK, static_cast<short>(basicAnsiColorIndex(base)));
    }
}

int CheckerboardRenderer::colorPairForIntensity(float intensity) const {
    if (intensity <= 0.0f) return kOffPair;
    if (!extendedColor_) return kPairBase + 1;

    float clamped = std::clamp(intensity, 0.0f, 1.0f);
    int level = std::clamp(static_cast<int>((1.0f - clamped) * kIntensityLevels), 0, kIntensityLevels - 1);
    return kPairBase + 1 + level;
}

bool CheckerboardRenderer::isBrightParity(int row, int col) const {
    return ((row + col) % 2 == 0) != invertedParity_;
}

CheckerboardRenderer::FadeStyle CheckerboardRenderer::randomFadeStyle() {
    std::uniform_int_distribution<int> dist(0, 2);
    return static_cast<FadeStyle>(dist(rng_));
}

void CheckerboardRenderer::igniteCell(int idx) {
    cells_[idx].age = 0.0f;
    cells_[idx].style = randomFadeStyle();
}

CheckerboardRenderer::FadeResult CheckerboardRenderer::computeFade(FadeStyle style, float age) {
    switch (style) {
        case FadeStyle::Linear: {
            if (age >= kLinearFadeDurationSeconds) return {};
            return {1.0f - age / kLinearFadeDurationSeconds, 0.0f, true};
        }
        case FadeStyle::Flicker: {
            if (age < kFlickerPhaseSeconds) {
                bool on = (static_cast<int>(age / kFlickerToggleSeconds) % 2) == 0;
                return {on ? 1.0f : 0.0f, 0.0f, true};
            }
            float t = age - kFlickerPhaseSeconds;
            if (t >= kFlickerFadePhaseSeconds) return {};
            return {1.0f - t / kFlickerFadePhaseSeconds, 0.0f, true};
        }
        case FadeStyle::Shrink: {
            if (age >= kShrinkDurationSeconds) return {};
            float progress = age / kShrinkDurationSeconds;
            // Dims and shrinks together, finishing both at the same time.
            return {1.0f - progress, progress, true};
        }
    }
    return {};
}

void CheckerboardRenderer::lightFullCheckerboardPattern() {
    for (int row = 0; row < kGridRows; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            if (isBrightParity(row, col)) {
                igniteCell(row * kGridCols + col);
            }
        }
    }
}

void CheckerboardRenderer::lightCellsForBeat(float loudness) {
    // invertedParity_ is left static for now (always false) -- the
    // periodic-inversion logic that used to flip it every few beats was
    // removed rather than disabled in place, since it's trivial to bring
    // back and this keeps the beat-handling code honest about what it
    // currently does.

    float l = std::clamp(loudness, 0.0f, 1.0f);

    if (l >= kFullPatternLoudnessThreshold) {
        lightFullCheckerboardPattern();
        return;
    }

    int count = std::clamp(
        static_cast<int>(std::round(kMinCellsPerBeat + l * (kMaxCellsPerBeat - kMinCellsPerBeat))),
        kMinCellsPerBeat, kMaxCellsPerBeat);

    // Only cells on the currently-"bright" checkerboard parity are
    // eligible -- sparse beats should still land on squares that fit the
    // pattern, not occasionally light one that would be "off".
    std::vector<int> allowed;
    allowed.reserve(kCellCount / 2);
    for (int row = 0; row < kGridRows; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            if (isBrightParity(row, col)) {
                allowed.push_back(row * kGridCols + col);
            }
        }
    }

    count = std::min(count, static_cast<int>(allowed.size()));

    // Distinct cells, no repeats -- same std::sample technique already
    // used for Matrix rain's loudness-scaled beat-pulse subset.
    std::vector<int> chosen;
    chosen.reserve(count);
    std::sample(allowed.begin(), allowed.end(), std::back_inserter(chosen), count, rng_);

    for (int idx : chosen) {
        igniteCell(idx);
    }
}

void CheckerboardRenderer::draw(float loudness, bool beatPulse) {
    if (beatPulse) lightCellsForBeat(loudness);

    for (CellState& cell : cells_) {
        cell.age += config::kFrameDeltaSeconds;
    }

    getmaxyx(stdscr, height_, width_);
    erase();

    if (width_ <= 0 || height_ <= 0) {
        refresh();
        return;
    }

    // Paint each cell as a rectangle: erase() above already blanked every
    // character to ' ', so the color path only needs to change attributes
    // over each cell's span -- mvchgat() is ncurses' primitive for exactly
    // that ("recolor N characters starting here", no character redraw),
    // rather than the attron()/mvaddch()/attroff() dance this used to do.
    //
    // Every cell is painted "off" first, then -- if it's alive -- its
    // (possibly shrunk) inner region is painted "on" over that. This one
    // path handles all three fade styles uniformly: Linear/Flicker never
    // shrink (inset stays 0, so the "on" region covers the whole cell),
    // while Shrink's growing inset peels the outer ring off, leaving the
    // "off" paint underneath visible again.
    for (int row = 0; row < kGridRows; ++row) {
        auto [yStart, yEnd] = evenSpan(height_, kGridRows, row);
        int cellRows = yEnd - yStart;

        for (int col = 0; col < kGridCols; ++col) {
            auto [xStart, xEnd] = evenSpan(width_, kGridCols, col);
            int cellChars = xEnd - xStart;
            if (cellChars <= 0 || cellRows <= 0) continue;

            int idx = row * kGridCols + col;
            // Enforced here too, not just at the point cells get lit: an
            // off-parity cell renders as off regardless of whatever its
            // stored state is.
            FadeResult fade =
                isBrightParity(row, col) ? computeFade(cells_[idx].style, cells_[idx].age) : FadeResult{};

            int inset = 0;
            if (fade.alive) {
                int maxInset = std::min(cellChars, cellRows) / 2;
                inset = static_cast<int>(std::round(fade.shrinkFraction * maxInset));
            }
            int ix0 = xStart + inset;
            int ix1 = xEnd - inset;
            int iy0 = yStart + inset;
            int iy1 = yEnd - inset;
            bool hasInnerRegion = fade.alive && ix1 > ix0 && iy1 > iy0;

            if (colorEnabled_) {
                for (int y = yStart; y < yEnd; ++y) {
                    mvchgat(y, xStart, cellChars, A_NORMAL, kOffPair, nullptr);
                }
                if (hasInnerRegion) {
                    short onPair = static_cast<short>(colorPairForIntensity(fade.intensity));
                    for (int y = iy0; y < iy1; ++y) {
                        mvchgat(y, ix0, ix1 - ix0, A_NORMAL, onPair, nullptr);
                    }
                }
            } else {
                // No color support at all -- fall back to a plain
                // character so lit cells are still visible.
                for (int y = yStart; y < yEnd; ++y) {
                    for (int x = xStart; x < xEnd; ++x) {
                        mvaddch(y, x, ' ');
                    }
                }
                if (hasInnerRegion && fade.intensity > 0.0f) {
                    for (int y = iy0; y < iy1; ++y) {
                        for (int x = ix0; x < ix1; ++x) {
                            mvaddch(y, x, '#');
                        }
                    }
                }
            }
        }
    }

    // Force a full, non-differential repaint rather than relying on
    // ncurses' internal change-tracking optimizer to compute a minimal
    // diff. This is load-bearing, not a style choice: without it, wrong
    // cells would intermittently render as lit on real hardware despite
    // ncurses' own internal buffer (verified via mvinch/PAIR_NUMBER)
    // always holding the correct state -- see README for the full
    // writeup of that investigation.
    redrawwin(stdscr);
    refresh();
}

} // namespace mitm
