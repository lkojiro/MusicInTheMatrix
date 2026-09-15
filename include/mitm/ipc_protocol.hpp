#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mitm::ipc {

// Wire protocol between the controller process and subordinate visualizer
// processes, exchanged as newline-free JSON objects over a length-prefixed
// framing (see UnixSocketConnection). Deliberately tiny: the controller
// mainly tells a subordinate whether it's the "active" (live-rendering)
// window right now, or asks it to quit. Subordinates run their own
// AudioCapture/FftProcessor pipeline independently -- no audio or frame
// data crosses this socket, which keeps the protocol trivial and avoids
// any bandwidth/sync concerns as the number of windows grows.

// Subordinate -> controller (or child -> host), sent once right after
// connecting. `id` is the connecting process's own pid (unique without
// needing the other side to pre-assign one) except in the explicit
// --controller/--subordinate path, where the controller still assigns
// sequential ids at spawn time. `spawnToken`, when non-empty, matches a
// token the host generated when spawning this window via 'n' -- lets the
// host correlate "the window I just spawned and have an AppleScript
// handle for" with "the connection that just said hello", since the two
// happen on different threads/timings with nothing else linking them.
// Empty for windows the host didn't spawn itself (manually launched by
// the user, or connections from windows a *child* spawned) -- the host
// has no way to close those explicitly; see runHost in host.cpp.
struct HelloMessage {
    int id = 0;
    long pid = 0;
    std::string spawnToken;
};

// Subordinate -> controller, sent just before a subordinate exits on its
// own (e.g. the user pressed 'q' in that window) so the controller can
// drop it from rotation instead of waiting on a dead socket.
struct ByeMessage {
    int id = 0;
};

// Controller -> subordinate: you are now (or no longer) the live window.
struct ActivateMessage {
    bool active = false;
};

// Controller -> subordinate: shut down cleanly.
struct ShutdownMessage {};

// Subordinate -> controller (or child -> host): "the user pressed 'n' in
// my window, please spawn a new window on my behalf." Only the
// orchestrator holds a WindowLauncher (and, in the host/child path, the
// spawn-token bookkeeping needed to later explicitly close() what it
// spawns -- see pendingHandles/pendingHellos in host.cpp), so a
// subordinate that isn't itself the orchestrator can't spawn directly;
// it just asks. The new window ends up a sibling of the requester, not a
// child *of* it -- there's still exactly one orchestrator per process
// tree. runHost handles this (see the accept thread's per-connection
// reader in host.cpp); the explicit --controller path doesn't listen for
// it yet, so sending this to a plain --controller is a silent no-op
// there.
struct SpawnRequestMessage {};

// Controller -> subordinate, broadcast to every connected subordinate
// whenever the window set changes (one connects or disconnects) -- the
// controller's current view of "what does the whole multi-window setup
// look like right now". Infrastructure for future multi-window effects
// that need this (see GlobalConfig in global_config.hpp, which a
// subordinate updates from this message); nothing consumes it yet beyond
// storing it.
struct ConfigMessage {
    int windowCount = 0;
    std::vector<int> windowIds;
};

inline nlohmann::json toJson(const HelloMessage& m) {
    return {{"type", "hello"}, {"id", m.id}, {"pid", m.pid}, {"spawn_token", m.spawnToken}};
}

inline nlohmann::json toJson(const ByeMessage& m) {
    return {{"type", "bye"}, {"id", m.id}};
}

inline nlohmann::json toJson(const ActivateMessage& m) {
    return {{"type", "activate"}, {"active", m.active}};
}

inline nlohmann::json toJson(const ShutdownMessage&) {
    return {{"type", "shutdown"}};
}

inline nlohmann::json toJson(const SpawnRequestMessage&) {
    return {{"type", "spawn_request"}};
}

inline nlohmann::json toJson(const ConfigMessage& m) {
    return {{"type", "config"}, {"window_count", m.windowCount}, {"window_ids", m.windowIds}};
}

// Returns the "type" field, or an empty string if the message is malformed.
inline std::string messageType(const nlohmann::json& msg) {
    return msg.value("type", std::string());
}

} // namespace mitm::ipc
