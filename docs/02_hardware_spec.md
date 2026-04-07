# Filament Runout Sensor – Hardware Specification

**Version**: 1.1 | **Date**: 2026-03-31 | **Author**: tobi01001

---

## 1. Bill of Materials (BOM)

| # | Component | Value / Part | Qty | Notes |
|---|-----------|-------------|-----|-------|
| 1 | MCU | ESP32 Dev Board (38-pin, 4 MB Flash) | 1 | Any ESP32 with exposed GPIO 25/26/27 |
| 2 | Optical encoder | PMW3360 or PMW3389 mouse sensor module **OR** generic quadrature optical encoder | 1 | Must provide two-channel (A/B) quadrature output |
| 3 | Filament guide | Printed PLA/PETG guide body | 1 | Mounts encoder wheel against filament path |
| 4 | Pull-up resistors | 10 kΩ | 2 | **Optional.** GPIO 25/26 use the ESP32's built-in pull-ups (≈ 45 kΩ), which are sufficient for most encoders. Add external 10 kΩ resistors only if signal quality is poor on long cable runs (> ~30 cm). |
| 5 | Status LED | Common-cathode RGB LED | 1 | Optional; connect via 330 Ω resistors to spare GPIO |
| 6 | Decoupling capacitor | 100 nF ceramic | 1 | Across ESP32 3.3 V / GND near encoder supply |
| 7 | Connector | JST-XH 4-pin (or similar) | 1 | For encoder cable to ESP32 |
| 8 | USB-C / Micro-USB cable | Power / programming | 1 | Matches ESP32 board variant |
| 9 | 3-pin dupont cable | Runout signal wiring | 1 | GPIO 27, GND, and optional 3.3 V |
| 10 | **OLED display** (optional) | **SSD1306 128×64 I²C** (0.96″, GND/VCC/SCL/SDA) | 1 | Shows live state, velocities, tick count, IP |

---

## 2. GPIO Pin Assignments

| GPIO | Direction | Signal | Notes |
|------|-----------|--------|-------|
| **25** | Input | Encoder Channel A (ChA) | Pull-up enabled; interrupt on CHANGE |
| **26** | Input | Encoder Channel B (ChB) | Pull-up enabled; interrupt on CHANGE |
| **27** | Output | Klipper runout signal | **Active LOW**; idle = HIGH, fault = LOW |
| **21** | I²C SDA | OLED display data | Default ESP32 hardware I²C SDA (optional) |
| **22** | I²C SCL | OLED display clock | Default ESP32 hardware I²C SCL (optional) |

> **All encoder and runout pins are configurable** at compile-time in `firmware/src/config.h`.
> OLED pins default to the ESP32's hardware I²C port (GPIO 21/22) but can be
> changed via `OLED_SDA_PIN` / `OLED_SCL_PIN` in `config.h`.

---

## 3. Encoder Wiring

### 3.1 Quadrature Encoder (generic)

```
Encoder Board          ESP32 Dev Board
──────────────         ──────────────
  VCC  ─────────────── 3.3V
  GND  ─────────────── GND
  ChA  ─────────────── GPIO 25  (internal pull-up enabled in firmware; no external resistor needed)
  ChB  ─────────────── GPIO 26  (internal pull-up enabled in firmware; no external resistor needed)
```

> **Internal pull-ups**: The firmware calls `pinMode(PIN_ENCODER_CHA, INPUT_PULLUP)` and `pinMode(PIN_ENCODER_CHB, INPUT_PULLUP)`, so the ESP32's built-in ≈ 45 kΩ pull-ups are always active.  External resistors are only needed if the encoder cable is longer than ≈ 30 cm and you observe signal glitches.

### 3.2 Reusing an old optical mouse

The simplest no-extra-hardware approach is to harvest the encoder mechanism from a **ball mouse** or any optical mouse that exposes quadrature A/B signals on its scroll-wheel encoder:

```
Old mouse encoder     ESP32 Dev Board
─────────────────     ──────────────
  VCC  ───────────── 3.3V
  GND  ───────────── GND
  ChA  ───────────── GPIO 25
  ChB  ───────────── GPIO 26
```

Quadrature signals are directly compatible with the firmware's ISR — no adapter required.

#### What about PMW3360 / PMW3389 SPI modules?

