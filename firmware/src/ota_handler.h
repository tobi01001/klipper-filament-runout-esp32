#pragma once

/**
 * @file ota_handler.h
 * @brief OTA (Over-The-Air) update support.
 *
 * Two update paths are provided:
 *
 *  1. **PlatformIO / VS Code push** – ArduinoOTA runs a lightweight UDP/TCP
 *     service.  From VS Code, select the "esp32dev_ota" environment and click
 *     Upload (or run `pio run -e esp32dev_ota -t upload`).  The device must be
 *     reachable on the local network and WiFi must be connected.
 *
 *  2. **GitHub Releases pull** – Call ota_github_update() (or POST /api/ota/update
 *     via the web UI) to have the device fetch the latest release from GitHub,
 *     compare the tag against FIRMWARE_VERSION, and flash the "firmware.bin"
 *     asset if a newer version is available.  The device reboots automatically
 *     after a successful flash.
 *
 * MIT License – Copyright (c) 2026 tobi01001
 */

#include <Arduino.h>

/**
 * @brief Initialise ArduinoOTA for PlatformIO / VS Code push-based updates.
 *
 * Must be called after WiFi has started (STA or AP mode).
 * Advertises the device under the mDNS hostname defined by OTA_HOSTNAME.
 *
 * @param hostname  mDNS hostname (e.g. "filament-sensor" → filament-sensor.local)
 * @param password  OTA authentication password
 */
void ota_init(const char *hostname, const char *password);

/**
 * @brief Service ArduinoOTA events.
 *
 * Must be called frequently from the main protocol loop (Core 0).
 * A missed call only delays the OTA handshake, it never corrupts state.
 */
void ota_handle();

/**
 * @brief Request a GitHub-release OTA check and update.
 *
 * Non-blocking: schedules a FreeRTOS task that performs the actual
 * HTTP download and flashing in the background.  Repeated calls while
 * an update is already in progress are ignored.
 *
 * The device reboots automatically when the flash succeeds.
 */
void ota_github_update_request();

/**
 * @brief Short status string for the last GitHub OTA operation.
 *
 * Returns one of: "idle", "checking", "up-to-date", "updating", "ok",
 * "failed".  Thread-safe (read of a volatile pointer to a string literal).
 */
const char *ota_get_status();

/**
 * @brief Latest release tag found during the most recent GitHub API check.
 *
 * Returns "" until a successful API response has been received.
 */
const char *ota_get_latest_tag();
