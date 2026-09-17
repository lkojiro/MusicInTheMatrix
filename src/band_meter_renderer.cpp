#include "mitm/band_meter_renderer.hpp"

#include "mitm/color_scheme.hpp"
#include "mitm/fft_processor.hpp"
#include "mitm/visualizer_config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include <ncurses.h>

namespace mitm {

namespace {

// Band boundaries in Hz -- see the class's own doc comment.
constexpr float kLowMidBoundaryHz = 70.0f;
constexpr float kMidHighBoundaryHz = 700.0f;

// How long a segment takes to fade from full brightness to off after it
// stops being lit, in seconds -- "a short fade", not a lingering one.
constexpr float kFadeDurationSeconds = 0.1f;
constexpr int kFadeLevels = 3; // brightness steps in that fade

// How long a segment takes to fade *in*, from dark to full brightness,
// the moment it becomes lit -- the fade-out's mirror image (see
// kFadeInDurationSeconds' use in draw()'s pass 1).
constexpr float kFadeInDurationSeconds = 0.05f;

// A segment below an unlit one doesn't wait for that segment to
// *completely* finish fading before it starts descending itself -- just
// until it's dropped one brightness step (see the aboveBrightness gate
// in draw()'s pass 1). That keeps a constant one-step gap between
// vertically adjacent segments' brightness throughout the whole drain,
// reading as a smooth gradient sweeping down the bar instead of each
// segment fully blinking out in turn before the next one even starts.
constexpr float kStepBrightness = 1.0f / static_cast<float>(kFadeLevels);

// Fixed per-segment-zone colors, bottom to top -- these bars deliberately
// do NOT follow GlobalConfig::baseColor (see the class doc comment for
// why): a VU meter's 4 zones are a fixed convention per scheme, not a
// cosmetic choice that should change with the rest of the UI's color.
// Xterm-256 color-cube coordinates, same representation TerminalRenderer
// etc. use via color_scheme.hpp.
//
// Default and Candy read bottom-to-top as "calm to intense" (green up
// through red for Default, lime through purple for Candy);
// Warm/Cool/Greyscale run the opposite direction on purpose (their
// brightest/most saturated shade at the bottom, dimming toward the top)
// -- each still its own hue family (or, for Greyscale, no hue at all).
// kSchemeCount/scheme_ (see the header) select which row of this table
// setupColors() reads.
constexpr int kZoneCount = 4;
constexpr int kSchemeCount = 5;
constexpr BaseColor kSchemeZoneColors[kSchemeCount][kZoneCount] = {
    // Default: green -> yellow -> orange -> red. (5, 2, 0) is xterm-256
    // index 208, a standard "orange".
    {{0, 5, 0}, {5, 5, 0}, {5, 2, 0}, {5, 0, 0}},
    // Warm: gold -> orange -> red-orange -> deep red.
    {{5, 5, 1}, {5, 4, 0}, {5, 2, 0}, {4, 0, 0}},
    // Cool: bright cyan -> sky blue -> blue -> deep blue.
    {{1, 5, 5}, {0, 4, 5}, {0, 2, 5}, {0, 0, 4}},
    // Greyscale: bright to dim, no hue at all.
    {{5, 5, 5}, {4, 4, 4}, {3, 3, 3}, {2, 2, 2}},
    // Candy: lime -> turquoise -> pink -> cyan
    {{1, 5, 2}, {0, 3, 4}, {5, 0, 4}, {0, 5, 5}},
};

// Color pair ids reserved for this renderer (26-41: 4 zones' lit pairs,
// then 4 zones x kFadeLevels fade pairs), kept clear of
// MatrixRainRenderer's (1-8), TerminalRenderer's (10),
// OscilloscopeRenderer's (15), and CheckerboardRenderer's (20-25) -- all
// visual-mode renderers are constructed up front for arrow-key mode
// switching, so their color pairs share one global namespace.
constexpr short kColorPairBase = 26;
short litPair(int zone) { return static_cast<short>(kColorPairBase + zone); }
short fadePair(int zone, int level) {
    return static_cast<short>(kColorPairBase + kZoneCount + zone * kFadeLevels + level);
}

// Which of the 4 zones (see kZoneColors) segment `segmentFromBottom` (of
// `totalSegments`, 0 = the bottom-most) belongs to -- the top ~1/8 of the
// bar is red, the next ~1/4 orange, the next ~1/4 yellow, and the rest
// (the bottom ~3/8) green, the same proportions the original fixed
// 2/4/4/6-out-of-16 split worked out to, just scaled to whatever
// totalSegments actually is now that it varies with terminal height
// (see segmentCount_) instead of always being 16. Each zone's segment
// count is clamped to at least 1 so a short bar with only a handful of
// segments still shows all 4 colors somewhere rather than the smaller
// zones vanishing entirely.
int zoneForSegment(int segmentFromBottom, int totalSegments) {
    int fromTop = totalSegments - 1 - segmentFromBottom;
    int red = std::max(1, totalSegments * 2 / 16);
    int orange = std::max(1, totalSegments * 4 / 16);
    int yellow = std::max(1, totalSegments * 4 / 16);
    if (fromTop < red) return 3;
    if (fromTop < red + orange) return 2;
    if (fromTop < red + orange + yellow) return 1;
    return 0;
}

// Sorts config::kBucketCount log-spaced FFT buckets into low/mid/high by
// each bucket's midpoint frequency (its start/end Hz averaged) against
// the two boundaries above -- a bucket whose range straddles a boundary
// lands wherever its midpoint does, rather than needing to be split
// across two bands.
void classifyBuckets(std::vector<size_t>& low, std::vector<size_t>& mid, std::vector<size_t>& high) {
    using namespace config;
    for (size_t b = 0; b < kBucketCount; ++b) {
        auto [startHz, endHz] = bucketFrequencyRange(kSampleRate, kWindowSize, kBucketCount, b);
        float midHz = (startHz + endHz) / 2.0f;
        if (midHz < kLowMidBoundaryHz) {
            low.push_back(b);
        } else if (midHz < kMidHighBoundaryHz) {
            mid.push_back(b);
        } else {
            high.push_back(b);
        }
    }
}

// Sum of `buckets` at `indices`, un-normalized -- this band's raw share
// of spectral energy. Used two ways in draw(): summed across all 3 bands
// to find each of Mid/High's percentage of the combined total (see
// there for why), and on its own (via bandAbsoluteLevel() below) for
// Low, which is deliberately measured differently from its two
// siblings.
float bandEnergy(const std::vector<float>& buckets, const std::vector<size_t>& indices) {
    float sum = 0.0f;
    for (size_t i : indices) {
        if (i < buckets.size()) sum += buckets[i];
    }
    return sum;
}

// Low's own absolute level: bandEnergy() averaged by how many buckets
// went into it (so Low reads consistently regardless of exactly how many
// low-frequency buckets classifyBuckets() happened to assign it) and
// clamped to [0, 1], the same convention every other renderer's
// magnitude values already follow. Unlike Mid/High (see draw()), this
// never looks at the other two bands at all -- Low fills according to
// how loud the bass itself is, not what fraction of the total spectrum
// it happens to make up, so a quiet mix that's mostly bass doesn't read
// as a full Low bar just because there's little else going on.
float bandAbsoluteLevel(const std::vector<float>& buckets, const std::vector<size_t>& indices) {
    if (indices.empty()) return 0.0f;
    return std::clamp(bandEnergy(buckets, indices) / static_cast<float>(indices.size()), 0.0f, 1.0f);
}

// The [start, end) span of the `index`-th of `count` equal pieces that
// evenly divide a run of `total` cells -- used for the 3 boxes' column
// spans (see segmentSpan() below for why the segments *within* a box
// don't use this same scheme). Any remainder is spread one cell at a
// time across the first pieces (so sizes differ by at most one) rather
// than dumping it all into the last piece -- fine here since a
// column-width difference of one character between Low/Mid/High is
// unnoticeable, unlike a segment being visibly taller than its
// neighbors.
std::pair<int, int> evenSpan(int total, int count, int index) {
    int base = total / count;
    int remainder = total % count;
    int start = index * base + std::min(index, remainder);
    int end = start + base + (index < remainder ? 1 : 0);
    return {start, end};
}

// The [start, end) row span of the s-th of `segmentCount` LED segments,
// each exactly `slotHeight` rows tall, stacked in a box with
// `interiorHeight` interior rows -- deliberately NOT
// interiorHeight/segmentCount (which is how `segmentCount` was worked
// out to begin with, in draw(); re-deriving it here via a second floor
// division could round to a different value for a short enough
// interiorHeight, undermining the fixed height this exists for) nor
// evenSpan()'s "spread the remainder across the first few pieces" (a
// handful of segments ending up visibly taller than the rest reads as a
// layout bug, not a design choice, and -- since a segment only gets a
// gap row below it when it has 2+ rows to spare -- so would the gap
// disappearing for some segments but not others at the exact same
// terminal size). Any rows interiorHeight has left over after
// segmentCount slots of slotHeight each become a blank margin above the
// topmost segment (s == 0) instead, so every segment is identically
// sized and either all of them have a gap or none do, consistently, at
// any height.
std::pair<int, int> segmentSpan(int interiorHeight, int segmentCount, int slotHeight, int s) {
    int margin = interiorHeight - slotHeight * segmentCount;
    int start = margin + s * slotHeight;
    return {start, start + slotHeight};
}

struct Band {
    const char* name; // canonical mixed-case spelling; see bandLabel()
};

// The box title for `name` -- padded with one leading/trailing space (so
// callers can center it with a little breathing room from the border),
// and cased to match the bar's own loudness: upper when it's more than
// half full, lower otherwise -- the same threshold and the same moment
// (see the caller's bold-state check) that also bolds the border, so the
// title reads as shouting when the bar does and stays quiet the rest of
// the time.
std::string bandLabel(const char* name, bool loud) {
    std::string label = " ";
    for (const char* p = name; *p != '\0'; ++p) {
        label += static_cast<char>(loud ? std::toupper(static_cast<unsigned char>(*p))
                                         : std::tolower(static_cast<unsigned char>(*p)));
    }
    label += ' ';
    return label;
}

// Builds and centers `name`'s title (see bandLabel()) onto row 0 of `win`
// -- shared by the initial (always-lowercase) label drawn when a box is
// first built and the later redraw whenever the bold/case state actually
// flips, so the "how wide, how centered" logic can't drift between the
// two call sites.
void drawLabel(WINDOW* win, const char* name, bool loud, int boxWidth) {
    std::string label = bandLabel(name, loud);
    int labelX = std::max(1, (boxWidth - static_cast<int>(label.size())) / 2);
    mvwaddnstr(win, 0, labelX, label.c_str(), boxWidth - 2);
}

// What one segment's row span needs painted, decided in a separate pass
// before any drawing happens -- see the two-phase comment in draw().
struct SegmentPlan {
    int rowStart = 0;
    int rowEnd = 0;
    int fillRowEnd = 0; // [rowStart, fillRowEnd) is colored/reverse-video; [fillRowEnd, rowEnd) is the plain gap row
    int fillAttrs = A_NORMAL;
    bool colored = false;
};

} // namespace

BandMeterRenderer::BandMeterRenderer() {
    classifyBuckets(lowBuckets_, midBuckets_, highBuckets_);
    // segmentBrightness_ defaults to all-0 via its member initializer --
    // no sentinel needed here, since "not lit yet" and "brightness 0"
    // are the same real state, not a special-cased fake one.
    setupColors(); // requires curses_util::init() to have already run -- see header
}

BandMeterRenderer::~BandMeterRenderer() {
    for (WINDOW* win : windows_) {
        if (win) delwin(win);
    }
}

void BandMeterRenderer::invalidate() {
    // Sabotages draw()'s "did the terminal size actually change" check
    // so it rebuilds unconditionally next call, regardless of whether
    // the size changed -- see the class doc comment. -1 can never match
    // a real getmaxyx() result.
    builtScreenWidth_ = -1;
    builtScreenHeight_ = -1;
}

void BandMeterRenderer::nextScheme() { scheme_ = (scheme_ + 1) % kSchemeCount; }

void BandMeterRenderer::prevScheme() { scheme_ = (scheme_ + kSchemeCount - 1) % kSchemeCount; }

void BandMeterRenderer::setupColors() {
    if (!has_colors()) {
        colorEnabled_ = false;
        return;
    }
    start_color();
    colorEnabled_ = true;
    extendedColor_ = (COLORS >= 256);

    // The current scheme's 4 zone colors (see kSchemeZoneColors) --
    // deliberately NOT GlobalConfig::baseColor. This is the one renderer
    // that ignores the window's color-cycling keys' usual meaning and
    // the web panel's color picker entirely; see the class doc comment
    // for why.
    for (int zone = 0; zone < kZoneCount; ++zone) {
        const BaseColor& color = kSchemeZoneColors[scheme_][zone];
        short fg = extendedColor_ ? static_cast<short>(cubeToXterm256(color.r, color.g, color.b))
                                   : static_cast<short>(basicAnsiColorIndex(color));
        // Reverse video -- black text on the zone color -- rather than a
        // colored '#' on the default background, so the filled portion
        // of each meter reads as a solid block of color, same idea as
        // CheckerboardRenderer's lit cells.
        init_pair(litPair(zone), COLOR_BLACK, fg);

        // A short brightness ramp shared by both the fade-out and the
        // fade-in (see kFadeDurationSeconds/kFadeInDurationSeconds and
        // draw()'s pass 1) -- only meaningful with a 256-color palette to
        // actually shade from, same restriction CheckerboardRenderer's
        // own ramp has; a basic terminal just switches a segment
        // instantly on/off instead, no fade either direction.
        if (extendedColor_) {
            std::vector<int> ramp = makeBrightnessRamp(color, kFadeLevels);
            for (int level = 0; level < kFadeLevels; ++level) {
                init_pair(fadePair(zone, level), COLOR_BLACK, static_cast<short>(ramp[static_cast<size_t>(level)]));
            }
        }
    }
}

void BandMeterRenderer::draw(const std::vector<float>& buckets) {
    int screenHeight = 0;
    int screenWidth = 0;
    getmaxyx(stdscr, screenHeight, screenWidth);

    if (screenWidth < 3 || screenHeight < 3) return;

    const Band bands[3] = {
        {"Low"},
        {"Mid"},
        {"High"},
    };

    // Rebuild the 3 boxes -- border, title, and all -- only when the
    // terminal's dimensions actually changed since the last call (or on
    // the very first call ever, since builtScreenWidth_/Height_ start at
    // -1). Once drawn, a box's border and title never need to be redrawn
    // again on an unchanged-size terminal (the overwhelmingly common
    // case) -- see the class doc comment for why redoing this every
    // frame was visibly flickering the borders.
    if (screenWidth != builtScreenWidth_ || screenHeight != builtScreenHeight_) {
        for (WINDOW*& win : windows_) {
            if (win) delwin(win);
            win = nullptr;
        }
        builtScreenWidth_ = screenWidth;
        builtScreenHeight_ = screenHeight;

        // How many kSegmentHeightRows-tall segments the new box interior
        // actually fits -- every box shares the same screenHeight (only
        // their widths differ), so this is computed once here rather
        // than separately per box below. segmentBrightness_ is resized
        // (and reset to all-0, same reasoning as boldBorder_ just below)
        // to match, since a size change means the old entries may no
        // longer even correspond to the same segments.
        segmentCount_ = std::max(1, (screenHeight - 2) / kSegmentHeightRows);
        for (auto& band : segmentBrightness_) band.assign(static_cast<size_t>(segmentCount_), 0.0f);

        // A freshly (re)built box is always drawn not-bold below,
        // regardless of what boldBorder_ says from before the rebuild --
        // reset it to match, so the per-frame check further down (which
        // only redraws the border on an actual state change) doesn't
        // mistake "already bold before this rebuild, audio still loud"
        // for "nothing changed" and skip re-applying it.
        boldBorder_.fill(false);

        // delwin() only frees the WINDOW* structs above -- it doesn't
        // erase whatever they'd already drawn onto the actual screen. A
        // resize that shrinks the terminal (or otherwise changes the
        // per-box widths/heights, e.g. a width change alone still
        // reflowing how evenSpan() divides the 3 boxes) means the new
        // boxes below won't necessarily cover every cell the old ones
        // did, so without clearing stdscr first, stale border/fill
        // content from the previous layout lingers outside the new
        // boxes' bounds. This is a one-time cost exactly when a rebuild
        // is already happening (a real resize, or invalidate() after
        // switching into this mode), not a per-frame one, so it doesn't
        // reintroduce the flicker this design otherwise avoids. Order
        // matters here the same way it does for any other wnoutrefresh()
        // call in this codebase: this has to run before the boxes' own
        // wnoutrefresh() calls later in this same frame, or their
        // freshly drawn content would just get wiped by this blank
        // stdscr composited on top of it afterward.
        erase();
        wnoutrefresh(stdscr);

        // Three side-by-side boxes, each its own newwin() (like
        // drawHostStatus()'s stacked boxes) rather than one box() call
        // on stdscr, since adjacent boxes need their own separate
        // borders between them, not one continuous outline.
        for (int i = 0; i < 3; ++i) {
            auto [x0, x1] = evenSpan(screenWidth, 3, i);
            int boxWidth = x1 - x0;
            if (boxWidth < 3) continue; // no room for a border plus any interior

            WINDOW* win = newwin(screenHeight, boxWidth, 0, x0);
            box(win, 0, 0);

            // Built not-loud (lowercase) regardless of the bar's actual
            // level right now -- the per-frame bold/case check just
            // below always runs at least once right after this, on the
            // very same frame, and corrects it immediately if the level
            // already warrants it.
            drawLabel(win, bands[i].name, false, boxWidth);

            windows_[static_cast<size_t>(i)] = win;
        }
    }

    // Mid and High each fill to an even blend of two different readings:
    // their band's own absolute level (bandAbsoluteLevel(), same
    // measure Low uses) and their percentage of the combined energy
    // across all 3 bands (Low's included in that total, even though Low
    // itself isn't blended in this way -- see below). Neither alone was
    // right: percentage on its own is scale-invariant, so Mid/High would
    // sit at the same height whether the music's overall level just
    // dropped or not, completely decoupled from how loud things actually
    // are; absolute level on its own is what originally needed fixing --
    // bass usually carries so much more raw energy that Mid/High read as
    // almost always empty even when they're genuinely doing something.
    // Averaging the two keeps both properties at once: the percentage
    // half gives them the relative boost they need to ever show up
    // against the bass, while the absolute half keeps them moving up and
    // down with the music's actual level instead of just its spectral
    // shape. Low fills to its own absolute level alone instead
    // (bandAbsoluteLevel()) -- it's usually the loudest part of any mix
    // already, so it doesn't need the percentage half's boost the way
    // its siblings do. Computed once here, up front, since a percentage
    // inherently needs all 3 sums (Low's included) before Mid/High's
    // share of the total can be worked out; totalEnergy == 0 (true
    // silence) leaves both bands' percentage half at 0 rather than
    // dividing by zero.
    float lowEnergy = bandEnergy(buckets, lowBuckets_);
    float midEnergy = bandEnergy(buckets, midBuckets_);
    float highEnergy = bandEnergy(buckets, highBuckets_);
    float totalEnergy = lowEnergy + midEnergy + highEnergy;
    float midPercentage = totalEnergy > 0.0f ? midEnergy / totalEnergy : 0.0f;
    float highPercentage = totalEnergy > 0.0f ? highEnergy / totalEnergy : 0.0f;
    std::array<float, 3> fillFraction = {
        bandAbsoluteLevel(buckets, lowBuckets_),
        (bandAbsoluteLevel(buckets, midBuckets_) + midPercentage) / 2.0f,
        (bandAbsoluteLevel(buckets, highBuckets_) + highPercentage) / 2.0f,
    };

    // Every frame, regardless: repaint just each box's interior fill --
    // the only part that ever actually changes. Every interior row is
    // explicitly set (to a colored space if filled, a plain space
    // otherwise) rather than only touching newly-filled rows, so a row
    // that becomes unfilled again (the level dropping) gets properly
    // cleared instead of leaving a stale colored cell behind.
    for (int i = 0; i < 3; ++i) {
        WINDOW* win = windows_[static_cast<size_t>(i)];
        if (!win) continue;

        int boxHeight = 0;
        int boxWidth = 0;
        getmaxyx(win, boxHeight, boxWidth);
        int interiorWidth = boxWidth - 2;
        int interiorHeight = boxHeight - 2;
        if (interiorWidth <= 0 || interiorHeight <= 0) {
            wnoutrefresh(win);
            continue;
        }

        // This band's fill fraction, computed above (Low: its own
        // absolute level; Mid/High: percentage of the combined total) --
        // no peak-hold smoothing here either way; the per-segment
        // fade-out below is what keeps this from reading as flicker
        // (each segment lingers, dimming, for kFadeDurationSeconds after
        // it turns off, rather than the whole bar's height itself
        // needing to decay gradually to feel stable).
        float raw = fillFraction[static_cast<size_t>(i)];

        // Not truncated -- the fractional part is exactly how far into
        // whichever segment the level currently sits (see `target`
        // below), which is what lets that one segment show proportional
        // partial brightness instead of the whole meter only ever moving
        // in whole-segment jumps.
        float rawPosition = raw * static_cast<float>(segmentCount_);

        // Two passes, deliberately in opposite directions:
        //
        // Pass 1 (top segment to bottom, s = 0..segmentCount_-1) decides
        // every segment's brightness. This direction is load-bearing,
        // not cosmetic: a segment isn't allowed to *descend* until the
        // segment above it has dropped by one brightness step (see
        // aboveBrightness below) -- selling a top-to-bottom brightness
        // gradient sweeping down the bar instead of every unlit segment
        // fading out in parallel. Working out segment N's state needs
        // segment N-1's (the one above it, since s increases downward)
        // already-finalized brightness from this same frame, which is
        // only available by processing in this order.
        //
        // Pass 2 (bottom segment to top, s = segmentCount_-1..0) does
        // the actual drawing, using pass 1's results -- painting the
        // meter in the same bottom-up order it fills, rather than the
        // order pass 1 happened to compute in.
        std::vector<SegmentPlan> plan(static_cast<size_t>(segmentCount_));

        // Sentinel for "nothing above to wait on" -- lets the topmost
        // segment start descending immediately, same as if the
        // (nonexistent) segment above it had already dropped a step.
        float aboveBrightness = 0.0f;

        // The border goes bold, and the title switches from lowercase to
        // uppercase, whenever any segment in the top half of the bar is
        // still lit or fading -- not the instantaneous raw level itself.
        // Reading it off segmentBrightness_ (as each segment's finalized
        // just below, for this same frame) rather than `raw` directly
        // makes the bold state track what the bar is actually *showing*
        // -- including a top segment's own lingering fade-out -- so it
        // stays bold a little after the level itself dips back under
        // half, the same "sticky" trailing feel the fade already gives
        // the segments themselves, rather than snapping off exactly on
        // the instant. Folded into this same pass rather than a second
        // scan afterward -- segmentFromBottom/brightness are already
        // right here, finalized for this segment, so there's nothing a
        // separate pass over the top half would see that this one
        // doesn't.
        bool shouldBeBold = false;

        for (int s = 0; s < segmentCount_; ++s) {
            auto [rowStart, rowEnd] = segmentSpan(interiorHeight, segmentCount_, kSegmentHeightRows, s);

            int segmentFromBottom = segmentCount_ - 1 - s;
            int zone = zoneForSegment(segmentFromBottom, segmentCount_);

            // Target brightness for this segment: 1 if it's entirely
            // below the current level, 0 if entirely above, or -- for
            // whichever single segment the level's fractional position
            // actually falls inside -- how far up through that segment's
            // own range it sits. That segment settles at whichever of
            // the fade ramp's kFadeLevels shades is closest, rather than
            // snapping straight to fully lit the instant the level
            // crosses into its range: the ramp built for the transient
            // fade doubles as extra brightness resolution for the
            // meter's steady-state reading too.
            float target = std::clamp(rawPosition - static_cast<float>(segmentFromBottom), 0.0f, 1.0f);

            // A single continuous brightness value, moved incrementally
            // toward `target` -- not reset from a fixed endpoint -- so a
            // segment that reverses direction mid-fade (unlit just long
            // enough to start dimming, then lit again before finishing)
            // continues smoothly from wherever it already was instead of
            // jumping back to "freshly off" or "freshly on" first. That
            // jump was the actual cause of a flicker where a bar
            // draining down and a bar filling up collided on the same
            // segment.
            //
            // Rising toward a higher target is never gated by neighbors
            // -- always free to climb over kFadeInDurationSeconds.
            // Descending toward a lower target only proceeds once the
            // segment above has dropped by at least one brightness step
            // (aboveBrightness <= 1 - kStepBrightness); otherwise this
            // segment stays frozen wherever it currently is -- which, if
            // it was mid-fade-in when the level dropped, means holding
            // at its current partial brightness rather than snapping
            // down, until it's this segment's turn to start descending
            // too. Neither direction ever overshoots past `target`.
            float& brightness = segmentBrightness_[static_cast<size_t>(i)][static_cast<size_t>(segmentFromBottom)];
            if (target >= brightness) {
                brightness = std::min(target, brightness + config::kFrameDeltaSeconds / kFadeInDurationSeconds);
            } else if (aboveBrightness <= 1.0f - kStepBrightness) {
                brightness = std::max(target, brightness - config::kFrameDeltaSeconds / kFadeDurationSeconds);
            }
            aboveBrightness = brightness;

            if (segmentFromBottom >= segmentCount_ / 2 && brightness > 0.0f) {
                shouldBeBold = true;
            }

            int fillAttrs = A_NORMAL;
            bool colored = false;
            if (!extendedColor_) {
                // No brightness ramp to shade through -- binary on/off
                // (no partial-brightness segment either), same as before
                // this renderer had a fade concept at all.
                bool fullyLit = segmentFromBottom < static_cast<int>(rawPosition);
                if (fullyLit && colorEnabled_) {
                    fillAttrs = COLOR_PAIR(litPair(zone));
                    colored = true;
                } else if (fullyLit) {
                    fillAttrs = A_REVERSE; // no color support at all -- still show *something* lit
                    colored = true;
                }
            } else if (brightness >= 1.0f) {
                fillAttrs = COLOR_PAIR(litPair(zone));
                colored = true;
            } else if (brightness > 0.0f) {
                int level = std::clamp(static_cast<int>((1.0f - brightness) * kFadeLevels), 0, kFadeLevels - 1);
                fillAttrs = COLOR_PAIR(fadePair(zone, level));
                colored = true;
            }

            // Reserve the row closest to the segment above as a gap
            // (always plain, never colored) when there's room for one,
            // so lit/fading segments read as discrete blocks rather than
            // one continuous bar. Every segment is exactly
            // kSegmentHeightRows tall (segmentSpan()'s only caller always
            // passes that as slotHeight), so this only ever takes the
            // "no room for a gap" branch if that constant itself were
            // ever dropped to 1.
            int fillRowEnd = kSegmentHeightRows >= 2 ? rowEnd - 1 : rowEnd;

            plan[static_cast<size_t>(s)] = SegmentPlan{rowStart, rowEnd, fillRowEnd, fillAttrs, colored};
        }

        // Only ever redrawn on that actual crossing, not every frame --
        // re-running box() at 125Hz regardless of whether anything
        // changed is exactly what used to flicker this renderer's
        // borders (see the class doc comment); gating on a real state
        // change is the same fix applied here. box() only touches the
        // border cells, but row 0 IS a border row, so the title label
        // has to be redrawn right after or it'd be erased along with the
        // rest of that row -- bold right along with it, so the title
        // matches the border it sits on rather than looking like it was
        // left behind.
        bool& isBold = boldBorder_[static_cast<size_t>(i)];
        if (shouldBeBold != isBold) {
            isBold = shouldBeBold;
            if (isBold) wattron(win, A_BOLD);
            box(win, 0, 0);
            drawLabel(win, bands[i].name, isBold, boxWidth);
            if (isBold) wattroff(win, A_BOLD);
        }

        // Pass 2: paint bottom segment first, working up -- every
        // segment's full row span is always explicitly repainted
        // (colored if lit or fading, plain otherwise, gap row always
        // plain) rather than only touching what changed, so a segment
        // that finishes fading doesn't leave a stale colored row behind.
        // Each row is blanked with a single mvwhline() call rather than
        // interiorWidth individual mvwaddch() calls -- mvwhline() picks
        // up the window's currently-set attributes (see the wattron/
        // wattroff bracketing the fill rows below) exactly the same way
        // addch() does, just in one call per row instead of one per cell.
        auto fillRows = [&](int rowStart, int rowEnd) {
            for (int row = rowStart; row < rowEnd; ++row) {
                mvwhline(win, 1 + row, 1, ' ', interiorWidth);
            }
        };
        for (int s = segmentCount_ - 1; s >= 0; --s) {
            const SegmentPlan& p = plan[static_cast<size_t>(s)];

            if (p.colored) wattron(win, p.fillAttrs);
            fillRows(p.rowStart, p.fillRowEnd);
            if (p.colored) wattroff(win, p.fillAttrs);

            fillRows(p.fillRowEnd, p.rowEnd);
        }

        wnoutrefresh(win);
    }

    doupdate();
}

} // namespace mitm
