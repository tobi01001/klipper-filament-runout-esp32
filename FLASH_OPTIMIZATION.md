# Flash Usage Optimization

This document records the firmware-size investigation for the ESP32 Filament
Runout Sensor and lists the concrete steps taken (and still available) to
reduce flash consumption.

---

## 1. Current situation

| Metric | Value |
|--------|-------|
| Platform | espressif32 @ 6.13.0 |
| Framework | Arduino (ESP-IDF + FreeRTOS) |
| Default OTA partition (`default.csv`) | 1,280 KB per app slot |
| **Firmware size (pre-optimization)** | **> 1,280 KB** (exceeded default.csv) |
| **Firmware size (post-optimization)** | **1,156 KB** (measured) |
| Partition used (`min_spiffs.csv`) | **1,920 KB** per app slot |
| Flash used of partition | 60.2 % (1,156 KB / 1,920 KB) |
| Flash headroom | 765 KB |
| RAM used | 18.4 % (60 KB / 320 KB) |

The firmware exceeds the default ESP32 OTA partition and therefore requires the
`min_spiffs.csv` partition table (which is set in `platformio.ini`).

---

## 2. Top flash consumers

| Component | Estimated flash |
|-----------|----------------|
| WiFi + lwIP stack (precompiled) | ~350–450 KB |
| mbedTLS (HTTPS for GitHub OTA) | ~100–150 KB |
| WebServer / HTTP client | ~80–120 KB |
| Arduino core + FreeRTOS (precompiled) | ~150–200 KB |
| mDNS / ArduinoOTA | ~50–80 KB |
| U8g2 OLED driver (when enabled) | ~45–55 KB |
| LittleFS driver | ~30–40 KB |
| WebSockets library | ~30–50 KB |
| Application code (`src/*.cpp`) | ~30–60 KB |
| String literals / PROGMEM | ~10–20 KB |
| Bootloader + partition metadata | ~32 KB (fixed) |

---

## 3. Optimizations applied

### 3.1 `-Os` compiler flag (applied ✅)

**What:** Compile the application with GCC's size-optimization level instead
of the default speed-optimization level (`-O2`).

```ini
; platformio.ini
build_flags  = -Os
build_unflags = -O2 -O3
```

**Savings:** ~50–100 KB of application `.text` / `.rodata`.  
**Risk:** Very low. A handful of hot paths may be 1–5% slower; unnoticeable on
a microcontroller running 100 Hz / 50 Hz control loops.

---

### 3.2 Partition table: `min_spiffs.csv` (applied ✅)

**What:** Replace the default partition table (1,280 KB app slots) with
`min_spiffs.csv` (1,920 KB app slots, 192 KB LittleFS).

```ini
; platformio.ini
board_build.partitions = min_spiffs.csv
```

**Effect:** Provides enough headroom for the current firmware. Does not reduce
binary size but prevents flash-overflow errors.  
**Risk:** None. LittleFS data partition is reduced from ~1.5 MB to 192 KB, but
this project stores only a single `index.html` (< 50 KB).

---

### 3.3 Disable Bluetooth (applied ✅)

**What:** Bluetooth is never started in this application.  The old code
included `<esp_bt.h>` and called `btStop()` / `esp_bt_controller_disable()`
in the deep-sleep path as a defensive measure.  Because those symbols are
**referenced**, the linker pulls in the full precompiled BT library objects
regardless of any `-DCONFIG_BT_ENABLED=0` preprocessor flag.

The fix is a `-DDISABLE_BT` build flag that guards the include and the calls:

```cpp
// firmware/src/main.cpp
#ifndef DISABLE_BT
#include <esp_bt.h>
#endif
…
#ifndef DISABLE_BT
    btStop();
    esp_bt_controller_disable();
#endif
```

```ini
; platformio.ini
build_flags = -DDISABLE_BT
```

**Why `-DCONFIG_BT_ENABLED=0` did not work:**  
That define is a Kconfig preprocessor symbol that affects header guards in the
ESP-IDF source.  It does NOT change which precompiled `.a` library files the
linker picks up.  The BT libraries were still linked because the application
code called BT symbols directly.  Removing those references (via
`DISABLE_BT`) allows `--gc-sections` to eliminate the BT library objects.

