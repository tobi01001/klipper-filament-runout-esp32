# ESP32 Intelligent Filament Runout Sensor

![License](https://img.shields.io/badge/License-MIT-green)
![Platform](https://img.shields.io/badge/Platform-ESP32-blue)
![Framework](https://img.shields.io/badge/Framework-Arduino%20%2B%20FreeRTOS-orange)
![Build](https://img.shields.io/badge/Build-PlatformIO-purple)

Dual-core ESP32 firmware that detects **stuck, tangled, or exhausted filament**
during 3D printing by measuring actual filament *motion* through an optical
quadrature encoder — not just spool presence.  Integrates with Klipper via a
Moonraker API poll and a direct runout GPIO signal.

---

## ✨ Features

| Feature | Detail |
|---------|--------|
| **True motion detection** | Detects stuck/tangled filament, not only empty spool |
| **Dual-core FreeRTOS** | Core 1 handles ISR + speed calc; Core 0 handles WiFi + logic |
| **Quadrature Gray-code decoder** | ISR latency < 5 µs; direction + velocity |
| **EMA velocity filter** | Smooth 50 Hz speed estimate (α = 0.3) |
| **Moonraker integration** | 5 Hz HTTP poll of extruder velocity |
| **Configurable fault timeout** | Default 2 s; adjustable 0.5–10 s |
| **Web configuration UI** | Mobile-friendly dashboard; no app required |
| **NVS persistence** | All settings survive power cycles and firmware updates |
| **WiFi AP fallback** | First-boot or credential-loss → AP mode for setup |
| **Exponential backoff reconnect** | WiFi loss recovery ≤ 60 s |
| **ArduinoOTA** | Push firmware directly from VS Code / PlatformIO over WiFi |
| **GitHub Releases OTA** | Pull latest firmware from GitHub releases via the web UI |
| **OLED display** (optional) | SSD1306 128×64 I²C; shows state, velocities, ticks, IP; blinks on FAULT |

---

## 📁 Structure

```
esp32-filament-sensor/
├── LICENSE
├── README.md                        ← You are here
├── docs/
│   ├── 01_overview.md               ← High-level architecture
│   ├── 02_hardware_spec.md          ← BOM, wiring, schematic
│   ├── 03_software_spec.md          ← API, tasks, data structures
│   └── 04_user_instructions.md      ← Setup, calibration, troubleshooting
└── firmware/
    ├── platformio.ini
    └── src/
        ├── config.h                 ← All pin & timing constants + OLED flags
        ├── types.h                  ← Shared structs & state enum
        ├── encoder.h / .cpp         ← Quadrature ISR + 50 Hz Core 1 task
        ├── moonraker_client.h / .cpp← HTTP poll of extruder velocity
        ├── fault_detector.h / .cpp  ← State machine + GPIO 27 runout signal
        ├── nvs_config.h / .cpp      ← Preferences (NVS) load/save
        ├── ota_handler.h / .cpp     ← ArduinoOTA + GitHub release OTA
        ├── web_handler.h / .cpp     ← Embedded SPA + REST API (port 80)
        ├── display_handler.h / .cpp ← SSD1306 OLED driver (optional, #ifdef ENABLE_OLED)
        └── main.cpp                 ← Entry point, task creation
```

---

## ⚡ Quick Start

```bash
# 1. Install PlatformIO (if not already installed)
pip install platformio

# 2. Build and flash
cd esp32-filament-sensor/firmware
pio run --target upload

# 3. Monitor serial output
pio device monitor
```

On first boot: connect to WiFi `FilamentSensor` (password `sensor1234`)
and open `http://192.168.4.1` to configure your network and Moonraker details.

---

## 🔌 GPIO Wiring (default)

| ESP32 GPIO | Signal | Direction |
|-----------|--------|-----------|
| 25 | Encoder Channel A | Input (pull-up) |
| 26 | Encoder Channel B | Input (pull-up) |
| 27 | Klipper runout (active LOW) | Output |
| 21 | OLED SDA (optional) | I²C |
| 22 | OLED SCL (optional) | I²C |

All pins are configurable in `firmware/src/config.h`.

---

## 🌐 Web Interface

Navigate to `http://<sensor-ip>` for the live dashboard:

- Real-time state badge (`IDLE`, `PRINTING`, `FAULT`, …)
- Encoder velocity bar + extruder velocity comparison
- One-click **Reset Fault** button
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
- **Enable OLED display** toggle (persisted in NVS, no re-flash)

---

## 📚 Documentation

| Document | Contents |
|----------|----------|
| [01 Overview](docs/01_overview.md) | Architecture, state machine, performance targets |
| [02 Hardware Spec](docs/02_hardware_spec.md) | BOM, wiring, schematic, power |
| [03 Software Spec](docs/03_software_spec.md) | Tasks, API reference, memory budget |
| [04 User Instructions](docs/04_user_instructions.md) | Step-by-step setup, calibration, troubleshooting |

---

## 📜 License

MIT License – Copyright (c) 2026 tobi01001.  See [LICENSE](LICENSE).