These sensors communicate over **SPI**, not quadrature.  The ESP32 has a native SPI peripheral, so no adapter MCU is needed — but the current firmware does not yet implement SPI reading.  If you want to use a PMW3360/PMW3389 module directly, the firmware's `encoder.cpp` would need to be extended with SPI polling code (the hardware wiring would use MISO/MOSI/SCK/CS pins instead of GPIO 25/26).  A pre-built mouse-sensor breakout that **already converts to quadrature output** can still be used with the current firmware unchanged.

---

## 4. OLED Display Wiring (Optional)

The firmware supports a standard **0.96″ SSD1306 OLED** module (128×64, I²C)
connected to the ESP32's hardware I²C port.  Typical 4-pin modules are labelled
**GND / VCC / SCL / SDA** and connect directly without any additional components.

```
SSD1306 OLED         ESP32 Dev Board
────────────         ──────────────
  GND  ─────────── GND
  VCC  ─────────── 3.3V   (most 0.96" modules accept 3.3–5 V; use 3.3V)
  SCL  ─────────── GPIO 22  (hardware I²C SCL, configurable via OLED_SCL_PIN)
  SDA  ─────────── GPIO 21  (hardware I²C SDA, configurable via OLED_SDA_PIN)
```

> **I²C address**: Most modules use **0x3C** (jumper = GND / default).
> If yours uses 0x3D (jumper = VCC), change `OLED_I2C_ADDR` in `config.h`.

### What is displayed

| Row | Content |
|-----|---------|
| Title bar | System state (`INIT` / `READY` / `PRINTING` / `FAULT` / …) – **blinks inverted when FAULT** |
| Row 1 | Encoder velocity (mm/s) |
| Row 2 | Extruder velocity from Moonraker (mm/s) |
| Row 3 | Cumulative tick count + direction symbol (`>` / `<` / `=`) |
| Row 4 | Sensor IP address (or `WiFi offline`) |

### Enabling / disabling

- **Compile-time**: comment out `#define ENABLE_OLED` in `config.h` to exclude
  all display code from the firmware (saves ~50 kB flash and 1 kB RAM).
- **Runtime**: toggle **Enable OLED display** in the web interface
  Configuration card → **Save & Apply** (persisted in NVS, no re-flash needed).

---

## 5. Runout Signal Wiring to Klipper Host

```
ESP32 GPIO 27  ──────────────── Klipper host GPIO (filament sensor pin)
ESP32 GND      ──────────────── Klipper host GND
```

Klipper `printer.cfg` snippet:

```ini
[filament_switch_sensor filament_runout]
switch_pin: ^!<HOST_GPIO_PIN>   ; pull-up enabled, active LOW
pause_on_runout: True
runout_gcode: FILAMENT_RUNOUT
```

Replace `<HOST_GPIO_PIN>` with the Raspberry Pi / host GPIO connected to
ESP32 GPIO 27 (e.g., `gpio21`).

---

## 6. Schematic (Simplified)

```
 Encoder ChA ───────────────── GPIO 25  [~45 kΩ internal pull-up to 3.3V]
 Encoder ChB ───────────────── GPIO 26  [~45 kΩ internal pull-up to 3.3V]
 Encoder VCC ───────────────── 3.3V
 Encoder GND ───────────────── GND

 GPIO 27 ──────────────────────────────── Klipper runout pin
 GND    ───────────────────────────────── Klipper GND
```

> External pull-up resistors (10 kΩ to 3.3 V) are optional and only needed for cable runs > ~30 cm.

---

## 7. Mechanical Mounting

- The encoder sensing wheel must contact the filament with light, consistent pressure.
- A printed guide body with an adjustable spring-loaded arm is recommended.
- Wheel diameter and gear ratio determine the **calibration factor** (mm/tick).
- After mounting, calibrate using the web interface (see User Instructions).

---

## 8. Power Supply

| Supply | Voltage | Current (typical) |
|--------|---------|------------------|
| ESP32 via USB | 5 V | < 250 mA |
| Encoder module | 3.3 V from ESP32 | < 20 mA |
| OLED display (SSD1306) | 3.3 V from ESP32 | < 20 mA (typical ~10 mA) |
| Total | 5 V USB | < 300 mA |

A standard 5 V / 0.5 A USB charger or the printer's USB port is sufficient.

---

## 9. Environmental Ratings

| Parameter | Value |
|-----------|-------|
| Operating temperature | 0 °C – 60 °C |
| Storage temperature | -20 °C – 70 °C |
| Humidity (non-condensing) | 10 – 80 % RH |
| Enclosure recommendation | Printed PLA enclosure with ventilation slots |
