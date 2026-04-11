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
 *  Triggered via the web UI or POST /api/ota/check, /api/ota/update,
 *  /api/ota/update-ui.
 *
 *  Firmware and Web UI are versioned independently:
 *    • Firmware releases carry the tag prefix "fw-v"  (e.g. fw-v1.1.6).
 *    • Web UI releases carry the tag prefix "ui-v"    (e.g. ui-v1.1.6).
 *    • Old-style "v*" tags are treated as firmware for backward compatibility.
 *
 *  Steps performed by the check task:
 *    1. HTTPS GET to the GitHub Releases list API (up to 10 recent releases).
 *    2. Scan the list for the latest fw-v* tag (firmware) and latest ui-v* tag
 *       (Web UI), extracting asset download URLs for each.
 *    3. Compare fw tag against FIRMWARE_VERSION; compare ui tag against the
 *       ui-version embedded in /index.html on LittleFS – independently.
 *    4. Report status: "update-available" (firmware newer), "ui-update-available"
 *       (UI newer, firmware current), or "up-to-date".
 *
 *  Steps performed by the update task (firmware flash):
 *    5. Stream "firmware.bin" from the latest fw release via httpUpdate.
 *    6. Reboot on success; set status "failed" on error.
 *    (Web UI is NOT updated during a firmware flash – UI has its own update path.)
 *
 *  Steps performed by the UI-only update task:
 *    7. Download "index.html" from the latest ui-v* release and write to LittleFS.
 *    8. Serve the new UI immediately without a reboot.
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
#include <WiFi.h>
#include <LittleFS.h>
#endif

// ─── Module state ──────────────────────────────────────────────────────────────
static volatile const char *s_status         = "idle";
static char                 s_latest_fw_tag[32] = ""; // latest firmware release (fw-v* or v*)
static char                 s_latest_ui_tag[32] = ""; // latest Web UI release   (ui-v*)
static volatile bool        s_task_running   = false;
// Mutex protecting s_latest_fw_tag, s_latest_ui_tag, s_asset_url, s_html_url,
// s_ui_update_available, and s_error_msg
// (written by OTA task, read by web handler task).
static SemaphoreHandle_t    s_tag_mutex      = nullptr;
// Last error detail string – populated on every failure, cleared on new attempt.
// Exposed via ota_get_error() so the web UI can show more than "failed".
static char                 s_error_msg[128] = "";
// Whether the Web UI in the latest ui-v* release is newer than LittleFS.
static volatile bool        s_ui_update_available = false;
// Asset download URLs cached by the check task so the update task can skip the
// redundant GitHub API round-trip between "Check for Updates" and "Apply Update".
static char                 s_asset_url[256] = "";
static char                 s_html_url[256]  = "";

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

// ─── LittleFS UI download helper ─────────────────────────────────────────────
/**
 * @brief Download @p url and write the response body to @p lfs_path on LittleFS.
 *
 * Uses an atomic write pattern: downloads to a temp file first, then renames
 * to the target path so the live file is never left in a truncated state.
 * Returns true on success.  On failure, sets the error message and returns
 * false; the live file at @p lfs_path is left untouched.
 */
