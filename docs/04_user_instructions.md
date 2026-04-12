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

## Step 1b – DHT22 Environment Sensor Setup (Optional)

The **DHT22 (AM2302)** sensor adds ambient temperature and humidity monitoring.
It connects directly to the ESP32 with three wires and requires no additional
components if you use a breakout module (which includes the pull-up resistor).

| DHT22 module pin | ESP32 pin | Notes |
|-----------------|-----------|-------|
| GND | GND | Common ground |
| VCC | 3.3 V | Most breakout modules accept 3.3–5 V |
| DATA | GPIO 4 | Default; runtime configurable without reflashing |

> If you are using a **bare DHT22 sensor** (no PCB breakout), add a
> **10 kΩ pull-up resistor** from the DATA pin to 3.3 V.

### Enabling / disabling DHT22 support

DHT22 support is **compiled in by default**.

- **Enable at runtime** (no reflash): web UI → **Configuration** →
  toggle **Enable DHT22 sensor** → **Save & Apply**.
- **Disable at compile time** (removes all DHT22 code, saves ~15 kB flash):
  add `-DDISABLE_DHT` to `build_flags` in `platformio.ini`.

### Changing the DHT22 data pin

The default data pin is **GPIO 4**.  To use a different pin:

1. Open `http://<sensor-ip>` in your browser.
2. Scroll to the **Pin Configuration** card.
3. Change the **DHT22 data** field.
4. Click **Save & Reboot** – the new assignment is saved to NVS and
   survives subsequent reboots.

### Reading DHT22 data

Once wired and enabled, DHT22 readings appear in:

| Location | How to access |
|----------|---------------|
| Web UI | Live values on the dashboard |
| REST API | `GET http://<sensor-ip>/api/status` → fields `dht_temp`, `dht_humidity`, `dht_valid` |
| REST API (DHT only) | `GET http://<sensor-ip>/api/dht` → `{"enabled":true,"valid":true,"temp":23.4,"humidity":45.2}` |

Serial log (DEBUG_LOG_ENABLED build):

```
[DHT] DHT22 initialised on GPIO 4
[DHT] T=23.4°C H=45.2%
```

If `dht_valid` is `false` in the API response, check wiring and confirm
the data pin matches the runtime or compile-time pin configuration.

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

### Option A – GPIO pin (classic wired approach)

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

### Option B – Moonraker HTTP Sensor (no wiring required)

This option uses the ESP32's existing Wi-Fi connection.  When it works, sensor
data appears in the Mainsail **Sensors** panel and can be queried via the
Moonraker REST API.

