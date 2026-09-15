#include <unistd.h> // getpid

#include "mitm/host_discovery.hpp"
#include "mitm/modes.hpp"
#include "mitm/unix_socket.hpp"

int main(int argc, char** argv) {
    mitm::AppArgs args = mitm::parseArgs(argc, argv);

    if (args.controller) return mitm::runController(args);
    if (args.subordinate) return mitm::runSubordinate(args);

    // Default: auto-discover whether a host is already running (see
    // modes.hpp's doc comment). A pid liveness check alone can't tell a
    // truly-dead host from one that's still mid-startup, so the real
    // decision is an actual connect attempt (discarded either way --
    // runSubordinate makes its own fresh connection, which will succeed
    // near-instantly having already been proven reachable here).
    auto hostInfo = mitm::readHostLockFile();
    if (hostInfo && mitm::isProcessAlive(hostInfo->pid)) {
        std::string connectError;
        if (mitm::connectUnixSocket(hostInfo->socketPath, connectError)) {
            mitm::AppArgs childArgs = args;
            childArgs.subordinate = true;
            childArgs.id = static_cast<int>(getpid());
            childArgs.socketPath = hostInfo->socketPath;
            return mitm::runSubordinate(childArgs);
        }
    }

    // No live host found (missing, stale pid, or unreachable) -- become
    // one. UnixSocketServer's constructor cleans up any stale socket file
    // at this path itself, so there's no extra staleness handling needed
    // here beyond just proceeding.
    return mitm::runHost(args);
}
