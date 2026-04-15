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
# parameter_fault:
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

## Step 5b – WiFi-only Integration (no `type: http`, no GPIO wire)

If your Moonraker does not support `type: http` **and** you prefer not to run a
GPIO wire between the ESP32 and the Raspberry Pi, all sensor functions are still
available over WiFi using standard shell commands.  This is the recommended
approach for most users on current Moonraker builds.

### How it works

| What you want | How it is delivered |
|---------------|---------------------|
| Pause the print when filament runs out | ESP32 sends the configured **fault G-code** (default `PAUSE`) **directly to Klipper** via its Moonraker WebSocket connection the instant a fault is detected — no extra config required |
| Enable / disable sensor from macros | `FILAMENT_SENSOR_ENABLE` / `FILAMENT_SENSOR_DISABLE` macros call `curl` shell commands |
| Reset a fault and resume printing | `FILAMENT_SENSOR_RESET` macro calls a `curl` shell command |
| Read DHT22 ambient temperature + humidity | `READ_AMBIENT_SENSOR` macro queries `/api/dht` and prints the result to the Mainsail console |
| Check sensor state on demand | `READ_SENSOR_STATUS` macro queries `/api/status` and prints state, fault flag, encoder velocity, and nozzle temperature |

> **Prerequisite**: the ESP32 must have a valid Moonraker IP and port configured
> (web UI → **Configuration** → Moonraker IP / Port).  The ESP32 opens its own
> WebSocket to Moonraker on boot; when a fault is detected it sends the fault
> G-code without any polling or extra setup on the Klipper side.

---

### Prerequisites

| Item | Notes |
|------|-------|
| `curl` on the Klipper host | `sudo apt install curl` |
| `gcode_shell_command.py` | Install via KIAUH *Advanced → Install gcode shell command*, or manually (see Option B above) |

---

### 1. moonraker.conf

Add the shell commands from `klipper/moonraker.conf` (sections 2 and 3) to your
`moonraker.conf`, replacing `<ESP32_IP>` with the sensor's IP address:

```ini
# ── Sensor control (already in section 2) ──────────────────────────────────
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

# ── Sensor monitoring (section 3) ──────────────────────────────────────────
# Reads DHT22 temperature + humidity; verbose:True prints to G-code console
[shell_command read_dht_sensor]
command: bash -c "curl -sf --max-time 3 http://192.168.1.42/api/dht | python3 -c \"import json,sys; d=json.load(sys.stdin); print('Ambient: ' + str(round(d['temp'],1)) + '\u00b0C  ' + str(round(d['humidity'],1)) + '%RH') if d.get('valid') else print('DHT22: no valid reading (check wiring and pin config)')\""
timeout: 8.0
verbose: True

# Reads full sensor status (state, fault, encoder velocity, nozzle temperature)
[shell_command read_sensor_status]
command: bash -c "curl -sf --max-time 3 http://192.168.1.42/api/status | python3 -c \"import json,sys; s=json.load(sys.stdin); print('Sensor: state=' + s.get('state','?') + '  fault=' + str(s.get('fault',False)) + '  enc=' + str(round(s.get('enc_vel',0),2)) + 'mm/s  nozzle=' + str(round(s.get('nozzle_temp',0),1)) + '\u00b0C')\""
timeout: 8.0
verbose: True
```

---

### 2. printer.cfg macros

Add the macros from `klipper/printer.cfg` to your `printer.cfg`:

```ini
# ── Sensor arm / disarm / reset ────────────────────────────────────────────
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

# ── Ambient sensor monitoring ───────────────────────────────────────────────
[gcode_macro READ_AMBIENT_SENSOR]
description: Print DHT22 temperature and humidity to the G-code console
gcode:
    RUN_SHELL_COMMAND CMD=read_dht_sensor

[gcode_macro READ_SENSOR_STATUS]
description: Print ESP32 sensor state, fault flag, and velocities to the console
gcode:
    RUN_SHELL_COMMAND CMD=read_sensor_status
```

Restart Klipper and Moonraker:

```bash
sudo systemctl restart klipper moonraker
```

---

### 3. PRINT_START / PRINT_END integration

