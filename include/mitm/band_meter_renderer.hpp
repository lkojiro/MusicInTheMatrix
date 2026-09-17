#pragma once

#include <array>
#include <vector>

#include <ncurses.h>

namespace mitm {

// Three vertical VU-meter-style progress bars -- low (<80Hz), mid
// (80Hz-700Hz), and high (>700Hz) -- each its own bordered ncurses box
// titled "Low"/"Mid"/"High" (upper case and bold, see boldBorder_ below,
// whenever any segment in the top 3/4 of the bar is still lit or
// fading -- i.e. roughly whenever the bar is a quarter full; lower case
// and plain otherwise) -- deliberately reading off whether the bar is
// still *showing* anything up there rather than the instantaneous raw
// level, so this lags the level exactly as long as that segment's own
// fade-out does, the same "sticky" trailing feel the fade already gives
// the segments themselves, instead of the border snapping the instant
// the level dips under a quarter. Each bar
// is a stack of discrete segments, not one continuous fill -- a classic
// LED VU meter look, lighting from the bottom up one segment at a time
// as that band gets louder, a thin gap between segments where there's
// room for one. Segments are a fixed height (kSegmentHeightRows rows
// each, see segmentSpan() in the .cpp) rather than a fixed *count*: the
// number of segments a bar actually shows (segmentCount_) is worked out
// from the terminal's current height instead, so a taller terminal shows
// more segments at the same size rather than the same segment count
// stretched taller (and a shorter one shows fewer, rather than every
// segment getting squeezed thinner) -- recomputed alongside windows_
// whenever a rebuild happens. The one segment the level's fractional position actually falls inside
// doesn't just snap to fully lit the moment the level reaches it: it
// settles at whichever of the fade ramp's kFadeLevels brightness steps
// is closest to how far up through its own range the level currently
// sits (see the `target` computation in draw()'s pass 1) -- the same
// ramp built for the transient fade doing double duty as extra
// resolution for the meter's steady-state reading, rather than the bar
// only ever moving in whole-segment jumps.
//
// Segment color is a fixed zone by position, not the window's base
// color -- each of 4 zones (top 2 segments, next 4, next 4, and the rest
// -- the bottom 6) gets its own hue, but which 4 hues come from one of
// several fixed color schemes (see kSchemeZoneColors in the .cpp) rather
// than GlobalConfig::baseColor: this is the one renderer that ignores
// the web panel's color picker entirely. The up/down keys, otherwise
// "change the window's color" everywhere else, do something different
// here instead of nothing: they cycle through the schemes themselves
// (nextScheme()/prevScheme(), wired up in subordinate.cpp) rather than
// picking a single hue, since a VU meter's whole point is those 4 zones
// reading as low/rising/high/peak, not one flat color.
//
// A segment that turns back off fades out over kFadeDurationSeconds, and
// one that turns on fades in over kFadeInDurationSeconds (see the .cpp),
// rather than switching instantly either way -- tracked per segment as a
// single continuous brightness value (segmentBrightness_ below), not two
// independent "time since it turned off"/"time since it turned on"
// counters. That distinction matters the moment a segment reverses
// direction mid-fade (the level dips just enough to start it fading out,
// then rises again before it finishes) -- two independent counters would
// each reset from their own fixed endpoint, so the segment would jump to
// "just went dark, fading in from black" instead of continuing smoothly
// from wherever it already was, which is exactly what read as flicker
// when a bar draining down and a bar filling up collided on the same
// segment. A single brightness value sidesteps that by construction: it
// only ever moves incrementally from its current value toward whichever
// end is currently the target, so reversing direction just means it
// starts moving the other way from right where it already is.
//
// Fade-outs (moving down) cascade top-to-bottom rather than firing in
// parallel: a segment isn't allowed to *descend* until the one directly
// above it has dropped by one brightness step (not necessarily finished
// completely -- see draw()'s two-pass structure in the .cpp), keeping a
// constant one-step brightness gap between vertically adjacent segments
// for the whole drain, reading as a smooth gradient sweeping down the
// bar rather than each LED fully blinking out in turn before the next
// even starts. Fade-ins (moving up) aren't gated by neighbors at all --
// every segment is always free to rise toward lit as soon as it should
// be.
//
// Each bar's lit-segment count is its own band's absolute level alone
// (bandAbsoluteLevel() in the .cpp) -- no cross-band percentage/share
// blending; Low, Mid, and High are all measured the same way, so a bar
// fills according to how loud that part of the spectrum actually is,
// not what fraction of the total it happens to make up. No peak-hold
// smoothing on any of this -- the per-segment fade above is what keeps
// a bar from reading as flicker (each segment lingers, dimming, after
// it turns off) rather than a second layer of smoothing on top of it.
//
// Which of the FFT's log-spaced buckets fall into which band is worked
// out once at construction (see classifyBuckets() in the .cpp) using
// FftProcessor::bucketFrequencyRange() with the same
// sampleRate/windowSize/bucketCount the host's own FftProcessor was
// built with (config::kSampleRate/kWindowSize/kBucketCount) -- this
// renderer never runs its own FFT, just groups the bucket magnitudes
// it's handed each frame (see AudioEventSink) by the frequency range
// each one already covers.
//
// Unlike every other renderer here, this one keeps its 3 box windows
// alive across draw() calls (see windows_ below) instead of creating and
// destroying them every frame: the box borders and "Low"/"Mid"/"High"
// titles never change, so they're drawn exactly once (and again only if
// the terminal is resized) -- draw() itself only ever touches each box's
// interior fill rows. Recreating and re-bordering all 3 boxes on every
// single ~8ms tick was the actual cause of a whole-window flicker
// (borders included, not just the fill) that this design avoids
// entirely rather than working around: repeatedly redrawing
// alternate-charset box-drawing characters at 125Hz is visibly janky on
// real terminals even when the final content never changes. This is
// also why this header (uniquely among the renderers) includes
// <ncurses.h> directly -- it needs WINDOW* as real member state, not
// just a type used transiently inside a .cpp function.
//
// The "only rebuild the border on a resize" assumption breaks the moment
// something else fully repaints the screen in between (another mode's
// renderer while this one's inactive, or the idle screen) -- the
// terminal's dimensions haven't changed, so draw() would otherwise skip
// re-drawing box()/the title entirely, leaving stale characters from
// whatever last owned the screen where this bar's border used to be.
// invalidate() is the caller's escape hatch for exactly that: whoever
// dispatches to this renderer (see the switch in subordinate.cpp) must
// call it once when *switching into* this mode -- i.e. whenever the
// previous frame's draw call wasn't this same renderer's -- so the very
// next draw() forces a full rebuild regardless of whether the terminal
// size actually changed.
//
// Does NOT own the ncurses session (no initscr()/endwin() here) -- see
// TerminalRenderer for why (arrow-key mode switching needs all
// renderers to share one session). The constructor sets up its own
// color pairs in a range reserved apart from every other renderer's so
// all can coexist.
class BandMeterRenderer {
public:
    BandMeterRenderer();
    ~BandMeterRenderer();

