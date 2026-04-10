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
    bool     sensor_enabled;         // Enable/disable fault triggering at runtime
    bool     display_enabled;        // Enable/disable the OLED display at runtime
    char     fault_gcode[64];        // GCODE sent to Moonraker via WebSocket on fault
#ifdef ENABLE_DHT
    bool     dht_enabled;            // Enable/disable DHT22 temperature+humidity readings
#endif
};

// ─── Live Status (shared read by web handler) ─────────────────────────────────
struct SensorStatus {
    SystemState state;
    EncoderData encoder;          // Latest encoder snapshot
    float       extruder_vel;     // Latest extruder velocity from Moonraker
    float       nozzle_temp;      // Current nozzle temperature (C)
    float       nozzle_target;    // Current nozzle target temperature (C)
    bool        fault_active;
    bool        wifi_connected;
    bool        moonraker_connected;   // WebSocket transport connected
    bool        moonraker_subscribed;  // Object subscription established
    bool        moonraker_stale;       // No recent extruder update
    char        klippy_state[16];      // Moonraker server.info klippy_state
    char        ip_address[16];   // Current station IP (dotted quad)
#ifdef ENABLE_DHT
    float       dht_temperature;  // Ambient temperature in °C (valid when dht_valid is true)
    float       dht_humidity;     // Relative humidity in %RH (valid when dht_valid is true)
    bool        dht_valid;        // true when the last DHT22 read succeeded
#endif
};

// ─── Calibration State Machine ───────────────────────────────────────────────────
enum class CalState : uint8_t {
    IDLE,       // No calibration in progress
    SENT,       // GCODE sent, waiting for encoder motion to start
    MOVING,     // Encoder moving, accumulating ticks
    SETTLING,   // Motion stopped, waiting for settle period
    DONE,       // Result computed; cal_factor available
    FAILED      // Timed out or encoder gave no ticks
};

struct CalibrationStatus {
    CalState state;
    float    result_cal_factor;  // computed mm/tick (valid when state == DONE)
    int32_t  measured_ticks;     // net ticks counted
    float    requested_mm;       // extrude distance requested
    char     error[32];          // error description (valid when state == FAILED)
};
