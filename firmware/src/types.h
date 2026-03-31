#pragma once

#include <stdint.h>
#include "config.h"

// ─── System State Machine ─────────────────────────────────────────────────────
enum class SystemState : uint8_t {
    INIT,       // Power-on, hardware initialisation
    WIFI_CONN,  // Attempting WiFi connection
    WIFI_FAIL,  // Connection failed, retrying with backoff
    READY,      // Connected, monitoring (extruder idle)
    IDLE,       // Connected, printer idle (extruder velocity == 0)
    PRINTING,   // Extruder active, filament motion expected
    FAULT       // Runout detected – GPIO 27 pulled LOW
};

// ─── Data Transferred Core 1 → Core 0 via Queue ──────────────────────────────
struct EncoderData {
    int32_t  tick_count;       // Total accumulated ticks since boot
    int32_t  tick_delta;       // Ticks since last 20 ms update
    int8_t   direction;        // +1 forward, -1 reverse, 0 stopped
    uint32_t timestamp_ms;     // millis() at time of snapshot
    float    velocity_mm_s;    // EMA-filtered filament velocity (mm/s)
    bool     btn_pressed;      // Encoder push-button active (debounced, active LOW)
};

// ─── Persistent Runtime Configuration (NVS) ──────────────────────────────────
struct SensorConfig {
    float    cal_factor;              // mm per encoder tick
    uint32_t timeout_ms;             // no-motion fault timeout (ms)
    float    min_ext_vel;            // extruder velocity threshold (mm/s)
    int32_t  motion_threshold;       // minimum |ticks| to register motion
    char     moonraker_ip[40];       // Moonraker host IP or hostname
    uint16_t moonraker_port;         // Moonraker port (default 7125)
    char     wifi_ssid[64];          // Station SSID
    char     wifi_pass[64];          // Station password
    char     ota_hostname[32];       // ArduinoOTA mDNS hostname (default: "filament-sensor")
    char     ota_password[32];       // ArduinoOTA push password (default: "ota1234")
};

// ─── Live Status (shared read by web handler) ─────────────────────────────────
struct SensorStatus {
    SystemState state;
    EncoderData encoder;          // Latest encoder snapshot
    float       extruder_vel;     // Latest extruder velocity from Moonraker
    bool        fault_active;
    bool        wifi_connected;
    char        ip_address[16];   // Current station IP (dotted quad)
};