```ini
# [gcode_macro PRINT_START]
# gcode:
#     # … your existing PRINT_START gcode …
#     FILAMENT_SENSOR_RESET    # clear any leftover fault from a previous print
#     FILAMENT_SENSOR_ENABLE   # arm the ESP32 sensor (WiFi command, no wiring)
#     READ_AMBIENT_SENSOR      # log ambient temperature + humidity to console

# [gcode_macro PRINT_END]
# gcode:
#     # … your existing PRINT_END gcode …
#     FILAMENT_SENSOR_DISABLE  # disarm sensor while idle
```

---

### 4. Runout fault behaviour (WiFi-only)

When the ESP32 detects a runout:

1. The ESP32 sends the configured **fault G-code** (`PAUSE` by default) directly
   to Klipper via the Moonraker WebSocket.  The print pauses **without** any
   polling delay — response time is the same as Option A.
2. GPIO 27 pulls LOW at the same instant (hardware fallback).
3. The Mainsail dashboard shows the sensor state as `FAULT`.

To resume after fixing the filament:

```gcode
FILAMENT_SENSOR_RESET   ; clears FAULT state on the ESP32
RESUME                  ; resume the paused print
```

Or use the **Reset Fault** button in the ESP32 web UI (`http://<sensor-ip>`).

---

### 5. DHT22 data in the console

Call `READ_AMBIENT_SENSOR` (or `READ_SENSOR_STATUS`) from a macro or the
Mainsail console.  Because the shell commands use `verbose: True`, the output
appears directly in the Mainsail / Fluidd G-code console:

```
// Ambient: 23.4°C  45.2%RH
// Sensor: state=PRINTING  fault=False  enc=3.47mm/s  nozzle=215.0°C
```

The data can also be queried at any time with `curl` directly:

```bash
curl http://<sensor-ip>/api/dht
# {"enabled":true,"valid":true,"temp":23.4,"humidity":45.2}

curl http://<sensor-ip>/api/status
# {"state":"PRINTING","fault":false,"enc_vel":3.47,"nozzle_temp":215.0,...}
```

