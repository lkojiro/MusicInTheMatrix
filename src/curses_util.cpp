#include "mitm/curses_util.hpp"

#include <algorithm>
#include <cstdio>

#include <ncurses.h>

namespace mitm::curses_util {

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

void setWindowTitle(const std::string& title) {
    std::printf("\033]0;%s\007", title.c_str());
    std::fflush(stdout);
}

} // namespace mitm::curses_util
