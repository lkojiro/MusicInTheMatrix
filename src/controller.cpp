#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <csignal>

#include "mitm/color_scheme.hpp"
#include "mitm/curses_util.hpp"
#include "mitm/global_config.hpp"
#include "mitm/ipc_protocol.hpp"
#include "mitm/modes.hpp"
#include "mitm/unix_socket.hpp"
#include "mitm/window_launcher.hpp"

namespace mitm {

namespace {

std::atomic<bool> g_stopRequested{false};

void handleSigint(int /*signal*/) {
    g_stopRequested = true;
}

struct Subordinate {
    long windowHandle = -1;
    std::unique_ptr<UnixSocketConnection> connection;
};

// Sends every connected subordinate the controller's current view of the
// window set, and updates the controller's own GlobalConfig the same way
// (mostly for symmetry/future controller-side use -- the controller
// doesn't render anything itself). Caller must already hold subsMutex.
void broadcastConfig(std::unordered_map<int, Subordinate>& subordinates) {
    ipc::ConfigMessage msg;
    for (auto& [id, sub] : subordinates) {
        if (sub.connection) msg.windowIds.push_back(id);
    }
    std::sort(msg.windowIds.begin(), msg.windowIds.end());
    msg.windowCount = static_cast<int>(msg.windowIds.size());

    updateGlobalConfig([&](GlobalConfig& c) {
        c.windowCount = msg.windowCount;
        c.windows.clear();
        for (int id : msg.windowIds) c.windows.push_back(WindowInfo{id});
    });

    for (auto& [id, sub] : subordinates) {
        if (sub.connection) sub.connection->send(ipc::toJson(msg));
    }
}

} // namespace

int runController(const AppArgs& args) {
    std::signal(SIGINT, handleSigint);

    curses_util::setWindowTitle("mitm — CONTROLLER");

    std::string execPath = currentExecutablePath();
    if (execPath.empty()) {
        std::fprintf(stderr, "Controller: couldn't resolve this executable's path\n");
        return 1;
    }

    std::unique_ptr<UnixSocketServer> server;
    try {
        server = std::make_unique<UnixSocketServer>(args.socketPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Controller: %s\n", e.what());
        return 1;
    }

    AppleScriptWindowLauncher launcher;
    std::mutex subsMutex;
    std::unordered_map<int, Subordinate> subordinates;

    std::printf("Controller: spawning %d subordinate window(s), socket=%s\n", args.count,
                args.socketPath.c_str());
    for (int i = 1; i <= args.count; ++i) {
        // The trailing "; exit" matters: `do script` just types the command
        // into a fresh shell, which returns to its own prompt afterward --
        // it does not exit itself just because the command did. Without an
        // explicit exit, Terminal's "close if the shell exited cleanly"
        // preference never has anything to fire on, and spawned windows
        // pile up even after every subordinate process has ended.
        // Each subordinate gets its own color rather than all inheriting
        // this controller's --color -- see colorNameForIndex().
        std::string command = "'" + execPath + "' --subordinate --id=" + std::to_string(i) + " --socket='" +
                               args.socketPath + "' --visual=" + args.visualMode + " --color=" +
                               colorNameForIndex(i - 1) + "; exit";
        long handle = launcher.spawn(command);
        if (handle < 0) {
            std::fprintf(stderr, "Controller: failed to spawn window %d\n", i);
        }
        std::lock_guard<std::mutex> lock(subsMutex);
        subordinates[i].windowHandle = handle;
    }

    // Accept loop: one background thread per connection, just enough to
    // notice disconnects ("bye" or the socket dropping) and drop that
    // window from rotation.
    std::thread acceptThread([&] {
        while (!g_stopRequested.load()) {
            auto conn = server->accept();
            if (!conn) break; // listening socket closed (shutdown)

            nlohmann::json hello;
            if (!conn->receive(hello) || ipc::messageType(hello) != "hello") {
                continue; // ignore anything that doesn't identify itself properly
            }
            int id = hello.value("id", -1);
            std::printf("Controller: window %d connected\n", id);

            UnixSocketConnection* connPtr = conn.get();
            {
                std::lock_guard<std::mutex> lock(subsMutex);
                subordinates[id].connection = std::move(conn);
                broadcastConfig(subordinates);
            }

            std::thread([&subsMutex, &subordinates, id, connPtr] {
                nlohmann::json msg;
                while (connPtr->receive(msg)) {
                    // Nothing to act on today besides noticing disconnect
                    // below -- "bye" is just an explicit version of that.
                }
                std::lock_guard<std::mutex> lock(subsMutex);
                subordinates.erase(id);
                broadcastConfig(subordinates);
                std::printf("Controller: window %d disconnected\n", id);
            }).detach();
        }
    });

    constexpr auto kRotateInterval = std::chrono::seconds(5);
    int activeId = -1;
    auto lastRotate = std::chrono::steady_clock::now() - kRotateInterval; // rotate right away

    while (!g_stopRequested.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastRotate >= kRotateInterval) {
            std::lock_guard<std::mutex> lock(subsMutex);

            std::vector<int> connectedIds;
            for (auto& [id, sub] : subordinates) {
                if (sub.connection) connectedIds.push_back(id);
            }
            std::sort(connectedIds.begin(), connectedIds.end());

            if (!connectedIds.empty()) {
                auto it = std::find(connectedIds.begin(), connectedIds.end(), activeId);
                int nextId = (it == connectedIds.end() || std::next(it) == connectedIds.end())
                                 ? connectedIds.front()
                                 : *std::next(it);

                if (activeId != nextId && subordinates.count(activeId) && subordinates[activeId].connection) {
                    subordinates[activeId].connection->send(ipc::toJson(ipc::ActivateMessage{false}));
                }
                subordinates[nextId].connection->send(ipc::toJson(ipc::ActivateMessage{true}));
                if (activeId != nextId) {
                    std::printf("Controller: window %d is now active\n", nextId);
                }
                activeId = nextId;
            }
            lastRotate = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::printf("\nController: shutting down...\n");
    std::vector<long> handlesToClose;
    {
        std::lock_guard<std::mutex> lock(subsMutex);
        for (auto& [id, sub] : subordinates) {
            if (sub.connection) sub.connection->send(ipc::toJson(ipc::ShutdownMessage{}));
            handlesToClose.push_back(sub.windowHandle);
        }
    }

    // Wait for subordinates to actually disconnect (their socket closes
    // right at the end of their own cleanup, just before the process
    // exits) rather than guessing a fixed delay. This matters: closing a
    // window while its process is still the foreground job silently fails
    // -- Terminal pops a "this will terminate the running process"
    // confirmation sheet that nothing here can answer, so `close` returns
    // without error but the window never actually closes.
    constexpr auto kShutdownWaitTimeout = std::chrono::seconds(3);
    auto waitStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - waitStart < kShutdownWaitTimeout) {
        bool allGone;
        {
            std::lock_guard<std::mutex> lock(subsMutex);
            allGone = subordinates.empty();
        }
        if (allGone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (long handle : handlesToClose) {
        launcher.close(handle);
    }

    // The process is exiting right after this either way, so rather than
    // finding a clean way to unblock the accept() call, just let the
    // thread go with the process.
    acceptThread.detach();

    return 0;
}

} // namespace mitm
