# ESP32 Intelligent Filament Runout Sensor

![License](https://img.shields.io/badge/License-MIT-green)
![Platform](https://img.shields.io/badge/Platform-ESP32-blue)
![Framework](https://img.shields.io/badge/Framework-Arduino%20%2B%20FreeRTOS-orange)
![Build](https://img.shields.io/badge/Build-PlatformIO-purple)

Dual-core ESP32 firmware that detects **stuck, tangled, or exhausted filament**
during 3D printing by measuring actual filament *motion* through a KY-040
quadrature rotary encoder — not just spool presence.  Integrates with Klipper
via a Moonraker API poll and a direct runout GPIO signal.

---

## ✨ Features

| Feature | Detail |
|---------|--------|
| **True motion detection** | Detects stuck/tangled filament, not only empty spool |
| **Dual-core FreeRTOS** | Core 1 handles ISR + speed calc; Core 0 handles WiFi + logic |
| **Quadrature Gray-code decoder** | ISR latency < 5 µs; direction + velocity |
| **EMA velocity filter** | Smooth 50 Hz speed estimate (α = 0.3) |
| **Moonraker integration** | WebSocket subscribe/notify of extruder velocity |
| **Fault G-code** | Sends a configurable G-code command (default `PAUSE`) to Klipper via WebSocket on fault |
| **Configurable fault timeout** | Default 2 s; adjustable 0.5–10 s |
| **Physical fault reset** | Press the KY-040 knob to clear a runout fault |
| **DHT22 environment sensor** (optional) | Monitors ambient temperature (°C) and humidity (%RH); exposed via web UI and `/api/status` |
| **Web configuration UI** | Mobile-friendly dashboard; no app required |
| **NVS persistence** | All settings (including GPIO pin assignments) survive power cycles and firmware updates |
| **WiFi AP fallback** | First-boot or credential-loss → AP mode for setup |
| **Exponential backoff reconnect** | WiFi loss recovery ≤ 60 s |
| **ArduinoOTA** | Push firmware directly from VS Code / PlatformIO over WiFi |
| **GitHub Releases OTA** | Pull latest firmware from GitHub releases via the web UI |
| **OLED display** (optional) | SSD1306 128×64 I²C; shows state, velocities, ticks, IP; blinks on FAULT |

---

## 📁 Structure

```
klipper-filament-runout-esp32/
├── .vscode/
│   └── extensions.json              ← Recommends PlatformIO IDE for VSCode
├── platformio.ini                   ← PlatformIO project root (open here in VSCode)
├── LICENSE
├── README.md                        ← You are here
├── docs/
│   ├── 01_overview.md               ← High-level architecture
│   ├── 02_hardware_spec.md          ← BOM, wiring, schematic (KY-040)
│   ├── 03_software_spec.md          ← API, tasks, data structures
│   └── 04_user_instructions.md      ← Setup, calibration, troubleshooting
└── firmware/
    └── src/
        ├── config.h                 ← All pin & timing constants + OLED flags
        ├── types.h                  ← Shared structs & state enum
        ├── encoder.h / .cpp         ← Quadrature ISR + SW button + 50 Hz Core 1 task
        ├── moonraker.h / .cpp       ← WebSocket-based Klipper / Moonraker client
        ├── fault_detector.h / .cpp  ← State machine + GPIO 27 runout signal
        ├── nvs_config.h / .cpp      ← Preferences (NVS) load/save (sensor config)
        ├── pin_config.h / .cpp      ← Runtime GPIO pin assignments with NVS persistence
        ├── ota_handler.h / .cpp     ← ArduinoOTA + GitHub release OTA
        ├── ota_runtime.h / .cpp     ← OTA lifecycle integration
        ├── wifi_handler.h / .cpp    ← Non-blocking WiFi state machine + captive portal
        ├── web_handler.h / .cpp     ← Embedded SPA + REST API (port 80)
        ├── display_handler.h / .cpp ← SSD1306 OLED driver (optional, #ifdef ENABLE_OLED)
        └── main.cpp                 ← Entry point, task creation
```

---

## ⚡ Quick Start

### Option A – Visual Studio Code (recommended)

1. Install the [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension for VSCode.
2. Open the **repository root** folder in VSCode (`File › Open Folder…`).
3. VSCode will prompt you to install the recommended extension if not already present.
4. Click **PlatformIO: Build** (✓ icon in the status bar) to compile.
5. Connect your ESP32 and click **PlatformIO: Upload** (→ icon) to flash.

### Option B – Command Line

```bash
# 1. Install PlatformIO Core (if not already installed)
pip install platformio

# 2. Build and flash (run from the repository root)
pio run --target upload

# 3. Monitor serial output
pio device monitor
```

On first boot: connect to WiFi `FilamentSensor` (password `sensor1234`)
and open `http://192.168.4.1` to configure your network and Moonraker details.

---

## 🔌 GPIO Wiring (default)

### KY-040 Rotary Encoder → ESP32

| KY-040 Pin | ESP32 GPIO | Signal | Notes |
|-----------|-----------|--------|-------|
| GND | GND | Ground | |
| + | 3.3 V | Power | |
| CLK | 25 | Channel A | Input, internal pull-up |
| DT | 26 | Channel B | Input, internal pull-up |
| SW | 32 | Push-button | Input, internal pull-up – press to reset fault |

### ESP32 → Klipper Host

| ESP32 GPIO | Signal | Direction |
|-----------|--------|-----------|
| 27 | Klipper runout (active LOW) | Output |
| 21 | OLED SDA (optional) | I²C |
| 22 | OLED SCL (optional) | I²C |

### DHT22 Environment Sensor → ESP32 (optional)

| DHT22 Pin | ESP32 GPIO | Signal | Notes |
|----------|-----------|--------|-------|
| GND | GND | Ground | |
| VCC | 3.3 V | Power | Most breakout modules accept 3.3–5 V |
| DATA | 4 | Data | 10 kΩ pull-up included on most breakout modules |

All pins are configurable in `firmware/src/config.h` (compile-time defaults) or
at runtime via the web interface — see [Runtime Pin Configuration](#-runtime-pin-configuration) below.

---

## 🎛️ Runtime Pin Configuration

Pin assignments for all GPIO peripherals can be changed **without recompiling the firmware** through the web interface.

### How to change a pin

1. Open `http://<sensor-ip>` in your browser.
2. Scroll to the **Pin Configuration** card.
3. Enter the desired GPIO numbers.
4. Click **Save & Reboot** — the device saves the new assignments to NVS and restarts automatically.

After the reboot the new pin assignments are active and persistent across future reboots.

### Configurable pins

| Field | Default GPIO | Notes |
|-------|-------------|-------|
| Encoder Ch-A | 25 | KY-040 CLK — input, internal pull-up |
| Encoder Ch-B | 26 | KY-040 DT — input, internal pull-up |
| Encoder button | 32 | KY-040 SW — input; GPIO 32/33 support `INPUT_PULLUP`; 34–39 require external pull-up |
| Runout output | 27 | Active-LOW to Klipper; must be output-capable (GPIO 0–33) |
| DHT22 data | 4 | Single-wire data line; shown only when DHT22 support is compiled in (default: on) |

### Constraints

- GPIO 6–11 are reserved for the internal SPI flash and cannot be used.
- GPIO 34–39 are **input-only** and cannot be used as the runout output.
- All five pins must be unique; duplicate assignments are rejected with an error.
- OLED I²C pins (21/22) are not exposed here; change them in `config.h` and reflash if needed.

### API

The configuration is also accessible via the REST API:

```bash
# Read current pin assignments
curl http://<sensor-ip>/api/pins

# Update one or more pins (device reboots on success)
curl -X POST http://<sensor-ip>/api/pins \
     -H 'Content-Type: application/json' \
     -d '{"runout_pin":33,"dht_pin":5}'
```

**`GET /api/pins`** response:
```json
{"enc_a_pin":25,"enc_b_pin":26,"enc_btn_pin":32,"runout_pin":27,"dht_pin":4}
```

**`POST /api/pins`** success (device reboots ~200 ms later):
```json
{"ok":true,"reboot":true}
```

**`POST /api/pins`** validation error:
```json
{"ok":false,"error":"runout_pin: must be output-capable (GPIO 0-33)"}
```

### NVS storage

Pin assignments are stored in the NVS namespace **`pin_cfg`** with keys `enc_a`, `enc_b`, `enc_btn`, `runout`, and `dht`. The compile-time defaults from `config.h` are used on first boot when no NVS entries exist, so a factory reset or NVS erase restores the original wiring.

---

## 🌐 Web Interface

Navigate to `http://<sensor-ip>` for the live dashboard:

- Real-time state badge (`IDLE`, `PRINTING`, `FAULT`, …)
- Encoder velocity bar + extruder velocity comparison
- One-click **Reset Fault** button (or press the KY-040 knob)
- Full configuration form (calibration, timeout, Moonraker, WiFi)
- **Firmware Update** card – check GitHub and update in one click

---

## 🔄 OTA Firmware Updates

Two update paths are supported.

### Path 1 – VS Code / PlatformIO push (ArduinoOTA)

The device advertises itself on the local network as `filament-sensor.local`
(configurable via `OTA_HOSTNAME` in `config.h`).

```bash
# Upload over WiFi using the dedicated OTA environment
pio run -e esp32dev_ota -t upload --upload-port filament-sensor.local
# or with the device IP:
pio run -e esp32dev_ota -t upload --upload-port 192.168.1.42
```

You can also select `esp32dev_ota` as the active environment in VS Code and
click the **Upload** button.  The OTA password defaults to `ota1234`
(set `OTA_PASSWORD` in `config.h` to change it; update `upload_flags` in
`platformio.ini` accordingly).

### Path 2 – GitHub Releases pull

1. Open `http://<sensor-ip>` in your browser.
2. Scroll to the **Firmware Update** card.
3. Click **Check & Update from GitHub**.

The device calls the GitHub Releases API, compares the `tag_name` with the
compiled-in `FIRMWARE_VERSION`, and – if a newer release is found – streams the
`firmware.bin` asset to the inactive OTA flash partition and reboots.

**How to publish a new release:**
1. Bump `FIRMWARE_VERSION` in `firmware/src/config.h`.
2. Build: `pio run -e esp32dev`
3. Upload `.pio/build/esp32dev/firmware.bin` as a release asset named
   **`firmware.bin`** to a new GitHub release tagged `v<version>` (e.g. `v1.1.0`).

**Web interface configuration:**
- **Enable OLED display** toggle (persisted in NVS, no re-flash)

---

## 📚 Documentation

| Document | Contents |
|----------|----------|
| [01 Overview](docs/01_overview.md) | Architecture, state machine, performance targets |
| [02 Hardware Spec](docs/02_hardware_spec.md) | BOM, KY-040 wiring, schematic, power |
| [03 Software Spec](docs/03_software_spec.md) | Tasks, API reference, memory budget |
| [04 User Instructions](docs/04_user_instructions.md) | Step-by-step setup, calibration, troubleshooting |

---

## 📜 License

MIT License – Copyright (c) 2026 tobi01001.  See [LICENSE](LICENSE).
