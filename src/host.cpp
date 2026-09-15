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
};

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
                    if (ipc::messageType(msg) == "spawn_request") {
                        // A child's 'n' -- spawn it a sibling, same as if
                        // this host's own 'n' had been pressed. Nothing
                        // else to act on besides that and noticing
                        // disconnect below.
                        spawnChild();
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

    VisualMode mode = parseVisualMode(args.visualMode);

    std::vector<float> samples;
    std::vector<float> buckets;

    while (!g_stopRequested.load()) {
        switch (curses_util::pollInput()) {
            case curses_util::InputAction::Quit:
                g_stopRequested = true;
                break;
            case curses_util::InputAction::NextMode:
                mode = nextVisualMode(mode);
                break;
            case curses_util::InputAction::PrevMode:
                mode = prevVisualMode(mode);
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

        switch (mode) {
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
    std::vector<long> handlesToClose;
    {
        std::lock_guard<std::mutex> lock(childrenMutex);
        for (auto& [id, child] : children) {
            if (child.connection) child.connection->send(ipc::toJson(ipc::ShutdownMessage{}));
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
    // delay below, on top of waiting for disconnect itself.
    constexpr auto kShutdownWaitTimeout = std::chrono::seconds(3);
    constexpr auto kPostDisconnectGrace = std::chrono::milliseconds(400);
    auto waitStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - waitStart < kShutdownWaitTimeout) {
        bool allGone;
        {
            std::lock_guard<std::mutex> lock(childrenMutex);
            allGone = children.empty();
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

    removeHostLockFileIfOwnedBy(ownId);

    // The process is exiting right after this either way, so rather than
    // finding a clean way to unblock the accept() call, just let the
    // thread go with the process.
    acceptThread.detach();

    return 0;
}

} // namespace mitm
