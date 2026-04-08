# Migration: espressif32 5.x → 6.13.0

**PR**: #16 – Fix flash overflow for espressif32 6.13.0: switch to min_spiffs partition  
**Date**: 2026-04-08  
**Platform change**: `espressif32` (unpinned, resolved to 5.2.0) → `espressif32@6.13.0`  
**Framework**: Arduino-ESP32 3.20017 (2.0.17), toolchain-xtensa-esp32 8.4.0+2021r2-patch5

---

## Why This Change

espressif32 6.13.0 ships Arduino-ESP32 2.0.17 which brings a newer ESP-IDF 4.4.8 SDK,
bug fixes for WiFi and OTA reliability, and toolchain improvements. The platform version is
now pinned to allow reproducible builds and controlled upgrades.

---

## Migration Checklist

### Platform and Build

| Item | Status | Evidence |
|---|---|---|
| Platform pin updated to espressif32 6.13.0 for esp32dev | ✅ Done | `platformio.ini` line 13 |
| Full rebuild succeeds with no unresolved symbols | ✅ Pass | `pio run -e esp32dev` exits 0; firmware.elf linked cleanly |
| Warnings reviewed – no new critical warnings | ✅ Pass | Zero warnings in project source files; see [Build Warnings](#build-warnings) |
| Board, partition, and upload settings revalidated | ✅ Done | `board_build.partitions = min_spiffs.csv` added; see [Partition Change](#partition-change) |
| Incompatible global libraries guarded | ✅ Done | `lib_ignore` added for async/RP2040 libs; see [Library Guard](#library-guard) |

### Compatibility and Libraries

| Item | Status | Evidence |
|---|---|---|
| ArduinoJson 7.4.3 compatible with Arduino-ESP32 2.x | ✅ Pass | Uses `JsonDocument` (not deprecated `DynamicJsonDocument`); builds clean |
| U8g2 2.36.18 compatible | ✅ Pass | No Arduino-specific internals used; hardware I2C via standard Wire |
| WebSockets 2.7.3 compatible | ✅ Pass | Configured with `NETWORK_ESP32`; plain `WiFiClient` path (no TLS/wss) |
| Built-in libraries (WiFi, ArduinoOTA, HTTPUpdate, etc.) | ✅ Pass | All resolved to framework version 2.0.0 matching Arduino-ESP32 2.0.17 |
| No deprecated ESP-IDF private API used | ✅ Pass | Only public `esp_wifi.h`, `esp_bt.h` headers included |
| `WiFiEvent_t` / `WiFiEventInfo_t` callback signature | ✅ Pass | `on_wifi_event(WiFiEvent_t, WiFiEventInfo_t)` – correct 2.x signature |
| Disconnect reason field access | ✅ Pass | `info.wifi_sta_disconnected.reason` – correct 2.x union member |
| `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` event enum | ✅ Pass | New-style event constants, not legacy `SYSTEM_EVENT_*` |

### FreeRTOS and ISR Safety

| Item | Status | Evidence |
|---|---|---|
| No blocking logic in ISR paths | ✅ Pass | ISR uses only `micros()`, `digitalRead()`, `portENTER/EXIT_CRITICAL_ISR`, `millis()` – all ISR-safe on LX6 |
| ISR → task queue handoff | ✅ Pass | `xQueueOverwrite` (single slot, never blocks writer or reader) |
| Queue depths – no overflow at peak rate | ✅ Pass | `ENCODER_QUEUE_LEN = 1`; overwrite semantics mean backpressure is impossible |
| `s_tick_count` access correctness | ✅ Pass | Written in ISR under `portENTER_CRITICAL_ISR(&s_mux)`; read in task under `portENTER_CRITICAL(&s_mux)` – same spinlock object |
| `g_last_motion_ms` cross-core access | ✅ Pass | `volatile uint32_t`; single 32-bit write is atomic on Xtensa LX6 (comment in `fault_detector.cpp` line 210) |
| No priority inversions or deadlocks | ✅ Pass | All mutexes taken with bounded timeouts (10–50 ms); ISR never takes a mutex |
| Task stack sizes reviewed | ✅ Pass | Core 0: 10 240 B (OLED) / 8 192 B; Core 1: 4 096 B; OTA task: 12 288 B. RAM usage 19.5 % post-migration |

> **Note**: Runtime stack high-water mark telemetry requires hardware. Recommend reading
> `uxTaskGetStackHighWaterMark()` for each task in a soak run and comparing against
> configured stack sizes.

### WiFi and Network Recovery

| Item | Status | Evidence |
|---|---|---|
| Bounded retry behavior | ✅ Pass | Backoff doubles from 1 000 ms to max 60 000 ms (`WIFI_RECONNECT_MAX_MS`) |
| Non-blocking reconnect | ✅ Pass | `StaPhase` state machine in `wifi_tick()`; no `delay()` or blocking calls |
| Captive portal AP fallback | ✅ Pass | Started on connect timeout; stopped on successful STA connection |
| WiFi connect timeout | ✅ Pass | `WIFI_CONNECT_TIMEOUT_MS = 30 000 ms` |
| Service state determinism after link loss | ✅ Pass | `wifi_is_connected()` flag gates OTA and Moonraker; no state corruption on disconnect |
| No reboot loops | ✅ Pass | No `esp_restart()` on WiFi failure; reconnect loop is non-destructive |

> **Runtime tests T1/T2 require hardware.** See [Test Matrix](#test-matrix).

### OTA Reliability

| Item | Status | Evidence |
|---|---|---|
| OTA writes to inactive partition only | ✅ Pass | `httpUpdate.update()` uses ESP-IDF OTA to inactive slot; verified by `min_spiffs.csv` (both `app0` and `app1` present) |
| Redirect following for GitHub CDN | ✅ Pass | `httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)` |
| Auto-reboot on OTA success | ✅ Pass | `httpUpdate.rebootOnUpdate(true)` |
| Safe failure path | ✅ Pass | `s_status = "failed"`, OLED overlay cleared, `s_task_running = false` |
| Duplicate task guard | ✅ Pass | `s_task_running` flag prevents a second OTA task from launching |
| Firmware recoverable after interrupted OTA | ✅ Pass | Inactive partition written only; boot partition unchanged until `esp_ota_set_boot_partition()` succeeds |
| ArduinoOTA `onProgress` callback type | ✅ Pass | `(unsigned int progress, unsigned int total)` – unchanged in 2.0.17 |

> **Runtime tests T3/T4 require hardware.** See [Test Matrix](#test-matrix).

### Known Incompatibility Patterns Checked

| Pattern | Status | Notes |
|---|---|---|
| Legacy `WiFiEvent_t` callback (1-arg) | ✅ Not present | Code uses 2-arg `(WiFiEvent_t, WiFiEventInfo_t)` callback |
| Legacy `SYSTEM_EVENT_*` enum values | ✅ Not present | Code uses `ARDUINO_EVENT_*` constants |
| `DynamicJsonDocument` / `StaticJsonDocument` | ✅ Not present | Code uses `JsonDocument` (ArduinoJson 7 API) |
| Implicit `String` → `const char*` coercion | ✅ Not present | All string arguments use `.c_str()` explicitly |
| `HTTPC_FORCE_FOLLOW_REDIRECTS` | ✅ Not present | Uses `HTTPC_STRICT_FOLLOW_REDIRECTS` (GET only, correct for firmware download) |
| `esp_idf_version.h` private includes | ✅ Not present | Only public `esp_wifi.h`, `esp_bt.h` |

---

## Partition Change

espressif32 6.13.0 (Arduino-ESP32 2.0.17) produces a larger binary than 5.x.
The firmware grew to **1 311 461 bytes**, exceeding the default partition's 1 280 KB OTA slot
by 741 bytes.

| Partition table | App slot size | LittleFS size | Fit? |
|---|---|---|---|
| `default.csv` (before) | 1 280 KB (1 310 720 B) | 1 408 KB | ❌ overflow by 741 B |
| `min_spiffs.csv` (after) | 1 920 KB (1 966 080 B) | 128 KB | ✅ 66.7 % used |

`min_spiffs.csv` keeps both `app0` and `app1` OTA slots, so both push (ArduinoOTA) and pull
(GitHub Releases) OTA paths continue to work. The 128 KB filesystem area is sufficient for
the 21 KB `index.html` web asset.

---

## Library Guard

A `lib_ignore` block was added to `platformio.ini` to prevent the PlatformIO LDF from
picking up incompatible libraries that may be installed globally in a developer's environment:

```ini
lib_ignore =
    AsyncTCP
    AsyncTCP_RP2040W
    ESPAsyncTCP
    ESPAsyncWebServer
```

These libraries are incompatible with ESP32 Arduino-2.x because they reference
`ip_addr_t.addr` (removed in lwIP 2.x) or RP2040-specific defines. Without `lib_ignore`,
a global install of any of these would cause a hard build error.

---

## Build Warnings

Full rebuild with `espressif32@6.13.0` produces **zero warnings** in project source files.
All warnings observed during investigation were from the globally installed `AsyncTCP_RP2040W`
library (not part of this project), which is now guarded by `lib_ignore`.

```
Environment    Status    Duration
-------------  --------  --------
esp32dev       SUCCESS   ~30 s (clean build)

RAM:   [==        ]  19.5% (used 63 972 bytes from 327 680 bytes)
Flash: [=======   ]  66.7% (used 1 311 461 bytes from 1 966 080 bytes)
```

---

## Test Matrix

The following tests require physical hardware. Results to be filled in before promotion
to wider deployment.

| Test ID | Area | Scenario | Pass Criteria | Result |
|---|---|---|---|---|
| T1 | ISR | Burst interrupt integrity: high-rate encoder pulses for a fixed window | No missed critical events, no queue overflow, no watchdog reset | [ ] Pass [ ] Fail |
| T2 | WiFi | Reconnect under active ISR load: force AP disconnect/reconnect while encoder is active | Reconnect within WIFI_CONNECT_TIMEOUT_MS, service resumes, no deadlock | [ ] Pass [ ] Fail |
| T3 | OTA | Baseline OTA success: stable network, flash via GitHub Releases pull | OTA completes, device reboots, web UI reachable after reboot | [ ] Pass [ ] Fail |
| T4 | OTA + WiFi | OTA under instability: induce packet loss or brief AP drop during OTA | Safe failure (s_status = "failed") or clean recovery; no boot loop; firmware remains bootable | [ ] Pass [ ] Fail |
| T5 | Soak | 12–24 h uptime with periodic WiFi disruption and ISR bursts | No reset storms, stable memory trend, correct fault detection throughout | [ ] Pass [ ] Fail |

---

## Rollback Plan

1. Revert `platform = espressif32@6.13.0` to `platform = espressif32@5.2.0` in `platformio.ini`.
2. Remove the `board_build.partitions = min_spiffs.csv` line (default partition fits 5.x binary).
3. Remove the `lib_ignore` block (optional; harmless to keep).
4. Run `pio run -e esp32dev` to verify clean build.
5. Flash via USB: `pio run -e esp32dev -t upload`.
6. Confirm web UI reachable and OTA push functional.

---

## Deployment Notes

- Deploy to a single device first; confirm T1–T4 results before wider rollout.
- After soak test (T5), promote to the remaining fleet.
- The default OTA password (`ota1234`) should be changed in production via the web UI.
