# Filament Runout Sensor – User Instructions

**Version**: 1.1 | **Date**: 2026-03-31 | **Author**: tobi01001

---

## Prerequisites

| Item | Requirement |
|------|-------------|
| ESP32 dev board | Any 38-pin variant with GPIO 25, 26, 27 available |
| Optical / quadrature encoder | Mounted in the filament path (see HW spec) |
| **SSD1306 OLED** (optional) | 0.96″ 128×64 I²C module, connected to GPIO 21 (SDA) / 22 (SCL) |
| PlatformIO | Installed in VS Code or standalone CLI |
| Python | ≥ 3.7 (required by PlatformIO) |
| WiFi network | 2.4 GHz, WPA2 |
| Klipper host | Running Moonraker ≥ 0.8 on the same local network |

---

## Step 1 – Clone & Open the Project

```bash
cd ~/ender5-pro-klipper/esp32-filament-sensor/firmware
pio project init   # (if you need to regenerate .pio cache)
```

Or open the `firmware/` folder directly in VS Code with the PlatformIO
extension installed.

---

## Step 1a – OLED Display Setup (Optional)

If you have a **0.96″ SSD1306 OLED module** (128×64, I²C), connect it to
the ESP32 before flashing:

| OLED pin | ESP32 pin | Notes |
|----------|-----------|-------|
| GND | GND | Common ground |
| VCC | 3.3 V | **Use 3.3 V**, not 5 V, to match ESP32 logic |
| SCL | GPIO 22 | Hardware I²C clock |
| SDA | GPIO 21 | Hardware I²C data |

> **I²C address**: the jumper on the back of the board selects the address.
> Default is `0x3C` (jumper to GND).  If your module uses `0x3D` (jumper to VCC),
> change `OLED_I2C_ADDR` in `firmware/src/config.h` before flashing.

The display is **enabled by default** and shows:
- System state (blinking inverted on FAULT)
- Encoder velocity (mm/s)
- Extruder velocity from Moonraker (mm/s)
- Cumulative tick count + motion direction
- Sensor IP address

To **disable the display at compile time** (saves ~50 kB flash), comment out
`#define ENABLE_OLED` in `config.h`.

To **disable the display at runtime** without re-flashing, open the web UI,
uncheck **Enable OLED display**, and click **Save & Apply**.

---

## Step 2 – First-Boot WiFi Setup (AP Mode)

On first boot the ESP32 has no saved WiFi credentials and starts in
**Access Point mode**:

| Setting | Value |
|---------|-------|
| SSID | `FilamentSensor` |
| Password | `sensor1234` |
| Web UI URL | `http://192.168.4.1` |

1. Connect your phone or laptop to the `FilamentSensor` WiFi network.
2. Open `http://192.168.4.1` in a browser.
3. Scroll to **Configuration** → enter your home WiFi **SSID** and **Password**.
4. Enter the **Moonraker IP** and **port** (default `7125`).
5. Click **Save & Apply**.
6. Power-cycle the ESP32 – it will now connect to your home network.

---

## Step 3 – Find the Sensor IP Address

After restarting in station mode, the sensor's IP address is shown in the
web interface title and in the serial monitor:

```
[WiFi] Connected, IP: 192.168.1.42
[WEB]  HTTP server started on port 80
```

Bookmark `http://192.168.1.42` or assign a static DHCP lease in your router.

---

## Step 4 – Calibrate the Encoder

The **calibration factor** converts raw encoder ticks to millimetres of
filament movement.  Accurate calibration prevents false positives and gives
meaningful velocity readings.

### Automatic calibration procedure

1. Open the web UI (`http://<sensor-ip>`).
2. Heat the nozzle to printing temperature.
3. **Manually push exactly 100 mm of filament** through the sensor (use calipers
   or mark the filament with a marker 100 mm apart).
4. Note the **Tick count** displayed on the status page before and after.
5. Calculate:

   ```
   cal_factor = 100.0 / |tick_after − tick_before|   (mm/tick)
   ```

6. Enter the calculated value in **Calibration factor (mm/tick)** and click
   **Save & Apply**.

### Example

| | Value |
|-|-------|
| Ticks before push | 0 |
| Ticks after pushing 100 mm | 8 340 |
| Calculated cal_factor | `100 / 8340 ≈ 0.01199 mm/tick` |

---

## Step 5 – Configure Klipper

Add the following to your `printer.cfg` (adjust the GPIO pin to match the
Raspberry Pi pin connected to ESP32 GPIO 27):

