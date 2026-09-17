#pragma once

#include <string>
#include <vector>

namespace mitm::curses_util {

// Shared ncurses lifecycle helpers. ncurses is process-global state
// (stdscr etc.), and a single run now needs to switch between renderers
// live (arrow-key mode switching) without tearing down and reinitializing
// the terminal on every switch -- so init()/teardown() are owned once by
// the top-level run loop (standalone.cpp / subordinate.cpp), not by
// individual renderers.

void init();
void teardown();

// One key's worth of input per call, non-blocking. Every recognized key
// is read here (a single getch() per frame) rather than each caller
// polling separately, since a second getch() call in the same frame
// would consume a different buffered keystroke, not re-check the same
// one.
enum class InputAction {
    None,
    Quit,        // 'q' / 'Q'
    NextMode,    // right arrow
    PrevMode,    // left arrow
    NextColor,   // up arrow
    PrevColor,   // down arrow
    SpawnWindow, // 'n' / 'N'
};
InputAction pollInput();

// Shared idle screen for subordinate windows that aren't currently the
// active visualizer, regardless of which visual mode they'd otherwise run.
void drawIdleScreen(const std::string& label);

// The host's own screen: it no longer renders a visualizer itself (see
// host.cpp), just this status display -- a large "MITM" banner (made of
// '#', its own bordered ncurses window, titled with `version`, flanked by
// a small FFT-reactive equalizer built from `fftMagnitudes` -- values
// roughly in [0, 1], low frequency first, same convention as
// TerminalRenderer::draw()) above a second bordered window listing each
// connected subordinate (already formatted by the caller, e.g.
// "Window 0: bars-left / green"), with the quit/spawn key prompt as that
// second window's own title.
void drawHostStatus(const std::string& version, const std::vector<std::string>& windowLines,
                     const std::vector<float>& fftMagnitudes);

// Sets the terminal window/tab title via the standard xterm OSC 0
// escape sequence, widely supported including by Terminal.app. Doesn't
// actually need ncurses at all (it's consumed by the terminal emulator
// itself, never rendered into the character grid), so it's safe to call
// either before curses_util::init() or any time after -- it won't
// disturb whatever's currently on screen.
void setWindowTitle(const std::string& title);

} // namespace mitm::curses_util