> **DHT22 not in the Sensors panel?**  The Mainsail Sensors panel is populated
> only by Moonraker's `[sensor]` block.  Without `type: http` support, the DHT22
> readings are available in the console (via `READ_AMBIENT_SENSOR`) and the ESP32
> web UI, but not as a Mainsail chart/history entry.
> See [Step 5d – MQTT Integration](#step-5d--mqtt-integration) for a
> path that restores full Sensors panel support without `type: http`.

---

### 6. Why `RUN_SHELL_COMMAND` does not block printing

**Short answer:** the `[shell_command]` mechanism in `moonraker.conf` is
executed by Moonraker's asyncio event loop in a subprocess — the Klipper
G-code queue does briefly pause at `RUN_SHELL_COMMAND` until the command
returns, but for a local-network `curl` call this takes ~100 ms or less,
which is far shorter than the motion look-ahead buffer.  In practice the
print never pauses visibly.

**Detailed explanation:**

| Layer | What happens |
|-------|-------------|
| Klipper macro | `RUN_SHELL_COMMAND CMD=read_dht_sensor` sends a request to Moonraker's HTTP API |
| Moonraker | Launches `curl` as an asyncio subprocess; this is non-blocking to Moonraker's own event loop |
| Klipper G-code queue | Pauses at `RUN_SHELL_COMMAND` until Moonraker streams back the result (typically 50–200 ms on a local network) |
| Klipper motion system | Continues executing already-planned moves from the look-ahead buffer — the printer **keeps moving** during this pause |

The previous blocking experience typically happens when:
- A shell command is called **repeatedly during active printing** (e.g., inside
  a `[delayed_gcode]` that fires every 5 s), causing the G-code queue to stall
  periodically.
- The subprocess has no timeout and the target is unreachable (hangs for 30+ s).
- A Klipper-side `[gcode_shell_command]` (Klipper extra, not Moonraker) is
  used — its subprocess runs directly in Klipper's reactor and can stall the
  G-code queue if it takes too long.

**The approach used here avoids these pitfalls:**
- `READ_AMBIENT_SENSOR` is designed to be called from `PRINT_START` — once,
  before active extrusion starts — so the G-code queue pause is irrelevant.
- `curl --max-time 3` ensures the command always returns within 3 s even if
  the ESP32 is unreachable, limiting the worst-case stall.
- `FILAMENT_SENSOR_ENABLE/DISABLE/RESET` are also called only at print
  start/end where a brief G-code pause is harmless.

> **Do not** call `READ_AMBIENT_SENSOR` or `READ_SENSOR_STATUS` from a
> `[delayed_gcode]` that fires during active printing.  For periodic monitoring
> without blocking, use the MQTT integration described in Step 5d.

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

## Step 5d – MQTT Integration

MQTT integration is now **fully implemented** in the firmware.  The ESP32 publishes
sensor state to an MQTT broker and subscribes to command topics, enabling
bidirectional control from Moonraker automations, FHEM, Home Assistant, Node-RED,
or any MQTT-capable system.

---

### Topic Map

All topics share a configurable **topic prefix** (default `esp32/filament_sensor`).

| Topic | Direction | Payload | Retained |
|-------|-----------|---------|----------|
| `<prefix>/state` | ESP32 → broker | JSON object (see below) | ✓ |
| `<prefix>/availability` | ESP32 → broker | `"online"` or `"offline"` (LWT) | ✓ |
| `<prefix>/cmd/enable` | broker → ESP32 | `"1"` / `"true"` / `"on"` to enable; `"0"` / `"false"` / `"off"` to disable | — |
| `<prefix>/cmd/reset` | broker → ESP32 | Any payload (e.g. `"1"`) | — |

#### State payload example

```json
{
  "state":          "PRINTING",
  "fault":          false,
  "sensor_enabled": true,
  "enc_vel":        3.47,
  "ext_vel":        3.50,
  "motion_ago_ms":  350,
  "nozzle_temp":    215.0,
  "dht_temp":       23.4,
  "dht_humidity":   45.2,
  "dht_valid":      true
}
```

State is published:
- Immediately on every state change (fault, reset, enable/disable)
- As a heartbeat every **5 seconds** while connected

---

### 1. Configure the broker in the web UI

Open the device web UI → **MQTT** tab and fill in:

| Field | Description |
|-------|-------------|
| **Broker hostname / IP** | IP or hostname of your MQTT broker |
| **Port** | Default `1883`; use `8883` for TLS (not yet supported) |
| **Username / Password** | Leave blank for anonymous brokers |
| **Topic prefix** | Default `esp32/filament_sensor`; change to avoid collisions |
| **Enable MQTT client** | Tick to activate |

Click **Save & Apply**.  The page shows **connected ✓** once the broker accepts
the connection.

> **Note**: Changes take effect immediately — no reboot required.

---

### 2. MQTT Broker options

#### Option A – Mosquitto on Raspberry Pi (recommended)

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable --now mosquitto
```

Verify by listening for ESP32 messages:

```bash
mosquitto_sub -t "esp32/filament_sensor/#" -v
```

#### Option B – FHEM MQTT2_SERVER (no separate broker needed)

FHEM can act as the broker itself.  Add to `fhem.cfg`:

```perl
define mqtt2srv MQTT2_SERVER 1883 global
attr   mqtt2srv room Klipper
```

FHEM clients then connect to `<FHEM_HOST>:1883`.

---

### 3. FHEM Integration (MQTT2_SERVER)

Add the following to your `fhem.cfg` (or use the FHEM web UI → Edit files):

```perl
# ── Auto-create ESP32 device from retained state topic ──────────────────────
define filament_sensor MQTT2_DEVICE
attr   filament_sensor IODev      mqtt2srv
attr   filament_sensor readingList esp32/filament_sensor/state:.* { \
    my %d = %{json2nameValue($EVENT)}; \
    map { ("r:$_", $d{$_}) } keys %d \
}
attr   filament_sensor setList \
    enable:1,0         esp32/filament_sensor/cmd/enable $VALUE \
    reset:noArg        esp32/filament_sensor/cmd/reset  1
attr   filament_sensor room Klipper
attr   filament_sensor icon it_wifi
```

This maps every key in the JSON payload to a FHEM reading (`r:state`,
`r:fault`, `r:enc_vel`, etc.) and adds two set commands:

```
set filament_sensor enable 1    # enable the sensor
set filament_sensor enable 0    # disable the sensor
set filament_sensor reset       # clear an active fault
```

#### FHEM availability indicator

```perl
attr filament_sensor readingList \
    esp32/filament_sensor/availability:.* r:availability
```

`r:availability` will be `online` or `offline`.

#### FHEM notify on fault

```perl
define n_filament_fault notify filament_sensor:r:fault:.* {
    if (ReadingsVal("filament_sensor","r:fault","false") eq "true") {
        fhem("set alarm_siren on");   # replace with your action
    }
}
```

---

### 4. Moonraker / Klipper Integration

Add the following to `moonraker.conf`, replacing `<BROKER_IP>` with the
broker IP (usually `localhost` or `127.0.0.1` if Mosquitto runs on the
Raspberry Pi):

```ini
# ── Moonraker MQTT client ────────────────────────────────────────────────────
[mqtt]
address: <BROKER_IP>
# port: 1883        # default; uncomment to override
# username:         # uncomment if your broker requires authentication
# password:

# ── ESP32 filament sensor via MQTT ───────────────────────────────────────────
[sensor esp32_filament_runout]
type: mqtt
state_topic: esp32/filament_sensor/state
state_response_template:
    {% set d = payload | fromjson %}
    {% do set_result("fault",          d.get("fault", false) | int) %}
    {% do set_result("sensor_enabled", d.get("sensor_enabled", true) | int) %}
    {% do set_result("enc_vel",        d.get("enc_vel", 0.0) | float) %}
    {% do set_result("motion_ago_ms",  d.get("motion_ago_ms", 0) | float) %}
    {% do set_result("nozzle_temp",    d.get("nozzle_temp", 0.0) | float) %}
    {% do set_result("dht_temp",       d.get("dht_temp", 0.0) | float) %}
    {% do set_result("dht_humidity",   d.get("dht_humidity", 0.0) | float) %}
qos: 1

# ── Parameter metadata (Mainsail Sensors panel + history logging) ─────────────
parameter_fault:
    history_field: filament_fault

parameter_enc_vel:
    units: mm/s
    history_field: encoder_velocity

parameter_motion_ago_ms:
    units: ms

parameter_nozzle_temp:
    units: °C
    history_field: nozzle_temperature

parameter_dht_temp:
    units: °C
    history_field: ambient_temperature

parameter_dht_humidity:
    units: %RH
    history_field: ambient_humidity
```

Restart Moonraker:

```bash
sudo systemctl restart moonraker
```

The sensor then appears in the Mainsail **Sensors** panel with live values
and history graphs.

#### Controlling the sensor from a Klipper macro

Add to `printer.cfg`:

```ini
[gcode_macro FILAMENT_SENSOR_ENABLE]
description: Enable ESP32 filament runout sensor via MQTT
gcode:
    {action_call_remote_method("publish_mqtt_topic",
        topic="esp32/filament_sensor/cmd/enable",
        payload="1")}

[gcode_macro FILAMENT_SENSOR_DISABLE]
description: Disable ESP32 filament runout sensor via MQTT
gcode:
    {action_call_remote_method("publish_mqtt_topic",
        topic="esp32/filament_sensor/cmd/enable",
        payload="0")}

[gcode_macro FILAMENT_FAULT_RESET]
description: Reset ESP32 filament fault via MQTT
gcode:
    {action_call_remote_method("publish_mqtt_topic",
        topic="esp32/filament_sensor/cmd/reset",
        payload="1")}
```

---

### 5. Testing

Verify published messages with `mosquitto_sub`:

```bash
mosquitto_sub -t "esp32/filament_sensor/#" -v
```

Send test commands:

```bash
# Disable sensor
mosquitto_pub -t "esp32/filament_sensor/cmd/enable" -m "0"

# Re-enable sensor
mosquitto_pub -t "esp32/filament_sensor/cmd/enable" -m "1"

# Clear fault
mosquitto_pub -t "esp32/filament_sensor/cmd/reset"  -m "1"
```

---

### 6. Bidirectional control summary

```
FHEM / Klipper macro
        │  publish cmd/enable, cmd/reset
        ▼
   MQTT Broker (Mosquitto or FHEM MQTT2_SERVER)
        │                        ▲
        │  subscribe              │  publish state, availability
        ▼                        │
   ESP32 filament sensor ────────┘
        │  fault → GPIO 27 LOW
        ▼
   Klipper [filament_switch_sensor] → PAUSE macro
```

> **Note**: The GPIO runout signal (GPIO 27 → LOW) fires independently of MQTT
> and provides a hardware-level backup even when the network is unavailable.

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
