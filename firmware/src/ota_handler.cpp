/**
 * @file ota_handler.cpp
 * @brief OTA update implementation for ESP32 filament sensor.
 *
 * Implements two update paths:
 *
 *  Path 1 – ArduinoOTA (PlatformIO / VS Code push)
 *  ──────────────────────────────────────────────────
 *  ArduinoOTA listens on UDP port 3232 and TCP port 3232.  To use it:
 *    • Select the "esp32dev_ota" PlatformIO environment.
 *    • Set the device IP (or mDNS hostname) in platformio.ini or via
 *      the --upload-port flag.
 *    • Click Upload in VS Code or run `pio run -e esp32dev_ota -t upload`.
 *
 *  Path 2 – GitHub Releases pull
 *  ──────────────────────────────────────────────────
 *  Triggered via the web UI or POST /api/ota/update.
 *  Steps performed by the background task:
 *    1. HTTPS GET to the GitHub Releases API for the latest release.
 *    2. Parse tag_name and find the "firmware.bin" browser_download_url.
 *    3. Compare tag against FIRMWARE_VERSION (simple x.y.z integers).
 *    4. If newer, stream the binary via httpUpdate to the inactive partition.
 *    5. Reboot on success; set status "failed" on error.
 *
 * Note: HTTPS connections use setInsecure() (no certificate validation).
 *       This is acceptable for a local-network embedded device but should be
 *       replaced with a root-CA bundle for stricter security requirements.
 *
 * MIT License – Copyright (c) 2026 tobi01001
 */

#include "ota_handler.h"
#include "config.h"
#include "display_handler.h"
#include "debug_log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <ArduinoOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if ENABLE_GITHUB_OTA
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#endif

// ─── Module state ──────────────────────────────────────────────────────────────
static volatile const char *s_status         = "idle";
static char                 s_latest_tag[32] = "";
static volatile bool        s_task_running   = false;
// Mutex protecting s_latest_tag (written by OTA task, read by web handler task).
static SemaphoreHandle_t    s_tag_mutex      = nullptr;
// Last error detail string – populated on every failure, cleared on new attempt.
// Exposed via ota_get_error() so the web UI can show more than "failed".
static char                 s_error_msg[128] = "";

// ─── Task mode ────────────────────────────────────────────────────────────────
// When true the task only checks the latest release without flashing.
static volatile bool        s_check_only     = false;

#if ENABLE_GITHUB_OTA

// ─── Error message helper ─────────────────────────────────────────────────────
// Writes the error string under s_tag_mutex so ota_get_error() can read it from
// any task without a torn read.  Must only be called from the OTA task context.
static void set_error_msg(const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(s_error_msg, tmp, sizeof(s_error_msg) - 1);
        s_error_msg[sizeof(s_error_msg) - 1] = '\0';
        xSemaphoreGive(s_tag_mutex);
    } else {
        DBG_PRINTLN("[OTA] set_error_msg: mutex timeout – error detail not stored");
    }
}

// ─── Version comparison ────────────────────────────────────────────────────────
/**
 * @brief Return true when @p tag represents a version newer than FIRMWARE_VERSION.
 *
 * Both strings must be "x.y.z" (an optional leading 'v' in @p tag is stripped).
 */
static bool version_is_newer(const char *tag) {
    const char *v = (tag && tag[0] == 'v') ? tag + 1 : tag;

    int cur_major = 0, cur_minor = 0, cur_patch = 0;
    int new_major = 0, new_minor = 0, new_patch = 0;

    if (sscanf(FIRMWARE_VERSION, "%d.%d.%d", &cur_major, &cur_minor, &cur_patch) != 3)
        return false;
    if (!v || sscanf(v, "%d.%d.%d", &new_major, &new_minor, &new_patch) != 3)
        return false;

    if (new_major != cur_major) return new_major > cur_major;
    if (new_minor != cur_minor) return new_minor > cur_minor;
    return new_patch > cur_patch;
}

