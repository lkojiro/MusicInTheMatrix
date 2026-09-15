#include "mitm/control_server.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace mitm {

namespace {

// The whole control panel: one HTML file, inline CSS/JS, no external
// requests (fetch() only ever calls back into this same server). Polls
// GET /api/state once a second and re-renders; every action just POSTs
// a small JSON body and waits for the next poll to confirm it (rather
// than optimistically updating the DOM itself) -- simple, at the cost of
// a click's effect taking up to ~1s to visibly confirm.
constexpr const char* kControlPanelHtml = R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>mitm control</title>
<style>
  :root { color-scheme: dark; }
  body {
    background: #0b0b0b; color: #d0d0d0;
    font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace;
    font-size: 14px; margin: 0; padding: 24px;
  }
  h1 { color: #6f6; font-size: 16px; font-weight: normal; margin: 0 0 4px; }
  .sub { color: #777; margin: 0 0 20px; font-size: 12px; }
  button, select {
    background: #1a1a1a; color: #d0d0d0; border: 1px solid #333;
    border-radius: 4px; padding: 5px 10px; font: inherit; cursor: pointer;
  }
  button:hover, select:hover { border-color: #555; }
  button.primary { border-color: #3a6; color: #6f6; }
  button.danger { border-color: #a33; color: #f66; }
  table { border-collapse: collapse; width: 100%; max-width: 720px; }
  th, td { text-align: left; padding: 8px 12px; border-bottom: 1px solid #222; }
  th { color: #777; font-weight: normal; font-size: 12px; text-transform: uppercase; }
  .swatch {
    display: inline-block; width: 10px; height: 10px; border-radius: 50%;
    margin-right: 6px; vertical-align: middle;
  }
  .host-badge {
    background: #143; color: #6f6; border-radius: 3px; padding: 1px 6px;
    font-size: 11px; margin-left: 6px;
  }
  #banner {
    display: none; background: #400; color: #faa; padding: 8px 12px;
    border-radius: 4px; margin-bottom: 16px;
  }
  #toolbar { margin-bottom: 16px; }
</style>
</head>
<body>
<h1>Music in the Matrix</h1>
<p class="sub">control panel -- polls every second</p>
<div id="banner">Can't reach the host. It may have quit -- this page won't recover on its own.</div>
<div id="toolbar">
  <button class="primary" onclick="spawn()">+ Spawn Window</button>
  <button onclick="randomizeAll()">🎲 Randomize All</button>
</div>
<table>
  <thead><tr><th>Window</th><th>Mode</th><th>Color</th><th></th></tr></thead>
  <tbody id="rows"></tbody>
</table>
<script>
const COLORS = ["red", "green", "blue", "yellow", "cyan", "magenta", "white"];
const MODES = ["bars-left", "bars-right", "bars-middle", "matrix", "checkerboard"];
const SWATCH_HEX = {
  red: "#f33", green: "#3f3", blue: "#39f", yellow: "#ee3",
  cyan: "#3ee", magenta: "#e3e", white: "#eee"
};

function optionsFor(list, current) {
  return list.map(v => `<option value="${v}"${v === current ? " selected" : ""}>${v}</option>`).join("");
}

function render(windows) {
  const rows = document.getElementById("rows");
  rows.innerHTML = windows.map(w => {
    const label = w.isHost ? `Host <span class="host-badge">0</span>` : `Window ${w.displayNumber}`;
    const closeLabel = w.isHost ? "Quit All" : "Close";
    const closeClass = w.isHost ? "danger" : "";
    const swatch = SWATCH_HEX[w.colorName] || "#555";
    return `<tr>
      <td><span class="swatch" style="background:${swatch}"></span>${label}</td>
      <td><select onchange="setMode(${w.displayNumber}, this.value)">${optionsFor(MODES, w.mode)}</select></td>
      <td><select onchange="setColor(${w.displayNumber}, this.value)">${optionsFor(COLORS, w.colorName)}</select></td>
      <td><button class="${closeClass}" onclick="closeWindow(${w.displayNumber})">${closeLabel}</button></td>
    </tr>`;
  }).join("");
}

async function post(path, body) {
  await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body || {})
  });
}

function spawn() { post("/api/spawn"); }
function closeWindow(id) { post("/api/close", { id }); }
function setMode(id, mode) { post("/api/mode", { id, mode }); }
function setColor(id, color) { post("/api/color", { id, color }); }

function randomPick(list) { return list[Math.floor(Math.random() * list.length)]; }

// Fetches a fresh window list rather than reusing whatever render() last
// drew, so a window that connected or disconnected a moment ago (outside
// this page's own 1s poll cycle) doesn't get skipped or 404 -- each
// window's mode and color are picked independently, not the same pair
// for all of them.
async function randomizeAll() {
  const r = await fetch("/api/state");
  if (!r.ok) return;
  const data = await r.json();
  await Promise.all(data.windows.flatMap(w => [
    post("/api/mode", { id: w.displayNumber, mode: randomPick(MODES) }),
    post("/api/color", { id: w.displayNumber, color: randomPick(COLORS) }),
  ]));
  poll(); // don't wait up to 1s to see the result
}

// render() replaces every row's innerHTML wholesale, <select> elements
// included -- fine most of the time, but destroying and recreating a
// <select> out from under an open (or just-clicked, POST still in
// flight) dropdown closes it and can make the choice look like it never
// took, since the freshly-rebuilt element gets its "selected" option
// from whatever state was last fetched, not the pick the user just
// hasn't heard back confirmed yet. So: skip a poll's render entirely
// while any dropdown in the table has focus, and catch up immediately
// (rather than waiting up to 1s) the moment focus leaves one.
function rowsFocused() {
  const el = document.activeElement;
  return !!(el && el.tagName === "SELECT" && document.getElementById("rows").contains(el));
}

async function poll() {
  if (rowsFocused()) return;
  try {
    const r = await fetch("/api/state");
    if (!r.ok) throw new Error("bad status");
    const data = await r.json();
    document.getElementById("banner").style.display = "none";
    render(data.windows);
  } catch (e) {
    document.getElementById("banner").style.display = "block";
  }
}

document.getElementById("rows").addEventListener("focusout", () => {
  // A small delay so the onchange handler's POST (fired just before
  // this) has a moment to actually reach the host before we re-fetch.
  setTimeout(poll, 100);
});

poll();
setInterval(poll, 1000);
</script>
</body>
</html>
)HTML";

// Reads from `fd` into `buf` until `terminator` is found within it (or
// `maxBytes` is exceeded, in which case this gives up and returns
// false -- a malformed or oversized request header isn't worth trying
// harder for on a purely local control panel). Leaves whatever was read
// past the terminator in `buf` too, since that's the start of the body.
bool readUntil(int fd, std::string& buf, const std::string& terminator, size_t maxBytes) {
    char chunk[4096];
    while (buf.find(terminator) == std::string::npos) {
        if (buf.size() > maxBytes) return false;
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false; // closed or error
        buf.append(chunk, static_cast<size_t>(n));
    }
    return true;
}

// Case-insensitive search for a header's value within the raw header
// block (everything before the blank line). Minimal on purpose: no
// folding, no repeated-header merging -- this server only ever needs
// Content-Length.
std::string findHeader(const std::string& headerBlock, const std::string& name) {
    std::string lower = headerBlock;
    std::string lowerName = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t pos = lower.find(lowerName + ":");
    if (pos == std::string::npos) return "";
    size_t valueStart = pos + lowerName.size() + 1;
    size_t lineEnd = headerBlock.find("\r\n", valueStart);
    if (lineEnd == std::string::npos) lineEnd = headerBlock.size();
    std::string value = headerBlock.substr(valueStart, lineEnd - valueStart);
    // Trim leading/trailing whitespace.
    size_t start = value.find_first_not_of(" \t");
    size_t end = value.find_last_not_of(" \t");
    if (start == std::string::npos) return "";
    return value.substr(start, end - start + 1);
}

bool writeAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        default: return "Error";
    }
}

} // namespace

