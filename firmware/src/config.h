#pragma once

// ─── GPIO Pin Assignments ─────────────────────────────────────────────────────
#define PIN_ENCODER_CHA   25   // Quadrature encoder Channel A (rising + falling)
#define PIN_ENCODER_CHB   26   // Quadrature encoder Channel B (rising + falling)
#define PIN_ENCODER_BTN   32   // Encoder push-button (active LOW, internal pull-up)
                               // GPIO 32/33 support INPUT_PULLUP; 34-39 do NOT.
#define PIN_RUNOUT        27   // Runout output to Klipper filament sensor (active LOW)

// ─── OLED Display (SSD1306, 128×64, I²C) ─────────────────────────────────────
// ENABLE_OLED is defined by default.  To exclude all display code at compile
// time (saves ~50 kB flash and 1 kB RAM), either:
//   • Pass -DDISABLE_OLED in build_flags (platformio.ini) – preferred, no
//     source-file edit required.
//   • Or manually comment out the #define ENABLE_OLED line below.
// When enabled the display shows live state, encoder velocity, extruder
// velocity, tick count, and the sensor IP address.  It can also be toggled at
// runtime via the web interface without re-flashing.
#ifndef DISABLE_OLED
#define ENABLE_OLED
#endif

#ifdef ENABLE_OLED
#define OLED_SDA_PIN    21        // I²C SDA (default ESP32 hardware I²C)
#define OLED_SCL_PIN    22        // I²C SCL (default ESP32 hardware I²C)
#define OLED_I2C_ADDR   0x3C      // SSD1306 I²C address: 0x3C or 0x3D
#define OLED_WIDTH      128       // Display width in pixels
#define OLED_HEIGHT     64        // Display height in pixels
#define OLED_UPDATE_MS  100UL     // Display refresh period
#define OLED_DEFAULT_EN true      // Enable display by default on first boot
#endif

// ─── FreeRTOS / Task Configuration ───────────────────────────────────────────
#ifdef ENABLE_OLED
// U8g2 full-buffer mode allocates 1 kB on the heap, but its rendering helpers
// need a bit more stack headroom.  Bump Core 0 stack to 10 kB when OLED is on.
#define CORE0_TASK_STACK  10240 // bytes – protocol core + OLED driver headroom
#else
#define CORE0_TASK_STACK  8192  // bytes – protocol core (WiFi, HTTP, fault detect)
#endif
#define CORE1_TASK_STACK  4096  // bytes – real-time core (encoder ISR, speed calc)
#define CORE0_TASK_PRIO   5     // FreeRTOS priority
#define CORE1_TASK_PRIO   10    // Higher priority → lower ISR latency
#define ENCODER_QUEUE_LEN 1     // Single-slot overwriting queue (always latest state)

// ─── Timing Constants ─────────────────────────────────────────────────────────
#define ENCODER_UPDATE_MS  20UL   // Core 1 speed calc period (50 Hz)
#define ENCODER_ISR_DEBOUNCE_US 120UL // Ignore edges closer than this (mechanical bounce guard)
#define ENCODER_USE_PULSE_SERVICE false // true: count GPIO25 pulses only (speed-focused, no reverse direction)
#define ENCODER_USE_FULL_STEP true  // true: accumulate gray-code steps and count one movement tick per
                                    //       ENCODER_FULL_STEP_THRESHOLD steps — matches the detent cadence
                                    //       of a KY-040 (3–4 gray-code steps per mechanical detent).
                                    //       false: count every individual gray-code edge as a raw tick (x4).
// Minimum accumulated gray-code steps (absolute) to count as one movement tick.
// A standard KY-040 generates 3–4 steps per detent; setting this to 3 ensures every
// detent is counted while also rejecting sub-detent electrical noise.
#define ENCODER_FULL_STEP_THRESHOLD 3
#define MOONRAKER_POLL_MS  200UL  // Core 0 Moonraker poll period (5 Hz)
#define MOONRAKER_INFO_MS  2000UL // JSON-RPC server.info interval while not ready
#define MOONRAKER_SUB_RETRY_MS 2000UL // Retry interval for subscribe request
#define MOONRAKER_STALE_MS 2500UL // No extruder update beyond this => stale
#define MOONRAKER_WS_RECONNECT_MIN_MS 1000UL // Initial reconnect delay
#define MOONRAKER_WS_RECONNECT_MAX_MS 10000UL // Maximum reconnect delay
#define CORE0_LOOP_MS      10UL   // Core 0 main loop tick (100 Hz)