> **⚠️ Moonraker compatibility warning**
>
> The `type: http` sensor type in `[sensor]` is **not supported** by most
> current Moonraker installations.  If you see any of these warnings in your
> Moonraker log after adding the sensor block, `type: http` is not available
> on your Moonraker build:
>
> ```
> Failed to configure sensor [sensor esp32_filament_runout]
> Unsupported sensor type: http
>
> Unparsed config option 'url: http://...' detected in section [sensor esp32_filament_runout]
> Unparsed config option 'timeout: 3.0' detected ...
> Unparsed config option 'poll_rate: 5.0' detected ...
> ```
>
> **If you see those warnings:**
> - Remove or comment out the entire `[sensor esp32_filament_runout]` section
>   from `moonraker.conf` to stop the warnings.
> - Fault detection and pause-on-runout still work fully via:
>   - **Option A** (GPIO wire) – see above.
>   - **Fault G-code** – the ESP32 sends a configurable G-code command directly
>     to Klipper via the Moonraker WebSocket the moment a fault is detected
>     (see [Step 5c – Fault G-code](#step-5c--fault-gcode-configuration) below).
> - DHT22 temperature and humidity data can still be read directly from the
>   ESP32 web UI or REST API (`GET http://<sensor-ip>/api/dht`).

#### Prerequisites (Option B, when `type: http` IS supported)

| Item | Notes |
|------|-------|
| Moonraker that supports `type: http` | Check by adding the block; if no warnings appear it is supported |
| `curl` on the Klipper host | `sudo apt install curl` |
| `gcode_shell_command.py` | Install via KIAUH *Advanced → Install gcode shell command*, or see below |

**Install `gcode_shell_command.py` manually** (if not already present):

```bash
wget -O ~/klipper/klippy/extras/gcode_shell_command.py \
  https://raw.githubusercontent.com/dw-0/kiauh/master/resources/gcode_shell_command.py
sudo systemctl restart klipper
```

#### 1. moonraker.conf

Copy the contents of `klipper/moonraker.conf` from this repository into your
`moonraker.conf`, replacing `<ESP32_IP>` with the sensor's IP address.

The `[sensor esp32_filament_runout]` block is commented out by default in the
shipped `klipper/moonraker.conf` because of the compatibility issue above.
Uncomment it only if your Moonraker does not show the warnings listed above:

```ini
# Uncomment if your Moonraker supports type: http (no warnings appear)
# [sensor esp32_filament_runout]
# type: http
# url: http://192.168.1.42/api/status   # ← your ESP32 IP
# timeout: 3.0
# poll_rate: 5.0
#
# parameter_sensor_enabled:
#     units: bool
# parameter_fault:
#     units: bool
#     history_field: filament_fault
# parameter_enc_vel:
#     units: mm/s
#     history_field: encoder_velocity
# parameter_ext_vel:
#     units: mm/s
#     history_field: extruder_velocity
# parameter_motion_ago_ms:
#     units: ms
# parameter_nozzle_temp:
#     units: °C
#     history_field: nozzle_temperature
# parameter_dht_temp:
#     units: °C
#     history_field: ambient_temperature
# parameter_dht_humidity:
#     units: %RH
#     history_field: ambient_humidity
```

The `[shell_command]` blocks (enable/disable/reset) do **not** require `type: http`
and should always be added:

```ini
[shell_command filament_sensor_enable]
command: curl -s -X POST -H "Content-Type: application/json" -d '{"enabled":true,"persist":false}' http://192.168.1.42/api/sensor
timeout: 5.0
verbose: False

[shell_command filament_sensor_disable]
command: curl -s -X POST -H "Content-Type: application/json" -d '{"enabled":false,"persist":false}' http://192.168.1.42/api/sensor
timeout: 5.0
verbose: False

[shell_command filament_sensor_reset]
command: curl -s -X POST http://192.168.1.42/api/reset
timeout: 5.0
verbose: False
```

#### 2. printer.cfg macros

Copy the macros from `klipper/printer.cfg` in this repository (or add them
manually):

```ini
[gcode_macro FILAMENT_SENSOR_ENABLE]
description: Enable the ESP32 filament runout sensor
gcode:
    RUN_SHELL_COMMAND CMD=filament_sensor_enable
    M118 ESP32 filament sensor: ENABLED

[gcode_macro FILAMENT_SENSOR_DISABLE]
description: Disable the ESP32 filament runout sensor
gcode:
    RUN_SHELL_COMMAND CMD=filament_sensor_disable
    M118 ESP32 filament sensor: DISABLED

[gcode_macro FILAMENT_SENSOR_RESET]
description: Reset an active filament fault on the ESP32 sensor
gcode:
    RUN_SHELL_COMMAND CMD=filament_sensor_reset
    M118 ESP32 filament sensor: fault RESET
```

Restart Klipper and Moonraker:

```bash
sudo systemctl restart klipper moonraker
```

#### 3. Using the macros

The three macros are available as buttons in Mainsail and can be called from
G-code:

| Macro | When to use |
|-------|-------------|
| `FILAMENT_SENSOR_ENABLE` | In `PRINT_START` to arm the sensor |
| `FILAMENT_SENSOR_DISABLE` | In `PRINT_END` or during filament changes |
| `FILAMENT_SENSOR_RESET` | After clearing a runout, before `RESUME` |

> **persist flag**: `persist: false` applies the enable/disable only in the
> ESP32's RAM and is lost on reboot.  Change to `"persist":true` in
> `moonraker.conf` if you want the state to survive power cycles.

---

## Step 5c – Fault G-code Configuration

When the ESP32 detects a fault it sends a configurable G-code command **directly
to Klipper** via its Moonraker WebSocket connection.  This works regardless of
whether you use the GPIO wiring or the Moonraker sensor – as long as the ESP32
can reach Moonraker on the network.

The default command is `PAUSE`.  To change it:

1. Open `http://<sensor-ip>` in your browser.
2. Scroll to **Configuration** → **Fault G-code command**.
3. Enter your desired command, for example:
   - `PAUSE` (default) — pauses the print
   - `FILAMENT_RUNOUT_HANDLER` — calls a custom macro
   - `M600` — triggers a filament-change sequence
4. Click **Save & Apply**.

You can also change it permanently via the API (survives reboots):

```bash
curl -X POST http://<sensor-ip>/api/config \
     -H 'Content-Type: application/json' \
     -d '{"fault_gcode":"FILAMENT_RUNOUT_HANDLER"}'
```

### Example custom runout macro in printer.cfg

```ini
[gcode_macro FILAMENT_RUNOUT_HANDLER]
description: Custom action triggered by ESP32 fault G-code on filament runout
gcode:
    M118 Filament runout detected – pausing print
    PAUSE
    # Optional: beep notification
    # M300 S440 P500
```

> **Note**: The fault G-code is sent over the WebSocket only when the ESP32
> is connected to Moonraker.  If network connectivity is lost, the GPIO runout
> signal (GPIO 27 → LOW) still fires independently and Klipper handles it via
> the `[filament_switch_sensor]` block (Option A).

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
| `dht_valid: false` in `/api/status` | DHT22 not wired, wrong pin, or no pull-up | Check DATA wire and GPIO assignment in Pin Configuration; bare sensor needs 10 kΩ pull-up to 3.3 V |
| DHT22 fields absent from `/api/status` | DHT22 compiled out | Firmware was built with `-DDISABLE_DHT`; rebuild without that flag |
| Moonraker warnings about `type: http` | Moonraker build does not support HTTP sensors | Remove `[sensor esp32_filament_runout]` from `moonraker.conf`; use GPIO (Option A) and fault G-code for runout handling |
| Fault detected but print not pausing | `fault_gcode` not set or Moonraker WebSocket disconnected | Check **Fault G-code command** in web UI; ensure Moonraker IP/port are correct; verify GPIO wiring for failsafe |

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
