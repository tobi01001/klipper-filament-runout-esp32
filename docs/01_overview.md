# Filament Runout Sensor – System Overview

**Project**: ESP32 Intelligent Filament Runout Sensor for Klipper  
**Version**: 1.0  
**Date**: 2026-03-30  
**Author**: tobi01001  
**License**: MIT

---

## 1. Purpose

This sub-project adds a highly reliable filament runout detector to the Ender 5 Pro
Klipper setup.  Unlike a simple microswitch that only detects a fully empty spool,
this sensor uses an **optical mouse encoder** to measure actual filament *motion*.
It can therefore detect:

- Empty spool (filament runs out)
- Stuck / tangled filament (spool present but not moving)
- Broken filament mid-print
- Clogged extruder (motor running but filament not advancing)

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     ESP32 Dual-Core MCU                      │
│                                                              │
│  ┌─────────────────────────┐  ┌──────────────────────────┐  │
│  │  Core 0  (Protocol)     │  │  Core 1  (Real-time)     │  │
│  │  · WiFi management      │  │  · Encoder ISR (GPIO 25/26)│ │
│  │  · Moonraker HTTP poll  │  │  · Quadrature decoding   │  │
│  │  · Fault state machine  │  │  · Speed calculation     │  │
│  │  · Web config server    │  │  · EMA velocity filter   │  │
│  │  · NVS persistence      │  │                          │  │
│  └──────────┬──────────────┘  └──────────┬───────────────┘  │
│             │                             │                  │
│             └──── encoder_queue (1 slot) ─┘                  │
│             └──── g_last_motion_ms (volatile) ───────────────┘
└─────────────────────┬──────────────┬────────────────────────┘
                      │              │
                 [Moonraker]   [Optical Encoder]
                      │              │
                 [Klipper] ←── [GPIO 27 Runout Pin]
```

---

## 3. Core Allocation Rationale

| Core | Responsibilities | Priority | Rationale |
|------|-----------------|----------|-----------|
| **Core 1** | ISR, quadrature decode, speed calc | High (10) | Dedicated real-time; ISR latency < 5 µs |
| **Core 0** | WiFi, HTTP, fault detect, web server | Medium (5) | Handles all blocking I/O without affecting encoder |

FreeRTOS inter-core communication:
- **`encoder_queue`** – single-slot overwriting queue; Core 1 pushes `EncoderData` at 50 Hz; Core 0 peeks the latest snapshot.
- **`g_last_motion_ms`** – volatile `uint32_t` updated inside the ISR; atomic single-word read on Xtensa LX6.

---

## 4. System State Machine

```
           ┌─────────┐
           │  INIT   │  Power-on, hardware init
           └────┬────┘
                │
                ▼
           ┌──────────┐
      ┌────┤ WIFI_CONN ├────┐
      │    └──────────┘    │
  [Timeout]           [Connected]
      │                    │
      ▼                    ▼
 ┌──────────┐         ┌─────────┐
 │ WIFI_FAIL│◄─[retry]─┤  READY  │
 └──────────┘         └────┬────┘
                           │
          ┌────────────────┼──────────────────┐
          │                │                  │
       [idle]         [extruding]          [fault]
          │                │                  │
          ▼                ▼                  ▼
      ┌──────┐        ┌─────────┐        ┌───────┐
      │ IDLE │◄───────┤ PRINTING│────────►│ FAULT │
      └──────┘        └─────────┘        └───┬───┘
          ▲                                   │
          └────────── [Reset / Clear] ─────────┘
```

**FAULT** pulls GPIO 27 LOW → Klipper pauses/stops the print.

---

## 5. Key Performance Targets

| Metric | Target |
|--------|--------|
| Encoder ISR latency | < 5 µs |
| Fault detection latency | < 500 ms |
| Speed calculation rate | 50 Hz (20 ms) |
| Moonraker poll rate | 5 Hz (200 ms) |
| Runout signal latency after fault | < 100 ms |
| WiFi auto-reconnect | < 60 s |
| Firmware heap usage | < 200 KB |
