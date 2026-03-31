#pragma once

// ─── GPIO Pin Assignments ─────────────────────────────────────────────────────
#define PIN_ENCODER_CHA   25   // Quadrature encoder Channel A (rising + falling)
#define PIN_ENCODER_CHB   26   // Quadrature encoder Channel B (rising + falling)
#define PIN_ENCODER_BTN   32   // Encoder push-button (active LOW, internal pull-up)
                               // GPIO 32/33 support INPUT_PULLUP; 34-39 do NOT.
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
//
// ─── Wheel diameter & ticks-per-revolution → cal_factor ─────────────────────
// The quadrature decoder counts 4 electrical edges per mechanical pulse.
// If your encoder has N detents per revolution and P pulses per detent:
//   ENCODER_TICKS_PER_REV = N * P * 4   (full quadrature, both edges, both channels)
//
// IMPORTANT: verify ENCODER_TICKS_PER_REV by watching the serial output:
//   Hold the filament still, rotate the wheel exactly one full revolution slowly,
//   then read the accumulated tick_count change. That number is your true value.
//
// Typical cheap 20-detent encoders, 1 pulse/detent, full quadrature → 80 ticks/rev.
// If yours shows 20 ticks/rev in practice, it uses half-quad internally → use 20.
//
#define ENCODER_WHEEL_DIAM_MM   10.0f   // Pinch wheel outer diameter in mm
#define ENCODER_TICKS_PER_REV   70      // Measured ticks for one full revolution
                                        // (detents × pulses/detent × 4 for full-quad)
                                        // Common: 20 detents × 1 pulse × 4 = 80
                                        //         20 detents × 1 pulse × 1 = 20 (half-quad)
#define DEFAULT_CAL_FACTOR  (M_PI * ENCODER_WHEEL_DIAM_MM / ENCODER_TICKS_PER_REV)
// With defaults above: (π × 10) / 80 ≈ 0.3927 mm/tick
#define DEFAULT_TIMEOUT_MS     2000UL       // ms of no filament motion → fault
#define DEFAULT_MIN_EXT_VEL    0.5f         // mm/s extruder velocity → "printing"
#define DEFAULT_MOTION_THRESH  1            // minimum |ticks| to count as motion
#define DEFAULT_MOONRAKER_IP   "192.168.1.100"
#define DEFAULT_MOONRAKER_PORT 7125

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
#define SERIAL_BAUD                115200


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
#define NVS_KEY_OTA_HOST "ota_hostname"
#define NVS_KEY_OTA_PASS "ota_password"

// ─── Firmware Version ─────────────────────────────────────────────────────────
// Bump this when cutting a new GitHub release so the OTA checker can compare.
#define FIRMWARE_VERSION  "1.0.0"

// ─── OTA (Over-The-Air Update) ────────────────────────────────────────────────
// mDNS hostname advertised by ArduinoOTA; reachable as <OTA_HOSTNAME>.local on
// the local network once mDNS is working.
#define OTA_HOSTNAME     "filament-sensor"

// Password required for the espota push protocol (PlatformIO / VS Code upload).
// Change this to something unique before deploying in a shared network.
#define OTA_PASSWORD     "ota1234"

// GitHub repository for the automatic update check.
// The OTA handler calls the GitHub Releases API, compares the latest tag_name
// with FIRMWARE_VERSION, and – if newer – streams the "firmware.bin" asset
// through HTTPUpdate to the inactive OTA partition.
#define GITHUB_OWNER     "tobi01001"
#define GITHUB_REPO      "klipper-filament-runout-esp32"
#define GITHUB_API_URL   "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases/latest"

// Maximum time to wait for the GitHub API or asset download (ms).
#define OTA_HTTP_TIMEOUT_MS  20000
