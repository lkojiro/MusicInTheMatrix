#include "mitm/window_launcher.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <mach-o/dyld.h> // _NSGetExecutablePath
#include <unistd.h>

namespace mitm {

namespace {

// Escapes backslashes and double quotes so `command` can be embedded
// inside an AppleScript double-quoted string literal.
std::string escapeForAppleScript(const std::string& command) {
    std::string out;
    out.reserve(command.size());
    for (char c : command) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Writes `script` to a fresh temp file and runs it with `osascript`,
// returning the process's captured stdout (empty on any failure). Using a
// temp file rather than `-e` per line keeps quoting simple, and rather
// than a shell heredoc keeps this independent of the caller's shell.
std::string runAppleScript(const std::string& script) {
    char tmpPath[] = "/tmp/mitm_launcher_XXXXXX.applescript";
    int fd = mkstemps(tmpPath, static_cast<int>(std::strlen(".applescript")));
    if (fd < 0) return "";

    bool writeOk = ::write(fd, script.data(), script.size()) == static_cast<ssize_t>(script.size());
    ::close(fd);
    if (!writeOk) {
        ::unlink(tmpPath);
        return "";
    }

    std::string cmd = std::string("osascript ") + tmpPath + " 2>/dev/null";
    std::string output;
    if (FILE* pipe = popen(cmd.c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            output += buf;
        }
        pclose(pipe);
    }

    ::unlink(tmpPath);
    return output;
}

} // namespace

long AppleScriptWindowLauncher::spawn(const std::string& command) {
    // `do script` with no target window always opens a new Terminal window
    // and brings it to front, so reading `window 1` (frontmost) right
    // after reliably identifies the one we just created.
    std::string script = "tell application \"Terminal\"\n"
                          "    activate\n"
                          "    do script \"" + escapeForAppleScript(command) + "\"\n"
                          "    delay 0.3\n"
                          "    return id of window 1\n"
                          "end tell\n";

    std::string output = runAppleScript(script);
    if (output.empty()) return -1;

    char* end = nullptr;
    long windowId = std::strtol(output.c_str(), &end, 10);
    if (end == output.c_str()) return -1; // no digits parsed at all

    return windowId;
}

void AppleScriptWindowLauncher::close(long handle) {
    if (handle < 0) return;

    // Terminal's dictionary doesn't support `window id N` as a reference
    // form (it raises "Can't get window id N", -1728) despite `id` being a
    // readable property -- explicitly walking the window list and
    // comparing ids is what actually works. Note this only closes windows
    // that are idle at a shell prompt: closing one with a foreground
    // process still running pops Terminal's "this will terminate the
    // running process" confirmation sheet, which nothing here can answer
    // headlessly, so `close` returns without error but the window stays
    // open. In practice this is fine -- the controller always tells a
    // subordinate to shut down over the socket first (see runController),
    // so by the time close() is called the window's shell should already
    // be idle or gone.
    std::string script = "tell application \"Terminal\"\n"
                          "    try\n"
                          "        repeat with w in windows\n"
                          "            if id of w is " + std::to_string(handle) + " then\n"
                          "                close w saving no\n"
                          "                exit repeat\n"
                          "            end if\n"
                          "        end repeat\n"
                          "    end try\n"
                          "end tell\n";
    runAppleScript(script);
}

std::string currentExecutablePath() {
    char pathBuf[PATH_MAX];
    uint32_t size = sizeof(pathBuf);
    if (_NSGetExecutablePath(pathBuf, &size) != 0) {
        return "";
    }
    char resolved[PATH_MAX];
    if (realpath(pathBuf, resolved)) {
        return resolved;
    }
    return pathBuf;
}

} // namespace mitm
