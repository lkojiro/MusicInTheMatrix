#pragma once

#include <cstddef>
#include <random>
#include <vector>

namespace mitm {

// Classic "digital rain" effect: vertical strings of characters falling
// down the screen, spawning above the top edge and despawning once fully
// past the bottom. Each string keeps a fixed length (chosen randomly at
// spawn time) and randomly re-rolls individual characters as it falls;
// the leading (bottom-most) character is drawn brightest, fading toward
// the trailing end.
//
// Unlike TerminalRenderer's bar chart, this is a pure visual effect --
// not audio-driven, so it doesn't need AudioCapture/FftProcessor at all.
//
// Does NOT own the ncurses session: the constructor sets up color pairs
// via setupColors(), which requires curses_util::init() to have already
// been called by the caller (standalone.cpp / subordinate.cpp) -- shared
// with TerminalRenderer so a single run can switch live between the two
// via arrow keys without tearing down and reinitializing the terminal.
class MatrixRainRenderer {
public:
    MatrixRainRenderer();

    MatrixRainRenderer(const MatrixRainRenderer&) = delete;
    MatrixRainRenderer& operator=(const MatrixRainRenderer&) = delete;

    // Advances the simulation by one frame and redraws it.
    // `loudness` (expected roughly [0, 1]) continuously scales how often
    // new strings spawn and brightens the whole scene; `beatPulse`
    // triggers a one-frame burst of extra strings across random columns,
    // plus a brief brightness flash on a random subset of existing
    // strings -- the louder `loudness` is at that moment, the larger the
    // subset (see kBeatPulseFractionAtFullLoudness). Pass loudness=0,
    // beatPulse=false for the original non-reactive animation (e.g. when
    // no audio is available).
    void draw(float loudness, bool beatPulse);

private:
    struct Drop {
        int x = 0;
        float headRow = 0.0f;             // fractional row of the leading character; floor()'d when drawing
        float fallDurationSeconds = 0.0f; // time to cross the full screen height, top to bottom
        int length = 0;
        std::vector<char> chars;     // chars[0] is the head, chars[length-1] is the trailing (dimmest) end
        float pulseIntensity = 0.0f; // 1.0 right when beat-pulsed, decays to 0
    };

    Drop makeDrop(int x);
    void spawnDrops(float loudness);
    void spawnBeatBurst();
    void triggerBeatPulse(float loudness);
    char randomChar();
    void setupColors();
    int colorPairForLevel(int level) const;

    int width_ = 0;
    int height_ = 0;
    std::vector<Drop> drops_;
    std::mt19937 rng_;

    bool colorEnabled_ = false;
    bool extendedColor_ = false; // true if the terminal offers a 256-color palette
};

} // namespace mitm
