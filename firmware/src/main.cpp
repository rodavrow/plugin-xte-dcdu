// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
//
// XTE-DCDU ESP32-S3 firmware: listens on TCP, receives JPEG-encoded DCDU
// frames from the X-Plane plugin, and renders them on an ILI9341 SPI TFT.
// Also polls 11 push-buttons and sends button-event packets back to the plugin
// over the same TCP connection.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "config.h"

// ============================================================================
// Display setup
//   Default      : ILI9341 over SPI driven by LovyanGFX.
//   JC3248W535   : AXS15231B QSPI panel driven by Arduino_GFX.
//
// Both branches expose a `tft` global that supports the same subset of calls
// used in this file (fillScreen, fillRect, setTextColor, setTextSize,
// setCursor, print*, width, height, setRotation, startWrite, endWrite),
// plus the helpers `display_begin()` and `push_image()` that bridge the two
// libraries' divergent names (init vs begin, pushImage vs draw16bitRGBBitmap).
// ============================================================================
#if defined(BOARD_JC3248W535)

#include <Arduino_GFX_Library.h>

static Arduino_DataBus* g_bus = new Arduino_ESP32QSPI(
    45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);

// Underlying AXS15231B panel: native 320(W) x 480(H) portrait.
static Arduino_GFX* g_panel = new Arduino_AXS15231B(
    g_bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */,
    320 /* width */, 480 /* height */,
    0 /* col_off1 */, 0 /* row_off1 */, 0 /* col_off2 */, 0 /* row_off2 */);

// The AXS15231B over QSPI does NOT cope with arbitrary partial CASET/RASET
// writes (the vendor ESP-IDF driver intentionally skips RASET in QSPI mode
// and only issues RAMWR/RAMWRC against full-screen windows). To stay on the
// supported path, route every draw through a PSRAM canvas and push the whole
// frame in one shot with flush(). Memory cost: 320*480*2 = ~300 KB PSRAM
// (board has 8 MB).
static Arduino_Canvas* g_canvas = new Arduino_Canvas(
    320 /* width */, 480 /* height */, g_panel, 0 /* output_x */, 0 /* output_y */, 0 /* rotation */);

static Arduino_GFX& tft = *g_canvas;

// Arduino_GFX names its 16-bit colors RGB565_*; alias to the TFT_* names below.
#define TFT_BLACK   RGB565_BLACK
#define TFT_RED     RGB565_RED
#define TFT_GREEN   RGB565_GREEN

static inline void display_begin() {
    // begin() on the canvas also begins the underlying panel and allocates
    // the framebuffer in PSRAM.
    tft.begin();
    pinMode(1, OUTPUT);
    digitalWrite(1, HIGH);  // backlight on (BL = GPIO1, active HIGH)
}

static inline void push_image(int x, int y, int w, int h, uint16_t* buf) {
    tft.draw16bitRGBBitmap(x, y, buf, w, h);
}

// Canvas mode: nothing is visible until we flush the framebuffer to the panel.
static inline void display_flush() {
    g_canvas->flush();
}

