#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <csignal>
#include <unistd.h> // getpid

#include "mitm/audio_capture.hpp"
#include "mitm/beat_detector.hpp"
#include "mitm/checkerboard_renderer.hpp"
#include "mitm/color_scheme.hpp"
#include "mitm/control_server.hpp"
#include "mitm/curses_util.hpp"
#include "mitm/fft_processor.hpp"
#include "mitm/global_config.hpp"
#include "mitm/host_discovery.hpp"
#include "mitm/ipc_protocol.hpp"
#include "mitm/matrix_rain_renderer.hpp"
#include "mitm/modes.hpp"
#include "mitm/terminal_renderer.hpp"
#include "mitm/unix_socket.hpp"
#include "mitm/visual_mode.hpp"
#include "mitm/visualizer_config.hpp"
#include "mitm/window_launcher.hpp"

namespace mitm {

namespace {

std::atomic<bool> g_stopRequested{false};

void handleSigint(int /*signal*/) {
    g_stopRequested = true;
}

struct Child {
    long windowHandle = -1; // -1 unless spawn-token-correlated (see below)
    std::unique_ptr<UnixSocketConnection> connection;
    // The host's best current knowledge of this child's own mode/color,
    // for the control server's dashboard (see control_server.hpp).
    // Starts as "unknown" (empty) until its first ipc::StateMessage
    // arrives, normally within moments of hello -- see the accept
    // thread's per-connection reader below.
    std::string mode;
    std::string colorName;
};

// Waits (bounded) for every id in `ids` to disappear from `children`
// (indicating its process actually finished disconnecting -- see the
// "closing a window with a live process" gotcha in the README), then
// closes whichever of them we have an AppleScript handle for. Shared by
// full-host-shutdown (closing every child) and the control server's
// single-window close (see control_server.hpp) so both get the same
// wait-then-grace-delay-then-close ordering rather than two copies of
// it drifting apart. Runs synchronously in whatever thread calls it --
// callers that shouldn't block (an HTTP request handler, in particular)
// should run this in their own detached thread instead.
void waitAndCloseWindows(std::mutex& childrenMutex, std::unordered_map<int, Child>& children,
                          AppleScriptWindowLauncher& launcher, const std::vector<int>& ids,
                          const std::vector<long>& handlesToClose) {
    constexpr auto kShutdownWaitTimeout = std::chrono::seconds(3);
    constexpr auto kPostDisconnectGrace = std::chrono::milliseconds(400);
    auto waitStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - waitStart < kShutdownWaitTimeout) {
        bool allGone;
        {
            std::lock_guard<std::mutex> lock(childrenMutex);
            allGone = std::none_of(ids.begin(), ids.end(), [&](int id) { return children.count(id) != 0; });
        }
        if (allGone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!handlesToClose.empty()) {
        std::this_thread::sleep_for(kPostDisconnectGrace);
    }
    for (long handle : handlesToClose) {
        launcher.close(handle);
    }
}

// Sends every connected child the host's current view of the window set,
// and updates the host's own GlobalConfig the same way. Caller must
// already hold childrenMutex.
//
// windowOrder carries more than membership: its *order* is what every
// window (host included) derives its simple, human-readable display
// number from -- position 0 is the host (windowOrder is seeded with
// ownId at construction and that entry is never removed), and each
// child's position is its 1-based slot in connection order. Removing a
// disconnected id from the middle of this vector naturally closes the
// gap for everyone after it, so the set of display numbers in use is
// always exactly 0..n-1 with nothing to explicitly renumber -- each
// child just recomputes "where am I in the list I was just sent" on
// every broadcast. The real id (pid, or spawn-token-correlated window
// handle) stays exactly what it always was; this is purely a label.
void broadcastConfig(int ownId, const std::vector<int>& windowOrder,
                      std::unordered_map<int, Child>& children) {
    ipc::ConfigMessage msg;
    msg.windowIds = windowOrder;
    msg.windowCount = static_cast<int>(msg.windowIds.size());

    updateGlobalConfig([&](GlobalConfig& c) {
        c.windowCount = msg.windowCount;
        c.windows.clear();
        for (int id : msg.windowIds) c.windows.push_back(WindowInfo{id});
    });

    for (auto& [id, child] : children) {
        if (child.connection) child.connection->send(ipc::toJson(msg));
    }
}

} // namespace

int runHost(const AppArgs& args) {
    using namespace config;

    std::signal(SIGINT, handleSigint);

    int ownId = static_cast<int>(::getpid());

    // The host is always display number 0 -- it's windowOrder[0] below and
    // that entry is never removed, so unlike children this never needs to
    // be recomputed or re-sent.
    curses_util::setWindowTitle("mitm — HOST (0)");

    // Best-effort: a failed lock file write just means other instances
    // won't be able to discover us this session (they'll each become
    // their own host instead) -- not worth failing the whole run over.
    if (!writeHostLockFile({args.socketPath, ownId})) {
        std::fprintf(stderr, "Host: couldn't write %s -- other instances won't find this one\n",
                     kHostLockFilePath);
    }

    std::unique_ptr<UnixSocketServer> server;
    try {
        server = std::make_unique<UnixSocketServer>(args.socketPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Host: %s\n", e.what());
        removeHostLockFileIfOwnedBy(ownId);
        return 1;
    }

    updateGlobalConfig([&](GlobalConfig& c) {
        c.baseColor = parseColorName(args.colorName);
        c.windowCount = 1;
        c.windows = {WindowInfo{ownId}};
    });

    std::mutex childrenMutex;
    std::unordered_map<int, Child> children;
    // Correlating a spawned window's AppleScript handle with its eventual
    // hello needs to handle *either* arrival order: the spawned process
    // runs concurrently with this thread's still-blocked launcher.spawn()
    // call (which waits out AppleScript's own ~0.3s+ delay), so its hello
    // can easily reach the accept thread before spawn() here has even
    // returned a handle to store. pendingHandles covers "spawn finished,
    // hello not seen yet" (token -> handle); pendingHellos covers the
    // opposite (token -> child id), so whichever side runs second finds
    // the other's entry and completes the correlation immediately.
    // Every window spawned via 'n' goes through spawnChild() below and
    // gets an entry here regardless of whether that key was pressed in
    // this host's own window or relayed from a child's (see
    // ipc::SpawnRequestMessage) -- either way this host did the actual
    // spawn() call and can later close() it. The one kind of window that
    // never gets an entry is one the user launched manually (just typing
    // `./build/mitm` in their own fresh terminal); its process still
    // exits cleanly on shutdown, just not necessarily its window.
    std::unordered_map<std::string, long> pendingHandles;
    std::unordered_map<std::string, int> pendingHellos;
    long spawnCounter = 0;

    // Connection order, host first -- see the broadcastConfig() doc
    // comment above for why this drives display numbers.
    std::vector<int> windowOrder{ownId};

    // Declared up here (rather than down with the rest of the render
    // setup, where the equivalent host-side 'n' handling used to live)
    // so spawnChild below -- and therefore the accept thread's
    // per-connection reader, which needs to call it for spawn requests
    // arriving from children -- can capture them by reference.
    AppleScriptWindowLauncher launcher;
    std::string execPath = currentExecutablePath();

    // Spawns one new auto-discovering window and registers its
    // spawn-token so the accept thread can later fill in its
    // AppleScript handle (see pendingHandles/pendingHellos above).
    // Callable from any thread: AppleScriptWindowLauncher is stateless
    // per call (just shells out to osascript), and the only shared state
    // touched here (spawnCounter, pendingHandles, pendingHellos,
    // children) is under childrenMutex. Used both for this host's own
    // 'n' key and for a spawn_request relayed from a child's 'n' (see
    // ipc::SpawnRequestMessage) -- either way the new window ends up a
    // sibling of the requester, a child of this host.
    auto spawnChild = [&]() {
        if (execPath.empty()) return; // can't resolve our own path -- silently skip
        std::string token;
        std::string spawnColor;
        {
            std::lock_guard<std::mutex> lock(childrenMutex);
            long index = spawnCounter++;
            token = std::to_string(ownId) + "-" + std::to_string(index);
            // Each spawned window gets its own color rather than
            // inheriting this host's --color -- see colorNameForIndex().
            spawnColor = colorNameForIndex(static_cast<int>(index));
        }
        std::string command = "'" + execPath + "' --device='" + args.deviceNameHint + "' --color='" +
                               spawnColor + "' --spawn-token='" + token + "'; exit";
        // Blocks for ~0.3-1s (AppleScript/Terminal overhead) -- fine off
        // the main render thread, but do the actual spawn() call outside
        // the lock so it doesn't stall the accept thread or other
        // concurrent spawns for that whole stretch.
        long handle = launcher.spawn(command);
        std::lock_guard<std::mutex> lock(childrenMutex);
        auto it = pendingHellos.find(token);
        if (it != pendingHellos.end()) {
            // Its hello already arrived while we were waiting on
            // AppleScript -- fill in the handle directly.
            if (children.count(it->second)) children[it->second].windowHandle = handle;
            pendingHellos.erase(it);
        } else {
            pendingHandles[token] = handle;
        }
    };

    std::thread acceptThread([&] {
        while (!g_stopRequested.load()) {
            auto conn = server->accept();
            if (!conn) break; // listening socket closed (shutdown)

            nlohmann::json hello;
            if (!conn->receive(hello) || ipc::messageType(hello) != "hello") {
                continue;
            }
            int id = hello.value("id", -1);
            std::string spawnToken = hello.value("spawn_token", std::string());

            UnixSocketConnection* connPtr = conn.get();
            {
                std::lock_guard<std::mutex> lock(childrenMutex);
                Child& child = children[id];
                child.connection = std::move(conn);
                if (!spawnToken.empty()) {
                    auto it = pendingHandles.find(spawnToken);
                    if (it != pendingHandles.end()) {
                        // The spawn already finished and left its handle
                        // waiting for us.
                        child.windowHandle = it->second;
                        pendingHandles.erase(it);
                    } else {
                        // We beat the spawn here -- leave a note so it can
                        // fill in the handle once launcher.spawn() returns.
                        pendingHellos[spawnToken] = id;
                    }
                }
                // A reconnect under the same id (shouldn't normally happen,
                // but cheap to guard) would otherwise duplicate its slot.
                if (std::find(windowOrder.begin(), windowOrder.end(), id) == windowOrder.end()) {
                    windowOrder.push_back(id);
                }
                // Every window renders live simultaneously now -- no
                // rotation, so a newly-connected child is activated right
                // away rather than waiting its turn.
                child.connection->send(ipc::toJson(ipc::ActivateMessage{true}));
                broadcastConfig(ownId, windowOrder, children);
            }
            std::printf("Host: window %d connected\n", id);

            std::thread([&childrenMutex, &children, &windowOrder, &spawnChild, ownId, id, connPtr] {
                nlohmann::json msg;
                while (connPtr->receive(msg)) {
                    std::string type = ipc::messageType(msg);
                    if (type == "spawn_request") {
                        // A child's 'n' -- spawn it a sibling, same as if
                        // this host's own 'n' had been pressed.
                        spawnChild();
                    } else if (type == "state") {
                        // This child's current mode/color, for the
                        // control server's dashboard (see
                        // control_server.hpp) -- sent right after hello
                        // and again whenever either actually changes, so
                        // this stays accurate even when it changed
                        // locally (arrow keys), not through the web UI.
                        std::lock_guard<std::mutex> lock(childrenMutex);
                        auto it = children.find(id);
                        if (it != children.end()) {
                            it->second.mode = msg.value("mode", std::string());
                            it->second.colorName = msg.value("color_name", std::string());
                        }
                    }
                }
                std::lock_guard<std::mutex> lock(childrenMutex);
                children.erase(id);
                windowOrder.erase(std::remove(windowOrder.begin(), windowOrder.end(), id),
                                   windowOrder.end());
                broadcastConfig(ownId, windowOrder, children);
                std::printf("Host: window %d disconnected\n", id);
            }).detach();
        }
    });

    // Audio is best-effort: arrow keys can switch to bars at any time
    // regardless of the starting mode, and Matrix rain still works (just
    // without loudness/beat reactivity) if this fails.
    AudioCapture audio(kSampleRate, 1 << 15, args.deviceNameHint);
    bool audioOk = audio.start();
    if (!audioOk) {
        std::fprintf(stderr,
                      "Host: no audio input (%s) -- bars mode will show as just the baseline; "
                      "Matrix rain will run without music reactivity.\n",
                      audio.lastError().c_str());
    }

    FftProcessor fft(kSampleRate, kWindowSize, kBucketCount);
    BeatDetector beatDetector;

    curses_util::init();
    TerminalRenderer barsRenderer;
    MatrixRainRenderer matrixRenderer;
    CheckerboardRenderer checkerboardRenderer;

    // Atomic (not a plain local) because the control server's HTTP
    // handler thread below also writes it, controlling the host's own
    // window the same way a remote SetModeMessage controls a child's --
    // see control_server.hpp.
    std::atomic<VisualMode> mode{parseVisualMode(args.visualMode)};

    // The host's own color *name* -- GlobalConfig only carries the
    // parsed BaseColor, not the string the control server's dashboard
    // wants to display/round-trip in a <select>. Lives under
    // childrenMutex alongside windowOrder/children (not its own mutex),
    // since the dashboard reads all of it together as one snapshot.
    std::string hostColorName = args.colorName;
    // What color this process last actually applied to
    // matrixRenderer/checkerboardRenderer -- compared against
    // GlobalConfig::baseColor once a frame below so a control-server-
    // driven color change (which can only safely touch GlobalConfig,
    // not ncurses, from that request-handling thread) gets picked up
    // and applied on the main thread, which owns the ncurses session.
    BaseColor lastAppliedColor = parseColorName(args.colorName);

    // Sends `id` a shutdown message and closes its window once it
    // actually disconnects, without blocking the caller -- see
    // waitAndCloseWindows() above. Used by the control server's
    // per-window close (see control_server.hpp); the host's own full
    // shutdown (closing every child at once) still does this inline
    // near the bottom of this function since it can afford to block --
    // the process is exiting right after either way.
    auto closeChildAsync = [&](int id) {
        long handle = -1;
        {
            std::lock_guard<std::mutex> lock(childrenMutex);
            auto it = children.find(id);
            if (it == children.end()) return;
            if (it->second.connection) it->second.connection->send(ipc::toJson(ipc::ShutdownMessage{}));
            handle = it->second.windowHandle;
        }
        std::vector<long> handles;
        if (handle >= 0) handles.push_back(handle);
        std::thread([&childrenMutex, &children, &launcher, id, handles] {
            waitAndCloseWindows(childrenMutex, children, launcher, {id}, handles);
        }).detach();
    };

    // The web control panel -- see control_server.hpp for the transport
    // and the embedded page itself. Every route below runs on whichever
    // per-connection thread ControlServer handed the request to, so
    // anything touching children/windowOrder/hostColorName takes
    // childrenMutex, same as every other thread that touches them.
    ControlServer webServer(args.webPort, [&](const HttpRequest& req) -> HttpResponse {
        if (req.method == "GET" && req.path == "/api/state") {
            nlohmann::json windows = nlohmann::json::array();
            {
                std::lock_guard<std::mutex> lock(childrenMutex);
                for (size_t i = 0; i < windowOrder.size(); ++i) {
                    int id = windowOrder[i];
                    nlohmann::json w;
                    w["displayNumber"] = static_cast<int>(i);
                    if (id == ownId) {
                        w["isHost"] = true;
                        w["mode"] = visualModeName(mode.load());
                        w["colorName"] = hostColorName;
                    } else {
                        w["isHost"] = false;
                        auto it = children.find(id);
                        w["mode"] = it != children.end() ? it->second.mode : "";
                        w["colorName"] = it != children.end() ? it->second.colorName : "";
                    }
                    windows.push_back(w);
                }
            }
            nlohmann::json result;
            result["windows"] = windows;
            return HttpResponse{200, "application/json", result.dump()};
        }

        if (req.method == "POST" && req.path == "/api/spawn") {
            spawnChild(); // blocks ~0.3-1s (AppleScript) -- fine for a button click
            return HttpResponse{200, "application/json", "{\"ok\":true}"};
        }

        // Every remaining route needs a JSON body with at least an
        // integer "id" (a *display* number, e.g. what the web UI shows
        // and what windowOrder indexes by -- not the real pid/handle
        // underneath it, which the browser never sees).
        nlohmann::json body = nlohmann::json::parse(req.body, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded() || !body.contains("id") || !body["id"].is_number_integer()) {
            return HttpResponse{400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}"};
        }
        int displayNumber = body["id"].get<int>();
        int realId = -1;
        {
            std::lock_guard<std::mutex> lock(childrenMutex);
            if (displayNumber >= 0 && static_cast<size_t>(displayNumber) < windowOrder.size()) {
                realId = windowOrder[static_cast<size_t>(displayNumber)];
            }
        }
        if (realId < 0) {
            return HttpResponse{404, "application/json", "{\"ok\":false,\"error\":\"no such window\"}"};
        }

        if (req.method == "POST" && req.path == "/api/close") {
            if (realId == ownId) {
                // Closing "window 0" means closing the host -- which
                // cascades to every child anyway (see the shutdown code
                // below), so that's exactly what quitting via 'q' does.
                g_stopRequested = true;
            } else {
                closeChildAsync(realId);
            }
            return HttpResponse{200, "application/json", "{\"ok\":true}"};
        }

        if (req.method == "POST" && req.path == "/api/mode") {
            std::string modeName = body.value("mode", std::string("bars"));
            if (realId == ownId) {
                mode = parseVisualMode(modeName);
            } else {
                std::lock_guard<std::mutex> lock(childrenMutex);
                auto it = children.find(realId);
                if (it != children.end() && it->second.connection) {
                    it->second.connection->send(ipc::toJson(ipc::SetModeMessage{modeName}));
                    it->second.mode = modeName; // optimistic -- the child's own state echo will confirm
                }
            }
            return HttpResponse{200, "application/json", "{\"ok\":true}"};
        }

        if (req.method == "POST" && req.path == "/api/color") {
            std::string colorName = body.value("color", std::string("green"));
            if (realId == ownId) {
                updateGlobalConfig([&](GlobalConfig& c) { c.baseColor = parseColorName(colorName); });
                std::lock_guard<std::mutex> lock(childrenMutex);
                hostColorName = colorName;
            } else {
                std::lock_guard<std::mutex> lock(childrenMutex);
                auto it = children.find(realId);
                if (it != children.end() && it->second.connection) {
                    it->second.connection->send(ipc::toJson(ipc::SetColorMessage{colorName}));
                    it->second.colorName = colorName; // optimistic, as above
                }
            }
            return HttpResponse{200, "application/json", "{\"ok\":true}"};
        }

        return HttpResponse{404, "application/json", "{\"ok\":false,\"error\":\"no such route\"}"};
    });
    std::printf("Host: control panel at http://127.0.0.1:%d\n", args.webPort);

    std::vector<float> samples;
    std::vector<float> buckets;

    while (!g_stopRequested.load()) {
        switch (curses_util::pollInput()) {
            case curses_util::InputAction::Quit:
                g_stopRequested = true;
                break;
            case curses_util::InputAction::NextMode:
                mode = nextVisualMode(mode.load());
                break;
            case curses_util::InputAction::PrevMode:
                mode = prevVisualMode(mode.load());
                break;
            case curses_util::InputAction::SpawnWindow:
                // Blocks for ~0.3-1s (AppleScript/Terminal overhead) --
                // this window's own rendering visibly pauses for that
                // stretch, same as it always did.
                spawnChild();
                break;
            case curses_util::InputAction::None:
                break;
        }
        if (g_stopRequested.load()) break;

        // Pick up a color change made via the control server -- it can
        // only safely touch GlobalConfig from its own thread, not
        // ncurses, so applying it (redefining the renderers' color
        // pairs) happens here instead, once a frame.
        BaseColor currentColor = getGlobalConfig().baseColor;
        if (currentColor != lastAppliedColor) {
            matrixRenderer.setupColors();
            checkerboardRenderer.setupColors();
            lastAppliedColor = currentColor;
        }

        // The host always renders -- every window (host and every
        // connected child) shows live visuals simultaneously, no rotation.
        float loudness = 0.0f;
        bool beatDetected = false;
        if (audioOk) {
            audio.readLatest(samples, kWindowSize);
            fft.process(samples, buckets);
            AudioFeatures features = beatDetector.update(samples, buckets);
            loudness = features.loudness;
            beatDetected = features.beatDetected;
        }

        switch (mode.load()) {
            case VisualMode::Bars:
                barsRenderer.draw(buckets);
                break;
            case VisualMode::Matrix:
                matrixRenderer.draw(loudness, beatDetected);
                break;
            case VisualMode::Checkerboard:
                checkerboardRenderer.draw(loudness, beatDetected);
                break;
        }

        std::this_thread::sleep_for(kFrameInterval);
    }

    curses_util::teardown();
    if (audioOk) audio.stop();

    std::printf("\nHost: shutting down, closing child windows...\n");
    std::vector<int> ids;
    std::vector<long> handlesToClose;
    {
        std::lock_guard<std::mutex> lock(childrenMutex);
        for (auto& [id, child] : children) {
            if (child.connection) child.connection->send(ipc::toJson(ipc::ShutdownMessage{}));
            ids.push_back(id);
            if (child.windowHandle >= 0) handlesToClose.push_back(child.windowHandle);
        }
    }

    // Wait for children to actually disconnect (their socket closes right
    // after they finish tearing down ncurses) before calling close() --
    // closing a window while its process is still the foreground job
    // silently fails (see the README's "A real gotcha" writeup). A socket
    // disconnect isn't quite process exit, though: subordinate.cpp's
    // shutdown order is endwin() -> disconnect -> PortAudio teardown ->
    // actual process exit, and that last stretch (PortAudio's Pa_Terminate
    // in particular) can take a real, non-negligible moment -- long enough
    // to lose the race against this loop's 100ms poll granularity and hit
    // the exact same silent-close failure. Hence the extra flat grace
    // delay, on top of waiting for disconnect itself -- see
    // waitAndCloseWindows() above (also used by the control server's
    // single-window close, so both paths share this exact ordering).
    // Blocking here (rather than the detached-thread version
    // closeChildAsync uses) is fine: the process is exiting right after
    // either way.
    waitAndCloseWindows(childrenMutex, children, launcher, ids, handlesToClose);

    removeHostLockFileIfOwnedBy(ownId);

    // The process is exiting right after this either way, so rather than
    // finding a clean way to unblock the accept() call, just let the
    // thread go with the process.
    acceptThread.detach();

    return 0;
}

} // namespace mitm
