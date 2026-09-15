#include "mitm/host_discovery.hpp"

#include <cerrno>
#include <fstream>
#include <sstream>

#include <signal.h> // kill
#include <unistd.h> // unlink

#include <nlohmann/json.hpp>

namespace mitm {

bool isProcessAlive(long pid) {
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    // EPERM means the process exists but we don't own it -- still alive
    // for our purposes. Anything else (chiefly ESRCH) means it's gone.
    return errno == EPERM;
}

std::optional<HostInfo> readHostLockFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;

    std::stringstream buffer;
    buffer << file.rdbuf();

    nlohmann::json j = nlohmann::json::parse(buffer.str(), /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    HostInfo info;
    info.socketPath = j.value("socket_path", std::string());
    info.pid = j.value("pid", 0L);
    if (info.socketPath.empty() || info.pid <= 0) return std::nullopt;

    return info;
}

bool writeHostLockFile(const HostInfo& info, const std::string& path) {
    nlohmann::json j = {{"socket_path", info.socketPath}, {"pid", info.pid}};

    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << j.dump();
    return static_cast<bool>(file);
}

void removeHostLockFileIfOwnedBy(long pid, const std::string& path) {
    auto info = readHostLockFile(path);
    if (info && info->pid == pid) {
        ::unlink(path.c_str());
    }
}

} // namespace mitm
