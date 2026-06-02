// XTE-DCDU: X-Plane plugin entry points and orchestration.
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless DCDU streamer: locks onto the ToLiss A320 panel texture, reads the
// DCDU rectangle via FBO+PBO, JPEG-encodes it on a worker thread, and ships
// changed frames over TCP to an ESP32-S3 display.

// winsock2.h before any X-Plane SDK header that may pull in windows.h.
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define _WINSOCKAPI_
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif

#include "XPLMDefs.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMDisplay.h"
#include "XPLMUtilities.h"
#include "XPLMDataAccess.h"
#include "XPLMPlanes.h"
#include "XPLMMenus.h"
#include "XPWidgets.h"
#include "XPStandardWidgets.h"

#include "log.hpp"
#include "config.hpp"
#include "texture_finder.hpp"
#include "readback.hpp"
#include "jpeg_encoder.hpp"
#include "tcp_client.hpp"
#include "change_detect.hpp"
#include "spsc_ring.hpp"
#include "gl_loader.hpp"
#include "button_handler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef XTE_DCDU_VERSION
#define XTE_DCDU_VERSION "0.1.0"
#endif

#if defined(_WIN32)
  #include <windows.h>
  #define DIR_SEP '\\'
#else
  #include <dlfcn.h>
  #define DIR_SEP '/'
#endif

namespace {

using namespace xtedcdu;

struct Frame {
    int                   w = 0;
    int                   h = 0;
    std::vector<uint8_t>  rgb;  // tightly packed, top-down
};

// ---- Shared state ----------------------------------------------------------

Config                            g_cfg;
std::string                       g_plugin_dir;     // directory containing the .xpl
std::string                       g_cfg_path;

std::atomic<bool>                 g_armed{false};
std::atomic<unsigned int>         g_panel_tex{0};
std::atomic<int>                  g_status_texture_id{-1};
std::atomic<int>                  g_status_connected{0};
std::atomic<int>                  g_status_frames_sent{0};
std::atomic<int>                  g_status_frames_skipped{0};
std::atomic<int>                  g_status_last_send_error{0};
std::atomic<bool>                 g_request_dump{false};

XPLMFlightLoopID                  g_loop = nullptr;
XPLMCommandRef                    g_cmd_reload   = nullptr;
XPLMCommandRef                    g_cmd_reconnect = nullptr;
XPLMCommandRef                    g_cmd_dump     = nullptr;
XPLMCommandRef                    g_cmd_next_tex = nullptr;
XPLMCommandRef                    g_cmd_settings = nullptr;

XPLMDataRef                       g_dr_tex      = nullptr;
XPLMDataRef                       g_dr_conn     = nullptr;
XPLMDataRef                       g_dr_sent     = nullptr;
XPLMDataRef                       g_dr_skipped  = nullptr;
XPLMDataRef                       g_dr_lasterr  = nullptr;

// ---- ESP32 Settings UI -----------------------------------------------------
// In-process editor for esp32_host / esp32_port. Backed by the Widgets API
// (XPLM widgets), opened from the Plugins menu.
XPLMMenuID                        g_menu_id   = nullptr;

XPWidgetID                        g_set_window      = nullptr;
XPWidgetID                        g_set_field_host  = nullptr;
XPWidgetID                        g_set_field_port  = nullptr;
XPWidgetID                        g_set_btn_save    = nullptr;
XPWidgetID                        g_set_btn_cancel  = nullptr;
XPWidgetID                        g_set_status      = nullptr;

Readback                          g_readback;
std::unique_ptr<TcpEndpoint>      g_tcp;
ChangeDetector                    g_change;

SpscRing<Frame, 4>                g_queue;

// Button events received from the ESP32. Written by the worker thread,
// read and dispatched by the flight-loop callback on the main X-Plane thread.
SpscRing<ButtonEvent, 32>         g_btn_queue;

std::thread                       g_worker;
std::atomic<bool>                 g_worker_run{false};
std::atomic<uint32_t>             g_seq{0};

std::atomic<int64_t>              g_last_send_ms{0};
std::atomic<int64_t>              g_last_image_ms{0};
std::atomic<uint64_t>             g_frames_changed{0};
std::atomic<uint64_t>             g_frames_unchanged{0};
std::atomic<uint64_t>             g_frames_rate_limited{0};
std::atomic<bool>                 g_draw_seen{false};
std::atomic<int>                  g_active_draw_phase{-1};

bool                              g_draw_reg_window = false;
bool                              g_draw_reg_gauges = false;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- Dataref accessors -----------------------------------------------------

int dr_read(void* refcon) {
    auto* a = static_cast<std::atomic<int>*>(refcon);
    return a->load(std::memory_order_relaxed);
}

XPLMDataRef reg_int_dr(const char* name, std::atomic<int>* slot) {
    return XPLMRegisterDataAccessor(name,
        xplmType_Int,
        /*writable*/ 0,
        dr_read, nullptr,
        nullptr, nullptr,
        nullptr, nullptr,
        nullptr, nullptr,
        nullptr, nullptr,
        nullptr, nullptr,
        slot, nullptr);
}

// ---- Plugin directory discovery -------------------------------------------

std::string get_plugin_directory() {
    // Plugin path relative to X-Plane root. Use XPLMGetPluginInfo if
    // available; otherwise fall back to current working dir.
    XPLMPluginID me = XPLMGetMyID();
    char path[1024] = {0};
    char sig[256] = {0};
    char desc[256] = {0};
    char name[256] = {0};
    XPLMGetPluginInfo(me, name, path, sig, desc);
    if (path[0] == 0) return ".";
    std::string p = path;
    // path is absolute or X-Plane relative; ensure absolute by going through
    // XPLMGetSystemPath if needed.
    if (p.size() > 1 && p[1] != ':' && p[0] != '/') {
        char sys[1024] = {0};
        XPLMGetSystemPath(sys);
        p = std::string(sys) + p;
    }
    auto slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return p.substr(0, slash);
}

// ---- Worker thread: JPEG encode + send ------------------------------------

// RGB888 (3 bytes/pixel) -> RGB565 little-endian (2 bytes/pixel) in one pass.
// Output layout per pixel: byte0 = ((g & 0x1c) << 3) | (b >> 3)
//                          byte1 = (r & 0xf8) | (g >> 5)
// (i.e. native uint16 LE on a little-endian host such as the ESP32.)
static void rgb888_to_rgb565_le(const uint8_t* rgb, int w, int h,
                                std::vector<uint8_t>& out) {
    const size_t n = static_cast<size_t>(w) * h;
    out.resize(n * 2);
    uint8_t* dst = out.data();
    for (size_t i = 0; i < n; ++i) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        uint16_t v = (uint16_t)((r & 0xF8) << 8) |
                     (uint16_t)((g & 0xFC) << 3) |
                     (uint16_t)(b >> 3);
        dst[i * 2 + 0] = (uint8_t)(v & 0xFF);
        dst[i * 2 + 1] = (uint8_t)(v >> 8);
    }
}