**Estimated savings:** 80–200 KB (exact amount depends on toolchain version and
which BT library objects were transitively pulled in).  
**Risk:** None. BT was never started. The `btStop()` calls were no-ops (they
return immediately when BT was never initialized) and `esp_bt_controller_disable()`
returned `ESP_ERR_INVALID_STATE` silently. Removing them produces identical
runtime behavior.

---

## 4. Further savings (not yet applied)

### 4.1 Link-Time Optimization (LTO) — optional

**What:** Pass `-flto` so GCC performs dead-code and inlining optimizations
across all translation units at link time, not just within each `.cpp` file.

**Known issue:** LTO requires `gcc-ar` (not plain `ar`) as the archiver.  A
ready-to-use fix is in `firmware/tools/enable_lto.py`.  Activate by
uncommenting two lines in `platformio.ini`:

```ini
build_flags =
    …
    -flto          ; ← uncomment

; extra_scripts = pre:firmware/tools/enable_lto.py   ; ← uncomment
```

**Estimated additional savings:** 30–80 KB on application code sections.  
**Risk:** Low. LTO only optimizes the code we compile; precompiled framework
libraries are unaffected. Adds ~20 % to link time.  
**Note:** Verify that `~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gcc-ar`
exists. If not, run `pio pkg install --tool toolchain-xtensa-esp32`.

---

### 4.2 Disable OLED driver (`-DDISABLE_OLED`)

**What:** If no OLED display is connected, the U8g2 driver can be compiled out
entirely via the existing guard in `config.h`.

```ini
build_flags = -DDISABLE_OLED
```

**Estimated savings:** ~45–55 KB flash + 1 KB RAM.  
**Risk:** None, provided no OLED is wired to the device.

---

### 4.3 Disable GitHub OTA (`ENABLE_GITHUB_OTA=0`)

**What:** The GitHub Releases OTA path uses `WiFiClientSecure` + mbedTLS for
HTTPS.  Setting `ENABLE_GITHUB_OTA 0` in `config.h` (or as a build flag)
removes the HTTPS client, `HTTPUpdate`, and most of the mbedTLS stack.

```ini
build_flags = -DENABLE_GITHUB_OTA=0
```

**Estimated savings:** 60–120 KB (mbedTLS accounts for most of this).  
**Trade-off:** Automatic GitHub firmware update through the web UI is
disabled. ArduinoOTA (PlatformIO push) continues to work.

---

### 4.4 Pre-compress web assets (gzip)

**What:** The web UI (`data/index.html`) lives in the LittleFS data partition,
not in the application flash. Storing a pre-gzip'd copy reduces the LittleFS
footprint and network transfer time.

```cpp
// In web_handler.cpp handle_root():
server.sendHeader("Content-Encoding", "gzip");
server.streamFile(f, "text/html");
// Serve data/index.html.gz instead of data/index.html
```

**Effect:** 0 KB savings in application flash; 30–70 % reduction in LittleFS
usage and HTTP payload size.  
**Risk:** Very low (gzip is universally supported).

---

## 5. Priority summary

| # | Action | Est. saving | Status | Risk |
|---|--------|------------|--------|------|
| 1 | `-Os` compiler flag | 50–100 KB | ✅ Applied | Low |
| 2 | `min_spiffs.csv` partition | headroom | ✅ Applied | None |
| 3 | `-DDISABLE_BT` + source guard | 80–200 KB | ✅ Applied | None |
| 4 | LTO (`-flto` + enable_lto.py) | 30–80 KB | opt-in | Low |
| 5 | `-DDISABLE_OLED` | 45–55 KB | opt-in | None (if no OLED) |
| 6 | `ENABLE_GITHUB_OTA=0` | 60–120 KB | opt-in | Medium (loses auto-update) |
| 7 | Pre-gzip web assets | LittleFS only | opt-in | Low |

**Combined savings from items 1–3 (all applied):** ~130–300 KB (measured: > 1,280 KB → 1,156 KB),
bringing the firmware within the original `default.csv` 1,280 KB limit.
The `min_spiffs.csv` partition is retained for comfortable OTA headroom.
