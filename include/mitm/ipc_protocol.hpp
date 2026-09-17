#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mitm::ipc {

// Wire protocol between the controller process and subordinate visualizer
// processes, exchanged as newline-free JSON objects over a length-prefixed
// framing (see UnixSocketConnection). Most of this is deliberately tiny --
// the controller mainly tells a subordinate whether it's the "active"
// (live-rendering) window right now, or asks it to quit. The one
// exception is AudioFrameMessage: the host runs the only
// AudioCapture/FftProcessor/BeatDetector pipeline (see AudioEventSink) and
// broadcasts its output to every connected subordinate once per
// processing tick, since subordinates no longer run that pipeline
// themselves.

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

// Host -> subordinate: switch to a different visual mode live. Same
// names as --visual (bars/matrix/checkerboard); unrecognized names fall
// back to bars, same as parseVisualMode(). Sent by the control server
// (see control_server.hpp) in response to a web UI action -- there's no
// keyboard shortcut for *remotely* changing another window's mode, only
// your own via the arrow keys.
struct SetModeMessage {
    std::string mode;
};

// Host -> subordinate: switch to a different color live. Same named
// presets as --color; see color_scheme.hpp. Unlike mode, there's no
// keyboard shortcut for changing a window's own color at all -- the web
// UI is the only way to change a running window's color.
struct SetColorMessage {
    std::string colorName;
};

// Subordinate -> host: "here's my current mode and color." Sent once
// right after hello (so the host's dashboard has an initial value rather
// than a gap), and again any time either actually changes -- whether
// that's this window's own arrow keys, or in response to a
// SetModeMessage/SetColorMessage it just received. A window's mode can
// change without the host ever telling it to (arrow keys are still
// local), so the host can't just assume its own last command reflects
// reality; this is what keeps control_server.hpp's dashboard honest.
struct StateMessage {
    std::string mode;
    std::string colorName;
};

// Host -> subordinate, broadcast to every connected subordinate once per
// processing tick: the current audio-reactive data (see AudioFrame in
// audio_event_sink.hpp), now that subordinates no longer capture/analyze
// audio themselves. Unlike every other message here, this one is sent at
// a real cadence (matched to the host's own capture tick, not just on
// state changes) -- see ChildBroadcastSink in host.cpp.
struct AudioFrameMessage {
    std::vector<float> buckets;
    std::vector<float> waveform;
    float loudness = 0.0f;
    bool beatDetected = false;
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

inline nlohmann::json toJson(const SetModeMessage& m) {
    return {{"type", "set_mode"}, {"mode", m.mode}};
}

inline nlohmann::json toJson(const SetColorMessage& m) {
    return {{"type", "set_color"}, {"color_name", m.colorName}};
}

inline nlohmann::json toJson(const StateMessage& m) {
    return {{"type", "state"}, {"mode", m.mode}, {"color_name", m.colorName}};
}

inline nlohmann::json toJson(const AudioFrameMessage& m) {
    return {{"type", "audio_frame"},
            {"buckets", m.buckets},
            {"waveform", m.waveform},
            {"loudness", m.loudness},
            {"beat_detected", m.beatDetected}};
}

// Returns the "type" field, or an empty string if the message is malformed.
inline std::string messageType(const nlohmann::json& msg) {
    return msg.value("type", std::string());
}

} // namespace mitm::ipc