ControlServer::ControlServer(int port, Handler handler) : handler_(std::move(handler)) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throw std::runtime_error(std::string("ControlServer: socket() failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only -- see the header comment

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error(std::string("ControlServer: bind() failed on port ") +
                                  std::to_string(port) + ": " + std::strerror(errno));
    }

    constexpr int kBacklog = 16;
    if (::listen(listenFd_, kBacklog) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error(std::string("ControlServer: listen() failed: ") + std::strerror(errno));
    }

    acceptThread_ = std::thread([this] { acceptLoop(); });
}

ControlServer::~ControlServer() {
    stopRequested_ = true;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR); // unblocks accept()
        ::close(listenFd_);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
}

void ControlServer::acceptLoop() {
    while (!stopRequested_.load()) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) break; // listening socket closed (shutdown) or real error either way
        // One detached thread per request: control-panel traffic is a
        // handful of infrequent clicks, not a workload worth pooling
        // threads for, and each connection closes itself right after its
        // one response (see kControlPanelHtml's doc comment).
        std::thread([this, fd] { handleConnection(fd); }).detach();
    }
}

void ControlServer::handleConnection(int fd) {
    std::string buf;
    constexpr size_t kMaxHeaderBytes = 16 * 1024;
    if (!readUntil(fd, buf, "\r\n\r\n", kMaxHeaderBytes)) {
        ::close(fd);
        return;
    }

    size_t headerEnd = buf.find("\r\n\r\n");
    std::string headerBlock = buf.substr(0, headerEnd);
    std::string bodySoFar = buf.substr(headerEnd + 4);

    size_t requestLineEnd = headerBlock.find("\r\n");
    std::string requestLine = headerBlock.substr(0, requestLineEnd);

    HttpRequest req;
    {
        size_t methodEnd = requestLine.find(' ');
        size_t pathEnd = requestLine.find(' ', methodEnd == std::string::npos ? 0 : methodEnd + 1);
        if (methodEnd == std::string::npos || pathEnd == std::string::npos) {
            ::close(fd);
            return;
        }
        req.method = requestLine.substr(0, methodEnd);
        std::string fullPath = requestLine.substr(methodEnd + 1, pathEnd - methodEnd - 1);
        req.path = fullPath.substr(0, fullPath.find('?')); // query strings aren't used -- see header comment
    }

    std::string contentLengthStr = findHeader(headerBlock, "Content-Length");
    size_t contentLength = contentLengthStr.empty() ? 0 : static_cast<size_t>(std::stoul(contentLengthStr));
    // A malformed/malicious Content-Length would otherwise drive an
    // unbounded read loop -- local control panel or not, cap it.
    constexpr size_t kMaxBodyBytes = 1 << 20; // 1 MiB
    if (contentLength > kMaxBodyBytes) {
        ::close(fd);
        return;
    }
    while (bodySoFar.size() < contentLength) {
        char chunk[4096];
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            ::close(fd);
            return;
        }
        bodySoFar.append(chunk, static_cast<size_t>(n));
    }
    req.body = bodySoFar.substr(0, contentLength);

    HttpResponse res;
    if (req.method == "GET" && req.path == "/") {
        res.status = 200;
        res.contentType = "text/html; charset=utf-8";
        res.body = kControlPanelHtml;
    } else {
        res = handler_(req);
    }

    std::string response = "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) +
                            "\r\nContent-Type: " + res.contentType +
                            "\r\nContent-Length: " + std::to_string(res.body.size()) +
                            "\r\nConnection: close\r\n\r\n" + res.body;
    writeAll(fd, response);
    ::close(fd);
}

} // namespace mitm
