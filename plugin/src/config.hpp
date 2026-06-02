// XTE-DCDU: configuration loader
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <string>
#include <cstdint>

namespace xtedcdu {

enum class TcpMode { Client, Server };

// ---- Button configuration --------------------------------------------------

constexpr int kButtonCount = 11;

// What the plugin does when it receives a button event from the ESP32.
enum class ButtonAction {
    None,      // button ignored
    Command,   // fire an X-Plane command on press
    Dataref,   // write a numeric value to a dataref on press (and optionally release)
};

struct ButtonConfig {
    ButtonAction action         = ButtonAction::None;
    std::string  command_name;   // used when action == Command
    std::string  dataref_name;   // used when action == Dataref
    float        value_press    = 1.0f;   // written on button press
    float        value_release  = 0.0f;   // written on button release
    bool         send_release   = true;   // whether to write value_release on release
};

// ---- Main config -----------------------------------------------------------

struct Config {
    // [aircraft]
    std::string acf_prefix      = "a320";
    int         panel_width     = 4096;
    int         panel_height    = 4096;

    // [window]
    int x1 = 1534, y1 = 2867, x2 = 1803, y2 = 3068;

    // [output]
    TcpMode     mode            = TcpMode::Client;
    std::string esp32_host      = "192.168.1.50";
    int         esp32_port      = 4711;
    std::string listen_host     = "0.0.0.0";
    int         listen_port     = 52501;
    int         reconnect_start_ms = 1000;
    int         reconnect_max_ms   = 30000;
    double      reconnect_factor   = 2.0;

    // [encoding]
    int         jpeg_quality      = 100;
    // When true, frames are sent as raw little-endian RGB565 (frame type
    // 0x04) instead of JPEG (type 0x01). Lossless and zero decode cost on
    // the ESP32, at the price of larger payloads (w*h*2 bytes vs ~5-25KB
    // JPEG). For the 269x201 DCDU that's 108KB/frame -- trivial on LAN.
    bool        raw_rgb565        = true;
    int         heartbeat_ms      = 2000;
    int         min_send_interval_ms = 100;

    // [readback]
    int         pbo_count            = 2;
    double      flight_loop_interval = 0.05;
    int         start_texture_id     = 0;
    int         texture_scan_max_id  = 10000;

    // [logging]
    std::string log_level = "normal";  // verbose | normal | quiet

    // [button.N] — one section per physical button (0 – kButtonCount-1).
    // Maps each button on the ESP32 to an X-Plane command or dataref.
    std::array<ButtonConfig, kButtonCount> buttons;

    int crop_w() const { return x2 - x1; }
    int crop_h() const { return y2 - y1; }
};

// Load an INI-style config from |path|. Returns true if file existed and parsed
// (even partially); returns false if the file does not exist. Out-of-range or
// unknown keys are logged but not fatal. On false return, |out| is left at its
// constructed defaults.
bool load_config(const std::string& path, Config& out);

// Write a default config (matching xte-dcdu.cfg.example) to |path|. Returns
// true on success.
bool write_default_config(const std::string& path);

// Patch the [output] esp32_host / esp32_port keys in the config file at |path|,
// preserving comments, formatting, and any other settings. If the file does not
// exist it is created. If the keys (or the [output] section) are missing they
// are appended. Returns true on success.
bool save_endpoint_to_config(const std::string& path,
                             const std::string& host,
                             int port);

// Apply config.log_level to the global logging level.
void apply_log_level(const Config& cfg);

} // namespace xtedcdu
