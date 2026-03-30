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
| **Moonraker integration** | 5 Hz HTTP poll of extruder velocity |
| **Configurable fault timeout** | Default 2 s; adjustable 0.5–10 s |
| **Physical fault reset** | Press the KY-040 knob to clear a runout fault |
| **Web configuration UI** | Mobile-friendly dashboard; no app required |
| **NVS persistence** | All settings survive power cycles and firmware updates |
| **WiFi AP fallback** | First-boot or credential-loss → AP mode for setup |
| **Exponential backoff reconnect** | WiFi loss recovery ≤ 60 s |

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
        ├── config.h                 ← All pin & timing constants
        ├── types.h                  ← Shared structs & state enum
        ├── encoder.h / .cpp         ← Quadrature ISR + SW button + 50 Hz Core 1 task
        ├── moonraker_client.h / .cpp← HTTP poll of extruder velocity
        ├── fault_detector.h / .cpp  ← State machine + GPIO 27 runout signal
        ├── nvs_config.h / .cpp      ← Preferences (NVS) load/save
        ├── web_handler.h / .cpp     ← Embedded SPA + REST API (port 80)
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

All pins are configurable in `firmware/src/config.h`.

---

## 🌐 Web Interface

Navigate to `http://<sensor-ip>` for the live dashboard:

- Real-time state badge (`IDLE`, `PRINTING`, `FAULT`, …)
- Encoder velocity bar + extruder velocity comparison
- One-click **Reset Fault** button (or press the KY-040 knob)
- Full configuration form (calibration, timeout, Moonraker, WiFi)

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
