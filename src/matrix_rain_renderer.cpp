#include "mitm/matrix_rain_renderer.hpp"

#include "mitm/color_scheme.hpp"
#include "mitm/global_config.hpp"
#include "mitm/visualizer_config.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <ncurses.h>

namespace mitm {

namespace {

constexpr char kCharset[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*<>/\\|";
constexpr int kCharsetLen = sizeof(kCharset) - 1; // exclude the trailing '\0'
// Plain ASCII rather than the movie's katakana: ncurses here is built
// against the system (narrow/single-byte) Curses, not ncursesw, so wide
// Unicode glyphs aren't a safe bet without confirming that build --
// worth revisiting if this project ever links ncursesw specifically.

constexpr int kMinLength = 4;
constexpr int kMaxLength = 20;

// Time to fall the full screen height, top to bottom -- each drop picks a
// random duration in this range at spawn time. Expressed as a duration
// (not a rows/second rate) so fall speed is derived from the *current*
// terminal height every frame: a drop always takes the same amount of
// time to cross the screen regardless of how tall it is, including if
// the terminal is resized mid-fall. Duration variety isn't from the spec
// -- it's a small embellishment matching the source material, easy to
// collapse to a single constant if uniform timing is preferred instead.
constexpr float kMinFallDurationSeconds = 1.0f;
constexpr float kMaxFallDurationSeconds = 3.0f;

// Expected new strings per column per second, scaled by loudness between
// these two bounds. At a ~200-column terminal, the low end averages
// roughly a couple of spawns per second; the high end several times that.
// (Reduced ~30% from the original 0.012/0.15 for a less dense screen.)
constexpr double kMinSpawnChancePerColumnPerSecond = 0.0084;
constexpr double kMaxSpawnChancePerColumnPerSecond = 0.105;

// Fraction of columns that get an extra immediate spawn on a beat pulse
// -- this is the "burst" that reads as a reaction to the beat, on top of
// (not instead of) the continuous loudness-driven spawning above. Not
// scaled by frame rate: it's a one-shot event per detected beat, not an
// ongoing rate.
constexpr double kBeatBurstFraction = 0.15;

// Rate (not probability) at which any single character gets
// re-randomized, per second -- this is the "change characters repeatedly
// while it falls" flicker. Scaled by config::kFrameDeltaSeconds into a
// per-frame probability below.
constexpr double kFlickerRatePerCharPerSecond = 4.8;

// Distinct brightness steps drawn along a string, head (0) to tail. Level
// 0 -- the brightest -- is reserved for pulsing (see below) and never
// used in normal, unpulsed rendering, so a beat pulse always has a
// visibly brighter step to jump to rather than the head already sitting
// at max brightness with nowhere left to go.
constexpr int kBrightnessLevels = 8;

// How fast a beat-triggered brightness pulse fades back to normal, in
// intensity/second -- 2.5 means a full-intensity pulse (1.0) takes ~400ms
// to decay to 0 (doubled from an original ~200ms for a more noticeable,
// less snappy flash).
constexpr float kPulseDecayPerSecond = 2.5f;

// How many brightness levels a full-intensity pulse can shift a
// character by. Deliberately less than kBrightnessLevels: a shift big
// enough to blow through the whole range would flatten every character
// in the string to the same brightest level, losing the head-to-tail
// gradient that makes it read as a string rather than a flat blob. A
// smaller shift brightens the whole thing while still preserving (a
// compressed version of) that gradient.
constexpr int kMaxPulseBrightnessShift = 3;

// How many brightness levels louder audio can shift the whole scene
// toward the bright end, continuously (not just on beats). Applied within
// the normal 1..kBrightnessLevels-1 range -- loud audio never reaches
// level 0, which stays a pulse-exclusive "brighter than anything sustained
// volume can produce" flash.
constexpr int kMaxLoudnessBrightnessShift = 4;

// Fraction of currently-visible drops that get brightness-pulsed on a
// beat at full loudness (1.0), scaled linearly down to 0 at loudness 0.
// Calibrated assuming loudness at beat time is roughly uniform over
// [0, 1] on average: the expected flashed fraction is then
// kBeatPulseFractionAtFullLoudness / 2. 0.4 targets an average of 1/5
// flashed per beat (scaled back from an original 2/3, which averaged 1/3
// and looked too busy). Real music won't be perfectly uniform (loud
// transients tend to correlate with beats), so treat this as an
// approximate target, not a guarantee.
constexpr double kBeatPulseFractionAtFullLoudness = 0.4;

} // namespace

MatrixRainRenderer::MatrixRainRenderer() : rng_(std::random_device{}()) {
    setupColors(); // requires curses_util::init() to have already run -- see header
}

void MatrixRainRenderer::setupColors() {
    if (!has_colors()) {
        colorEnabled_ = false;
        return;
    }
    start_color();
    colorEnabled_ = true;
    extendedColor_ = (COLORS >= 256);

    if (extendedColor_) {
        // A near-white head (231, deliberately colorless regardless of
        // base color -- it's the pulse-exclusive brightest step, not
        // part of the hue) fading through a brightness ramp derived from
        // the shared GlobalConfig::baseColor, then into a couple of
        // fixed dark-gray steps at the very tail (238, 236 -- "almost
        // off" isn't really about hue either). Only the middle 5 levels
        // actually carry the base color.
        std::vector<int> hueRamp = makeBrightnessRamp(getGlobalConfig().baseColor, 5);
        short kRamp[kBrightnessLevels] = {
            231,
            static_cast<short>(hueRamp[0]), static_cast<short>(hueRamp[1]), static_cast<short>(hueRamp[2]),
            static_cast<short>(hueRamp[3]), static_cast<short>(hueRamp[4]),
            238, 236,
        };
        for (int level = 0; level < kBrightnessLevels; ++level) {
            init_pair(static_cast<short>(level + 1), kRamp[level], COLOR_BLACK);
        }
    } else {
        // Basic terminal fallback: white head, base-color trail,
        // differentiated further with A_BOLD in draw().
        init_pair(1, COLOR_WHITE, COLOR_BLACK);
        init_pair(2, static_cast<short>(basicAnsiColorIndex(getGlobalConfig().baseColor)), COLOR_BLACK);
    }
}

int MatrixRainRenderer::colorPairForLevel(int level) const {
    if (!colorEnabled_) return 0;
    if (extendedColor_) return level + 1;
    return level == 0 ? 1 : 2;
}

char MatrixRainRenderer::randomChar() {
    std::uniform_int_distribution<int> dist(0, kCharsetLen - 1);
    return kCharset[dist(rng_)];
}

MatrixRainRenderer::Drop MatrixRainRenderer::makeDrop(int x) {
    Drop drop;
    drop.x = x;
    drop.headRow = -1.0f; // starts just above the top edge

    std::uniform_int_distribution<int> lengthDist(kMinLength, kMaxLength);
    drop.length = lengthDist(rng_);

    std::uniform_real_distribution<float> durationDist(kMinFallDurationSeconds, kMaxFallDurationSeconds);
    drop.fallDurationSeconds = durationDist(rng_);

    drop.chars.resize(drop.length);
    for (char& c : drop.chars) c = randomChar();

    return drop;
}

void MatrixRainRenderer::spawnDrops(float loudness) {
    float l = std::clamp(loudness, 0.0f, 1.0f);
    double ratePerSecond = kMinSpawnChancePerColumnPerSecond +
                            (kMaxSpawnChancePerColumnPerSecond - kMinSpawnChancePerColumnPerSecond) * l;
    double chancePerColumn = ratePerSecond * config::kFrameDeltaSeconds;

    std::uniform_real_distribution<double> chance(0.0, 1.0);
    for (int x = 0; x < width_; ++x) {
        if (chance(rng_) < chancePerColumn) {
            drops_.push_back(makeDrop(x));
        }
    }
}

void MatrixRainRenderer::spawnBeatBurst() {
    std::uniform_real_distribution<double> chance(0.0, 1.0);
    for (int x = 0; x < width_; ++x) {
        if (chance(rng_) < kBeatBurstFraction) {
            drops_.push_back(makeDrop(x));
        }
    }
}

void MatrixRainRenderer::triggerBeatPulse(float loudness) {
    if (drops_.empty()) return;

    double fraction = std::clamp(loudness, 0.0f, 1.0f) * kBeatPulseFractionAtFullLoudness;
    size_t count = std::min(drops_.size(), static_cast<size_t>(std::llround(fraction * drops_.size())));
    if (count == 0) return; // a quiet moment can legitimately flash nothing

    std::vector<size_t> indices(drops_.size());
    std::iota(indices.begin(), indices.end(), 0);

    std::vector<size_t> chosen;
    chosen.reserve(count);
    std::sample(indices.begin(), indices.end(), std::back_inserter(chosen), count, rng_);

    for (size_t i : chosen) {
        drops_[i].pulseIntensity = 1.0f;
    }
}

void MatrixRainRenderer::draw(float loudness, bool beatPulse) {
    getmaxyx(stdscr, height_, width_);
    erase();

    if (width_ <= 0 || height_ <= 0) {
        refresh();
        return;
    }

    if (beatPulse) triggerBeatPulse(loudness); // pick from drops as they exist before this frame's spawns

    spawnDrops(loudness);
    if (beatPulse) spawnBeatBurst();

    std::uniform_real_distribution<double> flicker(0.0, 1.0);

    double flickerChancePerFrame = kFlickerRatePerCharPerSecond * config::kFrameDeltaSeconds;

    // Same for every drop this frame -- louder audio brightens the whole
    // scene continuously, on top of (and independent from) any per-drop
    // beat pulse.
    int loudnessShift =
        static_cast<int>(std::round(std::clamp(loudness, 0.0f, 1.0f) * kMaxLoudnessBrightnessShift));

    for (auto& drop : drops_) {
        // Recomputed from the current height every frame (rather than
        // cached at spawn time) so a live resize doesn't leave in-flight
        // drops still paced by the screen's old height.
        float rowsPerSecond =
            drop.fallDurationSeconds > 0.0f ? static_cast<float>(height_) / drop.fallDurationSeconds : 0.0f;
        drop.headRow += rowsPerSecond * config::kFrameDeltaSeconds;

        if (drop.pulseIntensity > 0.0f) {
            drop.pulseIntensity = std::max(0.0f, drop.pulseIntensity - kPulseDecayPerSecond * config::kFrameDeltaSeconds);
        }

        for (int d = 0; d < drop.length; ++d) {
            if (flicker(rng_) < flickerChancePerFrame) {
                drop.chars[d] = randomChar();
            }
        }

        if (drop.x < 0 || drop.x >= width_) continue;

        // A pulsing drop's characters shift toward the brightest level in
        // proportion to how much pulse is left, then relax back to their
        // normal head-to-tail gradient as it decays.
        int pulseShift = static_cast<int>(std::round(drop.pulseIntensity * kMaxPulseBrightnessShift));

        int headRowInt = static_cast<int>(std::floor(drop.headRow));
        for (int d = 0; d < drop.length; ++d) {
            int row = headRowInt - d;
            if (row < 0 || row >= height_) continue;

            // Base (unpulsed) level starts at 1, not 0 -- the head sits
            // one step below max brightness by default, leaving level 0
            // free as a pulse-only "extra bright" step (see kBrightnessLevels).
            int baseLevel = std::min(d, kBrightnessLevels - 2) + 1;
            // Loudness brightens within the normal range but is clamped to
            // never reach level 0 itself -- only a pulse can do that.
            int loudnessAdjusted = std::clamp(baseLevel - loudnessShift, 1, kBrightnessLevels - 1);
            int level = std::clamp(loudnessAdjusted - pulseShift, 0, kBrightnessLevels - 1);
            int attrs = A_NORMAL;
            if (colorEnabled_) attrs |= COLOR_PAIR(colorPairForLevel(level));
            if (level == 0) attrs |= A_BOLD; // extra pop -- only reachable while pulsing

            attron(attrs);
            mvaddch(row, drop.x, drop.chars[d]);
            attroff(attrs);
        }
    }

    // Despawn once the entire string -- including its dimmest, trailing
    // (topmost) character -- has fallen past the bottom edge.
    drops_.erase(std::remove_if(drops_.begin(), drops_.end(),
                                 [this](const Drop& d) {
                                     float tailRow = d.headRow - static_cast<float>(d.length - 1);
                                     return tailRow > static_cast<float>(height_ - 1);
                                 }),
                 drops_.end());

    refresh();
}

} // namespace mitm
