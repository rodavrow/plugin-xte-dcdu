If you find this useful, you can [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/R5O720EQBI) - every little helps! Thank you.

# XTE-DCDU

Headless DCDU texture streamer for **ToLiss A320 in X-Plane 12**, paired with
a **Guition JC3248W535C** all-in-one ESP32-S3 + 3.5" 320×480 capacitive
touch display showing the live DCDU.

Two artefacts:

- **`plugin/`** — `Plugin-XTE-DCDU` X-Plane 12 plugin. Locks onto the panel
  texture, reads the DCDU rectangle via FBO+PBO async readback, encodes the
  frame (raw RGB565 by default, lossless; JPEG optional) on a worker thread,
  and ships changed frames over TCP.
- **`firmware/`** — ESP32-S3 firmware (PlatformIO / Arduino) for the
  **JC3248W535C** board. Listens on TCP, decodes the frame, and renders
  to the on-board AXS15231B QSPI 320×480 panel via Arduino_GFX. The eight
  push-buttons on header P2 are sent back to the plugin as button events.

> The plugin is designed to coexist with the unmodified
> [`XTextureExtractor`](https://github.com/waynepiekarski/XTextureExtractor)
> plugin in the same install. Different TCP port, different command
> namespace, different `XTE-DCDU:` log prefix.

## Parts to get

To build a physical DCDU module you will need:

- **Display board** — Guition **JC3248W535C** all-in-one ESP32-S3 with
  3.5" 320×480 capacitive touch display (AXS15231B QSPI panel). This is
  the only board the firmware in `firmware/` targets.
- **Front frame / bezel** — print [`3D Print Files/CDCU_Main_Panel.stl`](3D%20Print%20Files/CDCU_Main_Panel.stl)
  from this repo. It is a modification of James Bennet's original DCDU
  front frame, adjusted to fit the JC3248W535C and the eight push-buttons
  on header P2. See [`3D Print Files/README.md`](3D%20Print%20Files/README.md)
  for printing notes and attribution.
- **Everything else** (rear shell, button caps, light plate, wiring,
  hardware, full parts list) — from James Bennet's original DCDU post in
  the *A320 Cockpit Builders* Facebook group:
  <https://www.facebook.com/groups/817102292436795/permalink/2156310058516005/>

Only the modified front frame is redistributed here; please respect
James' original sharing terms for the rest of the parts.

## Author

Created and maintained by **David Rowlandson**.

## License

Copyright (C) 2026 David Rowlandson.

Licensed under the **GNU General Public License v3.0 or later**
(GPL-3.0-or-later). The full license text is in [`LICENSE`](LICENSE)
(also provided as [`COPYING`](COPYING)); see [`COPYRIGHT`](COPYRIGHT)
for the project notice and third-party attribution.

The texture-finding heuristic in `plugin/src/texture_finder.{cpp,hpp}` is
adapted from `waynepiekarski/XTextureExtractor` (Copyright (C) Wayne
Piekarski, also GPLv3) — see file header for attribution.

Firmware third-party credits and license notes:

- Captive Wi-Fi portal uses `tzapu/WiFiManager` (MIT).
- Display/graphics stack uses `lovyan03/LovyanGFX` (MIT AND BSD-2-Clause)
  for the generic SPI path and `moononournation/Arduino_GFX` for the
  JC3248W535 QSPI path.
- JPEG decode uses `bodmer/TJpg_Decoder`.

These components remain under their own upstream licenses; this repository's
top-level license remains GPL-3.0-or-later for XTE-DCDU project code.

---

## 1. Building the plugin

### Prerequisites

- A C++17 toolchain.
- CMake >= 3.16.
- The X-Plane SDK (CHeaders + per-OS libraries) unpacked at
  `plugin/XPLM/` such that `plugin/XPLM/CHeaders/XPLM/XPLMDefs.h` exists.
  See `plugin/XPLM/README.md`.
- `xxhash.h` from https://github.com/Cyan4973/xxHash placed at
  `plugin/third_party/xxhash/xxhash.h`. See `plugin/third_party/xxhash/README.md`.
- libjpeg-turbo is fetched automatically by CMake `FetchContent`.

### Windows (Visual Studio 2022, x64)

```powershell
cd plugin
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `plugin/build/Release/win.xpl`.

### Linux (GCC, x86_64)

```bash
cd plugin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output: `plugin/build/lin.xpl`.

### macOS (Xcode / Clang, universal optional)

```bash
cd plugin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output: `plugin/build/mac.xpl` (as a bundle). Codesign before distribution.

### Install layout

Copy into your X-Plane install:

```
X-Plane 12/Resources/plugins/Plugin-XTE-DCDU/
├── 64/
│   ├── win.xpl
│   ├── lin.xpl
│   └── mac.xpl
└── xte-dcdu.cfg            (copied from xte-dcdu.cfg.example, edited)
```

`cmake --install build --prefix <X-Plane 12>/Resources/plugins` will do the
same.

---

## 2. Building / flashing the firmware

The firmware targets the **Guition JC3248W535C** all-in-one board by
default: ESP32-S3-N16R8 + AXS15231B 3.5" 320×480 QSPI capacitive touch
LCD + 8-pin button header (P2). No external display wiring is required —
just USB-C for power/programming and (optionally) momentary buttons on P2.

### Prerequisites

- [PlatformIO Core](https://platformio.org/install/cli) (or the VS Code
  extension).
- A Guition **JC3248W535C** board and a USB-C cable.
- *(Optional)* Up to 8 momentary push-buttons wired to the P2 header (see
  pinout below). Buttons are not required for the display to work.

### Configure WiFi

On first boot (or whenever the saved network cannot be reached) the firmware starts an
open access point named **`XTE-DCDU-Setup`** and serves a captive portal:

1. On your phone or laptop, join the Wi-Fi network **`XTE-DCDU-Setup`**.
2. The captive portal should open automatically; if not, browse to
   `http://192.168.4.1/`.
3. Pick your home SSID, enter the passphrase, and press **Save**.
4. The ESP32 stores the credentials in NVS and reboots into normal station
   mode. The portal will not appear again unless the network becomes
   unreachable.

To force the portal on a device that already has saved credentials, **hold
BTN0** (GPIO 5 on the JC3248W535C P2 header) at power-on for at least 3
seconds. The display shows the setup screen while the portal is active.

The captive portal is powered by
[`tzapu/WiFiManager`](https://github.com/tzapu/WiFiManager) (MIT).

#### Optional: OTA upload password

`firmware/credentials.ini` is the only build-time secret file now (git-
ignored). It only needs to exist if you want to password-protect OTA uploads:

```ini
[credentials]
ota_password = your_ota_password   ; leave blank (default) to disable OTA auth
```

Copy `firmware/credentials.ini.example` to `firmware/credentials.ini` and
fill in the password. If the file is absent, the empty default in
`platformio.ini` is used and OTA auth is disabled.

### Build & flash

The default environment is `jc3248w535`, so no `-e` is needed:

```bash
cd firmware
pio run -t upload          # USB-C flash
pio device monitor         # 115200 baud
```

Once running on the LAN you can update over the air:

```bash
pio run -e jc3248w535_ota -t upload
```

On boot the display shows the assigned IP address and listening port. Set
the plugin's `esp32_host` / `esp32_port` to match.

### JC3248W535C wiring

**Display + ESP32-S3 + PSRAM are all on-board** — nothing to wire there.
The AXS15231B panel is driven over an internal QSPI bus that is fixed in
hardware; the firmware configures it automatically when built for this
board (`-DBOARD_JC3248W535`, set by the `jc3248w535` env).

Power & programming:

| Connector | Use                                               |
|-----------|---------------------------------------------------|
| USB-C     | 5 V power + serial console + flashing (CDC/JTAG). |

**P2 header — 8 user buttons (optional).** Each button is a momentary
SPST switch connected between the listed GPIO and GND. The firmware
enables internal pull-ups and reads them active-low, so no external
resistors are required. Pins are chosen to avoid the QSPI display bus.

| P2 pin | ESP32-S3 GPIO | Default action sent to plugin |
|--------|---------------|-------------------------------|
| BTN0   | GPIO 5        | `xtedcdu/btn/0`               |
| BTN1   | GPIO 6        | `xtedcdu/btn/1`               |
| BTN2   | GPIO 7        | `xtedcdu/btn/2`               |
| BTN3   | GPIO 15       | `xtedcdu/btn/3`               |
| BTN4   | GPIO 16       | `xtedcdu/btn/4`               |
| BTN5   | GPIO 46       | `xtedcdu/btn/5`               |
| BTN6   | GPIO 9        | `xtedcdu/btn/6`               |
| BTN7   | GPIO 14       | `xtedcdu/btn/7`               |
| GND    | GND           | Common return for all buttons |

Wiring a button is just `GPIO ────[ button ]──── GND`. Pins are
overridable at build time via `-DBTN_PIN_N=xx` flags in `platformio.ini`.

### Other ESP32-S3 boards

A generic `esp32s3` environment is also provided for an
ESP32-S3-DevKitC-1 driving an external ILI9341 SPI TFT, kept around for
development. Build it with `pio run -e esp32s3 -t upload`. Wiring is in
[`firmware/include/config.h`](firmware/include/config.h) (TFT_CS=10,
TFT_DC=11, TFT_RST=14, TFT_MOSI=13, TFT_SCK=12, TFT_MISO=9, TFT_BL
optional). The JC3248W535C path is the supported one.

---

## 3. Enclosure / 3D-printed parts

The [`3D Print Files/`](3D%20Print%20Files/) folder contains the printable
front frame for the DCDU module:

- **`CDCU_Main_Panel.stl`** — front frame / bezel sized for the
  JC3248W535C display and the 8 buttons on its P2 header.

This frame is a **modification of James Bennet's original DCDU front
frame design**, shared in the *A320 Cockpit Builders* Facebook group.
All credit for the original geometry goes to James; this fork only
adjusts the display cut-out, button positions and mounting bosses for
the JC3248W535C.

The original post contains the full parts list and the rest of the STLs
you will need for a complete DCDU module (rear shell, button caps, light
plate, etc.):

- <https://www.facebook.com/groups/817102292436795/permalink/2156310058516005/>

See [`3D Print Files/README.md`](3D%20Print%20Files/README.md) for print
settings and attribution detail.

---

## 4. Wire format

All multi-byte fields **little-endian**.

```
Frame:
  bytes  0..3   magic       = 0x44434455  ('D','C','D','U')   uint32 LE
  byte   4      version     = 0x01                            uint8
  byte   5      type        = 0x01 jpeg | 0x02 heartbeat      uint8
                            | 0x03 button | 0x04 raw RGB565
  bytes  6..9   seq                                           uint32 LE  (monotonic)
  bytes 10..11  width                                         uint16 LE  (image only)
  bytes 12..13  height                                        uint16 LE  (image only)
  bytes 14..17  payload_len                                   uint32 LE  (payload bytes; 0 for HB)
  bytes 18..    payload
```

- **0x01 JPEG**: payload is a JPEG bitstream (`FF D8 FF ...`),
  `payload_len` bytes long.
- **0x04 raw RGB565** *(default)*: payload is `width * height * 2`
  bytes of little-endian RGB565, row-major top-to-bottom. Lossless,
  no decode on the device. Toggle off with `raw_rgb565 = false` in
  `xte-dcdu.cfg` to fall back to JPEG.
- **0x02 heartbeat**: `width=height=payload_len=0`, total 18 bytes.
  Sent every `heartbeat_ms` if no image frame has gone out in that
  interval.
- **0x03 button**: sent device → plugin when a P2 button changes
  state. `payload_len = 0`; `width = button_index`, `height = state`
  (`1 = pressed`, `0 = released`).

Quick wire test on Linux/macOS without an ESP32:

```bash
nc -l 4711 | xxd | head -50
```

The first frame after aircraft load should show `44 43 44 55 01 ...` followed
by a JPEG starting with `FF D8 FF`.

---

## 5. Plugin commands and datarefs

### Commands

- `xtedcdu/reload_config` — reload `xte-dcdu.cfg`.
- `xtedcdu/reconnect`     — drop and re-establish the TCP link.
- `xtedcdu/dump_frame`    — write the next JPEG frame to
  `xte-dcdu-dump.jpg` next to the plugin (handy for offline inspection).

### Read-only int datarefs

- `xtedcdu/status/texture_id`              — currently locked panel texture id, or -1
- `xtedcdu/status/connected`               — 0/1 link state
- `xtedcdu/status/frames_sent`             — counter
- `xtedcdu/status/frames_skipped_nochange` — counter (incl. rate-limit drops)
- `xtedcdu/status/last_send_error`         — errno of last failed send, 0 if clean

---

## 6. Troubleshooting / log line reference

Every plugin log line is prefixed `XTE-DCDU: `. Notable messages:

| Message | Meaning |
|---|---|
| `XPluginStart vN.N.N` | plugin loaded |
| `config: file not found at '...'` | first run; default config is being written |
| `config: loaded from '...'` | config parsed OK |
| `config: line N: missing '=' in '...'` | malformed line ignored |
| `texture_finder: panel png at '...'` | aircraft panel atlas found on disk |
| `texture_finder: no NxN texture found in 1..M` | scan saw no candidate texture; will retry next minute or on aircraft load |
| `locked onto texture id N` | live readback armed |
| `readback: initialised fbo=N, X PBOs, crop WxH` | GL resources allocated |
| `readback: glMapBufferRange returned null` | GPU dropped a readback; usually transient |
| `tcp: connected to host:port` | client mode link up |
| `tcp: listening on host:port` | server mode listener up |
| `tcp: accepted client` | server mode link up |
| `tcp: connect(...) failed` | retrying with backoff |
| `tcp: send failed errno=N, dropping connection` | will reconnect |
| `worker: started` / `worker: stopped` | worker thread lifecycle |
| `dump: wrote N bytes to ...` | `xtedcdu/dump_frame` succeeded |

If the FPS counter changes when the plugin is enabled, check
`sim/operation/misc/frame_rate_period`. If you see a regression, set
`pbo_count = 1` in the config to confirm it's the async readback path
(should make things worse, not better — that's the "control" run).

---

## 7. Repository layout

```
xte-dcdu/
├── plugin/
│   ├── CMakeLists.txt
│   ├── src/                  C++ sources
│   ├── third_party/xxhash/   xxhash.h goes here
│   ├── XPLM/                 X-Plane SDK goes here
│   └── xte-dcdu.cfg.example
├── firmware/
│   ├── platformio.ini
│   ├── credentials.ini       (git-ignored; see credentials.ini.example)
│   ├── include/config.h
│   └── src/main.cpp
├── 3D Print Files/
│   ├── CDCU_Main_Panel.stl   modified front frame for JC3248W535C
│   └── README.md             attribution + print notes
├── LICENSE                   GPL-3.0 full text
├── COPYING                   GPL-3.0 full text (alternate filename)
├── COPYRIGHT                 project notice + third-party attribution
├── PRIVACY                   privacy policy
└── README.md
```

## 8. Known limitations / future work

- v1.0 supports a single `[aircraft]` section. Multi-aircraft via section
  suffixes (`[aircraft.a319]`, `[window.a319]`, …) is reserved for v1.1.
- Texture finder matches by dimensions only. The XTE "red dot at (0,0)"
  validation pass is *not* yet implemented; if dimensions collide on your
  install (rare), bump `start_texture_id` past the false positive.
- Firmware status overlay (seq / FPS / link) is not yet drawn.
- Firmware OTA is enabled (`xte-dcdu` hostname). Auth is disabled by default;
  set `ota_password` in `credentials.ini` to enable it. Either way, keep the
  device on a trusted LAN.
