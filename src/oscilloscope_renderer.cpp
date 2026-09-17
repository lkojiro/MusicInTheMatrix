#include "mitm/oscilloscope_renderer.hpp"

#include "mitm/color_scheme.hpp"
#include "mitm/global_config.hpp"

#include <algorithm>
#include <cmath>

#include <ncurses.h>

namespace mitm {

namespace {
// Color pair id reserved for this renderer, kept clear of
// MatrixRainRenderer's (1-8), TerminalRenderer's (10) and
// CheckerboardRenderer's (20+) -- all visual-mode renderers are
// constructed up front for arrow-key mode switching, so their color
// pairs share one global namespace.
constexpr short kWavePair = 15;
} // namespace

OscilloscopeRenderer::OscilloscopeRenderer() {
    setupColors(); // requires curses_util::init() to have already run -- see header
}

void OscilloscopeRenderer::setupColors() {
    if (!has_colors()) {
        colorEnabled_ = false;
        return;
    }
    start_color();
    use_default_colors(); // -1 background below means "leave the terminal's own background alone"
    colorEnabled_ = true;
    extendedColor_ = (COLORS >= 256);

    BaseColor base = getGlobalConfig().baseColor;
    // One solid color for the trace, same as TerminalRenderer's bars --
    // there's no brightness-ramp concept here, just the base hue.
    short fg = extendedColor_ ? static_cast<short>(cubeToXterm256(base.r, base.g, base.b))
                               : static_cast<short>(basicAnsiColorIndex(base));
    init_pair(kWavePair, fg, -1);
}

void OscilloscopeRenderer::draw(const std::vector<float>& samples) {
    int height = 0;
    int width = 0;
    getmaxyx(stdscr, height, width);
    erase();

    if (width <= 0 || height <= 0) {
        refresh();
        return;
    }

    // A dim center line (0 amplitude) so the trace has a visible
    // reference to bow away from, same role as TerminalRenderer's
    // bottom-row baseline.
    int centerY = height / 2;
    for (int x = 0; x < width; ++x) {
        mvaddch(centerY, x, '-');
    }

    size_t pointCount = samples.size();
    if (pointCount == 0) {
        refresh();
        return;
    }

    int amplitude = std::max(1, height / 2 - 1);

    auto rowForSample = [&](float sample) {
        float clamped = std::clamp(sample, -1.0f, 1.0f);
        int offset = static_cast<int>(std::lround(clamped * amplitude));
        return std::clamp(centerY - offset, 0, height - 1);
    };

    // Resamples `samples` (config::kWaveformPointCount entries) to
    // `column`'s exact fractional position across the terminal width via
    // linear interpolation between its two nearest points -- so the
    // trace always stretches to span the full width regardless of how
    // the waveform's point count compares to it, rather than laying out
    // one discrete column (or block of columns) per point and leaving
    // any leftover width as padding.
    auto sampleAtColumn = [&](int column) {
        if (pointCount == 1) return samples[0];
        float t = width > 1 ? static_cast<float>(column) / static_cast<float>(width - 1) : 0.0f;
        float pos = t * static_cast<float>(pointCount - 1);
        size_t idx0 = static_cast<size_t>(pos);
        size_t idx1 = std::min(idx0 + 1, pointCount - 1);
        float frac = pos - static_cast<float>(idx0);
        return samples[idx0] * (1.0f - frac) + samples[idx1] * frac;
    };

    int attrs = colorEnabled_ ? COLOR_PAIR(kWavePair) : A_NORMAL;
    attron(attrs);

    // Thin line, not a solid fill: a flat run between two columns draws
    // as '-'; a level change draws as a vertical run of '|' capped by a
    // single '/' or '\' at the row the line actually arrives on -- '/'
    // when that arrival row is above the departure row (the value rose),
    // '\' when it's below (the value fell). Only that one boundary row
    // gets the diagonal cap; every other row in the run is '|', so a
    // one-row step is just its diagonal cap and nothing else, while a
    // multi-row jump reads as a vertical run tipped by one slanted cell.
    // Every step here spans exactly one screen column (never more, never
    // padding) since sampleAtColumn() already stretched the data to fit.
    int prevY = rowForSample(sampleAtColumn(0));
    mvaddch(prevY, 0, '-');
    for (int col = 1; col < width; ++col) {
        int y = rowForSample(sampleAtColumn(col));
        bool rising = y < prevY; // smaller row = higher on screen = larger amplitude

        if (y == prevY) {
            mvaddch(y, col, '-');
        } else {
            for (int row = std::min(prevY, y); row <= std::max(prevY, y); ++row) {
                char ch = row == y ? (rising ? '/' : '\\') : '|';
                mvaddch(row, col, ch);
            }
        }
        prevY = y;
    }

    attroff(attrs);
    refresh();
}

} // namespace mitm