void worker_main() {
    XTED_LOG("worker: started");
    std::vector<uint8_t> jpeg_buf;
    jpeg_buf.reserve(64 * 1024);
    std::vector<uint8_t> raw_buf;
    raw_buf.reserve(128 * 1024);

    int64_t reconnect_ready_at = 0;

    while (g_worker_run.load(std::memory_order_acquire)) {
        // 1) Connection management
        if (!g_tcp->connected()) {
            int64_t t = now_ms();
            if (t < reconnect_ready_at) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (g_tcp->ensure_connected()) {
                g_status_connected.store(1, std::memory_order_relaxed);
                XTED_LOG("worker: TCP connected");
            } else {
                int wait = g_tcp->next_backoff_ms();
                reconnect_ready_at = now_ms() + wait;
                g_status_connected.store(0, std::memory_order_relaxed);
                g_status_last_send_error.store(g_tcp->last_errno(), std::memory_order_relaxed);
                XTED_ERR("worker: TCP connect failed (errno %d); retry in %d ms",
                         g_tcp->last_errno(), wait);
                continue;
            }
        }

        // 2) Drain frames; keep only newest if we're behind.
        Frame f;
        bool got = false;
        while (g_queue.pop(f)) got = true;

        int64_t t_now = now_ms();
        if (got) {
            const size_t bytes = static_cast<size_t>(f.w) * f.h * 3;

            // Honour a pending dump request first. We want this to fire on
            // the next captured frame regardless of change-detection or
            // rate-limit state, otherwise a static display (the common case
            // for DCDU between messages) would never produce a dump.
            if (g_request_dump.exchange(false)) {
                std::vector<uint8_t> dump_buf;
                size_t dn = encode_jpeg_rgb(f.rgb.data(), f.w, f.h,
                                            g_cfg.jpeg_quality, dump_buf);
                if (dn == 0) {
                    XTED_ERR("dump: jpeg encode failed");
                } else {
                    std::string out = g_plugin_dir + DIR_SEP + "xte-dcdu-dump.jpg";
                    std::ofstream of(out, std::ios::binary);
                    of.write(reinterpret_cast<const char*>(dump_buf.data()),
                             (std::streamsize)dump_buf.size());
                    XTED_LOG("dump: wrote %zu bytes (%dx%d) to %s",
                             dn, f.w, f.h, out.c_str());
                }
            }

            uint64_t cur_hash = 0;
            bool changed = g_change.differs(f.rgb.data(), bytes, &cur_hash);
            if (!changed) {
                uint64_t unchanged = g_frames_unchanged.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((unchanged % 200ull) == 0ull) {
                    XTED_LOG("worker: unchanged frames=%llu sent=%d skipped=%d",
                             (unsigned long long)unchanged,
                             g_status_frames_sent.load(std::memory_order_relaxed),
                             g_status_frames_skipped.load(std::memory_order_relaxed));
                }
                g_status_frames_skipped.fetch_add(1, std::memory_order_relaxed);
            } else {
                uint64_t changed_count = g_frames_changed.fetch_add(1, std::memory_order_relaxed) + 1;
                int64_t since = t_now - g_last_send_ms.load(std::memory_order_relaxed);
                if (since < g_cfg.min_send_interval_ms) {
                    // Too soon. Skip this frame but DO NOT update the stored
                    // hash -- otherwise the dropped content becomes the new
                    // baseline and any subsequent frames that match it will
                    // be filtered out as "unchanged", causing the device
                    // display to lag behind the real DCDU until the next
                    // truly-different frame arrives.
                    uint64_t rate_limited = g_frames_rate_limited.fetch_add(1, std::memory_order_relaxed) + 1;
                    if ((rate_limited % 100ull) == 0ull) {
                        XTED_LOG("worker: rate-limited changed frames=%llu (min_send_interval_ms=%d)",
                                 (unsigned long long)rate_limited, g_cfg.min_send_interval_ms);
                    }
                    g_status_frames_skipped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const uint8_t* payload = nullptr;
                    size_t         payload_len = 0;
                    uint8_t        frame_type  = 0;
                    bool           encode_ok   = false;

                    if (g_cfg.raw_rgb565) {
                        raw_buf.clear();
                        rgb888_to_rgb565_le(f.rgb.data(), f.w, f.h, raw_buf);
                        payload     = raw_buf.data();
                        payload_len = raw_buf.size();
                        frame_type  = kTypeImageRaw565;
                        encode_ok   = (payload_len > 0);
                    } else {
                        jpeg_buf.clear();
                        size_t n = encode_jpeg_rgb(f.rgb.data(), f.w, f.h,
                                                   g_cfg.jpeg_quality, jpeg_buf);
                        if (n == 0) {
                            XTED_ERR("worker: jpeg encode failed");
                        } else {
                            payload     = jpeg_buf.data();
                            payload_len = jpeg_buf.size();
                            frame_type  = kTypeImage;
                            encode_ok   = true;
                        }
                    }

                    if (encode_ok) {
                        uint32_t seq = g_seq.fetch_add(1) + 1;
                        FrameHeader h{};
                        h.magic       = kMagic;
                        h.version     = kVersion;
                        h.type        = frame_type;
                        h.seq         = seq;
                        h.width       = (uint16_t)f.w;
                        h.height      = (uint16_t)f.h;
                        h.payload_len = (uint32_t)payload_len;

                        uint8_t hdr[18];
                        pack_header(h, hdr);

                        bool ok = g_tcp->send_all(hdr, sizeof(hdr)) &&
                                  g_tcp->send_all(payload, payload_len);
                        if (ok) {
                            int sent = g_status_frames_sent.fetch_add(1, std::memory_order_relaxed) + 1;
                            if (sent == 1) XTED_LOG("worker: first frame sent (%zu bytes, type=0x%02x)",
                                                    payload_len, frame_type);
                            if ((sent % 30) == 0) {
                                XTED_LOG("worker: sent=%d changed=%llu unchanged=%llu rate_limited=%llu",
                                         sent,
                                         (unsigned long long)changed_count,
                                         (unsigned long long)g_frames_unchanged.load(std::memory_order_relaxed),
                                         (unsigned long long)g_frames_rate_limited.load(std::memory_order_relaxed));
                            }
                            g_last_send_ms.store(t_now, std::memory_order_relaxed);
                            g_last_image_ms.store(t_now, std::memory_order_relaxed);
                            // Only now commit the hash as the new baseline.
                            g_change.accept(cur_hash);
                        } else {
                            XTED_ERR("worker: image send failed (errno %d)", g_tcp->last_errno());
                            g_status_connected.store(0, std::memory_order_relaxed);
                            g_status_last_send_error.store(g_tcp->last_errno(), std::memory_order_relaxed);
                        }
                    }
                }
            }
        }

        // 3) Heartbeat
        int64_t since = t_now - g_last_send_ms.load(std::memory_order_relaxed);
        if (g_tcp->connected() && since >= g_cfg.heartbeat_ms) {
            FrameHeader h{};
            h.magic       = kMagic;
            h.version     = kVersion;
            h.type        = kTypeHeartbeat;
            h.seq         = g_seq.fetch_add(1) + 1;
            uint8_t hdr[18];
            pack_header(h, hdr);
            if (g_tcp->send_all(hdr, sizeof(hdr))) {
                g_last_send_ms.store(t_now, std::memory_order_relaxed);
            } else {
                XTED_ERR("worker: heartbeat send failed (errno %d)", g_tcp->last_errno());
                g_status_connected.store(0, std::memory_order_relaxed);
                g_status_last_send_error.store(g_tcp->last_errno(), std::memory_order_relaxed);
            }
        }

        // 4) Poll for incoming button events from the ESP32.
        {
            ButtonEvent be{};
            while (g_tcp->try_recv_button_event(be)) {
                if (!g_btn_queue.push(std::move(be))) {
                    XTED_VLOG("worker: button queue full, dropping btn=%u state=%u",
                              be.button_id, be.state);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    g_tcp->close();
    g_status_connected.store(0, std::memory_order_relaxed);
    XTED_LOG("worker: stopped");
}

// ---- Texture-finder pacing -------------------------------------------------

int64_t g_last_find_attempt_ms = 0;

void try_find_texture() {
    int64_t t = now_ms();
    if (g_panel_tex.load() != 0) return;
    if (t - g_last_find_attempt_ms < 60'000 && g_last_find_attempt_ms != 0) {
        XTED_VLOG("texture: scan throttled; next attempt in %lld ms",
                  (long long)(60'000 - (t - g_last_find_attempt_ms)));
        return;
    }
    g_last_find_attempt_ms = t;

    // Validate aircraft prefix.
    char filename[512] = {0};
    char acf_path[1024] = {0};
    XPLMGetNthAircraftModel(0, filename, acf_path);
    std::string fn(filename);
    std::string lo;
    lo.reserve(fn.size());
    for (char c : fn) lo += (char)std::tolower((unsigned char)c);
    std::string prefix = g_cfg.acf_prefix;
    for (auto& c : prefix) c = (char)std::tolower((unsigned char)c);
    if (lo.find(prefix) == std::string::npos) {
        XTED_LOG("texture: skipping scan; acf '%s' does not match prefix '%s'",
                 filename, g_cfg.acf_prefix.c_str());
        return;
    }

    // Validate panel PNG dimensions vs config (best-effort).
    std::string png = TextureFinder::find_panel_png_path();
    if (!png.empty()) {
        int pw = 0, ph = 0;
        if (TextureFinder::read_png_dimensions(png, pw, ph)) {
            if (pw != g_cfg.panel_width || ph != g_cfg.panel_height) {
                XTED_ERR("texture: panel PNG is %dx%d but config expects %dx%d",
                         pw, ph, g_cfg.panel_width, g_cfg.panel_height);
            }
        }
    }

    XTED_LOG("texture: scanning ids %d..%d for %dx%d fmt=0x8058",
             g_cfg.start_texture_id + 1,
             g_cfg.texture_scan_max_id,
             g_cfg.panel_width,
             g_cfg.panel_height);

    unsigned int id = TextureFinder::scan_for_panel_texture(
        g_cfg.panel_width, g_cfg.panel_height,
        g_cfg.start_texture_id, g_cfg.texture_scan_max_id);
    if (id == 0) {
        XTED_LOG("texture: scan complete; no matching texture found in %d..%d",
                 g_cfg.start_texture_id + 1, g_cfg.texture_scan_max_id);
        return;
    }

    g_panel_tex.store(id);
    g_status_texture_id.store((int)id, std::memory_order_relaxed);
    XTED_LOG("locked onto texture id %u", id);

    if (!g_readback.init(g_cfg.crop_w(), g_cfg.crop_h(), g_cfg.pbo_count)) {
        XTED_ERR("texture: readback init failed; disarming");
        g_panel_tex.store(0);
        g_status_texture_id.store(-1, std::memory_order_relaxed);
    }
    g_change.reset();
}

// ---- Draw callback (render thread) ----------------------------------------

int draw_cb(XPLMDrawingPhase phase, int, void*) {
    int active = g_active_draw_phase.load(std::memory_order_relaxed);
    if (active == -1) {
        g_active_draw_phase.store((int)phase, std::memory_order_relaxed);
        active = (int)phase;
        XTED_LOG("draw: selected active phase=%d", active);
    }
    if ((int)phase != active) {
        return 1;
    }

    if (!g_draw_seen.exchange(true)) {
        XTED_LOG("draw: callback active (phase=%d)", active);
    }

    if (!g_armed.load(std::memory_order_acquire)) return 1;

    unsigned int tex = g_panel_tex.load(std::memory_order_acquire);
    if (tex == 0) {
        try_find_texture();
        return 1;
    }
    if (!g_readback.initialised()) return 1;

    Frame f;
    f.w = g_cfg.crop_w();
    f.h = g_cfg.crop_h();
    if (!g_readback.readback(tex, g_cfg.panel_height,
                             g_cfg.x1, g_cfg.y1, g_cfg.x2, g_cfg.y2,
                             f.rgb)) {
        return 1;
    }
    if ((int)f.rgb.size() != f.w * f.h * 3) return 1;

    if (!g_queue.push(std::move(f))) {
        // Queue full — drop oldest, push new.
        Frame discarded;
        g_queue.pop(discarded);
        g_queue.push(std::move(f));
    }
    return 1;
}

// ---- Flight loop (low-rate housekeeping) ----------------------------------

float flight_loop_cb(float, float, int, void*) {
    // Dispatch any queued button events on the main X-Plane thread.
    ButtonEvent be{};
    while (g_btn_queue.pop(be)) {
        dispatch_button_event(g_cfg, be.button_id, be.state);
    }

    return g_cfg.flight_loop_interval > 0
        ? (float)g_cfg.flight_loop_interval
        : 0.05f;
}

// ---- ESP32 Settings window -------------------------------------------------

// Read a text field's current value into a std::string. Widgets store the
// descriptor as a null-terminated C string; the SDK gives us its length first.
std::string widget_get_text(XPWidgetID w) {
    int len = XPGetWidgetDescriptor(w, nullptr, 0);
    if (len <= 0) return {};
    std::string s(len, '\0');
    XPGetWidgetDescriptor(w, s.data(), len + 1);
    // s ends up null-padded if the SDK wrote a trailing \0; trim any trailing \0.
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

void set_status_msg(const char* msg) {
    if (g_set_status) XPSetWidgetDescriptor(g_set_status, msg);
}

// Apply the values currently in the text fields: validate, update g_cfg,
// persist to the cfg file, and force the worker to reconnect with the new
// endpoint.
void apply_settings_from_fields() {
    std::string host = widget_get_text(g_set_field_host);
    std::string port_s = widget_get_text(g_set_field_port);

    // Trim leading/trailing whitespace.
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string{};
        return s.substr(a, b - a + 1);
    };
    host = trim(host);
    port_s = trim(port_s);

    if (host.empty()) {
        set_status_msg("Host must not be empty.");
        return;
    }

    int port = 0;
    try {
        size_t p = 0;
        port = std::stoi(port_s, &p);
        if (p != port_s.size()) throw std::invalid_argument("trailing chars");
    } catch (...) {
        set_status_msg("Port must be an integer.");
        return;
    }
    if (port <= 0 || port > 65535) {
        set_status_msg("Port must be between 1 and 65535.");
        return;
    }

    g_cfg.esp32_host = host;
    g_cfg.esp32_port = port;

    if (!save_endpoint_to_config(g_cfg_path, host, port)) {
        set_status_msg("Saved in-memory; FAILED to write cfg file.");
        XTED_ERR("settings: failed to write '%s'", g_cfg_path.c_str());
    } else {
        set_status_msg("Saved. Reconnecting...");
        XTED_LOG("settings: esp32 endpoint set to %s:%d (persisted)",
                 host.c_str(), port);
    }

    if (g_tcp) {
        g_tcp->reconfigure(g_cfg);
        g_tcp->close();
        g_tcp->reset_backoff();
    }
}

int settings_widget_handler(XPWidgetMessage msg, XPWidgetID widget,
                            intptr_t p1, intptr_t /*p2*/) {
    if (msg == xpMessage_CloseButtonPushed && widget == g_set_window) {
        XPHideWidget(g_set_window);
        return 1;
    }
    if (msg == xpMsg_PushButtonPressed) {
        auto pressed = reinterpret_cast<XPWidgetID>(p1);
        if (pressed == g_set_btn_save) {
            apply_settings_from_fields();
            return 1;
        }
        if (pressed == g_set_btn_cancel) {
            XPHideWidget(g_set_window);
            return 1;
        }
    }
    return 0;
}

void create_settings_window() {
    // Geometry (X-Plane widget coords: origin = bottom-left of the screen).
    const int W = 380, H = 200;
    int sx = 0, sy = 0;
    XPLMGetScreenSize(&sx, &sy);
    const int x = (sx - W) / 2;
    const int y = (sy + H) / 2;   // top edge
    const int x2 = x + W;
    const int y2 = y - H;          // bottom edge

    g_set_window = XPCreateWidget(
        x, y, x2, y2,
        1 /*visible*/, "XTE-DCDU - ESP32 Settings",
        1 /*isRoot*/, 0 /*containerID*/, xpWidgetClass_MainWindow);
    XPSetWidgetProperty(g_set_window, xpProperty_MainWindowHasCloseBoxes, 1);

    auto add_caption = [&](int lx, int ly, int lx2, int ly2, const char* txt) {
        return XPCreateWidget(lx, ly, lx2, ly2,
                              1, txt, 0, g_set_window, xpWidgetClass_Caption);
    };
    auto add_field = [&](int lx, int ly, int lx2, int ly2, const char* txt) {
        XPWidgetID w = XPCreateWidget(lx, ly, lx2, ly2,
                                      1, txt, 0, g_set_window, xpWidgetClass_TextField);
        XPSetWidgetProperty(w, xpProperty_TextFieldType, xpTextEntryField);
        return w;
    };
    auto add_button = [&](int lx, int ly, int lx2, int ly2, const char* txt) {
        XPWidgetID w = XPCreateWidget(lx, ly, lx2, ly2,
                                      1, txt, 0, g_set_window, xpWidgetClass_Button);
        XPSetWidgetProperty(w, xpProperty_ButtonType, xpPushButton);
        return w;
    };

    // Row 1: host
    add_caption(x + 12, y - 30, x + 140, y - 50, "ESP32 host / IP:");
    g_set_field_host = add_field(x + 150, y - 30, x2 - 12, y - 52, g_cfg.esp32_host.c_str());

    // Row 2: port
    add_caption(x + 12, y - 70, x + 140, y - 90, "ESP32 port:");
    g_set_field_port = add_field(x + 150, y - 70, x + 250, y - 92,
                                 std::to_string(g_cfg.esp32_port).c_str());

    // Row 3: status line (becomes "Saved.", error messages, etc.)
    g_set_status = add_caption(x + 12, y - 115, x2 - 12, y - 135, "");

    // Row 4: buttons
    g_set_btn_save   = add_button(x + 12,  y - 160, x + 180, y - 184, "Save & Reconnect");
    g_set_btn_cancel = add_button(x + 200, y - 160, x + 300, y - 184, "Close");

    XPAddWidgetCallback(g_set_window, settings_widget_handler);
}

void open_settings_window() {
    if (!g_set_window) {
        create_settings_window();
        return;
    }
    // Refresh the field contents from the live config in case the file was
    // reloaded since the last time the window was opened.
    XPSetWidgetDescriptor(g_set_field_host, g_cfg.esp32_host.c_str());
    XPSetWidgetDescriptor(g_set_field_port, std::to_string(g_cfg.esp32_port).c_str());
    set_status_msg("");
    XPShowWidget(g_set_window);
    XPBringRootWidgetToFront(g_set_window);
}

void menu_handler(void* /*menu_ref*/, void* item_ref) {
    if ((intptr_t)item_ref == 1) {
        open_settings_window();
    }
}

// ---- Commands --------------------------------------------------------------

int cmd_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void* /*ref*/) {
    if (phase != xplm_CommandBegin) return 1;
    if (cmd == g_cmd_reload) {
        Config c;
        if (load_config(g_cfg_path, c)) {
            g_cfg = c;
            g_panel_tex.store(0);
            g_status_texture_id.store(-1, std::memory_order_relaxed);
            g_readback.shutdown();
            reset_button_cache();
            XTED_LOG("cmd: config reloaded");
        }
    } else if (cmd == g_cmd_reconnect) {
        if (g_tcp) {
            g_tcp->close();
            g_tcp->reset_backoff();
            XTED_LOG("cmd: reconnect requested");
        }
    } else if (cmd == g_cmd_dump) {
        g_request_dump.store(true);
        XTED_LOG("cmd: dump requested");
    } else if (cmd == g_cmd_settings) {
        open_settings_window();
    } else if (cmd == g_cmd_next_tex) {
        unsigned int cur = g_panel_tex.load();
        if (cur != 0) {
            // Bump start_texture_id past the current pick so the next
            // scan finds the next 4096x4096 candidate.
            g_cfg.start_texture_id = static_cast<int>(cur);
            XTED_LOG("cmd: next_texture; will skip past id=%u", cur);
        } else {
            XTED_LOG("cmd: next_texture; no current lock, resetting scan");
            g_cfg.start_texture_id = 0;
        }
        g_panel_tex.store(0);
        g_status_texture_id.store(-1, std::memory_order_relaxed);
        g_last_find_attempt_ms = 0;
        g_readback.shutdown();
        g_change.reset();
    }
    return 1;
}

XPLMCommandRef create_cmd(const char* name, const char* desc) {
    XPLMCommandRef c = XPLMCreateCommand(name, desc);
    XPLMRegisterCommandHandler(c, cmd_handler, /*before*/ 0, nullptr);
    return c;
}

} // namespace

// =============================================================================
// XPLM entry points
// =============================================================================

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strncpy(outName, "XTE-DCDU", 255);
    std::strncpy(outSig,  "com.user.xte-dcdu", 255);
    std::strncpy(outDesc, "Headless DCDU texture streamer for ToLiss A320 + ESP32", 255);

    XTED_LOG("XPluginStart v%s", XTE_DCDU_VERSION);

    g_plugin_dir = get_plugin_directory();
    g_cfg_path   = g_plugin_dir + DIR_SEP + "xte-dcdu.cfg";

    if (!load_config(g_cfg_path, g_cfg)) {
        XTED_LOG("config: writing default to '%s'", g_cfg_path.c_str());
        write_default_config(g_cfg_path);
        // Use defaults; do not fail.
    }

    g_tcp = std::make_unique<TcpEndpoint>(g_cfg);

    // Datarefs
    g_dr_tex     = reg_int_dr("xtedcdu/status/texture_id",            &g_status_texture_id);
    g_dr_conn    = reg_int_dr("xtedcdu/status/connected",             &g_status_connected);
    g_dr_sent    = reg_int_dr("xtedcdu/status/frames_sent",           &g_status_frames_sent);
    g_dr_skipped = reg_int_dr("xtedcdu/status/frames_skipped_nochange", &g_status_frames_skipped);
    g_dr_lasterr = reg_int_dr("xtedcdu/status/last_send_error",       &g_status_last_send_error);

    // Commands
    g_cmd_reload    = create_cmd("xtedcdu/reload_config", "Reload xte-dcdu.cfg");
    g_cmd_reconnect = create_cmd("xtedcdu/reconnect",     "Drop and re-establish TCP link");
    g_cmd_dump      = create_cmd("xtedcdu/dump_frame",    "Save next decoded frame as xte-dcdu-dump.jpg");
    g_cmd_next_tex  = create_cmd("xtedcdu/next_texture",  "Cycle to next matching panel texture candidate");
    g_cmd_settings  = create_cmd("xtedcdu/settings",      "Open ESP32 host/port settings window");

    // Plugins menu entry.
    int plugins_idx = XPLMAppendMenuItem(XPLMFindPluginsMenu(),
                                         "XTE-DCDU", nullptr, 1);
    g_menu_id = XPLMCreateMenu("XTE-DCDU", XPLMFindPluginsMenu(),
                               plugins_idx, menu_handler, nullptr);
    XPLMAppendMenuItem(g_menu_id,
                       "ESP32 Settings...",
                       (void*)(intptr_t)1, 1);

    // Draw callback (post-3D, pre-UI). Register both phases; use whichever
    // one X-Plane actually invokes first on this platform/setup.
    g_draw_reg_window = XPLMRegisterDrawCallback(draw_cb, xplm_Phase_Window,
                                                 /*before*/ 0, nullptr) != 0;
    g_draw_reg_gauges = XPLMRegisterDrawCallback(draw_cb, xplm_Phase_Gauges,
                                                 /*before*/ 0, nullptr) != 0;
    if (!g_draw_reg_window) XTED_ERR("draw: failed to register xplm_Phase_Window callback");
    if (!g_draw_reg_gauges) XTED_ERR("draw: failed to register xplm_Phase_Gauges callback");
    XTED_LOG("draw: registration window=%d gauges=%d",
             g_draw_reg_window ? 1 : 0,
             g_draw_reg_gauges ? 1 : 0);

    // Flight loop
    XPLMCreateFlightLoop_t fl{};
    fl.structSize       = sizeof(fl);
    fl.phase            = xplm_FlightLoop_Phase_AfterFlightModel;
    fl.callbackFunc     = flight_loop_cb;
    fl.refcon           = nullptr;
    g_loop = XPLMCreateFlightLoop(&fl);
    XPLMScheduleFlightLoop(g_loop, (float)g_cfg.flight_loop_interval, 1);

    // Worker thread
    g_worker_run.store(true);
    g_worker = std::thread(worker_main);

    return 1;
}

PLUGIN_API void XPluginStop() {
    XTED_LOG("XPluginStop");

    g_armed.store(false);

    g_worker_run.store(false);
    if (g_worker.joinable()) g_worker.join();

    if (g_draw_reg_window) {
        XPLMUnregisterDrawCallback(draw_cb, xplm_Phase_Window, 0, nullptr);
        g_draw_reg_window = false;
    }
    if (g_draw_reg_gauges) {
        XPLMUnregisterDrawCallback(draw_cb, xplm_Phase_Gauges, 0, nullptr);
        g_draw_reg_gauges = false;
    }
    if (g_loop) { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }

    auto unreg = [](XPLMCommandRef& c) {
        if (c) XPLMUnregisterCommandHandler(c, cmd_handler, 0, nullptr);
        c = nullptr;
    };
    unreg(g_cmd_reload);
    unreg(g_cmd_reconnect);
    unreg(g_cmd_dump);
    unreg(g_cmd_next_tex);
    unreg(g_cmd_settings);

    if (g_set_window) {
        XPDestroyWidget(g_set_window, 1 /*destroyChildren*/);
        g_set_window = nullptr;
    }
    if (g_menu_id) {
        XPLMDestroyMenu(g_menu_id);
        g_menu_id = nullptr;
    }

    auto unregdr = [](XPLMDataRef& r) {
        if (r) XPLMUnregisterDataAccessor(r);
        r = nullptr;
    };
    unregdr(g_dr_tex);
    unregdr(g_dr_conn);
    unregdr(g_dr_sent);
    unregdr(g_dr_skipped);
    unregdr(g_dr_lasterr);

    g_readback.shutdown();
    g_tcp.reset();
    reset_button_cache();
}

PLUGIN_API int XPluginEnable() {
    // Reload config on every enable so that edits made while the plugin was
    // disabled are picked up without requiring an X-Plane restart.
    Config c;
    if (load_config(g_cfg_path, c)) {
        g_cfg = c;
        apply_log_level(g_cfg);
        g_active_draw_phase.store(-1, std::memory_order_relaxed);
        g_draw_seen.store(false, std::memory_order_relaxed);
        XTED_LOG("texture: config acf_prefix='%s' panel=%dx%d scan=%d..%d",
                 g_cfg.acf_prefix.c_str(),
                 g_cfg.panel_width,
                 g_cfg.panel_height,
                 g_cfg.start_texture_id + 1,
                 g_cfg.texture_scan_max_id);
        g_panel_tex.store(0);
        g_status_texture_id.store(-1, std::memory_order_relaxed);
        g_last_find_attempt_ms = 0;
        g_readback.shutdown();
        g_change.reset();
        if (g_tcp) g_tcp->reconfigure(g_cfg);
        reset_button_cache();
        XTED_LOG("XPluginEnable: config reloaded");
    }
    g_armed.store(true);
    XTED_LOG("XPluginEnable");
    return 1;
}

PLUGIN_API void XPluginDisable() {
    g_armed.store(false);
    XTED_LOG("XPluginDisable");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, long inMsg, void*) {
    if (inMsg == XPLM_MSG_AIRPORT_LOADED || inMsg == XPLM_MSG_PLANE_LOADED) {
        XTED_LOG("msg: aircraft/airport loaded; resetting texture lookup");
        Config c;
        if (load_config(g_cfg_path, c)) g_cfg = c;
        g_panel_tex.store(0);
        g_status_texture_id.store(-1, std::memory_order_relaxed);
        g_last_find_attempt_ms = 0;
        g_readback.shutdown();
        g_change.reset();
        reset_button_cache();
    }
}
