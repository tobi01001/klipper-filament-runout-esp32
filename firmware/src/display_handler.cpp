#include "display_handler.h"

#ifdef ENABLE_OLED

#include <U8g2lib.h>
#include <Wire.h>
#include <stdio.h>

#include "config.h"
#include "fault_detector.h"  // state_name()

// ─── Module state ─────────────────────────────────────────────────────────────
// Full-buffer SSD1306 128×64 on hardware I²C.  The constructor takes the I²C
// address as the third argument (U8X8_PIN_NONE disables RST pin).
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_display(
    U8G2_R0,
    /*reset=*/U8X8_PIN_NONE,
    /*scl=*/OLED_SCL_PIN,
    /*sda=*/OLED_SDA_PIN);

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorStatus     *s_status       = nullptr;
static SensorConfig     *s_config       = nullptr;

static bool s_initialized   = false;
static bool s_power_save_on = false;  // tracks display power state

// ─── Internal helpers ─────────────────────────────────────────────────────────

// Draw a horizontal separator line.
static void draw_separator(uint8_t y) {
    s_display.drawHLine(0, y, OLED_WIDTH);
}

// Draw the state title bar.  On FAULT, the bar alternates between inverted
// (white background / black text) and normal to produce a visible 1 Hz blink.
static void draw_title(const char *label, bool fault, bool blink_phase) {
    const uint8_t bar_h = 16;

    if (fault && blink_phase) {
        // White box with black text
        s_display.setDrawColor(1);
        s_display.drawBox(0, 0, OLED_WIDTH, bar_h);
        s_display.setFont(u8g2_font_8x13B_tf);
        s_display.setFontMode(1);   // transparent so box shows through
        s_display.setDrawColor(0);  // black text on white box
        s_display.drawStr(2, 12, label);
        s_display.setFontMode(0);
        s_display.setDrawColor(1);
    } else {
        s_display.setFont(u8g2_font_8x13B_tf);
        s_display.setDrawColor(1);
        s_display.drawStr(2, 12, label);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void display_init(SemaphoreHandle_t status_mutex,
                  SemaphoreHandle_t config_mutex,
                  SensorStatus     *status,
                  SensorConfig     *config) {
    s_status_mutex = status_mutex;
    s_config_mutex = config_mutex;
    s_status       = status;
    s_config       = config;

    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

    s_display.setI2CAddress(OLED_I2C_ADDR << 1);  // U8g2 expects 8-bit address
    if (!s_display.begin()) {
        Serial.println("[OLED] WARNING: display not found at address 0x"
                       + String(OLED_I2C_ADDR, HEX));
        return;
    }

    s_initialized = true;
    Serial.println("[OLED] SSD1306 initialised (" +
                   String(OLED_WIDTH) + "x" + String(OLED_HEIGHT) +
                   ") at I2C 0x" + String(OLED_I2C_ADDR, HEX));

    // ── Boot splash ───────────────────────────────────────────────────────────
    s_display.clearBuffer();
    s_display.setFont(u8g2_font_8x13B_tf);
    s_display.setDrawColor(1);
    s_display.drawStr(4, 22, "Filament Sensor");
    s_display.setFont(u8g2_font_6x10_tf);
    s_display.drawStr(20, 38, "ESP32 / Klipper");
    s_display.drawStr(32, 52, "Starting...");
    s_display.sendBuffer();
}

void display_update() {
    if (!s_initialized) {
        return;
    }

    // ── Read display_enabled from config ──────────────────────────────────────
    bool enabled = true;
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        enabled = s_config->display_enabled;
        xSemaphoreGive(s_config_mutex);
    }

    if (!enabled) {
        if (!s_power_save_on) {
            s_display.setPowerSave(1);
            s_power_save_on = true;
        }
        return;
    }

    if (s_power_save_on) {
        s_display.setPowerSave(0);
        s_power_save_on = false;
    }

    // ── Snapshot status (brief mutex hold) ───────────────────────────────────
    SensorStatus snap{};
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snap = *s_status;
        xSemaphoreGive(s_status_mutex);
    }

    const bool is_fault = (snap.state == SystemState::FAULT);

    // Blink phase: toggle every display_update() call (≈ 1 Hz at 500 ms period)
    static bool blink = false;
    blink = !blink;

    // ── Render ────────────────────────────────────────────────────────────────
    s_display.clearBuffer();

    // Title bar (state name)
    draw_title(state_name(snap.state), is_fault, blink);

    // Separator below title
    draw_separator(16);

    // Data lines use 6×10 font (6 px wide, 10 px tall; baseline drawn at y)
    s_display.setFont(u8g2_font_6x10_tf);
    s_display.setDrawColor(1);

    char buf[22];  // 128 / 6 = 21 chars max per line

    // Row 1 – encoder velocity
    snprintf(buf, sizeof(buf), "Enc:%6.2f mm/s",
             static_cast<double>(snap.encoder.velocity_mm_s));
    s_display.drawStr(0, 28, buf);

    // Row 2 – extruder velocity (Moonraker)
    snprintf(buf, sizeof(buf), "Ext:%6.2f mm/s",
             static_cast<double>(snap.extruder_vel));
    s_display.drawStr(0, 39, buf);

    // Row 3 – tick count + direction symbol
    const char dir_sym = (snap.encoder.direction > 0) ? '>' :
                         (snap.encoder.direction < 0) ? '<' : '=';
    // Fit 21 chars: "Tck:XXXXXXXX D" where D is dir symbol
    snprintf(buf, sizeof(buf), "Tck:%8ld %c",
             static_cast<long>(snap.encoder.tick_count), dir_sym);
    s_display.drawStr(0, 50, buf);

    // Row 4 – IP address (or offline notice)
    if (snap.wifi_connected) {
        snprintf(buf, sizeof(buf), "%s", snap.ip_address);
    } else {
        snprintf(buf, sizeof(buf), "WiFi offline");
    }
    s_display.drawStr(0, 61, buf);

    s_display.sendBuffer();
}

#endif // ENABLE_OLED
