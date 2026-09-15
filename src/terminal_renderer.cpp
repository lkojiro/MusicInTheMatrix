#include "mitm/terminal_renderer.hpp"

#include "mitm/color_scheme.hpp"
#include "mitm/global_config.hpp"
#include "mitm/visualizer_config.hpp"

#include <algorithm>
#include <cmath>

#include <ncurses.h>

namespace mitm {

namespace {
// How fast a peak cap falls once it starts decaying, in rows/second --
// scaled by config::kFrameDeltaSeconds below so this stays the same real
// fall speed regardless of the configured frame rate.
constexpr float kPeakFallRowsPerSecond = 18.0f;

// Color pair id reserved for this renderer, kept clear of
// MatrixRainRenderer's (1-8) and CheckerboardRenderer's (20+) -- all
// visual-mode renderers are constructed up front for arrow-key mode
// switching, so their color pairs share one global namespace.
constexpr short kBarPair = 10;
}  // namespace

TerminalRenderer::TerminalRenderer() {
    setupColors(); // requires curses_util::init() to have already run -- see header
}

void TerminalRenderer::setupColors() {
    if (!has_colors()) {
        colorEnabled_ = false;
        return;
    }
    start_color();
    use_default_colors(); // -1 background below means "leave the terminal's own background alone"
    colorEnabled_ = true;
    extendedColor_ = (COLORS >= 256);

    BaseColor base = getGlobalConfig().baseColor;
    // Just one solid, full-brightness color -- bars mode has no
    // brightness ramp/gradient concept the way Matrix rain or
    // Checkerboard do, so there's no "levels" to pick from here, only
    // the base hue itself. Falls back to the nearest basic ANSI color on
    // terminals without a 256-color palette, same as the other renderers.
    short fg = extendedColor_ ? static_cast<short>(cubeToXterm256(base.r, base.g, base.b))
                               : static_cast<short>(basicAnsiColorIndex(base));
    init_pair(kBarPair, fg, -1);
}

void TerminalRenderer::draw(const std::vector<float>& magnitudes, BarLayout layout) {
    getmaxyx(stdscr, height_, width_);
    erase();

    if (width_ <= 0 || height_ <= 0) {
        refresh();
        return;
    }

    // Baseline: a constant row of '_' along the bottom so the screen never
    // looks totally empty during quiet audio. Bars ('#') draw on top of it.
    for (int x = 0; x < width_; ++x) {
        mvaddch(height_ - 1, x, '_');
    }

    size_t bucketCount = magnitudes.size();
    if (bucketCount == 0) {
        refresh();
        return;
    }

    // Reset peak state if the bucket count changed (e.g. first frame, or a
    // future config change) rather than reading stale/mismatched entries.
    if (peakRows_.size() != bucketCount) {
        peakRows_.assign(bucketCount, 0.0f);
    }

    // How many column-slots the current layout needs across the screen,
    // and where slot 0 sits: Left/Right need exactly one slot per bucket
    // (just anchored to opposite edges); Middle needs one shared center
    // slot for bucket 0 plus a mirrored pair of slots for every other
    // bucket, so it needs roughly twice the slots for the same bucket
    // count. `anchorOffset` is where any leftover width (from integer
    // division not dividing evenly) goes: none for Left (matches its
    // original left-edge-anchored behavior), all of it for Right (so the
    // *used* columns are the ones flush against the right edge), and
    // split in half for Middle (so the mirrored spread stays roughly
    // centered rather than drifting toward one edge).
    int totalSlots = layout == BarLayout::Middle ? 2 * static_cast<int>(bucketCount) - 1
                                                  : static_cast<int>(bucketCount);
    int colsPerSlot = std::max(1, width_ / totalSlots);
    int usedWidth = totalSlots * colsPerSlot;
    int anchorOffset = 0;
    switch (layout) {
        case BarLayout::Left:
            anchorOffset = 0;
            break;
        case BarLayout::Right:
            anchorOffset = width_ - usedWidth;
            break;
        case BarLayout::Middle:
            anchorOffset = (width_ - usedWidth) / 2;
            break;
    }
    int centerSlot = static_cast<int>(bucketCount) - 1; // only meaningful for Middle

    // Only the '#' bar body is colored -- the baseline/peak-hold '_'
    // markers stay the terminal's plain default foreground, same as
    // before this renderer had a color concept at all.
    int attrs = colorEnabled_ ? COLOR_PAIR(kBarPair) : A_NORMAL;

    for (size_t b = 0; b < bucketCount; ++b) {
        float mag = std::clamp(magnitudes[b], 0.0f, 1.0f);
        float barHeightF = mag * static_cast<float>(height_ - 1);
        int barHeight = static_cast<int>(barHeightF);

        // Peak cap: snap up instantly if the current bar is taller, else
        // decay toward the live bar (never below it -- the cap sits on top
        // of the bar, not inside it). Keyed by bucket, so this is exactly
        // the same regardless of which layout is currently drawing it.
        float& peak = peakRows_[b];
        if (barHeightF > peak) {
            peak = barHeightF;
        } else {
            peak = std::max(barHeightF, peak - kPeakFallRowsPerSecond * config::kFrameDeltaSeconds);
        }
        int peakRow = std::clamp(static_cast<int>(std::round(peak)), 0, height_ - 1);
        int peakY = height_ - 1 - peakRow;

        // Which slot(s) this bucket occupies: Left counts up from the
        // left, Right counts up from the right (bucket 0 lands on the
        // last slot, i.e. the right edge), Middle mirrors every non-zero
        // bucket to both sides of the shared center slot.
        int bi = static_cast<int>(b);
        int slotCount = 1;
        int slots[2] = {0, 0};
        switch (layout) {
            case BarLayout::Left:
                slots[0] = bi;
                break;
            case BarLayout::Right:
                slots[0] = static_cast<int>(bucketCount) - 1 - bi;
                break;
            case BarLayout::Middle:
                slots[0] = centerSlot + bi;
                if (bi != 0) {
                    slots[1] = centerSlot - bi;
                    slotCount = 2;
                }
                break;
        }

        for (int i = 0; i < slotCount; ++i) {
            int colStart = anchorOffset + slots[i] * colsPerSlot;

            attron(attrs);
            for (int row = 0; row < barHeight; ++row) {
                int y = height_ - 1 - row;
                for (int c = 0; c < colsPerSlot; ++c) {
                    int x = colStart + c;
                    if (x >= 0 && x < width_) {
                        mvaddch(y, x, '#');
                    }
                }
            }
            attroff(attrs);

            for (int c = 0; c < colsPerSlot; ++c) {
                int x = colStart + c;
                if (x >= 0 && x < width_) {
                    mvaddch(peakY, x, '_');
                }
            }
        }
    }

    refresh();
}

} // namespace mitm
