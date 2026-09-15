#pragma once

#include <string>

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
    SpawnWindow, // 'n' / 'N'
};
InputAction pollInput();

// Shared idle screen for subordinate windows that aren't currently the
// active visualizer, regardless of which visual mode they'd otherwise run.
void drawIdleScreen(const std::string& label);

// Sets the terminal window/tab title via the standard xterm OSC 0
// escape sequence, widely supported including by Terminal.app. Doesn't
// actually need ncurses at all (it's consumed by the terminal emulator
// itself, never rendered into the character grid), so it's safe to call
// either before curses_util::init() or any time after -- it won't
// disturb whatever's currently on screen.
void setWindowTitle(const std::string& title);

} // namespace mitm::curses_util
