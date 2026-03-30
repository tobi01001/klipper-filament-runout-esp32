#pragma once

// ─── GPIO Pin Assignments ─────────────────────────────────────────────────────
// KY-040 rotary encoder module pinout:
//   CLK (Channel A) → PIN_ENCODER_CHA
//   DT  (Channel B) → PIN_ENCODER_CHB
//   SW  (push button, active LOW) → PIN_ENCODER_SW
//   +   (VCC) → 3.3 V
//   GND → GND
#define PIN_ENCODER_CHA   25   // KY-040 CLK – quadrature Channel A (interrupt on CHANGE)
#define PIN_ENCODER_CHB   26   // KY-040 DT  – quadrature Channel B (interrupt on CHANGE)
#define PIN_ENCODER_SW    32   // KY-040 SW  – push-button (active LOW, internal pull-up)
#define PIN_RUNOUT        27   // Runout output to Klipper filament sensor (active LOW)

// ─── FreeRTOS / Task Configuration ───────────────────────────────────────────
#define CORE0_TASK_STACK  8192  // bytes – protocol core (WiFi, HTTP, fault detect)
#define CORE1_TASK_STACK  4096  // bytes – real-time core (encoder ISR, speed calc)
#define CORE0_TASK_PRIO   5     // FreeRTOS priority
#define CORE1_TASK_PRIO   10    // Higher priority → lower ISR latency
#define ENCODER_QUEUE_LEN 1     // Single-slot overwriting queue (always latest state)

// ─── Timing Constants ─────────────────────────────────────────────────────────
#define ENCODER_UPDATE_MS  20UL   // Core 1 speed calc period (50 Hz)
#define MOONRAKER_POLL_MS  200UL  // Core 0 Moonraker poll period (5 Hz)
#define CORE0_LOOP_MS      10UL   // Core 0 main loop tick (100 Hz)

// ─── Default Runtime Values (overridden by NVS on boot) ──────────────────────
#define DEFAULT_CAL_FACTOR     0.01f        // mm per encoder tick
#define DEFAULT_TIMEOUT_MS     2000UL       // ms of no filament motion → fault
#define DEFAULT_MIN_EXT_VEL    0.5f         // mm/s extruder velocity → "printing"
#define DEFAULT_MOTION_THRESH  1            // minimum |ticks| to count as motion
#define DEFAULT_MOONRAKER_IP   "192.168.1.100"
#define DEFAULT_MOONRAKER_PORT 7125

// ─── Encoder Button Debounce ──────────────────────────────────────────────────
#define ENCODER_BTN_DEBOUNCE_MS  50UL  // ignore edges within this window (ms)

// ─── EMA Velocity Filter ─────────────────────────────────────────────────────
#define EMA_ALPHA  0.3f   // exponential moving average weight (0 = no response)

// ─── WiFi ─────────────────────────────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS   30000UL  // wait before falling back to AP mode
#define WIFI_RECONNECT_MAX_MS     60000UL  // maximum backoff between retries
#define WIFI_AP_SSID              "FilamentSensor"
#define WIFI_AP_PASS              "sensor1234"

// ─── Web Server ───────────────────────────────────────────────────────────────
#define WEB_SERVER_PORT  80

// ─── Serial Debug ─────────────────────────────────────────────────────────────
#define SERIAL_BAUD  115200

// ─── NVS Namespace & Keys ────────────────────────────────────────────────────
// NVS namespace and key names are limited to 15 characters.
#define NVS_NAMESPACE    "filsns"
#define NVS_KEY_CAL      "cal_factor"
#define NVS_KEY_TIMEOUT  "timeout_ms"
#define NVS_KEY_MIN_VEL  "min_ext_vel"
#define NVS_KEY_THRESH   "mot_thresh"
#define NVS_KEY_MR_IP    "mr_ip"
#define NVS_KEY_MR_PORT  "mr_port"
#define NVS_KEY_SSID     "wifi_ssid"
#define NVS_KEY_PASS     "wifi_pass"
