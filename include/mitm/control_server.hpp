#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace mitm {

// A parsed HTTP request, minimal on purpose: method, path (query string
// stripped -- every route below that needs parameters takes them as a
// JSON body instead, GET /api/state aside, which needs none), and the
// raw request body.
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

// A tiny, deliberately unambitious HTTP/1.1 server: binds 127.0.0.1 only
// (this is a local control panel, not something meant to be reachable
// over a network), one thread per connection, no keep-alive (every
// response closes the connection right after -- fine for a control panel
// whose "load" is infrequent button clicks, not a real workload), no
// chunked transfer encoding, no HTTPS. See UnixSocketConnection's doc
// comment in unix_socket.hpp for why this project hand-rolls its own
// transports instead of reaching for a dependency -- same reasoning
// here, just a browser-compatible protocol instead of the length-
// prefixed JSON one everything else speaks.
//
// Serves an embedded HTML control panel at GET / itself; every other
// request is handed to `handler`, which callers use to implement the
// actual host commands (spawn/close/set mode/set color) -- see
// runHost's use of this in host.cpp for the routing table.
class ControlServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    // Binds and starts listening immediately (on a background accept
    // thread). Throws std::runtime_error on failure, e.g. the port's
    // already in use.
    ControlServer(int port, Handler handler);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

private:
    void acceptLoop();
    void handleConnection(int fd);

    int listenFd_ = -1;
    Handler handler_;
    std::atomic<bool> stopRequested_{false};
    std::thread acceptThread_;
};

} // namespace mitm