// ─── Default Runtime Values (overridden by NVS on boot) ──────────────────────
//
// ─── Wheel diameter & ticks-per-revolution → cal_factor ─────────────────────
// With ENCODER_USE_FULL_STEP enabled, one tick is counted per mechanical detent
// (once ENCODER_FULL_STEP_THRESHOLD gray-code steps have accumulated).
// For a 20-detent KY-040:   ENCODER_TICKS_PER_REV ≈ 20  (1 tick per detent)
//
// With ENCODER_USE_FULL_STEP disabled (raw x4 mode), every gray-code edge is a
// tick.  For a 20-detent KY-040:  ENCODER_TICKS_PER_REV ≈ 70–80 raw ticks/rev.
//
// IMPORTANT: always verify by running auto-calibration (web UI → Calibrate) or
//   by watching the serial output while rotating the wheel exactly one full turn.
//   The measured tick_count delta is your true ENCODER_TICKS_PER_REV.
//   Re-run calibration after changing ENCODER_USE_FULL_STEP or ENCODER_FULL_STEP_THRESHOLD.
//
#define ENCODER_WHEEL_DIAM_MM   10.0f   // Pinch wheel outer diameter in mm
// ENCODER_TICKS_PER_REV is defined relative to the active decode mode so that
// DEFAULT_CAL_FACTOR always gives a reasonable mm/tick estimate before calibration.
#if ENCODER_USE_FULL_STEP
#define ENCODER_TICKS_PER_REV   20      // ≈ 1 movement tick per detent (20-detent KY-040)
#else
#define ENCODER_TICKS_PER_REV   80      // ≈ 4 × 20 raw gray-code edges per revolution (x4 mode)
#endif
#define DEFAULT_CAL_FACTOR  (M_PI * ENCODER_WHEEL_DIAM_MM / ENCODER_TICKS_PER_REV)
// With defaults above: (π × 10) / 20 ≈ 1.5708 mm/tick
#define DEFAULT_TIMEOUT_MS     8000UL       // ms of no filament motion → fault
                                            // At 5 mm/s with a 5 cm sensor-to-extruder offset:
                                            // transit time ≈ 10 s, so an 8 s timeout fires with ~2 s to spare.
#define DEFAULT_MIN_EXT_VEL    0.3f         // mm/s extruder velocity → "printing"
#define DEFAULT_MOTION_THRESH  1            // minimum |ticks| to count as motion
#define DEFAULT_MOONRAKER_IP   "192.168.1.100"
#define DEFAULT_MOONRAKER_PORT 7125
#define DEFAULT_SENSOR_ENABLED true
#define DEFAULT_FAULT_GCODE    "PAUSE"     // GCODE sent to Moonraker via WebSocket on fault

// ─── Auto-Calibration ────────────────────────────────────────────────────────
#define CAL_EXTRUDE_MM         50.0f      // default calibration extrude distance (mm)
#define CAL_EXTRUDE_SPEED_MMPM 300.0f     // default extrude speed (mm/min = 5 mm/s)
#define CAL_WAIT_START_MS      3000UL     // max ms to wait for encoder motion to begin
#define CAL_WAIT_STOP_MS       30000UL    // max ms to wait for motion to stop
#define CAL_SETTLE_MS          400UL      // ms of near-zero velocity before result is computed
#define CAL_ENC_IDLE_MS        600UL      // ms of encoder ISR silence required to declare motion stopped

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
#define DEBUG_LOG_ENABLED          0      // 1 enables DBG_* macros in debug_log.h

