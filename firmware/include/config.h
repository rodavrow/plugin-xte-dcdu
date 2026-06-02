// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Network config. Wi-Fi credentials are NOT compile-time: they are captured
// at runtime through a captive-portal AP (see WiFiManager use in main.cpp)
// and persisted in NVS. The values below are only used for non-Wi-Fi knobs.
#ifndef LISTEN_PORT
#define LISTEN_PORT 4711
#endif
#ifndef LINK_TIMEOUT_MS
#define LINK_TIMEOUT_MS 5000
#endif
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "XTE-DCDU-Setup"
#endif
// AP password for the setup portal. Must be >=8 chars or empty for an open AP.
// Default is open so the user can find and connect to it without docs.
#ifndef WIFI_AP_PSK
#define WIFI_AP_PSK ""
#endif
// How long (seconds) the setup portal stays open before rebooting and
// retrying the saved network. 0 = forever.
#ifndef WIFI_PORTAL_TIMEOUT_S
#define WIFI_PORTAL_TIMEOUT_S 180
#endif
// How long (ms) to wait for the saved network on boot before falling back
// to the setup portal.
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 20000
#endif
// Hold this button (index into BTN_PIN_n) at boot for this many ms to wipe
// stored credentials and force the setup portal. Set BTN_RESET_INDEX to -1
// to disable.
#ifndef BTN_RESET_INDEX
#define BTN_RESET_INDEX 0
#endif
#ifndef BTN_RESET_HOLD_MS
#define BTN_RESET_HOLD_MS 3000
#endif

// ----------------------------------------------------------------------------
// Board selector.
//
// Default build: ESP32-S3-DevKitC-1 + external ILI9341 SPI TFT + 11 buttons.
// Pass -DBOARD_JC3248W535 to target the JC3248W535(C) all-in-one board
// (ESP32-S3 + 3.5" AXS15231B QSPI 320x480 display + 8 buttons on header P2).
//
// Board-specific overrides are applied *before* the generic defaults below,
// so the generic #ifndef blocks only fill in anything left unset.
// ----------------------------------------------------------------------------
#if defined(BOARD_JC3248W535)
  // Display: 320(W) x 480(H) native portrait; we render landscape 480x320.
  #define DISPLAY_WIDTH   480
  #define DISPLAY_HEIGHT  320
  // 8 buttons on header P2 (IO5, IO6, IO7, IO15, IO16, IO46, IO9, IO14).
  #define BTN_COUNT       8
  #define BTN_PIN_0       5
  #define BTN_PIN_1       6
  #define BTN_PIN_2       7
  #define BTN_PIN_3       15
  #define BTN_PIN_4       16
  #define BTN_PIN_5       46
  #define BTN_PIN_6       9
  #define BTN_PIN_7       14
#endif

// ----------------------------------------------------------------------------
// Display pinout: ILI9341 SPI on ESP32-S3-DevKitC-1 (default board).
// All pins overridable at build time via -D flags.
// Ignored when BOARD_JC3248W535 selects the AXS15231B QSPI panel.
// ----------------------------------------------------------------------------
#ifndef TFT_CS
#define TFT_CS    10
#endif
#ifndef TFT_DC
#define TFT_DC    11
#endif
#ifndef TFT_RST
#define TFT_RST   14
#endif
#ifndef TFT_MOSI
#define TFT_MOSI  13
#endif
#ifndef TFT_SCK
#define TFT_SCK   12
#endif
#ifndef TFT_MISO
#define TFT_MISO   9
#endif
#ifndef TFT_BL
#define TFT_BL    -1   // -1 if backlight tied to 3.3V
#endif

#ifndef SPI_HOST_USE
#define SPI_HOST_USE  SPI2_HOST   // FSPI on ESP32-S3
#endif

#ifndef SPI_FREQ_WRITE
#define SPI_FREQ_WRITE  40000000  // 40 MHz
#endif

#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH   320
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT  240
#endif

// Render mode: fit-to-height (preserves all DCDU content; horizontal
// letterboxing). Toggle to 0 for fit-to-width (vertical crop).
#ifndef DCDU_FIT_MODE_HEIGHT
#define DCDU_FIT_MODE_HEIGHT 1
#endif

// JPEG buffer size in PSRAM. 64 KB is overkill for a 269x201 q75 frame.
#ifndef JPEG_BUF_BYTES
#define JPEG_BUF_BYTES (64 * 1024)
#endif

// ----------------------------------------------------------------------------
// Push-button configuration.
//
// 11 momentary buttons, active-low with internal pull-up resistors enabled.
// Each button connects between the GPIO pin and GND — no external resistors
// needed.  Override any pin at build time with -D BTN_PIN_N=xx in
// platformio.ini build_flags.
//
// Default assignments avoid SPI-TFT pins (9–14) and ESP32-S3 strapping /
// USB pins (0, 3, 19, 20, 43, 44, 45, 46).
// ----------------------------------------------------------------------------
#ifndef BTN_COUNT
#define BTN_COUNT 11
#endif

#ifndef BTN_DEBOUNCE_MS
#define BTN_DEBOUNCE_MS 20   // ms of stable state before sending event
#endif

#ifndef BTN_POLL_MS
#define BTN_POLL_MS     10   // button-task polling interval (ms)
#endif

#ifndef BTN_PIN_0
#define BTN_PIN_0   1
#endif
#ifndef BTN_PIN_1
#define BTN_PIN_1   2
#endif
#ifndef BTN_PIN_2
#define BTN_PIN_2   4
#endif
#ifndef BTN_PIN_3
#define BTN_PIN_3   5
#endif
#ifndef BTN_PIN_4
#define BTN_PIN_4   6
#endif
#ifndef BTN_PIN_5
#define BTN_PIN_5   7
#endif
#ifndef BTN_PIN_6
#define BTN_PIN_6   8
#endif
#ifndef BTN_PIN_7
#define BTN_PIN_7  15
#endif
#ifndef BTN_PIN_8
#define BTN_PIN_8  16
#endif
#ifndef BTN_PIN_9
#define BTN_PIN_9  17
#endif
#ifndef BTN_PIN_10
#define BTN_PIN_10 18
#endif

