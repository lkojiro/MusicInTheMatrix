#pragma once

#include <string>

namespace mitm {

// Abstraction over "open a shell command in a new terminal window" /
// "close that window", so the controller doesn't need to know which
// terminal emulator or automation mechanism is behind it. Swap in an
// iTerm2- or kitty-native implementation later without touching the
// controller or the IPC protocol.
class WindowLauncher {
public:
    virtual ~WindowLauncher() = default;

    // Opens a new terminal window running `command` via the shell.
    // Returns an opaque, implementation-defined handle that can later be
    // passed to close(), or -1 on failure.
    virtual long spawn(const std::string& command) = 0;

    // Closes a previously spawned window. Should be a no-op (not an
    // error) if the window was already closed, e.g. because the process
    // running inside it exited on its own.
    virtual void close(long handle) = 0;
};

// Drives Terminal.app via AppleScript (`osascript`). No extra dependency
// beyond what ships with macOS. Each spawn() opens a new Terminal window
// and captures its window id (by reading the frontmost window's id
// immediately after `do script`, which always opens a new window) so
// close() can target that specific window later.
class AppleScriptWindowLauncher : public WindowLauncher {
public:
    long spawn(const std::string& command) override;
    void close(long handle) override;
};

// Resolves the absolute path to the currently-running binary. Useful
// alongside WindowLauncher for "spawn another copy of myself" -- a
// spawned Terminal window starts in an arbitrary working directory, so a
// relative path like argv[0] can't be relied on to still resolve there.
// Returns an empty string on failure. macOS-specific (_NSGetExecutablePath).
std::string currentExecutablePath();

} // namespace mitm
