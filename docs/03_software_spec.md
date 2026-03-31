# Filament Runout Sensor – Software Specification

**Version**: 1.1 | **Date**: 2026-03-31 | **Author**: tobi01001

---

## 1. Build Environment

| Item | Value |
|------|-------|
| Framework | Arduino (ESP-IDF FreeRTOS underneath) |
| Build system | PlatformIO |
| Board target | `esp32dev` |
| Platform | `espressif32` |
| External libraries | `bblanchon/ArduinoJson ^7.0.0`, `olikraus/U8g2 ^2.35.7` |
| Serial baud rate | 115 200 |

### 1.1 Directory Layout

```
firmware/
├── platformio.ini          # PlatformIO project config
└── src/
    ├── config.h            # Compile-time constants, pin assignments & OLED flags
    ├── types.h             # Shared data structures & enums
    ├── encoder.h/.cpp      # Quadrature ISR + Core 1 speed task
    ├── moonraker_client.h/.cpp  # HTTP polling of Klipper extruder velocity
    ├── fault_detector.h/.cpp    # State machine + runout GPIO logic
    ├── nvs_config.h/.cpp   # NVS (Preferences) load / save
    ├── web_handler.h/.cpp  # Embedded web UI + REST API
    ├── display_handler.h/.cpp   # SSD1306 OLED display (compiled only if ENABLE_OLED)
    └── main.cpp            # Entry point, task creation
```

---

## 2. Core 1 – Real-Time Encoder Subsystem

### 2.1 ISR: `encoder_isr()` (IRAM_ATTR)

**File**: `encoder.cpp`

The ISR is triggered on `CHANGE` events on both GPIO 25 (ChA) and GPIO 26 (ChB).
It uses a 16-entry Gray-code lookup table to determine tick direction:

```
state = (ChB << 1) | ChA

QEM[16] = { 0,+1,-1, 0,  // prev=0b00
            -1, 0, 0,+1,  // prev=0b01
            +1, 0, 0,-1,  // prev=0b10
             0,-1,+1, 0 } // prev=0b11

Forward (CW): 0b00→0b01→0b11→0b10→0b00  (each transition +1)
Reverse (CCW): 0b00→0b10→0b11→0b01→0b00 (each transition -1)
```

Shared variables (`s_tick_count`, `s_direction`, `g_last_motion_ms`) are
protected with `portENTER_CRITICAL_ISR` / `portEXIT_CRITICAL_ISR`.

**Timing guarantee**: ISR body < 5 µs at 240 MHz.

### 2.2 Task: `encoder_task()` – 50 Hz, pinned to Core 1

Runs every `ENCODER_UPDATE_MS` (20 ms) using `vTaskDelayUntil` for
jitter-free timing.

```
velocity_raw = (tick_delta × cal_factor) / (dt_ms / 1000)
velocity_ema = α × velocity_raw + (1 − α) × velocity_ema_prev
```

where `α = EMA_ALPHA = 0.3`.

The result is packaged as `EncoderData` and written to `encoder_queue`
with `xQueueOverwrite` (single-slot, always latest).

---

## 3. Core 0 – Protocol Subsystem

### 3.1 WiFi Manager (`core0_task`)

| Phase | Behaviour |
|-------|-----------|
| Startup | Connect using SSID / password from NVS |
| Timeout (30 s) | Fall back to AP mode (`FilamentSensor` / `sensor1234`) |
| Station disconnect | Exponential backoff: 1 s → 2 s → 4 s … max 60 s |
| AP mode | Web UI accessible at `192.168.4.1` for initial setup |

### 3.2 Moonraker Client (`moonraker_client.cpp`)

Polls every `MOONRAKER_POLL_MS` (200 ms = 5 Hz):

```
GET http://<moonraker_ip>:<moonraker_port>/printer/objects/query?extruder
```

Parses `result.status.extruder.velocity` (float, mm/s).  Returns `0.0`
on network error or missing field (safe fallback – no spurious fault).

### 3.3 Fault Detector (`fault_detector.cpp`)

```
IF extruder_velocity ≥ min_ext_vel   → state = PRINTING
   AND (now − g_last_motion_ms) ≥ timeout_ms
   → trigger_fault()                 → GPIO 27 = LOW, state = FAULT

IF extruder_velocity < min_ext_vel
   AND state == PRINTING
   → state = IDLE
```

`g_last_motion_ms` is updated by the ISR on every encoder edge;
`portENTER_CRITICAL_ISR` ensures it is written atomically.

### 3.4 Web Server (`web_handler.cpp`)

`WebServer` (ESP32 Arduino built-in) on port 80.

| Method | Route | Description |
|--------|-------|-------------|
| `GET` | `/` | Serve embedded single-page application |
| `GET` | `/api/status` | JSON snapshot of `SensorStatus` |
| `GET` | `/api/config` | JSON snapshot of `SensorConfig` |
| `POST` | `/api/config` | Update `SensorConfig` + persist to NVS |
| `POST` | `/api/reset` | Clear active fault, restore GPIO 27 HIGH |

#### Status response example
```json
{
  "state": "PRINTING",
  "enc_vel": 3.47,
  "ext_vel": 3.50,
  "ticks": 12480,
  "direction": 1,
  "motion_ago_ms": 18,
  "fault": false,
  "wifi": true,
  "ip": "192.168.1.42"
}
```

#### Config POST body
```json
{
  "cal_factor": 0.012,
  "timeout_ms": 2000,
  "min_ext_vel": 0.5,
  "motion_threshold": 1,
  "moonraker_ip": "192.168.1.100",
  "moonraker_port": 7125,
  "wifi_ssid": "MyNetwork",
  "wifi_pass": "secret",
  "display_enabled": true
}
```
`wifi_pass` is only updated if a non-empty value is supplied.

