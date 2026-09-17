#include "mitm/curses_util.hpp"

#include <algorithm>
#include <cstdio>

#include <ncurses.h>

namespace mitm::curses_util {

namespace {

// A tiny 5-wide/7-tall block-letter font, '#'-filled -- just enough
// characters for the "MITM" banner drawHostStatus() draws. Not a
// general-purpose font; add glyphs here only if the banner text changes.
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
// Each font "pixel" is drawn as a kBannerScale x kBannerScale block of
// '#' rather than a single character -- bump this to make the banner
// bigger/smaller without redrawing the glyphs themselves.
constexpr int kBannerScale = 2;

struct Glyph {
    char ch;
    const char* rows[kGlyphHeight];
};

constexpr Glyph kGlyphs[] = {
    {'M',
     {
         "#...#",
         "##.##",
         "#.#.#",
         "#...#",
         "#...#",
         "#...#",
         "#...#",
     }},
    {'I',
     {
         "#####",
         "..#..",
         "..#..",
         "..#..",
         "..#..",
         "..#..",
         "#####",
     }},
    {'T',
     {
         "#####",
         "..#..",
         "..#..",
         "..#..",
         "..#..",
         "..#..",
         "..#..",
     }},
};

const Glyph* findGlyph(char c) {
    for (const auto& g : kGlyphs) {
        if (g.ch == c) return &g;
    }
    return nullptr;
}

// The on-screen width/height of `text` rendered via drawBanner() below --
// needed by the caller to size/center the window that holds it before
// any drawing happens.
int bannerWidth(const std::string& text) {
    if (text.empty()) return 0;
    int letterWidth = kGlyphWidth * kBannerScale;
    int gap = kBannerScale; // one blank glyph-column's worth, scaled
    return static_cast<int>(text.size()) * letterWidth + (static_cast<int>(text.size()) - 1) * gap;
}

int bannerHeight() { return kGlyphHeight * kBannerScale; }

// Draws `text` as bannerHeight()-tall block letters made of '#', top-left
// corner at (y, x) within `win`. A character missing from kGlyphs just
// advances by one blank glyph's width, so unsupported input degrades to
// extra spacing rather than breaking layout.
void drawBanner(WINDOW* win, int y, int x, const std::string& text) {
    for (char c : text) {
        const Glyph* glyph = findGlyph(c);
        for (int row = 0; row < kGlyphHeight; ++row) {
            const char* line = glyph ? glyph->rows[row] : "     ";
            for (int col = 0; col < kGlyphWidth; ++col) {
                if (line[col] != '#') continue;
                for (int dy = 0; dy < kBannerScale; ++dy) {
                    for (int dx = 0; dx < kBannerScale; ++dx) {
                        mvwaddch(win, y + row * kBannerScale + dy, x + col * kBannerScale + dx, '#');
                    }
                }
            }
        }
        x += kGlyphWidth * kBannerScale + kBannerScale;
    }
}

// Downsamples `buckets` (kBucketCount FFT magnitudes, low frequency
// first) into whichever of `rows` this particular row covers, averaging
// if more than one bucket lands in it. Row 0 is always the lowest
// frequencies -- callers place row 0 at the top so the flanking bars in
// drawHostStatus() read low-to-high top-to-bottom, same convention as
// TerminalRenderer's bucket 0.
float rowMagnitude(const std::vector<float>& buckets, int row, int rows) {
    if (buckets.empty() || rows <= 0) return 0.0f;
    size_t n = buckets.size();
    size_t start = static_cast<size_t>(row) * n / static_cast<size_t>(rows);
    size_t end = static_cast<size_t>(row + 1) * n / static_cast<size_t>(rows);
    end = std::max(end, start + 1);
    end = std::min(end, n);
    float sum = 0.0f;
    for (size_t i = start; i < end; ++i) sum += buckets[i];
    return sum / static_cast<float>(end - start);
}

} // namespace

void init() {
    initscr();
    curs_set(0);            // hide the cursor
    noecho();
    nodelay(stdscr, TRUE);  // getch() returns immediately if no key is waiting
    keypad(stdscr, TRUE);
}

void teardown() {
    endwin();
}

InputAction pollInput() {
    switch (getch()) {
        case 'q':
        case 'Q':
            return InputAction::Quit;
        case KEY_RIGHT:
            return InputAction::NextMode;
        case KEY_LEFT:
            return InputAction::PrevMode;
        case KEY_UP:
            return InputAction::NextColor;
        case KEY_DOWN:
            return InputAction::PrevColor;
        case 'n':
        case 'N':
            return InputAction::SpawnWindow;
        default:
            return InputAction::None;
    }
}

void drawIdleScreen(const std::string& label) {
    int height = 0;
    int width = 0;
    getmaxyx(stdscr, height, width);
    erase();

    if (width > 0 && height > 0) {
        for (int x = 0; x < width; ++x) {
            mvaddch(height - 1, x, '_');
        }
        int y = height / 2;
        int x = std::max(0, (width - static_cast<int>(label.size())) / 2);
        mvaddnstr(y, x, label.c_str(), width);
    }

    refresh();
}

void drawHostStatus(const std::string& version, const std::vector<std::string>& windowLines,
                     const std::vector<float>& fftMagnitudes) {
    int screenHeight = 0;
    int screenWidth = 0;
    getmaxyx(stdscr, screenHeight, screenWidth);
    erase();

    if (screenWidth <= 0 || screenHeight <= 0) {
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }

    // Recreated fresh every call rather than kept as persistent state:
    // both windows are cheap to build (a handful of ncurses calls) and
    // this keeps drawHostStatus() stateless like every other draw
    // function here, tracking the terminal's current size every frame
    // (a live resize just changes what's computed below) instead of
    // needing separate resize-handling code.
    constexpr const char* kBannerText = "MITM";
    int bannerContentWidth = bannerWidth(kBannerText);
    constexpr int kTitlePaddingX = 2;
    int titleWidth = std::min(screenWidth, bannerContentWidth + 2 * kTitlePaddingX + 2);
    int titleHeight = std::min(screenHeight, bannerHeight() + 2);
    int titleX = std::max(0, (screenWidth - titleWidth) / 2);
    constexpr int titleY = 0;

    bool titleFits = titleWidth >= 3 && titleHeight >= 3;

    if (titleFits) {
        // A small equalizer flanking the title box: one horizontal bar
        // per screen row spanning the box's height, base flush against
        // the box's left/right edge and growing outward as that row's
        // (downsampled) FFT magnitude increases. Row 0 -- the top, right
        // under the box's top border -- is always the lowest frequencies,
        // same low-to-high top-to-bottom convention as the bar
        // visualizer's bucket 0. Drawn straight onto stdscr (outside
        // either box's own window) since it lives in the margin between
        // them and the screen edge -- and, importantly, refreshed via
        // stdscr *before* the title/windows boxes below rather than
        // after: wnoutrefresh() composites whichever window it's given
        // into the shared virtual screen in call order, so an erased
        // stdscr refreshed after a box would just paint blank cells back
        // over that box's rectangle.
        constexpr int kMaxBarLength = 8;
        int barRows = titleHeight - 2;
        int leftMargin = titleX;
        int rightMargin = screenWidth - (titleX + titleWidth);
        for (int row = 0; row < barRows; ++row) {
            float magnitude = std::clamp(rowMagnitude(fftMagnitudes, row, barRows), 0.0f, 1.0f);
            int barLength = static_cast<int>(magnitude * kMaxBarLength + 0.5f);
            int y = titleY + 1 + row;

            int leftLength = std::min(barLength, leftMargin);
            for (int i = 0; i < leftLength; ++i) mvaddch(y, titleX - 1 - i, '#');

            int rightLength = std::min(barLength, rightMargin);
            for (int i = 0; i < rightLength; ++i) mvaddch(y, titleX + titleWidth + i, '#');
        }
    }

    wnoutrefresh(stdscr);

    if (titleFits) {
        WINDOW* titleWin = newwin(titleHeight, titleWidth, titleY, titleX);
        box(titleWin, 0, 0);
        std::string versionLabel = " v" + version + " ";
        mvwaddnstr(titleWin, 0, 2, versionLabel.c_str(), titleWidth - 4);
        if (titleHeight - 2 >= bannerHeight()) {
            drawBanner(titleWin, 1, std::max(1, (titleWidth - bannerContentWidth) / 2), kBannerText);
        }
        wnoutrefresh(titleWin);
        delwin(titleWin);
    }

    constexpr int kOuterMarginX = 1;
    int windowsWidth = screenWidth > 2 * kOuterMarginX ? screenWidth - 2 * kOuterMarginX : screenWidth;
    int windowsX = screenWidth > 2 * kOuterMarginX ? kOuterMarginX : 0;
    int windowsY = titleY + titleHeight + 1; // one blank row between the two boxes
    int windowsHeight = screenHeight - windowsY;

    if (windowsWidth >= 3 && windowsHeight >= 3) {
        WINDOW* windowsWin = newwin(windowsHeight, windowsWidth, windowsY, windowsX);
        box(windowsWin, 0, 0);
        mvwaddnstr(windowsWin, 0, 2, " Connected Windows ", windowsWidth - 4);
        std::string quitPrompt = " [Q] Quit   [N] Spawn Window ";
        mvwaddnstr(windowsWin, windowsHeight - 1, 2, quitPrompt.c_str(), windowsWidth - 4);

        int maxLines = windowsHeight - 2;
        if (windowLines.empty()) {
            if (maxLines >= 1) {
                mvwaddnstr(windowsWin, 1, 2, "No windows connected yet -- press 'n' to spawn one.",
                           windowsWidth - 4);
            }
        } else {
            for (int i = 0; i < static_cast<int>(windowLines.size()) && i < maxLines; ++i) {
                mvwaddnstr(windowsWin, 1 + i, 2, windowLines[static_cast<size_t>(i)].c_str(),
                           windowsWidth - 4);
            }
        }

        wnoutrefresh(windowsWin);
        delwin(windowsWin);
    }

    doupdate();
}

void setWindowTitle(const std::string& title) {
    std::printf("\033]0;%s\007", title.c_str());
    std::fflush(stdout);
}

} // namespace mitm::curses_util
