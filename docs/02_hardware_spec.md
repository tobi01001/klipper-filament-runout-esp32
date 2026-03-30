# Filament Runout Sensor – Hardware Specification

**Version**: 1.1 | **Date**: 2026-03-30 | **Author**: tobi01001

---

## 1. Bill of Materials (BOM)

| # | Component | Value / Part | Qty | Notes |
|---|-----------|-------------|-----|-------|
| 1 | MCU | ESP32 Dev Board (38-pin, 4 MB Flash) | 1 | Any ESP32 with exposed GPIO 25/26/27/32 |
| 2 | Rotary encoder | **KY-040** rotary encoder module | 1 | 5-pin module: GND, +, SW, DT, CLK |
| 3 | Filament guide | Printed PLA/PETG guide body | 1 | Mounts encoder wheel against filament path |
| 4 | Pull-up resistors | 10 kΩ | 2 | **Optional.** GPIO 25/26/32 use the ESP32's built-in pull-ups (≈ 45 kΩ), which are sufficient for the KY-040. Add external 10 kΩ resistors only if signal quality is poor on long cable runs (> ~30 cm). |
| 5 | Status LED | Common-cathode RGB LED | 1 | Optional; connect via 330 Ω resistors to spare GPIO |
| 6 | Decoupling capacitor | 100 nF ceramic | 1 | Across ESP32 3.3 V / GND near encoder supply |
| 7 | Connector | JST-XH 5-pin (or similar) | 1 | For KY-040 cable to ESP32 |
| 8 | USB-C / Micro-USB cable | Power / programming | 1 | Matches ESP32 board variant |
| 9 | 3-pin dupont cable | Runout signal wiring | 1 | GPIO 27, GND, and optional 3.3 V |

---

## 2. GPIO Pin Assignments

| GPIO | Direction | Signal | Notes |
|------|-----------|--------|-------|
| **25** | Input | KY-040 CLK (Channel A) | Pull-up enabled; interrupt on CHANGE |
| **26** | Input | KY-040 DT  (Channel B) | Pull-up enabled; interrupt on CHANGE |
| **32** | Input | KY-040 SW  (push-button) | Pull-up enabled; active LOW – press to reset fault |
| **27** | Output | Klipper runout signal | **Active LOW**; idle = HIGH, fault = LOW |

> **All pins are configurable** at compile-time in `firmware/src/config.h`.

---

## 3. Encoder Wiring

### 3.1 KY-040 Rotary Encoder Module

The KY-040 is a 5-pin breakout board with a mechanical rotary encoder and an
integrated push-button switch.

```
KY-040 Pin     ESP32 Dev Board
──────────     ──────────────
  GND  ──────── GND
  +    ──────── 3.3 V
  SW   ──────── GPIO 32  (internal pull-up; press to reset a runout fault)
  DT   ──────── GPIO 26  (Channel B – internal pull-up)
  CLK  ──────── GPIO 25  (Channel A – internal pull-up)
```

> **Internal pull-ups**: The firmware calls `pinMode(PIN_ENC_x, INPUT_PULLUP)`
> for all three signal pins, so the ESP32's built-in ≈ 45 kΩ pull-ups are
> always active.  External resistors are only needed if the encoder cable is
> longer than ≈ 30 cm and you observe signal glitches.

**Push-button function**: Pressing the KY-040 knob clears an active runout
fault (same as the "Reset Fault" button in the web interface).

### 3.2 Generic Quadrature Encoder (alternative)

```
Encoder Board          ESP32 Dev Board
──────────────         ──────────────
  VCC  ─────────────── 3.3V
  GND  ─────────────── GND
  ChA  ─────────────── GPIO 25
  ChB  ─────────────── GPIO 26
```

> GPIO 32 (SW) can be left disconnected when using a generic encoder without
> a built-in button.

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
 KY-040 CLK ─────────────────── GPIO 25  [~45 kΩ internal pull-up to 3.3V]
 KY-040 DT  ─────────────────── GPIO 26  [~45 kΩ internal pull-up to 3.3V]
 KY-040 SW  ─────────────────── GPIO 32  [~45 kΩ internal pull-up to 3.3V]
 KY-040 +   ─────────────────── 3.3V
 KY-040 GND ─────────────────── GND

 GPIO 27 ──────────────────────────────── Klipper runout pin
 GND    ───────────────────────────────── Klipper GND
```

> External pull-up resistors (10 kΩ to 3.3 V) are optional and only needed for cable runs > ~30 cm.

---

## 6. Mechanical Mounting

- The encoder wheel must contact the filament with light, consistent pressure.
- A printed guide body with an adjustable spring-loaded arm is recommended.
- Wheel diameter and gear ratio determine the **calibration factor** (mm/tick).
- After mounting, calibrate using the web interface (see User Instructions).

---

## 7. Power Supply

| Supply | Voltage | Current (typical) |
|--------|---------|------------------|
| ESP32 via USB | 5 V | < 250 mA |
| KY-040 module | 3.3 V from ESP32 | < 5 mA |
| Total | 5 V USB | < 260 mA |

A standard 5 V / 0.5 A USB charger or the printer's USB port is sufficient.

---

## 8. Environmental Ratings

| Parameter | Value |
|-----------|-------|
| Operating temperature | 0 °C – 60 °C |
| Storage temperature | -20 °C – 70 °C |
| Humidity (non-condensing) | 10 – 80 % RH |
| Enclosure recommendation | Printed PLA enclosure with ventilation slots |
