// XTE-DCDU: TCP client/server for sending DCDU frames
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later

// winsock2.h MUST be included before any header that might include windows.h
// (e.g. XPLMUtilities.h which we pull in via log.hpp). Define _WINSOCKAPI_
// so that the old winsock.h is skipped if windows.h is included afterward.
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define _WINSOCKAPI_
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif

#include "tcp_client.hpp"
#include "log.hpp"

#include <cstring>
#include <cerrno>
#include <chrono>
#include <thread>

#if defined(_WIN32)
  using socklen_t = int;
  static int last_sock_error() { return WSAGetLastError(); }
  static void close_sock(int s) { if (s != -1) closesocket(s); }
  static bool wsa_started = false;
  static void ensure_wsa() {
      if (wsa_started) return;
      WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
      wsa_started = true;
  }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/ioctl.h>
  static int last_sock_error() { return errno; }
  static void close_sock(int s) { if (s != -1) ::close(s); }
  static void ensure_wsa() {}
#endif

namespace xtedcdu {

void pack_header(const FrameHeader& h, uint8_t out[18]) {
    auto u32 = [](uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)(v       & 0xff);
        p[1] = (uint8_t)((v>>8)  & 0xff);
        p[2] = (uint8_t)((v>>16) & 0xff);
        p[3] = (uint8_t)((v>>24) & 0xff);
    };
    auto u16 = [](uint8_t* p, uint16_t v) {
        p[0] = (uint8_t)(v       & 0xff);
        p[1] = (uint8_t)((v>>8)  & 0xff);
    };
    u32(out + 0,  h.magic);
    out[4] = h.version;
    out[5] = h.type;
    u32(out + 6,  h.seq);
    u16(out + 10, h.width);
    u16(out + 12, h.height);
    u32(out + 14, h.payload_len);
}

// Parse an 18-byte little-endian header buffer into a FrameHeader.
// Returns true if magic and version are valid.
static bool unpack_header_buf(const uint8_t buf[18], FrameHeader& h) {
    auto u32 = [](const uint8_t* p) -> uint32_t {
        return (uint32_t)p[0]
             | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16)
             | ((uint32_t)p[3] << 24);
    };
    auto u16 = [](const uint8_t* p) -> uint16_t {
        return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    };
    h.magic       = u32(buf + 0);
    h.version     = buf[4];
    h.type        = buf[5];
    h.seq         = u32(buf + 6);
    h.width       = u16(buf + 10);
    h.height      = u16(buf + 12);
    h.payload_len = u32(buf + 14);
    return h.magic == kMagic && h.version == kVersion;
}

TcpEndpoint::TcpEndpoint(const Config& cfg) : cfg_(cfg) {
    ensure_wsa();
    backoff_ms_ = cfg_.reconnect_start_ms;
}

void TcpEndpoint::reconfigure(const Config& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
    backoff_ms_ = cfg_.reconnect_start_ms;
    close_locked();
}

TcpEndpoint::~TcpEndpoint() { close(); }

bool TcpEndpoint::connected() const {
    std::lock_guard<std::mutex> g(mu_);
    return sock_ != -1;
}

void TcpEndpoint::close() {
    std::lock_guard<std::mutex> g(mu_);
    close_locked();
}

void TcpEndpoint::close_locked() {
    if (sock_ != -1) { close_sock(sock_); sock_ = -1; }
    if (listen_sock_ != -1) { close_sock(listen_sock_); listen_sock_ = -1; }
}

void TcpEndpoint::reset_backoff() { backoff_ms_ = cfg_.reconnect_start_ms; }

int TcpEndpoint::next_backoff_ms() {
    int b = backoff_ms_;
    long next = static_cast<long>(b * cfg_.reconnect_factor);
    if (next > cfg_.reconnect_max_ms) next = cfg_.reconnect_max_ms;
    if (next < cfg_.reconnect_start_ms) next = cfg_.reconnect_start_ms;
    backoff_ms_ = static_cast<int>(next);
    return b;
}

bool TcpEndpoint::ensure_connected() {
    std::lock_guard<std::mutex> g(mu_);
    if (sock_ != -1) return true;
    return cfg_.mode == TcpMode::Client ? open_client_locked()
                                        : open_server_locked();
}

bool TcpEndpoint::open_client_locked() {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portbuf[16];
    std::snprintf(portbuf, sizeof(portbuf), "%d", cfg_.esp32_port);

    addrinfo* res = nullptr;
    int rc = getaddrinfo(cfg_.esp32_host.c_str(), portbuf, &hints, &res);
    if (rc != 0 || !res) {
        XTED_ERR("tcp: getaddrinfo(%s:%d) failed", cfg_.esp32_host.c_str(), cfg_.esp32_port);
        last_errno_.store(rc, std::memory_order_relaxed);
        return false;
    }

    int s = -1;
    for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
        s = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == -1) continue;
        if (connect(s, a->ai_addr, (socklen_t)a->ai_addrlen) == 0) break;
        last_errno_.store(last_sock_error(), std::memory_order_relaxed);
        close_sock(s);
        s = -1;
    }
    freeaddrinfo(res);

    if (s == -1) {
        XTED_ERR("tcp: connect(%s:%d) failed (errno=%d)",
                 cfg_.esp32_host.c_str(), cfg_.esp32_port, last_errno_.load());
        return false;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    sock_ = s;
    XTED_LOG("tcp: connected to %s:%d", cfg_.esp32_host.c_str(), cfg_.esp32_port);
    reset_backoff();
    return true;
}

