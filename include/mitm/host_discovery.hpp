#pragma once

#include <optional>
#include <string>

namespace mitm {

// Where every instance looks for an already-running host. A plain launch
// (no explicit --controller/--subordinate) checks this file to decide
// whether to become the host or connect to one -- see main.cpp.
constexpr const char* kHostLockFilePath = "/tmp/.mitm-host";

struct HostInfo {
    std::string socketPath;
    long pid = 0;
};

// True if a process with this pid currently exists (kill(pid, 0), the
// standard liveness-check idiom -- sends no actual signal).
bool isProcessAlive(long pid);

// Reads and parses the lock file. Returns nullopt if it's missing,
// unreadable, or malformed -- all of which just mean "no host known",
// same as any other case where nothing is running yet.
std::optional<HostInfo> readHostLockFile(const std::string& path = kHostLockFilePath);

// Overwrites the lock file with `info`. Returns false on failure (e.g.
// /tmp unwritable) -- callers should keep running as a host anyway rather
// than fail the whole program over discovery being unavailable this
// session, just louder about the fact that other instances won't find it.
bool writeHostLockFile(const HostInfo& info, const std::string& path = kHostLockFilePath);

// Removes the lock file, but only if it still names `pid` -- guards
// against a host that's shutting down accidentally deleting a *different*
// (newer) host's lock file in the event of some races. Never treated as
// fatal if it fails; the lock file is only ever a discovery hint, and a
// stale one is handled gracefully by whoever reads it next (see
// isProcessAlive() combined with an actual connect attempt in main.cpp).
void removeHostLockFileIfOwnedBy(long pid, const std::string& path = kHostLockFilePath);

} // namespace mitm
