#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include <unistd.h> // getpid

#include "mitm/audio_capture.hpp"
#include "mitm/beat_detector.hpp"
#include "mitm/checkerboard_renderer.hpp"
#include "mitm/color_scheme.hpp"
#include "mitm/curses_util.hpp"
#include "mitm/fft_processor.hpp"
#include "mitm/global_config.hpp"
#include "mitm/ipc_protocol.hpp"
#include "mitm/matrix_rain_renderer.hpp"
#include "mitm/modes.hpp"
#include "mitm/terminal_renderer.hpp"
#include "mitm/unix_socket.hpp"
#include "mitm/visual_mode.hpp"
#include "mitm/visualizer_config.hpp"

namespace mitm {

namespace {

// Connects to the controller, sends hello, and spins up a background
// reader thread that keeps `active`/`shutdownRequested` up to date.
// Returns nullptr (having already printed an error) on failure.
std::unique_ptr<UnixSocketConnection> connectAndHandshake(const AppArgs& args, std::thread& readerOut,
                                                           std::atomic<bool>& active,
                                                           std::atomic<bool>& shutdownRequested,
                                                           std::atomic<int>& displayNumber) {
    std::string connectError;
    auto conn = connectUnixSocket(args.socketPath, connectError);
    if (!conn) {
        std::fprintf(stderr, "Subordinate %d: couldn't connect to controller at %s: %s\n", args.id,
                     args.socketPath.c_str(), connectError.c_str());
        return nullptr;
    }

    if (!conn->send(ipc::toJson(ipc::HelloMessage{args.id, static_cast<long>(getpid()), args.spawnToken}))) {
        std::fprintf(stderr, "Subordinate %d: failed to send hello to controller\n", args.id);
        return nullptr;
    }

    UnixSocketConnection* connPtr = conn.get();
    int myId = args.id;
    readerOut = std::thread([connPtr, &active, &shutdownRequested, &displayNumber, myId] {
        nlohmann::json msg;
        while (connPtr->receive(msg)) {
            std::string type = ipc::messageType(msg);
            if (type == "activate") {
                active = msg.value("active", false);
            } else if (type == "config") {
                // Kept in sync with the host/controller's view of the
                // whole window set.
                auto windowIds = msg.value("window_ids", nlohmann::json::array());
                updateGlobalConfig([&](GlobalConfig& c) {
                    c.windowCount = msg.value("window_count", 0);
                    c.windows.clear();
                    for (auto& idVal : windowIds) {
                        c.windows.push_back(WindowInfo{idVal.get<int>()});
                    }
                });

                // windowIds is in display order (see broadcastConfig() in
                // host.cpp/controller.cpp) -- this window's simple,
                // human-readable number is just its position in that
                // list. The real id (myId, pid-based or
                // controller-assigned) stays what it always was
                // internally; only the title shown to the user changes,
                // and only when this position actually shifts (e.g. a
                // lower-numbered sibling disconnected).
                for (size_t i = 0; i < windowIds.size(); ++i) {
                    if (windowIds[i].get<int>() == myId) {
                        displayNumber = static_cast<int>(i);
                        curses_util::setWindowTitle("mitm — CHILD (" + std::to_string(i) + ")");
                        break;
                    }
                }
            } else if (type == "shutdown") {
                break;
            }
        }
        // Either an explicit shutdown, or the connection dropped (e.g. the
        // controller process died) -- either way, this window should stop.
        shutdownRequested = true;
    });

    return conn;
}

} // namespace

int runSubordinate(const AppArgs& args) {
    using namespace config;

    // Placeholder until the host/controller's first ConfigMessage tells us
    // our actual display number (see the reader thread's "config" handler
    // in connectAndHandshake below) -- normally replaced within moments of
    // connecting.
    curses_util::setWindowTitle("mitm — CHILD (connecting...)");

    std::atomic<bool> active{false};
    std::atomic<bool> shutdownRequested{false};
    std::atomic<int> displayNumber{-1}; // -1 until the first ConfigMessage arrives
    std::thread reader;

    auto conn = connectAndHandshake(args, reader, active, shutdownRequested, displayNumber);
    if (!conn) return 1;

    // Populated before any renderer is constructed (they read baseColor in
    // their constructors). windowCount/windows start as "just me" and get
    // overwritten by the controller's first ConfigMessage broadcast, which
    // should arrive within moments of hello (see broadcastConfig() in
    // controller.cpp) -- but renderer construction can't wait on that
    // arriving over the network, hence a same-process fallback here.
    updateGlobalConfig([&](GlobalConfig& c) {
        c.baseColor = parseColorName(args.colorName);
        c.windowCount = 1;
        c.windows = {WindowInfo{args.id}};
    });

    // Audio is best-effort, for both modes: arrow keys can switch this
    // window to bars at any time regardless of the starting mode, and
    // Matrix rain still works (just without loudness/beat reactivity) if
    // this fails.
    AudioCapture audio(kSampleRate, 1 << 15, args.deviceNameHint);
    bool audioOk = audio.start();
    if (!audioOk) {
        std::fprintf(stderr,
                      "Subordinate %d: no audio input (%s) -- bars mode will show as just the baseline; "
                      "Matrix rain will run without music reactivity.\n",
                      args.id, audio.lastError().c_str());
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

    while (!shutdownRequested.load()) {
        switch (curses_util::pollInput()) {
            case curses_util::InputAction::Quit:
                shutdownRequested = true;
                break;
            case curses_util::InputAction::NextMode:
                mode = nextVisualMode(mode);
                break;
            case curses_util::InputAction::PrevMode:
                mode = prevVisualMode(mode);
                break;
            case curses_util::InputAction::SpawnWindow:
                // We don't have our own WindowLauncher (or, in the
                // host/child path, the spawn-token bookkeeping needed to
                // later explicitly close() what we'd spawn), so ask
                // whichever side is the orchestrator to do it instead --
                // the new window ends up a sibling of this one, a child
                // of the same host. runHost handles this (see
                // ipc::SpawnRequestMessage and the accept thread's
                // per-connection reader in host.cpp); the explicit
                // --controller path doesn't listen for it yet, so this
                // is silently a no-op there for now.
                conn->send(ipc::toJson(ipc::SpawnRequestMessage{}));
                break;
            case curses_util::InputAction::None:
                break;
        }
        if (shutdownRequested.load()) break;

        if (active.load()) {
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
        } else {
            int shownNumber = displayNumber.load();
            std::string idleLabel = shownNumber >= 0
                                         ? "window " + std::to_string(shownNumber) + " -- waiting to go live"
                                         : "connecting -- waiting to go live";
            curses_util::drawIdleScreen(idleLabel);
        }

        std::this_thread::sleep_for(kFrameInterval);
    }

    curses_util::teardown();

    conn->send(ipc::toJson(ipc::ByeMessage{args.id})); // best-effort; ignore failure
    conn->shutdown();                                  // unblock reader's receive()
    reader.join();

    if (audioOk) audio.stop();
    return 0;
}

} // namespace mitm
