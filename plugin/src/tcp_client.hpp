// XTE-DCDU: TCP client/server for sending DCDU frames
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>

#include "config.hpp"

namespace xtedcdu {

// ---- Wire format (little-endian throughout) --------------------------------

// 18-byte header + optional payload.
struct FrameHeader {
    uint32_t magic;       // 0x55444344 — LE bytes: 44 43 44 55 ('D','C','D','U')
    uint8_t  version;     // 0x01
    uint8_t  type;        // 0x01 image | 0x02 heartbeat | 0x03 button event
    uint32_t seq;
    uint16_t width;       // image: px width  | button event: button_id
    uint16_t height;      // image: px height | button event: state (0/1)
    uint32_t payload_len; // 0 for heartbeat and button events
};

// FrameHeader is a logical container only; struct padding means
// sizeof(FrameHeader) != 18. Always serialize via pack_header().

constexpr uint32_t kMagic           = 0x55444344u;
constexpr uint8_t  kVersion         = 0x01;
constexpr uint8_t  kTypeImage       = 0x01;  // plugin -> ESP32: JPEG payload
constexpr uint8_t  kTypeHeartbeat   = 0x02;
constexpr uint8_t  kTypeButtonEvent = 0x03;  // ESP32 -> plugin: button state change
constexpr uint8_t  kTypeImageRaw565 = 0x04;  // plugin -> ESP32: raw RGB565 LE payload (w*h*2 bytes)

// A button-press or button-release event received from the ESP32.
// Carried in the 18-byte header: type=0x03, width=button_id, height=state,
// payload_len=0. No additional payload bytes follow.
struct ButtonEvent {
    uint8_t  button_id;  // 0 – (kButtonCount - 1)
    uint8_t  state;      // 1 = pressed, 0 = released
    uint32_t seq;
};

// Pack a FrameHeader into 18 contiguous little-endian bytes.
void pack_header(const FrameHeader& h, uint8_t out[18]);

// ---- TcpEndpoint -----------------------------------------------------------

// One-shot endpoint: depending on cfg.mode this either dials the configured
// ESP32 host:port (Client) or accepts a single inbound connection (Server).
// Reconnects with exponential backoff on disconnect / dial failure.
//
// All send_*() and try_recv_button_event() are intended to be called from a
// single worker thread.
class TcpEndpoint {
public:
    explicit TcpEndpoint(const Config& cfg);
    ~TcpEndpoint();

    // Try once to open / accept; returns true if connected. Non-blocking-ish:
    // server mode polls the listener with a short timeout, client mode does
    // a blocking connect with OS default timeout.
    bool ensure_connected();

    // Send all bytes; returns true on success, false on error (and closes the
    // socket so the next ensure_connected() will reconnect).
    bool send_all(const void* data, size_t len);

    // True if the underlying socket is currently open.
    bool connected() const;

    // Close any open sockets. Safe to call from any thread; idempotent.
    void close();

    // Replace the config (e.g. new host/port) and close the current socket so
    // the worker reconnects immediately with the new settings.
    void reconfigure(const Config& cfg);

    // Backoff helpers for the reconnect loop.
    int  next_backoff_ms();
    void reset_backoff();

    int  last_errno() const { return last_errno_.load(std::memory_order_relaxed); }

    // Non-blocking poll for an incoming button event from the ESP32.
    // Returns true (and fills |out|) if a complete 18-byte button-event header
    // was available and valid. Returns false immediately if no data is ready.
    // Any unexpected incoming type is logged and discarded; the connection is
    // dropped on a framing error. Safe to call from the worker thread.
    bool try_recv_button_event(ButtonEvent& out);

private:
    Config cfg_;

    mutable std::mutex mu_;
    int sock_        = -1;  // active connection
    int listen_sock_ = -1;  // server-mode listening socket

    int   backoff_ms_ = 0;
    std::atomic<int> last_errno_{0};

    bool open_client_locked();
    bool open_server_locked();
    void close_locked();
};

} // namespace xtedcdu