    BandMeterRenderer(const BandMeterRenderer&) = delete;
    BandMeterRenderer& operator=(const BandMeterRenderer&) = delete;

    // `buckets` is the same config::kBucketCount-entry FFT magnitude
    // array every other bucket-driven mode uses.
    void draw(const std::vector<float>& buckets);

    // (Re-)initializes this renderer's zone color pairs from the current
    // scheme (see nextScheme()/prevScheme()) -- unlike every other
    // renderer's setupColors(), this doesn't read GlobalConfig::baseColor
    // at all (see the class doc comment). Must be called from the thread
    // that owns the ncurses session, and again after nextScheme()/
    // prevScheme() actually change anything (they don't call this
    // themselves, same division of labor color_scheme.hpp's own
    // nextColorName()/prevColorName() have with their callers).
    void setupColors();

    // Cycles to the next/previous fixed color scheme (wrapping both
    // ways) -- what the up/down keys drive for this renderer instead of
    // picking a single hue; see the class doc comment. Only updates
    // which scheme is selected; call setupColors() afterward to actually
    // apply it.
    void nextScheme();
    void prevScheme();

    // Forces the next draw() call to fully rebuild all 3 boxes (border
    // and title included) regardless of whether the terminal's size
    // actually changed -- see the class doc comment for why the caller
    // needs this when switching back into this mode after something
    // else has had the screen.
    void invalidate();

private:
    // Fixed height, in rows, of every segment's slot -- see the class
    // doc comment and segmentSpan() in the .cpp. Chosen to match what
    // 16 segments naturally worked out to at a 67-row terminal (a
    // 65-row box interior divided 16 ways lands on 4 rows/segment),
    // which is where this renderer's segment spacing originally looked
    // best -- fixing it here means it keeps looking exactly like that at
    // any terminal height, instead of segments getting taller or
    // shorter (and their gap row appearing or disappearing) as the
    // available height changes.
    static constexpr int kSegmentHeightRows = 4;