// ─── GitHub OTA background task ───────────────────────────────────────────────
static void github_update_task(void * /*param*/) {
    set_error_msg(""); // clear stale error from previous attempt
    s_status = "checking";
    DBG_PRINTF("[OTA] Checking GitHub releases at %s\n", GITHUB_API_URL);

    // ── Step 1: Fetch the latest release info ──────────────────────────────
    WiFiClientSecure api_client;
    api_client.setInsecure(); // skips certificate verification

    HTTPClient http;
    http.begin(api_client, GITHUB_API_URL);
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    http.addHeader("User-Agent", "ESP32-FilamentSensor/" FIRMWARE_VERSION);
    http.addHeader("Accept",     "application/vnd.github+json");

    const int api_code = http.GET();
    if (api_code != HTTP_CODE_OK) {
        DBG_PRINTF("[OTA] GitHub API error: HTTP %d\n", api_code);
        set_error_msg("GitHub API HTTP %d: %s", api_code,
                      http.errorToString(api_code).c_str());
        http.end();
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    // ── Step 2: Parse release JSON ─────────────────────────────────────────
    // Use a filter to discard unneeded fields and keep heap usage low.
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;

    JsonDocument doc;
    const DeserializationError json_err =
        deserializeJson(doc, http.getStream(),
                        DeserializationOption::Filter(filter));
    http.end();

    if (json_err) {
        DBG_PRINTF("[OTA] JSON parse error: %s\n", json_err.c_str());
        set_error_msg("JSON parse: %s", json_err.c_str());
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    const char *tag = doc["tag_name"] | "";
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(s_latest_tag, tag, sizeof(s_latest_tag) - 1);
        s_latest_tag[sizeof(s_latest_tag) - 1] = '\0';
        xSemaphoreGive(s_tag_mutex);
    }

    DBG_PRINTF("[OTA] Latest release: %s  (current: %s)\n",
                  tag, FIRMWARE_VERSION);

    // ── Step 3: Version check ──────────────────────────────────────────────
    if (!version_is_newer(tag)) {
        DBG_PRINTLN("[OTA] Firmware is up-to-date");
        s_status = "up-to-date";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    // If this is a check-only request, report that an update is available
    // and let the user confirm before flashing.
    if (s_check_only) {
        DBG_PRINTF("[OTA] Update available: %s -> confirm via web UI to apply\n", tag);
        s_status = "update-available";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    // ── Step 4: Find the firmware.bin asset URL ────────────────────────────
    char asset_url[256] = "";
    const JsonArray assets = doc["assets"].as<JsonArray>();
    for (const JsonObject asset : assets) {
        const char *name = asset["name"] | "";
        if (strcmp(name, "firmware.bin") == 0) {
            const char *url = asset["browser_download_url"] | "";
            strncpy(asset_url, url, sizeof(asset_url) - 1);
            asset_url[sizeof(asset_url) - 1] = '\0';
            break;
        }
    }

    if (asset_url[0] == '\0') {
        DBG_PRINTLN("[OTA] No firmware.bin asset found in release");
        set_error_msg("No firmware.bin asset in release %s", tag);
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    DBG_PRINTF("[OTA] Downloading: %s\n", asset_url);
    s_status = "updating";

    // ── Step 5: Stream firmware via HTTPUpdate ─────────────────────────────
    WiFiClientSecure fw_client;
    fw_client.setInsecure();

    // HTTPC_FORCE_FOLLOW_REDIRECTS is required: GitHub release asset URLs return
    // a 302 redirect to objects.githubusercontent.com.  STRICT mode also follows
    // GET→GET redirects in theory, but FORCE is unambiguous and more robust
    // across ESP32 Arduino framework versions.
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true); // reboot automatically on success

#ifdef ENABLE_OLED
    // Show OTA progress screen before the download begins and hook into the
    // HTTPUpdate progress callback so the bar advances with each written chunk.
    display_set_ota_active(true);
    display_show_ota_progress(0);

    httpUpdate.onProgress([](int current, int total) {
        if (total > 0) {
            display_show_ota_progress(
                (uint8_t)((uint32_t)current * 100u / (uint32_t)total));
        }
    });

    // Called just before the library triggers esp_restart() on success.
    httpUpdate.onEnd([]() {
        display_show_ota_reboot();
    });
#endif

    const t_httpUpdate_return result =
        httpUpdate.update(fw_client, asset_url);

    switch (result) {
        case HTTP_UPDATE_OK:
            // rebootOnUpdate(true) means we will not reach this line; the
            // device reboots inside httpUpdate.update().
            s_status = "ok";
            break;
        case HTTP_UPDATE_NO_UPDATES:
            s_status = "up-to-date";
#ifdef ENABLE_OLED
            display_set_ota_active(false);
#endif
            break;
        case HTTP_UPDATE_FAILED:
        default: {
            const String err_str = httpUpdate.getLastErrorString();
            DBG_PRINTF("[OTA] Update failed: %s\n", err_str.c_str());
            set_error_msg("%s", err_str.c_str());
            s_status = "failed";
#ifdef ENABLE_OLED
            display_set_ota_active(false);
#endif
            break;
        }
    }

    s_task_running = false;
    vTaskDelete(nullptr);
}
#endif

// ─── Public API ───────────────────────────────────────────────────────────────
void ota_init(const char *hostname, const char *password) {
    s_tag_mutex = xSemaphoreCreateMutex();

    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(password);

    ArduinoOTA.onStart([]() {
        const String type = (ArduinoOTA.getCommand() == U_FLASH)
                                ? "firmware"
                                : "filesystem";
        DBG_PRINTLN("[OTA] ArduinoOTA start – type: " + type);
        s_status = "updating";
#ifdef ENABLE_OLED
        display_set_ota_active(true);
        display_show_ota_progress(0);
#endif
    });

    ArduinoOTA.onEnd([]() {
        DBG_PRINTLN("\n[OTA] ArduinoOTA complete – rebooting");
        s_status = "ok";
#ifdef ENABLE_OLED
        display_show_ota_reboot();
        // s_ota_active intentionally left true: device reboots immediately after.
#endif
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        DBG_PRINTF("[OTA] Progress: %u%%\r", progress * 100 / total);
#ifdef ENABLE_OLED
        display_show_ota_progress((uint8_t)(progress * 100 / total));
#endif
    });

    ArduinoOTA.onError([](ota_error_t error) {
        DBG_PRINTF("[OTA] Error[%u]: ", error);
        switch (error) {
            case OTA_AUTH_ERROR:    DBG_PRINTLN("Auth failed");       break;
            case OTA_BEGIN_ERROR:   DBG_PRINTLN("Begin failed");      break;
            case OTA_CONNECT_ERROR: DBG_PRINTLN("Connect failed");    break;
            case OTA_RECEIVE_ERROR: DBG_PRINTLN("Receive failed");    break;
            case OTA_END_ERROR:     DBG_PRINTLN("End failed");        break;
            default:                DBG_PRINTLN("Unknown error");     break;
        }
        s_status = "failed";
#ifdef ENABLE_OLED
        display_set_ota_active(false);
#endif
    });

    ArduinoOTA.begin();
    DBG_PRINTF("[OTA] ArduinoOTA ready – hostname: %s\n", hostname);
}

void ota_handle() {
    ArduinoOTA.handle();
}

void ota_github_check_request() {
#if ENABLE_GITHUB_OTA
    if (s_task_running) {
        DBG_PRINTLN("[OTA] Task already running – check request ignored");
        return;
    }
    s_task_running = true;
    s_check_only   = true;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        github_update_task,
        "OTA_Check",
        12288,
        nullptr,
        2,
        nullptr,
        0   // Core 0: keeps I2C/display calls on the same core as the display driver
    );

    if (ok != pdPASS) {
        DBG_PRINTLN("[OTA] Failed to create check task");
        s_task_running = false;
        s_status = "failed";
    }
#else
    s_status = "disabled";
#endif
}

void ota_github_update_request() {
#if ENABLE_GITHUB_OTA
    if (s_task_running) {
        DBG_PRINTLN("[OTA] Update already in progress – request ignored");
        return;
    }
    s_task_running = true;
    s_check_only   = false;

    // Stack size 20480 bytes: the update path runs a full TLS handshake to the
    // GitHub CDN (objects.githubusercontent.com) on top of the API TLS session,
    // plus HTTPUpdate internals and the ESP32 flash-write call chain.  12 KB is
    // sufficient for check-only; the extra 8 KB covers the deeper call stack of
    // HTTPUpdate::runUpdate() → HTTPClient → WiFiClientSecure → mbedTLS.
    const BaseType_t ok = xTaskCreatePinnedToCore(
        github_update_task,
        "OTA_GitHub",
        20480,
        nullptr,
        2, // lower priority than protocol tasks
        nullptr,
        0   // Core 0: keeps I2C/display calls on the same core as the display driver
    );

    if (ok != pdPASS) {
        DBG_PRINTLN("[OTA] Failed to create update task");
        s_task_running = false;
        s_status = "failed";
    }
#else
    s_status = "disabled";
#endif
}

const char *ota_get_status() {
    return (const char *)s_status;
}

const char *ota_get_error() {
    static char buf[128] = "";
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(buf, s_error_msg, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        xSemaphoreGive(s_tag_mutex);
    }
    return buf;
}

const char *ota_get_latest_tag() {
    // Return a mutex-protected copy to avoid a data race with the OTA task.
    static char buf[32] = "";
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(buf, s_latest_tag, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        xSemaphoreGive(s_tag_mutex);
    }
    return buf;
}