```ini
[filament_switch_sensor filament_runout]
switch_pin: ^!gpio21       # active-LOW; adjust pin for your host board
pause_on_runout: True
runout_gcode:
    M118 Filament runout detected!
    PAUSE
insert_gcode:
    M118 Filament inserted
```

Restart Klipper:

```bash
sudo systemctl restart klipper
```

Verify in Mainsail / Fluidd that the **filament_runout** sensor shows as
*Detected: False* during normal operation.

---

## Step 6 – Adjust Fault Detection Parameters

In the web UI **Configuration** section:

| Parameter | Description | Recommended range |
|-----------|-------------|------------------|
| **Calibration factor (mm/tick)** | Physical conversion (set in Step 4) | 0.001 – 0.1 |
| **Fault timeout (ms)** | How long the filament can be stopped before alarm | 1 500 – 3 000 ms |
| **Min extruder velocity (mm/s)** | Below this, sensor ignores encoder (printer idle) | 0.3 – 1.0 |
| **Motion threshold (ticks)** | Minimum movement to reset idle timer | 1 – 5 |
| **Moonraker IP** | IP / hostname of Klipper host | e.g. `192.168.1.100` |
| **Moonraker port** | Moonraker API port | `7125` (default) |

---

## Step 7 – Test the Sensor

### 7.1 Normal print test

1. Start a print.
2. Watch the web UI: **State** should change to `PRINTING` once the extruder
   velocity exceeds the threshold.
3. **Encoder velocity** should closely match the reported **Extruder velocity**.

### 7.2 Fault simulation test

With a print running:

1. Pinch the filament firmly so it cannot move (do not cut it!).
2. Wait for **Fault timeout** seconds.
3. Confirm:
   - Web UI shows `FAULT` state.
   - GPIO 27 pulls LOW (measure with multimeter if needed).
   - Klipper pauses the print.

4. Release the filament, click **Reset Fault** in the web UI, then resume the
   print in Mainsail / Fluidd.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| State stays `WIFI_CONN` forever | Wrong SSID / password | Connect to AP mode, reconfigure |
| `enc_vel` always 0 | Encoder not wired / wrong pins | Check GPIO 25/26 wiring |
| Frequent false faults during printing | Timeout too short or cal_factor wrong | Increase timeout; recalibrate |
| No fault on empty spool | Timeout too long | Decrease `timeout_ms` |
| Web UI not reachable | IP changed | Check router DHCP table or serial monitor |
| `ext_vel` always 0 | Moonraker IP/port wrong or Klipper offline | Ping Moonraker from browser |
| OLED blank / no display | Display not found or disabled | Check wiring (SDA/SCL/VCC/GND); verify address in `config.h`; check serial for `[OLED] WARNING` |
| OLED shows "WiFi offline" | WiFi not connected | Normal during AP mode or reconnecting; check WiFi config |
| OLED title blinks rapidly | `FAULT` state active | Click **Reset Fault** in web UI after fixing the filament issue |

---

## Serial Debug Output

Connect a USB serial terminal at **115 200 baud** for real-time logs:

```
[MAIN] ESP32 Filament Runout Sensor booting…
[NVS]  Configuration loaded from NVS
[FD]   Fault detector initialised (runout pin 27)
[C0]   Core 0 task started
[C1]   Core 1 task started
[ENC]  Encoder initialised (ChA=25, ChB=26)
[WiFi] Connecting to 'MyNetwork' …
[WiFi] Connected, IP: 192.168.1.42
[WEB]  HTTP server started on port 80
[OLED] SSD1306 initialised (128x64) at I2C 0x3c
[FSM]  READY → PRINTING
[FSM]  PRINTING → FAULT
[FAULT] Filament runout detected! GPIO 27 → LOW
[FD]   Fault cleared, GPIO 27 → HIGH
[FSM]  FAULT → READY
```

Log prefix key: `[MAIN]` boot, `[C0]`/`[C1]` cores, `[ENC]` encoder,
`[MR]` Moonraker, `[FD]` fault detector, `[FSM]` state transitions,
`[NVS]` storage, `[WEB]` HTTP server.

---

## Firmware Update

```bash
cd ~/ender5-pro-klipper/esp32-filament-sensor/firmware
pio run --target upload
```

NVS configuration is preserved across firmware updates unless you explicitly
erase the flash:

```bash
pio run --target erase   # wipes everything including NVS
```