---

### 3.5 OLED Display (`display_handler.cpp`) – optional

Compiled only when `ENABLE_OLED` is defined in `config.h` (default: enabled).
Uses the **U8g2** library with full-buffer mode (`U8G2_SSD1306_128X64_NONAME_F_HW_I2C`)
on the ESP32 hardware I²C port (GPIO 21 SDA / GPIO 22 SCL, address `0x3C`).

All I²C/display calls run exclusively from Core 0 so no additional mutex is needed
for the display bus.

#### Display layout (128×64 pixels)

```
┌──────────────────────────────┐
│ PRINTING                     │  ← 8×13 bold font, title bar (16 px)
├──────────────────────────────┤  ← 1 px separator line
│ Enc:  3.47 mm/s              │  ← 6×10 font
│ Ext:  3.50 mm/s              │
│ Tck:    12480 >              │  ← tick count + direction symbol
│ 192.168.1.42                 │  ← IP address (or "WiFi offline")
└──────────────────────────────┘
```

**FAULT state**: the title bar alternates between an inverted white box with black
text and normal text at 1 Hz (each `display_update()` call, called every
`OLED_UPDATE_MS` = 500 ms).

#### Compile-time control

| Symbol | Location | Effect |
|--------|----------|--------|
| `#define ENABLE_OLED` | `config.h` | Include display code (default on) |
| *(comment out)* | `config.h` | Exclude driver entirely |
| `OLED_SDA_PIN` / `OLED_SCL_PIN` | `config.h` | I²C pin assignment |
| `OLED_I2C_ADDR` | `config.h` | I²C address (0x3C or 0x3D) |
| `OLED_UPDATE_MS` | `config.h` | Refresh period (default 500 ms) |
| `OLED_DEFAULT_EN` | `config.h` | First-boot default (true) |

#### Runtime control

`config.display_enabled` (persisted in NVS key `disp_en`) can be toggled
at any time via the web interface without re-flashing.  Setting it `false`
calls `u8g2.setPowerSave(1)` to blank the display and halt I²C traffic.

---

## 4. NVS Persistence (`nvs_config.cpp`)

Uses ESP32 `Preferences` library (namespace `"filsns"`, max 15 chars).

| NVS Key | Type | Default |
|---------|------|---------|
| `cal_factor` | float | 0.01 mm/tick |
| `timeout_ms` | uint32 | 2000 ms |
| `min_ext_vel` | float | 0.5 mm/s |
| `mot_thresh` | int32 | 1 tick |
| `mr_ip` | string | `192.168.1.100` |
| `mr_port` | uint32 | 7125 |
| `wifi_ssid` | string | *(empty)* |
| `wifi_pass` | string | *(empty)* |
| `disp_en` | bool | `true` |

On first boot (namespace missing) all defaults are written to NVS.

---

## 5. Shared Data Structures

```cpp
struct EncoderData {
    int32_t  tick_count;       // cumulative encoder ticks
    int32_t  tick_delta;       // ticks since last 20 ms update
    int8_t   direction;        // +1 fwd, -1 rev, 0 stopped
    uint32_t timestamp_ms;     // millis() at snapshot
    float    velocity_mm_s;    // EMA-filtered velocity (mm/s)
};

struct SensorConfig {
    float    cal_factor;        // mm / tick
    uint32_t timeout_ms;
    float    min_ext_vel;       // mm/s
    int32_t  motion_threshold;
    char     moonraker_ip[40];
    uint16_t moonraker_port;
    char     wifi_ssid[64];
    char     wifi_pass[64];
    bool     display_enabled;   // enable/disable SSD1306 OLED at runtime
};

struct SensorStatus {
    SystemState state;
    EncoderData encoder;
    float       extruder_vel;
    bool        fault_active;
    bool        wifi_connected;
    char        ip_address[16];
};
```

---

## 6. Memory Budget

| Region | Allocated | Notes |
|--------|-----------|-------|
| Core 0 task stack | 10 240 B (OLED on) / 8 192 B (OLED off) | WiFi / HTTP / display headroom |
| Core 1 task stack | 4 096 B | Minimal – no heap alloc in ISR |
| Encoder queue | 1 × `sizeof(EncoderData)` ≈ 24 B | |
| ArduinoJson document | ~512 B on stack (Moonraker parse) | |
| U8g2 full-frame buffer | 1 024 B heap (128×64 / 8) | Only when `ENABLE_OLED` |
| Embedded HTML | ~8 KB in flash (PROGMEM) | Slightly larger with OLED toggle |
| WiFi stack (IDF) | ~100 KB heap | ESP-IDF internal |
| **Total heap target** | **< 210 KB** | |

---

## 7. Compile-Time Configuration (`config.h`)

All key parameters can be adjusted before flashing without modifying
business logic:

```c
#define PIN_ENCODER_CHA   25
#define PIN_ENCODER_CHB   26
#define PIN_RUNOUT        27
#define ENCODER_UPDATE_MS 20
#define MOONRAKER_POLL_MS 200
#define DEFAULT_CAL_FACTOR 0.01f
#define DEFAULT_TIMEOUT_MS 2000UL
#define EMA_ALPHA          0.3f

// OLED display (comment out to disable entirely):
#define ENABLE_OLED
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22
#define OLED_I2C_ADDR   0x3C
#define OLED_UPDATE_MS  500UL
#define OLED_DEFAULT_EN true
```
