#include "mitm/terminal_renderer.hpp"

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
}  // namespace

void TerminalRenderer::draw(const std::vector<float>& magnitudes) {
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

    int colsPerBucket = std::max(1, width_ / static_cast<int>(bucketCount));

    // Reset peak state if the bucket count changed (e.g. first frame, or a
    // future config change) rather than reading stale/mismatched entries.
    if (peakRows_.size() != bucketCount) {
        peakRows_.assign(bucketCount, 0.0f);
    }

    for (size_t b = 0; b < bucketCount; ++b) {
        float mag = std::clamp(magnitudes[b], 0.0f, 1.0f);
        float barHeightF = mag * static_cast<float>(height_ - 1);
        int barHeight = static_cast<int>(barHeightF);

        // Peak cap: snap up instantly if the current bar is taller, else
        // decay toward the live bar (never below it -- the cap sits on top
        // of the bar, not inside it).
        float& peak = peakRows_[b];
        if (barHeightF > peak) {
            peak = barHeightF;
        } else {
            peak = std::max(barHeightF, peak - kPeakFallRowsPerSecond * config::kFrameDeltaSeconds);
        }
        int peakRow = std::clamp(static_cast<int>(std::round(peak)), 0, height_ - 1);

        int colStart = static_cast<int>(b) * colsPerBucket;
        for (int row = 0; row < barHeight; ++row) {
            int y = height_ - 1 - row;
            for (int c = 0; c < colsPerBucket; ++c) {
                int x = colStart + c;
                if (x < width_) {
                    mvaddch(y, x, '#');
                }
            }
        }

        int peakY = height_ - 1 - peakRow;
        for (int c = 0; c < colsPerBucket; ++c) {
            int x = colStart + c;
            if (x < width_) {
                mvaddch(peakY, x, '_');
            }
        }
    }

    refresh();
}

} // namespace mitm
