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

#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ─── Module state ──────────────────────────────────────────────────────────────
static volatile const char *s_status         = "idle";
static char                 s_latest_tag[32] = "";
static volatile bool        s_task_running   = false;
// Mutex protecting s_latest_tag (written by OTA task, read by web handler task).
static SemaphoreHandle_t    s_tag_mutex      = nullptr;

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
    s_status = "checking";
    Serial.printf("[OTA] Checking GitHub releases at %s\n", GITHUB_API_URL);

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
        Serial.printf("[OTA] GitHub API error: HTTP %d\n", api_code);
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
        Serial.printf("[OTA] JSON parse error: %s\n", json_err.c_str());
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

    Serial.printf("[OTA] Latest release: %s  (current: %s)\n",
                  tag, FIRMWARE_VERSION);

    // ── Step 3: Version check ──────────────────────────────────────────────
    if (!version_is_newer(tag)) {
        Serial.println("[OTA] Firmware is up-to-date");
        s_status = "up-to-date";
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
        Serial.println("[OTA] No firmware.bin asset found in release");
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    Serial.printf("[OTA] Downloading: %s\n", asset_url);
    s_status = "updating";

    // ── Step 5: Stream firmware via HTTPUpdate ─────────────────────────────
    WiFiClientSecure fw_client;
    fw_client.setInsecure();

    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true); // reboot automatically on success

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
            break;
        case HTTP_UPDATE_FAILED:
        default:
            Serial.printf("[OTA] Update failed: %s\n",
                          httpUpdate.getLastErrorString().c_str());
            s_status = "failed";
            break;
    }

    s_task_running = false;
    vTaskDelete(nullptr);
}

// ─── Public API ───────────────────────────────────────────────────────────────
void ota_init(const char *hostname, const char *password) {
    s_tag_mutex = xSemaphoreCreateMutex();

    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(password);

    ArduinoOTA.onStart([]() {
        const String type = (ArduinoOTA.getCommand() == U_FLASH)
                                ? "firmware"
                                : "filesystem";
        Serial.println("[OTA] ArduinoOTA start – type: " + type);
        s_status = "updating";
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] ArduinoOTA complete – rebooting");
        s_status = "ok";
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[OTA] Progress: %u%%\r", progress * 100 / total);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]: ", error);
        switch (error) {
            case OTA_AUTH_ERROR:    Serial.println("Auth failed");       break;
            case OTA_BEGIN_ERROR:   Serial.println("Begin failed");      break;
            case OTA_CONNECT_ERROR: Serial.println("Connect failed");    break;
            case OTA_RECEIVE_ERROR: Serial.println("Receive failed");    break;
            case OTA_END_ERROR:     Serial.println("End failed");        break;
            default:                Serial.println("Unknown error");     break;
        }
        s_status = "failed";
    });

    ArduinoOTA.begin();
    Serial.printf("[OTA] ArduinoOTA ready – hostname: %s\n", hostname);
}

void ota_handle() {
    ArduinoOTA.handle();
}

void ota_github_update_request() {
    if (s_task_running) {
        Serial.println("[OTA] Update already in progress – request ignored");
        return;
    }
    s_task_running = true;

    // Stack size 12288 bytes: TLS handshake (WiFiClientSecure) alone needs ~6 KB;
    // HTTPClient, JSON document, and HTTPUpdate consume the remainder.
    // Runs at priority 2, below the sensor tasks (5/10) so it cannot starve them.
    const BaseType_t ok = xTaskCreate(
        github_update_task,
        "OTA_GitHub",
        12288,
        nullptr,
        2, // lower priority than protocol tasks
        nullptr
    );

    if (ok != pdPASS) {
        Serial.println("[OTA] Failed to create update task");
        s_task_running = false;
        s_status = "failed";
    }
}

const char *ota_get_status() {
    return (const char *)s_status;
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
