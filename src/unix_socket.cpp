#include "mitm/unix_socket.hpp"

#include <arpa/inet.h> // htonl/ntohl
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace mitm {

namespace {

// Reads exactly `len` bytes into `buf`, looping over short reads.
// Returns false on EOF or error.
bool readExact(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(fd, p, remaining);
        if (n == 0) return false; // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

// Writes exactly `len` bytes from `buf`, looping over short writes.
bool writeExact(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

UnixSocketConnection::UnixSocketConnection(int fd) : fd_(fd) {}

UnixSocketConnection::~UnixSocketConnection() {
    close();
}

void UnixSocketConnection::shutdown() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
    }
}

void UnixSocketConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UnixSocketConnection::send(const nlohmann::json& message) {
    if (fd_ < 0) return false;
    std::string payload = message.dump();
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    return writeExact(fd_, &len, sizeof(len)) && writeExact(fd_, payload.data(), payload.size());
}

bool UnixSocketConnection::receive(nlohmann::json& out) {
    if (fd_ < 0) return false;

    uint32_t lenNet = 0;
    if (!readExact(fd_, &lenNet, sizeof(lenNet))) return false;
    uint32_t len = ntohl(lenNet);

    // A malformed or malicious length here would otherwise drive an
    // unbounded allocation -- this is local IPC between our own
    // processes, but there's no reason not to just cap it.
    constexpr uint32_t kMaxMessageBytes = 1 << 20; // 1 MiB
    if (len > kMaxMessageBytes) return false;

    std::string payload(len, '\0');
    if (len > 0 && !readExact(fd_, payload.data(), len)) return false;

    out = nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded();
}

UnixSocketServer::UnixSocketServer(const std::string& path) : path_(path) {
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throw std::runtime_error(std::string("UnixSocketServer: socket() failed: ") + std::strerror(errno));
    }

    ::unlink(path_.c_str()); // clear any stale socket file from a previous run

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error(std::string("UnixSocketServer: bind() failed for ") + path_ + ": " +
                                  std::strerror(errno));
    }

    constexpr int kBacklog = 16;
    if (::listen(listenFd_, kBacklog) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error(std::string("UnixSocketServer: listen() failed: ") + std::strerror(errno));
    }
}

UnixSocketServer::~UnixSocketServer() {
    if (listenFd_ >= 0) {
        ::close(listenFd_);
    }
    ::unlink(path_.c_str());
}

std::unique_ptr<UnixSocketConnection> UnixSocketServer::accept() {
    int clientFd = ::accept(listenFd_, nullptr, nullptr);
    if (clientFd < 0) return nullptr;
    return std::make_unique<UnixSocketConnection>(clientFd);
}

std::unique_ptr<UnixSocketConnection> connectUnixSocket(const std::string& path, std::string& error) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // A few short retries: the subordinate process may win a startup race
    // against the controller finishing its bind()/listen() call.
    constexpr int kMaxAttempts = 20;
    constexpr auto kRetryDelay = std::chrono::milliseconds(100);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            error = std::string("socket() failed: ") + std::strerror(errno);
            return nullptr;
        }

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return std::make_unique<UnixSocketConnection>(fd);
        }

        error = std::string("connect() failed for ") + path + ": " + std::strerror(errno);
        ::close(fd);
        std::this_thread::sleep_for(kRetryDelay);
    }

    return nullptr;
}

} // namespace mitm
