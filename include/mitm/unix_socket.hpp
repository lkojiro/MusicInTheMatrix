#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace mitm {

// A connected Unix domain socket, framing each JSON message as a 4-byte
// big-endian length prefix followed by that many bytes of UTF-8 JSON.
// Local-only IPC: no handshake, no compression, nothing a browser could
// ever speak -- if this project later needs that, bridge it with a small
// WebSocket-to-this-protocol proxy rather than replacing this transport.
class UnixSocketConnection {
public:
    // Takes ownership of an already-connected socket fd.
    explicit UnixSocketConnection(int fd);
    ~UnixSocketConnection();

    UnixSocketConnection(const UnixSocketConnection&) = delete;
    UnixSocketConnection& operator=(const UnixSocketConnection&) = delete;

    // Blocking send of one framed message. Returns false on any write
    // error (peer likely disconnected).
    bool send(const nlohmann::json& message);

    // Blocking read of one framed message. Returns false on EOF or error
    // (peer disconnected) -- callers should stop reading after that.
    bool receive(nlohmann::json& out);

    // Unblocks any thread currently inside a blocking receive()/send() on
    // this connection (they'll observe EOF/an error and return false),
    // without invalidating the fd the way close() would. Use this to stop
    // a dedicated reader thread cleanly, then join it, then let this
    // object's destructor (or an explicit close()) actually release the
    // fd -- calling close() concurrently with another thread still using
    // the fd is a race.
    void shutdown();

    void close();

private:
    int fd_;
};

// Listens on a Unix domain socket path, accepting one connection at a time.
class UnixSocketServer {
public:
    // Removes any stale socket file at `path`, then binds and listens.
    // Throws std::runtime_error on failure.
    explicit UnixSocketServer(const std::string& path);
    ~UnixSocketServer();

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    // Blocks until a client connects. Returns nullptr if the listening
    // socket was closed (e.g. during shutdown) while waiting.
    std::unique_ptr<UnixSocketConnection> accept();

private:
    std::string path_;
    int listenFd_ = -1;
};

// Connects to a controller's Unix domain socket. Returns nullptr (with
// `error` populated) on failure, including after retrying briefly in case
// the server hasn't started listening yet.
std::unique_ptr<UnixSocketConnection> connectUnixSocket(const std::string& path, std::string& error);

} // namespace mitm