static bool download_to_lfs(const char *url, const char *lfs_path) {
    // Derive the temp path by appending ".tmp" to the target path so this
    // helper works correctly for any LittleFS destination (not just /index.html).
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", lfs_path);
    LittleFS.remove(tmp_path); // clean any stale temp file from a previous attempt

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    http.addHeader("User-Agent", "ESP32-FilamentSensor/" FIRMWARE_VERSION);

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        DBG_PRINTF("[OTA] UI download HTTP %d\n", code);
        set_error_msg("UI download HTTP %d: %s", code,
                      http.errorToString(code).c_str());
        http.end();
        return false;
    }

    File f = LittleFS.open(tmp_path, "w");
    if (!f) {
        DBG_PRINTLN("[OTA] UI: LittleFS open for write failed");
        set_error_msg("UI: LittleFS open failed");
        http.end();
        return false;
    }

    WiFiClient *stream   = http.getStreamPtr();
    int32_t  remaining   = http.getSize(); // -1 for chunked encoding
    int32_t  written     = 0;
    uint8_t  buf[512];
    const uint32_t deadline = millis() + OTA_HTTP_TIMEOUT_MS;

    while (http.connected() && (remaining > 0 || remaining == -1)) {
        const int avail = stream->available();
        if (avail > 0) {
            const size_t chunk_size = (size_t)(avail < (int)sizeof(buf)
                                               ? avail : (int)sizeof(buf));
            const int chunk = stream->readBytes(buf, chunk_size);
            if (chunk > 0) {
                if (f.write(buf, (size_t)chunk) != (size_t)chunk) {
                    f.close();
                    LittleFS.remove(tmp_path);
                    http.end();
                    set_error_msg("UI: LittleFS write error");
                    return false;
                }
                written += chunk;
                if (remaining > 0) remaining -= chunk;
            }
        } else if (millis() > deadline) {
            DBG_PRINTLN("[OTA] UI download timed out");
            f.close();
            LittleFS.remove(tmp_path);
            http.end();
            set_error_msg("UI download timed out");
            return false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    f.close();
    http.end();

    if (written == 0) {
        LittleFS.remove(tmp_path);
        set_error_msg("UI: empty response");
        return false;
    }

    // Atomically replace the live file.
    LittleFS.remove(lfs_path);
    if (!LittleFS.rename(tmp_path, lfs_path)) {
        LittleFS.remove(tmp_path);
        set_error_msg("UI: LittleFS rename failed");
        return false;
    }

    DBG_PRINTF("[OTA] UI updated: %d bytes written to %s\n", (int)written, lfs_path);
    return true;
}
/**
 * @brief Return true when @p tag represents a firmware version newer than
 *        FIRMWARE_VERSION.
 *
 * Accepts "fw-v1.2.3" (new-style firmware tag), "v1.2.3" (old-style), or
 * plain "1.2.3".  The leading prefix is stripped before comparison.
 */
static bool version_is_newer(const char *tag) {
    const char *v = tag;
    if (strncmp(v, "fw-v", 4) == 0)      v += 4;
    else if (v[0] == 'v')                 v += 1;

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

/**
 * @brief Return true when @p tag represents a Web UI version newer than the
 *        ui-version currently stored in /index.html on LittleFS.
 *
 * Accepts "ui-v1.2.3" (new-style UI tag) or plain "1.2.3".  Returns false
 * when the LittleFS file is absent, unreadable, or has no ui-version tag.
 */
static bool ui_version_is_newer(const char *tag) {
    const char *v = tag;
    if (strncmp(v, "ui-v", 4) == 0) v += 4;
    else if (v[0] == 'v')            v += 1;

    // Read current UI version embedded in LittleFS /index.html.
    char cur_ui[32] = "";
    File f = LittleFS.open("/index.html", "r");
    if (f) {
        char head[512];
        const int n = f.readBytes(head, sizeof(head) - 1);
        head[n] = '\0';
        f.close();
        const char *needle = "ui-version\" content=\"";
        const char *found  = strstr(head, needle);
        if (found) {
            const char *start = found + strlen(needle);
            const char *end   = strchr(start, '"');
            if (end) {
                size_t vlen = (size_t)(end - start);
                if (vlen < sizeof(cur_ui)) {
                    strncpy(cur_ui, start, vlen);
                    cur_ui[vlen] = '\0';
                }
            }
        }
    }

    if (cur_ui[0] == '\0') return false; // unknown current version

    int cur_major = 0, cur_minor = 0, cur_patch = 0;
    int new_major = 0, new_minor = 0, new_patch = 0;

    if (sscanf(cur_ui, "%d.%d.%d", &cur_major, &cur_minor, &cur_patch) != 3)
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

    // ── Step 0: Verify WiFi is up before attempting any HTTPS calls ────────────
    if (WiFi.status() != WL_CONNECTED) {
        DBG_PRINTLN("[OTA] WiFi not connected – aborting OTA");
        set_error_msg("WiFi not connected");
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    char asset_url[256] = "";
    char html_url[256]  = "";

    // ── For update tasks: reuse asset URLs cached by the preceding check task ──
    // This avoids a redundant HTTPS round-trip to api.github.com between the user
    // clicking "Check for Updates" and then "Apply Update".  The check and update
    // tasks are serialised by s_task_running, so the cached URLs are always from
    // the most recent completed check.
    bool have_cached = false;
    if (!s_check_only && s_tag_mutex &&
        xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        have_cached = (s_asset_url[0] != '\0');
        if (have_cached) {
            strncpy(asset_url, s_asset_url, sizeof(asset_url) - 1);
            asset_url[sizeof(asset_url) - 1] = '\0';
            DBG_PRINTF("[OTA] Using cached firmware URL for %s\n", s_latest_fw_tag);
        }
        xSemaphoreGive(s_tag_mutex);
    }

    if (!have_cached) {
        // ── Step 1: Fetch the releases list ───────────────────────────────────
        DBG_PRINTF("[OTA] Checking GitHub releases at %s\n", GITHUB_RELEASES_URL);
        WiFiClientSecure api_client;
        api_client.setInsecure(); // skips certificate verification

        HTTPClient http;
        http.begin(api_client, GITHUB_RELEASES_URL);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
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

        // ── Step 2: Parse releases list JSON ──────────────────────────────────
        // The response is a JSON array.  Use a filter to keep only the fields
        // we need (tag_name and asset name + URL) to minimise heap pressure.
        JsonDocument filter;
        JsonArray    filter_arr  = filter.to<JsonArray>();
        JsonObject   filter_item = filter_arr.add<JsonObject>();
        filter_item["tag_name"]  = true;
        JsonArray  filter_assets = filter_item["assets"].to<JsonArray>();
        JsonObject filter_asset  = filter_assets.add<JsonObject>();
        filter_asset["name"]                    = true;
        filter_asset["browser_download_url"]    = true;

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

        // ── Step 3: Scan releases for latest fw-v* and ui-v* entries ──────────
        // Releases are returned newest-first; take the first matching entry for
        // each category.  Old-style "v*" tags count as firmware for backward compat.
        char fw_tag[32]  = "";
        char ui_tag[32]  = "";

        for (JsonObject release : doc.as<JsonArray>()) {
            const char *tag = release["tag_name"] | "";
            const bool  is_fw  = (strncmp(tag, "fw-v", 4) == 0) ||
                                  (tag[0] == 'v' && strncmp(tag, "ui-", 0) != 0 &&
                                   strncmp(tag, "ui-v", 4) != 0);
            const bool  is_ui  = (strncmp(tag, "ui-v", 4) == 0);

            if (is_fw && fw_tag[0] == '\0') {
                strncpy(fw_tag, tag, sizeof(fw_tag) - 1);
                fw_tag[sizeof(fw_tag) - 1] = '\0';
                for (JsonObject asset : release["assets"].as<JsonArray>()) {
                    const char *name = asset["name"] | "";
                    const char *url  = asset["browser_download_url"] | "";
                    if (strcmp(name, "firmware.bin") == 0) {
                        strncpy(asset_url, url, sizeof(asset_url) - 1);
                        asset_url[sizeof(asset_url) - 1] = '\0';
                        break;
                    }
                }
            }
            if (is_ui && ui_tag[0] == '\0') {
                strncpy(ui_tag, tag, sizeof(ui_tag) - 1);
                ui_tag[sizeof(ui_tag) - 1] = '\0';
                for (JsonObject asset : release["assets"].as<JsonArray>()) {
                    const char *name = asset["name"] | "";
                    const char *url  = asset["browser_download_url"] | "";
                    if (strcmp(name, "index.html") == 0) {
                        strncpy(html_url, url, sizeof(html_url) - 1);
                        html_url[sizeof(html_url) - 1] = '\0';
                        break;
                    }
                }
            }
            if (fw_tag[0] != '\0' && ui_tag[0] != '\0') break; // found both
        }

        const bool fw_newer = (fw_tag[0] != '\0') && version_is_newer(fw_tag);
        const bool ui_newer = (ui_tag[0] != '\0') && ui_version_is_newer(ui_tag);

        DBG_PRINTF("[OTA] Latest fw: %s (current: %s)%s\n",
                   fw_tag[0] ? fw_tag : "none", FIRMWARE_VERSION,
                   fw_newer ? " → update available" : "");
        DBG_PRINTF("[OTA] Latest ui: %s%s\n",
                   ui_tag[0] ? ui_tag : "none",
                   ui_newer ? " → update available" : "");

        // ── Step 4: Cache tags and URLs under the mutex ────────────────────────
        if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(s_latest_fw_tag, fw_tag, sizeof(s_latest_fw_tag) - 1);
            s_latest_fw_tag[sizeof(s_latest_fw_tag) - 1] = '\0';
            strncpy(s_latest_ui_tag, ui_tag, sizeof(s_latest_ui_tag) - 1);
            s_latest_ui_tag[sizeof(s_latest_ui_tag) - 1] = '\0';
            s_ui_update_available = ui_newer;
            // Cache asset URL so the update task can reuse it.
            s_asset_url[0] = '\0';
            if (asset_url[0] != '\0') {
                strncpy(s_asset_url, asset_url, sizeof(s_asset_url) - 1);
                s_asset_url[sizeof(s_asset_url) - 1] = '\0';
            }
            // Cache html URL for the UI-only update task.
            s_html_url[0] = '\0';
            if (html_url[0] != '\0') {
                strncpy(s_html_url, html_url, sizeof(s_html_url) - 1);
                s_html_url[sizeof(s_html_url) - 1] = '\0';
            }
            xSemaphoreGive(s_tag_mutex);
        }

        // ── Step 5: Set check-only status ──────────────────────────────────────
        if (s_check_only) {
            if (fw_newer) {
                DBG_PRINTF("[OTA] Firmware update available: %s\n", fw_tag);
                s_status = "update-available";
            } else if (ui_newer) {
                DBG_PRINTF("[OTA] UI update available: %s\n", ui_tag);
                s_status = "ui-update-available";
            } else {
                DBG_PRINTLN("[OTA] Everything is up-to-date");
                s_status = "up-to-date";
            }
            s_task_running = false;
            vTaskDelete(nullptr);
            return;
        }

        if (!fw_newer) {
            // Firmware is up-to-date; update task should not have been called.
            // Treat it as informational and exit cleanly.
            DBG_PRINTLN("[OTA] Firmware is already up-to-date – nothing to flash");
            if (ui_newer) {
                s_status = "ui-update-available";
            } else {
                s_status = "up-to-date";
            }
            s_task_running = false;
            vTaskDelete(nullptr);
            return;
        }
    } // end !have_cached

    if (asset_url[0] == '\0') {
        DBG_PRINTLN("[OTA] No firmware.bin asset found in release");
        set_error_msg("No firmware.bin asset in release %s", s_latest_fw_tag);
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    DBG_PRINTF("[OTA] Downloading firmware: %s\n", asset_url);
    s_status = "updating";

    // ── Step 6: Stream firmware via HTTPUpdate ─────────────────────────────────
    // Web UI (index.html) is managed by its own independent release track and
    // update path (/api/ota/update-ui).  Firmware updates do NOT automatically
    // overwrite the UI so the two versions remain independently managed.
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

// ─── GitHub UI-only update background task ────────────────────────────────────
/**
 * Downloads the "index.html" asset from the latest GitHub release and writes
 * it to /index.html on LittleFS.  No firmware flashing or reboot is performed.
 * Reuses the cached html URL from a preceding check task when available.
 * When no cache is present, performs a fresh releases list query and looks for
 * the latest ui-v* tag.
 */
static void github_update_ui_task(void * /*param*/) {
    set_error_msg("");
    s_status = "updating-ui";
    DBG_PRINTLN("[OTA] UI-only update requested");

    if (WiFi.status() != WL_CONNECTED) {
        DBG_PRINTLN("[OTA] WiFi not connected – aborting UI update");
        set_error_msg("WiFi not connected");
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    char html_url[256] = "";

    // Reuse URL cached by a preceding check task (avoids an extra API call).
    bool have_cached = false;
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        have_cached = (s_html_url[0] != '\0');
        if (have_cached) {
            strncpy(html_url, s_html_url, sizeof(html_url) - 1);
            html_url[sizeof(html_url) - 1] = '\0';
            DBG_PRINTF("[OTA] Using cached UI URL for %s\n", s_latest_ui_tag);
        }
        xSemaphoreGive(s_tag_mutex);
    } else {
        DBG_PRINTLN("[OTA] UI update: mutex timeout reading URL cache – will fetch API");
    }

    if (!have_cached) {
        DBG_PRINTF("[OTA] Fetching GitHub releases list at %s\n", GITHUB_RELEASES_URL);
        WiFiClientSecure api_client;
        api_client.setInsecure();

        HTTPClient http;
        http.begin(api_client, GITHUB_RELEASES_URL);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
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

        JsonDocument filter;
        JsonArray    filter_arr  = filter.to<JsonArray>();
        JsonObject   filter_item = filter_arr.add<JsonObject>();
        filter_item["tag_name"]  = true;
        JsonArray  filter_assets = filter_item["assets"].to<JsonArray>();
        JsonObject filter_asset  = filter_assets.add<JsonObject>();
        filter_asset["name"]                 = true;
        filter_asset["browser_download_url"] = true;

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

        // Scan for the latest ui-v* release and extract its index.html URL.
        char ui_tag[32] = "";
        for (JsonObject release : doc.as<JsonArray>()) {
            const char *tag = release["tag_name"] | "";
            if (strncmp(tag, "ui-v", 4) != 0) continue; // skip non-UI releases
            strncpy(ui_tag, tag, sizeof(ui_tag) - 1);
            ui_tag[sizeof(ui_tag) - 1] = '\0';
            for (JsonObject asset : release["assets"].as<JsonArray>()) {
                const char *name = asset["name"] | "";
                const char *url  = asset["browser_download_url"] | "";
                if (strcmp(name, "index.html") == 0) {
                    strncpy(html_url, url, sizeof(html_url) - 1);
                    html_url[sizeof(html_url) - 1] = '\0';
                    break;
                }
            }
            break; // first match is the most recent
        }

        // Cache for future operations.
        if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(s_latest_ui_tag, ui_tag, sizeof(s_latest_ui_tag) - 1);
            s_latest_ui_tag[sizeof(s_latest_ui_tag) - 1] = '\0';
            strncpy(s_html_url, html_url, sizeof(s_html_url) - 1);
            s_html_url[sizeof(s_html_url) - 1] = '\0';
            xSemaphoreGive(s_tag_mutex);
        } else {
            DBG_PRINTLN("[OTA] UI update: mutex timeout writing URL cache");
        }
    }

    if (html_url[0] == '\0') {
        DBG_PRINTLN("[OTA] No index.html asset found in any ui-v* release");
        set_error_msg("No index.html asset in UI release");
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    DBG_PRINTF("[OTA] Downloading UI to LittleFS: %s\n", html_url);
    if (!download_to_lfs(html_url, "/index.html")) {
        // set_error_msg already set by download_to_lfs.
        s_status = "failed";
        s_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    // Clear the ui_update_available flag since the UI is now current.
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_ui_update_available = false;
        xSemaphoreGive(s_tag_mutex);
    }

    DBG_PRINTLN("[OTA] UI updated on LittleFS – no reboot required");
    s_status = "ok";
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

void ota_github_update_ui_request() {
#if ENABLE_GITHUB_OTA
    if (s_task_running) {
        DBG_PRINTLN("[OTA] Task already running – UI update request ignored");
        return;
    }
    s_task_running = true;

    // 20480 bytes: TLS handshake + JSON parse + LittleFS write call chain.
    const BaseType_t ok = xTaskCreatePinnedToCore(
        github_update_ui_task,
        "OTA_UI",
        20480,
        nullptr,
        2,
        nullptr,
        0   // Core 0
    );

    if (ok != pdPASS) {
        DBG_PRINTLN("[OTA] Failed to create UI update task");
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

const char *ota_get_latest_fw_tag() {
    // Return a mutex-protected copy to avoid a data race with the OTA task.
    static char buf[32] = "";
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(buf, s_latest_fw_tag, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        xSemaphoreGive(s_tag_mutex);
    }
    return buf;
}

const char *ota_get_latest_ui_tag() {
    // Return a mutex-protected copy to avoid a data race with the OTA task.
    static char buf[32] = "";
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(buf, s_latest_ui_tag, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        xSemaphoreGive(s_tag_mutex);
    }
    return buf;
}

bool ota_get_ui_update_available() {
    bool val = false;
    if (s_tag_mutex && xSemaphoreTake(s_tag_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        val = s_ui_update_available;
        xSemaphoreGive(s_tag_mutex);
    }
    return val;
}

const char *ota_get_lfs_ui_version() {
    static char ver[32] = "";
    ver[0] = '\0';

    // The version is embedded as <meta name="ui-version" content="x.y.z"> near
    // the top of /index.html.  Reading only the first 512 bytes is enough.
#if ENABLE_GITHUB_OTA
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
        strncpy(ver, "unknown", sizeof(ver) - 1);
        return ver;
    }

    char head[512];
    const int n = f.readBytes(head, sizeof(head) - 1);
    head[n] = '\0';
    f.close();

    // Pattern: <meta name="ui-version" content="1.2.3">
    const char *needle = "ui-version\" content=\"";
    const char *found  = strstr(head, needle);
    if (found) {
        const char *start = found + strlen(needle);
        const char *end   = strchr(start, '"');
        if (end) {
            size_t vlen = (size_t)(end - start);
            if (vlen >= sizeof(ver)) vlen = sizeof(ver) - 1;
            strncpy(ver, start, vlen);
            ver[vlen] = '\0';
            return ver;
        }
    }
#endif
    strncpy(ver, "unknown", sizeof(ver) - 1);
    return ver;
}