bool TcpEndpoint::open_server_locked() {
    if (listen_sock_ == -1) {
        int s = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (s == -1) {
            last_errno_.store(last_sock_error(), std::memory_order_relaxed);
            return false;
        }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = htons((uint16_t)cfg_.listen_port);
        if (cfg_.listen_host.empty() || cfg_.listen_host == "0.0.0.0") {
            a.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            inet_pton(AF_INET, cfg_.listen_host.c_str(), &a.sin_addr);
        }
        if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
            last_errno_.store(last_sock_error(), std::memory_order_relaxed);
            XTED_ERR("tcp: bind(%s:%d) failed errno=%d",
                     cfg_.listen_host.c_str(), cfg_.listen_port, last_errno_.load());
            close_sock(s);
            return false;
        }
        if (listen(s, 1) != 0) {
            last_errno_.store(last_sock_error(), std::memory_order_relaxed);
            close_sock(s);
            return false;
        }
        listen_sock_ = s;
        XTED_LOG("tcp: listening on %s:%d",
                 cfg_.listen_host.c_str(), cfg_.listen_port);
    }

    // Poll-accept with a short timeout so callers can sleep between attempts.
#if defined(_WIN32)
    fd_set rd; FD_ZERO(&rd); FD_SET((SOCKET)listen_sock_, &rd);
    timeval tv{0, 200 * 1000};
    int n = select(0, &rd, nullptr, nullptr, &tv);
#else
    pollfd pfd{listen_sock_, POLLIN, 0};
    int n = poll(&pfd, 1, 200);
#endif
    if (n <= 0) return false;

    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    int c = (int)accept(listen_sock_, (sockaddr*)&peer, &plen);
    if (c == -1) {
        last_errno_.store(last_sock_error(), std::memory_order_relaxed);
        return false;
    }
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    sock_ = c;
    XTED_LOG("tcp: accepted client");
    reset_backoff();
    return true;
}

bool TcpEndpoint::send_all(const void* data, size_t len) {
    std::lock_guard<std::mutex> g(mu_);
    if (sock_ == -1) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = len;
    while (left) {
#if defined(_WIN32)
        int n = ::send(sock_, (const char*)p, (int)left, 0);
#else
        ssize_t n = ::send(sock_, p, left, 0);
#endif
        if (n <= 0) {
            last_errno_.store(last_sock_error(), std::memory_order_relaxed);
            XTED_LOG("tcp: send failed errno=%d, dropping connection",
                     last_errno_.load());
            close_sock(sock_); sock_ = -1;
            return false;
        }
        p += n; left -= (size_t)n;
    }
    return true;
}

bool TcpEndpoint::try_recv_button_event(ButtonEvent& out) {
    std::lock_guard<std::mutex> g(mu_);
    if (sock_ == -1) return false;

    // Check how many bytes are buffered without blocking.
#if defined(_WIN32)
    u_long avail = 0;
    if (ioctlsocket((SOCKET)sock_, FIONREAD, &avail) != 0) return false;
#else
    int avail = 0;
    if (::ioctl(sock_, FIONREAD, &avail) != 0) return false;
#endif
    if (avail < 18) return false;

    // A complete header is ready — read it.
    uint8_t buf[18];
    size_t got = 0;
    while (got < 18) {
#if defined(_WIN32)
        int r = ::recv(sock_, (char*)(buf + got), (int)(18 - got), 0);
#else
        ssize_t r = ::recv(sock_, buf + got, 18 - got, 0);
#endif
        if (r <= 0) {
            last_errno_.store(last_sock_error(), std::memory_order_relaxed);
            XTED_ERR("tcp: recv failed errno=%d, dropping connection",
                     last_errno_.load());
            close_locked();
            return false;
        }
        got += (size_t)r;
    }

    FrameHeader h{};
    if (!unpack_header_buf(buf, h)) {
        XTED_ERR("tcp: bad magic/version in incoming header (magic=0x%08x ver=%u); "
                 "dropping connection", h.magic, h.version);
        close_locked();
        return false;
    }

    if (h.type != kTypeButtonEvent) {
        // Unexpected type — skip any trailing payload to stay in frame, discard.
        XTED_ERR("tcp: unexpected incoming type 0x%02x (expected button event 0x%02x); "
                 "discarding", h.type, kTypeButtonEvent);
        if (h.payload_len > 0 && h.payload_len <= 65536) {
            // Drain payload bytes.
            uint8_t drain[512];
            uint32_t left = h.payload_len;
            while (left > 0) {
                uint32_t chunk = left < sizeof(drain) ? left : sizeof(drain);
#if defined(_WIN32)
                int r = ::recv(sock_, (char*)drain, (int)chunk, 0);
#else
                ssize_t r = ::recv(sock_, drain, chunk, 0);
#endif
                if (r <= 0) { close_locked(); return false; }
                left -= (uint32_t)r;
            }
        } else if (h.payload_len > 0) {
            // Oversized payload on an unknown type — can't safely skip; drop.
            close_locked();
        }
        return false;
    }

    if (h.payload_len != 0) {
        XTED_ERR("tcp: button event has unexpected payload_len=%u; discarding",
                 h.payload_len);
        return false;
    }

    out.button_id = (uint8_t)(h.width  & 0xFFu);
    out.state     = (uint8_t)(h.height & 0xFFu);
    out.seq       = h.seq;
    return true;
}

} // namespace xtedcdu