    // How many segments the current terminal height actually fits
    // (interiorHeight / kSegmentHeightRows, at least 1) -- see the class
    // doc comment. Recomputed only when a rebuild happens (alongside
    // windows_), not every frame; segmentBrightness_ below is resized to
    // match at the same time.
    int segmentCount_ = 0;

    // Which bucket indices (into a config::kBucketCount-sized array) sum
    // into each band, computed once at construction rather than every
    // frame.
    std::vector<size_t> lowBuckets_;
    std::vector<size_t> midBuckets_;
    std::vector<size_t> highBuckets_;

    // Current brightness, [0, 1], per segment (indexed
    // [band][segmentFromBottom], segmentFromBottom 0 = the bottom-most
    // segment) -- persisted across draw() calls, moved incrementally
    // toward 1 (lit) or 0 (unlit) each frame rather than reset from a
    // fixed endpoint; see the class doc comment for why that distinction
    // matters. Starts at 0 (nothing lit yet), a real value rather than a
    // sentinel -- unlike the age-based counters this replaced, 0 is
    // already the correct "not lit" state, not a special-cased fake one.
    // A vector, not a fixed-size array, since segmentCount_ -- and so
    // how many entries each band actually needs -- can change on a
    // resize; resized (and reset to all-0, same reasoning as
    // boldBorder_) alongside segmentCount_ itself.
    std::array<std::vector<float>, 3> segmentBrightness_{};

    // The 3 persistent box windows (index 0/1/2 = low/mid/high), null
    // until the first draw() call builds them (needs a real terminal
    // size, which isn't known at construction). Rebuilt -- border,
    // title, and all -- only when the terminal's dimensions actually
    // change; every other frame just repaints the interior fill.
    std::array<WINDOW*, 3> windows_{nullptr, nullptr, nullptr};
    int builtScreenWidth_ = -1;
    int builtScreenHeight_ = -1;

    // Whether each bar's border is currently drawn bold (index 0/1/2 =
    // low/mid/high) -- true whenever any segment in that bar's top 3/4
    // is still lit or fading (see the class doc comment for why that's
    // read off segmentBrightness_ rather than the raw level directly).
    // draw() only re-runs box() when this actually flips, not every
    // frame, same reasoning as windows_ only rebuilding on a real size
    // change.
    std::array<bool, 3> boldBorder_{false, false, false};

    bool colorEnabled_ = false;
    bool extendedColor_ = false; // true if the terminal offers a 256-color palette

    // Index into kSchemeZoneColors (see the .cpp) -- which of the fixed
    // color schemes (Default/Warm/Cool/Greyscale) is currently applied.
    // Persists across mode switches like everything else here, since
    // this object itself isn't recreated when the window switches away
    // from and back to this mode.
    int scheme_ = 0;
};

} // namespace mitm