#if DEBUG_LOG_ENABLED
#define CORE_DEBUG_LEVEL 3          // ESP-IDF log level (0-5, higher = more verbose)
#define CONFIG_ARDUHAL_LOG_COLORS   1 ; coloured serial output
#else
#define CORE_DEBUG_LEVEL 0          // 0 = no logs, 1 = ERROR, 2 = WARN, 3 = INFO, 4 = DEBUG, 5 = VERBOSE
#define CONFIG_ARDUHAL_LOG_COLORS   0  // coloured serial output
#endif


// ─── Velocity Sliding-Window Filter ──────────────────────────────────────────
// Ticks and elapsed time are accumulated over VEL_MEDIAN_N × ENCODER_UPDATE_MS
// before velocity is computed.  In full-step mode, movement ticks arrive only
// once per mechanical detent (≈1.57 mm/tick at default settings), so the window
// must be wide enough to contain at least two ticks even at the minimum printing
// speed (DEFAULT_MIN_EXT_VEL).
//
// Required window:
//   2 × cal_factor × 1000 / (DEFAULT_MIN_EXT_VEL × ENCODER_UPDATE_MS)
//   = 2 × (π × 10 / 20) × 1000 / (0.3 × 20) ≈ 524 → 525 (odd)
//
// This purely affects the smoothed velocity shown on the web UI and OLED display.
// Fault detection is driven by g_last_motion_ms, which is updated on every valid
// gray-code edge in the ISR and is unaffected by this filter.
// Odd integer ≥ 3.
#define VEL_MEDIAN_N  525


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
#define NVS_KEY_SENSOR_EN "sensor_en"
#define NVS_KEY_FAULT_GCODE "fault_gcode"

// ─── DHT22 Environment Sensor (temperature + humidity) ───────────────────────
// The DHT22 sensor is enabled by default.  To exclude all DHT22 code at compile
// time (saves ~15 kB flash and ~0.5 kB RAM), either:
//   • Pass -DDISABLE_DHT in build_flags (platformio.ini) – preferred.
//   • Or comment out the #define ENABLE_DHT line below.
// When enabled the device reads temperature and humidity every DHT_READ_INTERVAL_MS
// and exposes the values via /api/status and /api/dht.
#ifndef DISABLE_DHT
#define ENABLE_DHT
#endif

#ifdef ENABLE_DHT
#define DHT_PIN              4        // GPIO pin connected to DHT22 data line
                                      // GPIO 4 is unused by other peripherals on the
                                      // standard 38-pin ESP32 dev board wiring.
#define DHT_READ_INTERVAL_MS 3000UL   // Read period – DHT22 minimum sampling: 2 s
#define DEFAULT_DHT_ENABLED  true     // Enable DHT22 readings on first boot
#endif


// Bump this when cutting a new GitHub release so the OTA checker can compare.
#define FIRMWARE_VERSION  "1.2.1"

// ─── OTA (Over-The-Air Update) ────────────────────────────────────────────────
// mDNS hostname advertised by ArduinoOTA; reachable as <OTA_HOSTNAME>.local on
// the local network once mDNS is working.
#define OTA_HOSTNAME     "filament-sensor"

// Password required for the espota push protocol (PlatformIO / VS Code upload).
// Change this to something unique before deploying in a shared network.
#define OTA_PASSWORD     "ota1234"

// GitHub repository for the automatic update check.
// The OTA handler calls the GitHub Releases list API, scans for the latest
// firmware release (tag prefix "fw-v") and the latest Web UI release (tag
// prefix "ui-v"), then checks each independently against the running versions.
#define GITHUB_OWNER     "tobi01001"
#define GITHUB_REPO      "klipper-filament-runout-esp32"
// Returns up to 10 most-recent releases; enough to find the latest fw-v* and
// ui-v* entries even when they are not the single most-recent release.
#define GITHUB_RELEASES_URL \
    "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases?per_page=10"
#define ENABLE_GITHUB_OTA 1              // 0 removes HTTPS GitHub OTA path and mbedTLS dependency

// Maximum time to wait for the GitHub API or asset download (ms).
#define OTA_HTTP_TIMEOUT_MS  20000
#define NVS_KEY_DISP_EN  "disp_en"
#define NVS_KEY_DHT_EN   "dht_en"