#else  // ------ default ILI9341 SPI via LovyanGFX -----------------------

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI_HOST_USE;
            cfg.spi_mode    = 0;
            cfg.freq_write  = SPI_FREQ_WRITE;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = TFT_SCK;
            cfg.pin_mosi    = TFT_MOSI;
            cfg.pin_miso    = TFT_MISO;
            cfg.pin_dc      = TFT_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs    = TFT_CS;
            cfg.pin_rst   = TFT_RST;
            cfg.pin_busy  = -1;
            cfg.panel_width   = DISPLAY_HEIGHT; // before rotation
            cfg.panel_height  = DISPLAY_WIDTH;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable        = false;
            cfg.invert          = false;
            cfg.rgb_order       = false;
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }
        if (TFT_BL >= 0) {
            auto cfg = _light.config();
            cfg.pin_bl      = TFT_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX tft;

static inline void display_begin() { tft.init(); }

static inline void push_image(int x, int y, int w, int h, uint16_t* buf) {
    tft.pushImage(x, y, w, h, buf);
}

// LovyanGFX writes straight to the panel; no framebuffer to flush.
static inline void display_flush() {}

#endif  // BOARD_JC3248W535

#include <TJpg_Decoder.h>
static WiFiServer  server(LISTEN_PORT);
static WiFiClient  client;

// Mutex that guards client.write() calls from the button task and
// client assignment from the main loop. Held only for very brief operations.
static SemaphoreHandle_t g_client_mutex = nullptr;

// ============================================================================
// Button input: 11 momentary push-buttons, active-low, internal pull-up.
// Polled from a dedicated FreeRTOS task (core 0) so the main TCP receive
// loop (core 1) is never blocked during button debounce.
// ============================================================================

// Pin table is built from BTN_PIN_0 .. BTN_PIN_(BTN_COUNT-1).
// BOARD_JC3248W535 sets BTN_COUNT=8 so the high indices aren't referenced.
static const uint8_t k_btn_pins[BTN_COUNT] = {
    BTN_PIN_0, BTN_PIN_1, BTN_PIN_2, BTN_PIN_3,
#if BTN_COUNT > 4
    BTN_PIN_4,
#endif
#if BTN_COUNT > 5
    BTN_PIN_5,
#endif
#if BTN_COUNT > 6
    BTN_PIN_6,
#endif
#if BTN_COUNT > 7
    BTN_PIN_7,
#endif
#if BTN_COUNT > 8
    BTN_PIN_8,
#endif
#if BTN_COUNT > 9
    BTN_PIN_9,
#endif
#if BTN_COUNT > 10
    BTN_PIN_10,
#endif
};

struct BtnState {
    uint8_t  last_sent;    // 0 = released, 1 = pressed, 0xFF = unknown
    uint8_t  raw;          // most recent digitalRead result (0 = low = pressed)
    uint32_t last_change;  // millis() when raw last changed
};
static BtnState g_btn[BTN_COUNT];

static uint32_t g_btn_seq = 0;

// Send an 18-byte button-event packet on the current client connection.
// Protocol: same header format as image/heartbeat frames.
//   magic    = 0x55444344 (LE: 44 43 44 55)
//   version  = 0x01
//   type     = 0x03 (button event)
//   seq      = incrementing per-device counter
//   width    = button_id  (0 – BTN_COUNT-1)
//   height   = state      (1 = pressed, 0 = released)
//   payload_len = 0
static void send_button_event(uint8_t btn_id, uint8_t state) {
    if (xSemaphoreTake(g_client_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    if (!client || !client.connected()) {
        xSemaphoreGive(g_client_mutex);
        return;
    }
    uint32_t seq = ++g_btn_seq;
    uint8_t  hdr[18];
    hdr[0]  = 0x44; hdr[1]  = 0x43; hdr[2]  = 0x44; hdr[3]  = 0x55; // magic LE
    hdr[4]  = 0x01;  // version
    hdr[5]  = 0x03;  // type: button event
    hdr[6]  = (uint8_t)(seq);        hdr[7]  = (uint8_t)(seq >> 8);
    hdr[8]  = (uint8_t)(seq >> 16);  hdr[9]  = (uint8_t)(seq >> 24);
    hdr[10] = btn_id; hdr[11] = 0;   // width = button_id
    hdr[12] = state;  hdr[13] = 0;   // height = state
    hdr[14] = 0; hdr[15] = 0; hdr[16] = 0; hdr[17] = 0; // payload_len = 0
    client.write(hdr, sizeof(hdr));
    xSemaphoreGive(g_client_mutex);
    Serial.printf("button %u: %s\n", btn_id, state ? "pressed" : "released");
}

// FreeRTOS task: polls all buttons and sends events on state change.
// Pinned to core 0; the main TCP loop runs on core 1 (Arduino default).
static void button_task(void* /*param*/) {
    for (;;) {
        uint32_t now = millis();
        for (int i = 0; i < BTN_COUNT; ++i) {
            uint8_t raw = (digitalRead(k_btn_pins[i]) == LOW) ? 1u : 0u;
            if (raw != g_btn[i].raw) {
                g_btn[i].raw         = raw;
                g_btn[i].last_change = now;
            }
            // Debounce: state must be stable for BTN_DEBOUNCE_MS.
            if ((now - g_btn[i].last_change) >= BTN_DEBOUNCE_MS &&
                raw != g_btn[i].last_sent) {
                g_btn[i].last_sent = raw;
                send_button_event((uint8_t)i, raw);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

// ----------------------------------------------------------------------------
// PSRAM buffers
// ----------------------------------------------------------------------------
static uint8_t*  jpeg_buf       = nullptr;
static size_t   jpeg_cap        = 0;

// Decoded full-image RGB565 framebuffer (allocated lazily once jw/jh known).
static uint16_t* decoded_fb     = nullptr;
static int       decoded_w      = 0;
static int       decoded_h      = 0;
static size_t    decoded_cap    = 0;

// One row of the upscaled output (DISPLAY_WIDTH pixels).
static uint16_t  row_buf[DISPLAY_WIDTH];

static uint32_t  last_seen_ms   = 0;
static uint32_t  last_seq       = 0;
static uint32_t  frames_ok      = 0;
static bool      link_lost_drawn = false;

// ----------------------------------------------------------------------------
// TJpg_Decoder callback: stash MCU tile into the decoded framebuffer.
// TJpgDec gives us 16x16 tiles in RGB565 with byte order we configured below.
// ----------------------------------------------------------------------------
static bool tjpg_to_fb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!decoded_fb) return false;
    if (x >= decoded_w || y >= decoded_h) return true;
    int copy_w = min<int>(w, decoded_w - x);
    int copy_h = min<int>(h, decoded_h - y);
    for (int row = 0; row < copy_h; ++row) {
        uint16_t* dst = decoded_fb + (size_t)(y + row) * decoded_w + x;
        const uint16_t* src = bitmap + (size_t)row * w;
        memcpy(dst, src, (size_t)copy_w * sizeof(uint16_t));
    }
    return true;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static bool ensure_decoded_capacity(int w, int h) {
    size_t need = (size_t)w * h * sizeof(uint16_t);
    if (decoded_fb && decoded_cap >= need) {
        decoded_w = w; decoded_h = h;
        return true;
    }
    if (decoded_fb) { free(decoded_fb); decoded_fb = nullptr; decoded_cap = 0; }
    decoded_fb = (uint16_t*) ps_malloc(need);
    if (!decoded_fb) {
        Serial.printf("ps_malloc(%u) failed\n", (unsigned)need);
        return false;
    }
    decoded_cap = need;
    decoded_w = w; decoded_h = h;
    return true;
}

static bool read_exact(WiFiClient& c, uint8_t* buf, size_t n, uint32_t timeout_ms) {
    uint32_t deadline = millis() + timeout_ms;
    size_t got = 0;
    while (got < n) {
        if (!c.connected()) return false;
        if ((int32_t)(millis() - deadline) > 0) return false;
        int avail = c.available();
        if (avail > 0) {
            int r = c.read(buf + got, n - got);
            if (r > 0) got += (size_t)r;
        } else {
            delay(1);
        }
    }
    return true;
}

static void draw_status(const char* msg, uint16_t color) {
    tft.fillRect(0, tft.height() - 18, tft.width(), 18, TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(6, tft.height() - 16);
    tft.print(msg);
    display_flush();
}

static void show_link_lost() {
    if (link_lost_drawn) return;
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(20, tft.height() / 2 - 12);
    tft.print("LINK LOST");
    tft.setTextSize(2);
    tft.setCursor(20, tft.height() / 2 + 24);
    tft.printf("IP %s", WiFi.localIP().toString().c_str());
    display_flush();
    link_lost_drawn = true;
}

static void render_decoded() {
    // Compute target rect.
    int draw_w, draw_h, draw_x, draw_y;
#if DCDU_FIT_MODE_HEIGHT
    float scale = (float)tft.height() / (float)decoded_h;
    draw_w = (int)(decoded_w * scale + 0.5f);
    draw_h = tft.height();
    if (draw_w > tft.width()) draw_w = tft.width();
    draw_x = (tft.width() - draw_w) / 2;
    draw_y = 0;
#else
    float scale = (float)tft.width() / (float)decoded_w;
    draw_w = tft.width();
    draw_h = (int)(decoded_h * scale + 0.5f);
    if (draw_h > tft.height()) draw_h = tft.height();
    draw_x = 0;
    draw_y = (tft.height() - draw_h) / 2;
#endif

    // Letterbox borders.
    if (draw_x > 0) {
        tft.fillRect(0, 0, draw_x, tft.height(), TFT_BLACK);
        tft.fillRect(draw_x + draw_w, 0,
                     tft.width() - draw_x - draw_w, tft.height(), TFT_BLACK);
    }
    if (draw_y > 0) {
        tft.fillRect(0, 0, tft.width(), draw_y, TFT_BLACK);
        tft.fillRect(0, draw_y + draw_h,
                     tft.width(), tft.height() - draw_y - draw_h, TFT_BLACK);
    }

    // Nearest-neighbour upscale row by row, push as a single horizontal strip.
    tft.startWrite();
    for (int dy = 0; dy < draw_h; ++dy) {
        int sy = (dy * decoded_h) / draw_h;
        if (sy >= decoded_h) sy = decoded_h - 1;
        const uint16_t* src_row = decoded_fb + (size_t)sy * decoded_w;
        for (int dx = 0; dx < draw_w; ++dx) {
            int sx = (dx * decoded_w) / draw_w;
            row_buf[dx] = src_row[sx];
        }
        push_image(draw_x, draw_y + dy, draw_w, 1, row_buf);
    }
    tft.endWrite();
    display_flush();
}

// ============================================================================
// Wi-Fi provisioning via captive portal (tzapu/WiFiManager).
//
// Behaviour:
//   * On boot the firmware tries to associate with the SSID/PSK stored in
//     NVS by a previous successful provisioning.
//   * If nothing is stored, or the saved network can't be reached within
//     WIFI_CONNECT_TIMEOUT_MS, an open AP "XTE-DCDU-Setup" comes up with a
//     captive portal at http://192.168.4.1/. Connect from a phone/laptop,
//     pick your SSID, enter the PSK, hit Save. The ESP stores it in NVS
//     and reboots into normal STA mode.
//   * If the saved network later disappears for more than ~60 s, the
//     firmware reboots so the same flow re-runs (portal pops up again).
//   * Holding the reset button (BTN_PIN_<BTN_RESET_INDEX>) at boot for
//     BTN_RESET_HOLD_MS ms wipes the stored credentials and forces the
//     portal even if a valid network is saved.
// ============================================================================
static bool check_reset_button_held() {
#if BTN_RESET_INDEX < 0
    return false;
#else
    if (BTN_RESET_INDEX >= BTN_COUNT) return false;
    uint8_t pin = k_btn_pins[BTN_RESET_INDEX];
    pinMode(pin, INPUT_PULLUP);
    // Pin needs a moment to settle once pull-up is enabled.
    delay(5);
    if (digitalRead(pin) != LOW) return false;
    Serial.println("reset button held at boot - timing...");
    uint32_t t0 = millis();
    while (digitalRead(pin) == LOW) {
        if (millis() - t0 >= BTN_RESET_HOLD_MS) return true;
        delay(10);
    }
    return false;
#endif
}

static void show_portal_screen(const char* ssid, const char* ip) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("Wi-Fi setup needed");
    tft.setCursor(10, 40);
    tft.printf("Join AP:\n  %s\n", ssid);
    tft.setCursor(10, 90);
    tft.printf("Open:\n  http://%s/\n", ip);
    tft.setCursor(10, 140);
    tft.setTextSize(1);
    tft.println("(captive portal should auto-open)");
    display_flush();
}

static void start_wifi_or_portal(bool force_portal) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname("xte-dcdu");

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    wm.setAPCallback([](WiFiManager* m) {
        IPAddress ip = WiFi.softAPIP();
        Serial.printf("config portal up: SSID=%s IP=%s\n",
                      m->getConfigPortalSSID().c_str(), ip.toString().c_str());
        show_portal_screen(m->getConfigPortalSSID().c_str(), ip.toString().c_str());
    });

    if (force_portal) {
        Serial.println("forcing config portal (reset button or no creds)");
        wm.resetSettings();
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 40);
    tft.println("Connecting Wi-Fi...");
    display_flush();

    const char* ap_psk = (WIFI_AP_PSK[0] == '\0') ? nullptr : WIFI_AP_PSK;
    bool ok = wm.autoConnect(WIFI_AP_SSID, ap_psk);
    if (!ok) {
        Serial.println("WiFiManager timed out - restarting");
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(10, 10);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setTextSize(2);
        tft.println("Wi-Fi setup timeout\nRebooting...");
        display_flush();
        delay(1500);
        ESP.restart();
    }
    Serial.printf("Wi-Fi connected: SSID=%s IP=%s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
}

// ============================================================================
// setup() / loop()
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(50);

    // Initialise button GPIO pins before starting the button task.
    for (int i = 0; i < BTN_COUNT; ++i) {
        pinMode(k_btn_pins[i], INPUT_PULLUP);
        g_btn[i].raw         = (digitalRead(k_btn_pins[i]) == LOW) ? 1u : 0u;
        g_btn[i].last_sent   = 0xFF;   // force a send on first stable read
        g_btn[i].last_change = 0;
    }

    g_client_mutex = xSemaphoreCreateMutex();

    display_begin();
    tft.setRotation(3);          // landscape 320x240, display mounted inverted
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("XTE-DCDU booting...");

    if (!psramFound()) {
        Serial.println("PSRAM NOT FOUND - check board_build.arduino.memory_type");
    } else {
        Serial.printf("PSRAM free: %u\n", (unsigned)ESP.getFreePsram());
    }

    jpeg_cap = JPEG_BUF_BYTES;
    jpeg_buf = (uint8_t*) ps_malloc(jpeg_cap);
    if (!jpeg_buf) {
        Serial.println("JPEG buf alloc failed; aborting");
        tft.println("PSRAM ALLOC FAIL");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // Capture-portal-driven Wi-Fi provisioning. Replaces compile-time
    // WIFI_SSID/WIFI_PSK. If the user is holding the reset button at boot,
    // wipe stored credentials and force the portal.
    bool force_portal = check_reset_button_held();
    start_wifi_or_portal(force_portal);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.printf("SSID: %s\n", WiFi.SSID().c_str());
    tft.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    tft.printf("Port: %d\n", LISTEN_PORT);
    display_flush();

    ArduinoOTA.setHostname("xte-dcdu");
#if defined(OTA_PASSWORD)
    if (OTA_PASSWORD[0] != '\0') {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }
#endif
    ArduinoOTA.begin();

    server.begin();
    server.setNoDelay(true);

    // Byte order in the decoded RGB565 buffer must match what the active
    // display driver expects when handed raw uint16_t pixels.
    //   - LovyanGFX (ILI9341 SPI): wants big-endian on the wire, so swap.
    //   - Arduino_GFX Canvas (AXS15231B QSPI): stores pixels in native
    //     little-endian uint16 and the panel driver handles wire framing;
    //     swapping here scrambles every non-symmetric color (beige -> yellow,
    //     anti-aliased lines -> rainbow noise).
#if defined(BOARD_JC3248W535)
    TJpgDec.setSwapBytes(false);
#else
    TJpgDec.setSwapBytes(true);
#endif
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tjpg_to_fb);

    // Start button-polling task on core 0 (stack 2 KB is ample).
    xTaskCreatePinnedToCore(
        button_task,  // task function
        "btn_poll",   // name
        2048,         // stack size in bytes
        nullptr,      // parameter
        1,            // priority
        nullptr,      // task handle (not needed)
        0             // core: 0 (main loop runs on core 1)
    );

    last_seen_ms = millis();
}

void loop() {
    ArduinoOTA.handle();

    // Wi-Fi watchdog: if the saved network is gone for too long, reboot.
    // On reboot, WiFiManager will retry once and then re-open the portal
    // automatically — that satisfies the "AP appears on connect failure"
    // requirement without us having to re-enter portal mode inline (which
    // would tear down the TCP server, mutexes, etc.).
    {
        static uint32_t wifi_lost_at = 0;
        if (WiFi.status() != WL_CONNECTED) {
            if (wifi_lost_at == 0) {
                wifi_lost_at = millis();
                Serial.println("Wi-Fi disconnected");
            } else if (millis() - wifi_lost_at > 60000) {
                Serial.println("Wi-Fi down >60s, restarting for re-provisioning");
                ESP.restart();
            }
        } else {
            wifi_lost_at = 0;
        }
    }

    // Accept (one) client.
    if (!client || !client.connected()) {
        xSemaphoreTake(g_client_mutex, portMAX_DELAY);
        if (client) client.stop();
        client = server.accept();
        xSemaphoreGive(g_client_mutex);
        if (client) {
            Serial.printf("client connected: %s\n", client.remoteIP().toString().c_str());
            client.setNoDelay(true);
            last_seen_ms = millis();
            link_lost_drawn = false;
            // NOTE: do NOT blank the screen here. The X-Plane plugin uses
            // change-detection and only sends a frame when DCDU pixels
            // differ from the previous hash; on reconnect it usually has
            // nothing new to send for a while. Blanking would leave us
            // staring at black until the sim happens to update the DCDU.
            // Instead, keep whatever was on screen (last frame or boot
            // text) and overlay a small status line so it's obvious the
            // link is up. The first real frame will overwrite this.
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setTextSize(1);
            tft.setCursor(2, 2);
            tft.print("link up - waiting for frame...  ");
            display_flush();
        } else {
            if (millis() - last_seen_ms > LINK_TIMEOUT_MS) {
                show_link_lost();
            }
            delay(20);
            return;
        }
    }

    // Read 18-byte header.
    uint8_t hdr[18];
    if (!read_exact(client, hdr, sizeof(hdr), 1500)) {
        if (millis() - last_seen_ms > LINK_TIMEOUT_MS) {
            Serial.println("link timeout, dropping");
            client.stop();
            show_link_lost();
        }
        return;
    }

    // Parse little-endian header.
    uint32_t magic = (uint32_t)hdr[0]
                   | ((uint32_t)hdr[1] << 8)
                   | ((uint32_t)hdr[2] << 16)
                   | ((uint32_t)hdr[3] << 24);
    // 0x44434455 'D','C','D','U' as LE bytes 44 43 44 55 reads back as 0x55444344.
    if (magic != 0x55444344u) {
        Serial.printf("bad magic 0x%08x, dropping\n", magic);
        client.stop();
        return;
    }
    uint8_t  version = hdr[4];
    uint8_t  type    = hdr[5];
    uint32_t seq     = (uint32_t)hdr[6]
                     | ((uint32_t)hdr[7] << 8)
                     | ((uint32_t)hdr[8] << 16)
                     | ((uint32_t)hdr[9] << 24);
    uint16_t w       = (uint16_t)hdr[10] | ((uint16_t)hdr[11] << 8);
    uint16_t h       = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
    uint32_t plen    = (uint32_t)hdr[14]
                     | ((uint32_t)hdr[15] << 8)
                     | ((uint32_t)hdr[16] << 16)
                     | ((uint32_t)hdr[17] << 24);

    if (version != 0x01) {
        Serial.printf("bad version %u, dropping\n", version);
        client.stop();
        return;
    }

    last_seen_ms = millis();
    last_seq = seq;
    link_lost_drawn = false;

    if (type == 0x02) {
        // Heartbeat — nothing more to read.
        return;
    }
    if (type == 0x04) {
        // Raw RGB565 little-endian: w*h*2 bytes, read straight into
        // decoded_fb (which is also native LE uint16 on this board) and
        // skip the JPEG decode entirely.
        if (w == 0 || h == 0 || plen != (uint32_t)w * h * 2) {
            Serial.printf("bad raw565 header w=%u h=%u plen=%u\n", w, h, plen);
            client.stop();
            return;
        }
        if (!ensure_decoded_capacity(w, h)) {
            // Drain the payload so we stay framed for the next header.
            uint8_t scratch[512];
            uint32_t left = plen;
            while (left > 0) {
                uint32_t chunk = left > sizeof(scratch) ? sizeof(scratch) : left;
                if (!read_exact(client, scratch, chunk, 2000)) { client.stop(); return; }
                left -= chunk;
            }
            return;
        }
        if (!read_exact(client, (uint8_t*)decoded_fb, plen, 3000)) {
            Serial.println("raw565 payload read timeout");
            client.stop();
            return;
        }
        render_decoded();
        frames_ok++;
        return;
    }
    if (type != 0x01) {
        // Unknown type — skip payload to stay framed.
        if (plen > 0 && plen <= jpeg_cap) {
            read_exact(client, jpeg_buf, plen, 2000);
        } else if (plen > 0) {
            client.stop();
        }
        return;
    }
    if (plen == 0 || plen > jpeg_cap || w == 0 || h == 0) {
        Serial.printf("bad image header w=%u h=%u plen=%u\n", w, h, plen);
        client.stop();
        return;
    }
    if (!read_exact(client, jpeg_buf, plen, 3000)) {
        Serial.println("payload read timeout");
        client.stop();
        return;
    }

    // Decode at 1:1 into PSRAM, then nearest-neighbour upscale to display.
    uint16_t jw = 0, jh = 0;
    if (TJpgDec.getJpgSize(&jw, &jh, jpeg_buf, plen) != JDR_OK || jw == 0 || jh == 0) {
        Serial.println("getJpgSize failed");
        return;
    }
    if (!ensure_decoded_capacity(jw, jh)) return;

    // Clear FB (in case decode skips edge MCUs).
    memset(decoded_fb, 0, (size_t)jw * jh * sizeof(uint16_t));

    if (TJpgDec.drawJpg(0, 0, jpeg_buf, plen) != JDR_OK) {
        Serial.println("drawJpg decode failed");
        return;
    }

    render_decoded();
    frames_ok++;
}
