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
 *  2. **GitHub Releases pull** – Two-step process via the web UI:
 *     a. Call ota_github_check_request() (or POST /api/ota/check) to fetch
 *        release info and compare against FIRMWARE_VERSION.  Status becomes
 *        "update-available" or "up-to-date".
 *     b. When "update-available", the user confirms in the web UI and then
 *        calls ota_github_update_request() (or POST /api/ota/update) to
 *        download and flash the "firmware.bin" asset and, if present, update
 *        "index.html" on LittleFS.  The device reboots automatically after a
 *        successful firmware flash.
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
 * @brief Request a GitHub-release version check (no flashing).
 *
 * Non-blocking: schedules a FreeRTOS task that fetches the latest release info
 * from GitHub and compares it with FIRMWARE_VERSION.  On completion the status
 * is set to "update-available" or "up-to-date".  Repeated calls while a task
 * is already running are ignored.
 *
 * Use this before calling ota_github_update_request() so the user can see
 * what version is available and confirm before the actual flash.
 */
void ota_github_check_request();

/**
 * @brief Request a GitHub-release OTA check and update (flash + reboot).
 *
 * Non-blocking: schedules a FreeRTOS task that performs the actual
 * HTTP download and flashing in the background.  Repeated calls while
 * an update is already in progress are ignored.
 *
 * The device reboots automatically when the flash succeeds.
 * Call this only after ota_github_check_request() has confirmed an update
 * is available and the user has confirmed they want to proceed.
 */
void ota_github_update_request();

/**
 * @brief Download the latest index.html from GitHub Releases and write it to
 *        LittleFS without flashing firmware or rebooting.
 *
 * Non-blocking: schedules a FreeRTOS task that fetches the latest release
 * info (or reuses a cached URL from a preceding check), downloads the
 * "index.html" asset, and atomically replaces /index.html on LittleFS.
 * The updated UI is served immediately on the next page load.
 *
 * Use this to resolve a UI version mismatch without performing a full
 * firmware update.  Repeated calls while a task is already running are
 * ignored.
 */
void ota_github_update_ui_request();

/**
 * @brief Short status string for the last GitHub OTA operation.
 *
 * Returns one of: "disabled", "idle", "checking", "update-available",
 * "up-to-date", "updating", "updating-ui", "ok", "failed".  Thread-safe
 * (read of a volatile pointer to a string literal).
 */
const char *ota_get_status();

/**
 * @brief Detail string for the last OTA failure.
 *
 * Returns "" when no failure has occurred (or after the error has been
 * superseded by a new attempt).  Written only by the OTA background task;
 * safe to read from any other task or the web handler ISR context.
 *
 * Examples:
 *   "GitHub API HTTP -11: connection refused"
 *   "[HTTPUpdate] HTTP error: 302"
 *   "No firmware.bin asset in release v1.2.0"
 */
const char *ota_get_error();

/**
 * @brief Latest release tag found during the most recent GitHub API check.
 *
 * Returns "" until a successful API response has been received.
 */
const char *ota_get_latest_tag();

/**
 * @brief Version string read from the <meta name="ui-version"> tag in the
 *        /index.html file currently stored on LittleFS.
 *
 * Returns "unknown" when LittleFS is not mounted, the file does not exist,
 * or the version tag is absent (e.g. an old manually-uploaded index.html).
 *
 * The web handler uses this to populate the "UI version (LittleFS)" row in
 * the /api/ota JSON response, letting the user verify that the on-device web
 * UI matches the running firmware.
 */
const char *ota_get_lfs_ui_version();
