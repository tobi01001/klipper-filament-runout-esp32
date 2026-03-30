# Filament Runout Sensor – Hardware Specification

**Version**: 1.0 | **Date**: 2026-03-30 | **Author**: tobi01001

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

---

## 2. GPIO Pin Assignments

| GPIO | Direction | Signal | Notes |
|------|-----------|--------|-------|
| **25** | Input | Encoder Channel A (ChA) | Pull-up enabled; interrupt on CHANGE |
| **26** | Input | Encoder Channel B (ChB) | Pull-up enabled; interrupt on CHANGE |
| **27** | Output | Klipper runout signal | **Active LOW**; idle = HIGH, fault = LOW |

> **All three pins are configurable** at compile-time in `firmware/src/config.h`.

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

> **Internal pull-ups**: The firmware calls `pinMode(PIN_ENC_A, INPUT_PULLUP)` and `pinMode(PIN_ENC_B, INPUT_PULLUP)`, so the ESP32's built-in ≈ 45 kΩ pull-ups are always active.  External resistors are only needed if the encoder cable is longer than ≈ 30 cm and you observe signal glitches.

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

## 4. Runout Signal Wiring to Klipper Host

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

## 5. Schematic (Simplified)

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

## 6. Mechanical Mounting

- The encoder sensing wheel must contact the filament with light, consistent pressure.
- A printed guide body with an adjustable spring-loaded arm is recommended.
- Wheel diameter and gear ratio determine the **calibration factor** (mm/tick).
- After mounting, calibrate using the web interface (see User Instructions).

---

## 7. Power Supply

| Supply | Voltage | Current (typical) |
|--------|---------|------------------|
| ESP32 via USB | 5 V | < 250 mA |
| Encoder module | 3.3 V from ESP32 | < 20 mA |
| Total | 5 V USB | < 280 mA |

A standard 5 V / 0.5 A USB charger or the printer's USB port is sufficient.

---

## 8. Environmental Ratings

| Parameter | Value |
|-----------|-------|
| Operating temperature | 0 °C – 60 °C |
| Storage temperature | -20 °C – 70 °C |
| Humidity (non-condensing) | 10 – 80 % RH |
| Enclosure recommendation | Printed PLA enclosure with ventilation slots |
