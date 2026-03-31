#include "display_handler.h"

#ifdef ENABLE_OLED

#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>
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
static bool s_ota_active    = false;  // true while an OTA update is running

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

    // Skip normal rendering while an OTA update is active so the progress
    // screen is not overwritten by the sensor data layout.
    if (s_ota_active) {
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

    char buf[24];  // 128 / 6 = 21 chars max per line + null + guard bytes

    // Row 1 – encoder velocity (clamped to ±9999.99 so format never exceeds buffer)
    const float enc_vel_disp = fminf(fmaxf(snap.encoder.velocity_mm_s, -9999.99f), 9999.99f);
    snprintf(buf, sizeof(buf), "Enc:%7.2f mm/s",
             static_cast<double>(enc_vel_disp));
    s_display.drawStr(0, 28, buf);

    // Row 2 – extruder velocity from Moonraker (clamped to ±9999.99)
    const float ext_vel_disp = fminf(fmaxf(snap.extruder_vel, -9999.99f), 9999.99f);
    snprintf(buf, sizeof(buf), "Ext:%7.2f mm/s",
             static_cast<double>(ext_vel_disp));
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

void display_set_ota_active(bool active) {
    s_ota_active = active;
}

void display_show_ota_progress(uint8_t percent) {
    if (!s_initialized) return;

    const uint8_t pct = (percent > 100u) ? 100u : percent;

    // Wake display if it was powered down.
    if (s_power_save_on) {
        s_display.setPowerSave(0);
        s_power_save_on = false;
    }

    s_display.clearBuffer();

    // Title bar
    s_display.setFont(u8g2_font_8x13B_tf);
    s_display.setDrawColor(1);
    s_display.drawStr(20, 12, "OTA Update");
    draw_separator(16);

    // Percentage label
    char buf[8];
    snprintf(buf, sizeof(buf), "%3u%%", pct);
    s_display.drawStr(44, 36, buf);

    // Progress bar – 120 px wide, 12 px tall, 4 px from each edge
    const uint8_t bar_x = 4;
    const uint8_t bar_y = 42;
    const uint8_t bar_w = OLED_WIDTH - 8;
    const uint8_t bar_h = 12;
    s_display.drawFrame(bar_x, bar_y, bar_w, bar_h);
    if (pct > 0u) {
        const uint8_t fill_w = (uint8_t)(bar_w * pct / 100u);
        s_display.drawBox(bar_x, bar_y, fill_w, bar_h);
    }

    s_display.sendBuffer();
}

void display_show_ota_reboot() {
    if (!s_initialized) return;

    // Wake display if it was powered down.
    if (s_power_save_on) {
        s_display.setPowerSave(0);
        s_power_save_on = false;
    }

    s_display.clearBuffer();
    s_display.setFont(u8g2_font_8x13B_tf);
    s_display.setDrawColor(1);
    s_display.drawStr(14, 24, "OTA Complete");
    s_display.setFont(u8g2_font_6x10_tf);
    s_display.drawStr(22, 44, "Rebooting...");
    s_display.sendBuffer();
}

#endif // ENABLE_OLED
