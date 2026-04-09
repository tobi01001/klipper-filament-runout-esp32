#include "web_handler.h"
#include "fault_detector.h"
#include "nvs_config.h"
#include "ota_handler.h"
#include "moonraker.h"
#include "wifi_handler.h"
#include "config.h"
#include "debug_log.h"

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <LittleFS.h>

// ─── Module state ─────────────────────────────────────────────────────────────
static WebServer          s_server(WEB_SERVER_PORT);
static SemaphoreHandle_t  s_status_mutex = nullptr;
static SemaphoreHandle_t  s_config_mutex = nullptr;
static SensorStatus      *s_status       = nullptr;
static SensorConfig      *s_config       = nullptr;

// ─── Fallback page (served when LittleFS index.html is absent) ───────────────
// This is intentionally minimal. The full web UI lives in data/index.html and
// must be uploaded to the device filesystem with:  pio run --target uploadfs
static const char FALLBACK_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1.0"/>
  <title>Filament Runout Sensor – Setup Required</title>
  <style>
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
         background:#111;color:#eee;padding:20px;max-width:700px;margin:0 auto;line-height:1.6}
    h1{color:#f90;font-size:1.3rem;margin-bottom:12px}
    h2{color:#aaa;font-size:1rem;margin:18px 0 6px}
    .warn{background:#332200;border:1px solid #a60;border-radius:6px;padding:12px;margin-bottom:16px}
    .info{background:#1a2a1a;border:1px solid #0a5;border-radius:6px;padding:12px;margin-bottom:16px}
    code{background:#2a2a2a;padding:2px 6px;border-radius:3px;font-size:.9em}
    pre{background:#0a0a0a;padding:12px;border-radius:5px;overflow-x:auto;font-size:.85rem}
    ol{padding-left:1.4em}
    li{margin:4px 0}
    a{color:#06f}
  </style>
</head>
<body>
  <h1>&#9888; Web Interface Files Not Found</h1>
  <div class="warn">
    The web interface files have not been uploaded to the device filesystem (LittleFS).
    The device firmware is running normally &mdash; only the web UI is missing.
  </div>
  <h2>Upload via PlatformIO (recommended)</h2>
  <div class="info">
    <ol>
      <li>Clone or download the repository:<br>
        <a href="https://github.com/tobi01001/klipper-filament-runout-esp32">
          https://github.com/tobi01001/klipper-filament-runout-esp32</a></li>
      <li>Connect the device via USB</li>
      <li>Run: <code>pio run --target uploadfs</code><br>
        Or in VS&nbsp;Code: click <strong>Upload Filesystem Image</strong> in the PlatformIO toolbar</li>
      <li>Reset the device and reload this page</li>
    </ol>
  </div>
  <h2>Upload via Arduino IDE</h2>
  <div class="info">
    <ol>
      <li>Install the <a href="https://github.com/lorol/arduino-esp32littlefs-plugin">
        ESP32 LittleFS Filesystem Uploader plugin</a></li>
      <li>Download the <code>data/</code> folder from the repository and place its contents
        in your sketch&rsquo;s <code>data/</code> subfolder</li>
      <li>Use <strong>Tools &rarr; ESP32 LittleFS Data Upload</strong></li>
    </ol>
  </div>
  <h2>Download files directly</h2>
  <p><a href="https://raw.githubusercontent.com/tobi01001/klipper-filament-runout-esp32/main/data/index.html">
    data/index.html (raw)</a></p>
</body>
</html>
)rawhtml";

// ─── LittleFS helpers ─────────────────────────────────────────────────────────
static String get_content_type(const String &path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".gz"))   return "application/x-gzip";
    return "text/plain";
}

// ─── REST Handlers ────────────────────────────────────────────────────────────
static void handle_root() {
    File f = LittleFS.open("/index.html", "r");
    if (f) {
        s_server.streamFile(f, "text/html");
        f.close();
    } else {
        s_server.send_P(200, "text/html", FALLBACK_HTML);
    }
}

static void handle_status() {
    SensorStatus snap{};
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = *s_status;
        xSemaphoreGive(s_status_mutex);
    }

    const uint32_t motion_ago = millis() - snap.encoder.timestamp_ms;

    JsonDocument doc;
    doc["state"]       = state_name(snap.state);
    doc["enc_vel"]     = snap.encoder.velocity_mm_s;
    doc["ext_vel"]     = snap.extruder_vel;
    doc["ticks"]       = snap.encoder.tick_count;
    doc["direction"]   = snap.encoder.direction;
    doc["motion_ago_ms"] = motion_ago;
    doc["fault"]       = snap.fault_active;
    doc["wifi"]        = snap.wifi_connected;
    doc["ip"]          = snap.ip_address;
    doc["mr_connected"] = snap.moonraker_connected;
    doc["mr_subscribed"] = snap.moonraker_subscribed;
    doc["mr_stale"] = snap.moonraker_stale;
    doc["klippy_state"] = snap.klippy_state;
    doc["nozzle_temp"] = snap.nozzle_temp;
    doc["nozzle_target"] = snap.nozzle_target;

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_get_config() {
    SensorConfig snap{};
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = *s_config;
        xSemaphoreGive(s_config_mutex);
    }

    JsonDocument doc;
    doc["cal_factor"]       = snap.cal_factor;
    doc["timeout_ms"]       = snap.timeout_ms;
    doc["min_ext_vel"]      = snap.min_ext_vel;
    doc["motion_threshold"] = snap.motion_threshold;
    doc["moonraker_ip"]     = snap.moonraker_ip;
    doc["moonraker_port"]   = snap.moonraker_port;
    doc["wifi_ssid"]        = snap.wifi_ssid;
    doc["ota_hostname"]     = snap.ota_hostname;
    // Password is never sent to the client for security reasons.
    doc["sensor_enabled"]  = snap.sensor_enabled;
    doc["display_enabled"]  = snap.display_enabled;
    doc["fault_gcode"]      = snap.fault_gcode;

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_post_config() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, s_server.arg("plain"))) {
        s_server.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (doc["cal_factor"].is<float>())
            s_config->cal_factor = doc["cal_factor"].as<float>();
        if (doc["timeout_ms"].is<uint32_t>())
            s_config->timeout_ms = doc["timeout_ms"].as<uint32_t>();
        if (doc["min_ext_vel"].is<float>())
            s_config->min_ext_vel = doc["min_ext_vel"].as<float>();
        if (doc["motion_threshold"].is<int32_t>())
            s_config->motion_threshold = doc["motion_threshold"].as<int32_t>();
        if (doc["moonraker_port"].is<uint16_t>())
            s_config->moonraker_port = doc["moonraker_port"].as<uint16_t>();

        if (doc["moonraker_ip"].is<const char *>()) {
            strncpy(s_config->moonraker_ip,
                    doc["moonraker_ip"].as<const char *>(),
                    sizeof(s_config->moonraker_ip) - 1);
            s_config->moonraker_ip[sizeof(s_config->moonraker_ip) - 1] = '\0';
        }
        if (doc["wifi_ssid"].is<const char *>()) {
            strncpy(s_config->wifi_ssid,
                    doc["wifi_ssid"].as<const char *>(),
                    sizeof(s_config->wifi_ssid) - 1);
            s_config->wifi_ssid[sizeof(s_config->wifi_ssid) - 1] = '\0';
        }
        // Only update password if a non-empty value was supplied
        const char *new_pass = doc["wifi_pass"] | "";
        if (new_pass[0] != '\0') {
            strncpy(s_config->wifi_pass, new_pass,
                    sizeof(s_config->wifi_pass) - 1);
            s_config->wifi_pass[sizeof(s_config->wifi_pass) - 1] = '\0';
        }
        if (doc["ota_hostname"].is<const char *>()) {
            const char *new_host = doc["ota_hostname"].as<const char *>();
            if (new_host[0] != '\0') {
                strncpy(s_config->ota_hostname, new_host,
                        sizeof(s_config->ota_hostname) - 1);
                s_config->ota_hostname[sizeof(s_config->ota_hostname) - 1] = '\0';
            }
        }
        // Only update OTA password if a non-empty value was supplied
        const char *new_ota_pass = doc["ota_password"] | "";
        if (new_ota_pass[0] != '\0') {
            strncpy(s_config->ota_password, new_ota_pass,
                    sizeof(s_config->ota_password) - 1);
            s_config->ota_password[sizeof(s_config->ota_password) - 1] = '\0';
        }

        if (doc["display_enabled"].is<bool>())
            s_config->display_enabled = doc["display_enabled"].as<bool>();
        if (doc["fault_gcode"].is<const char *>()) {
            strncpy(s_config->fault_gcode,
                    doc["fault_gcode"].as<const char *>(),
                    sizeof(s_config->fault_gcode) - 1);
            s_config->fault_gcode[sizeof(s_config->fault_gcode) - 1] = '\0';
        }
        if (doc["sensor_enabled"].is<bool>()) {
          s_config->sensor_enabled = doc["sensor_enabled"].as<bool>();
          if (!s_config->sensor_enabled) {
            fault_detector_reset();
          }
        }

        nvs_save(*s_config);
        xSemaphoreGive(s_config_mutex);
    } else {
        s_server.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"mutex timeout\"}");
        return;
    }

    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_reset() {
    fault_detector_reset();
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_reboot() {
    // Send the response first so the client receives the confirmation before
    // the device resets.  The short delay gives the TCP stack time to flush.
    static constexpr unsigned REBOOT_FLUSH_DELAY_MS = 100;
    s_server.send(200, "application/json", "{\"ok\":true}");
    delay(REBOOT_FLUSH_DELAY_MS);
    ESP.restart();
}

static void handle_calibrate_get() {
    CalibrationStatus cal{};
    fault_detector_get_cal_status(&cal);

    const char *state_str;
    switch (cal.state) {
        case CalState::SENT:     state_str = "sent";     break;
        case CalState::MOVING:   state_str = "moving";   break;
        case CalState::SETTLING: state_str = "settling"; break;
        case CalState::DONE:     state_str = "done";     break;
        case CalState::FAILED:   state_str = "failed";   break;
        default:                 state_str = "idle";     break;
    }

    JsonDocument doc;
    doc["state"]      = state_str;
    doc["ticks"]      = cal.measured_ticks;
    doc["mm"]         = cal.requested_mm;
    doc["cal_factor"] = cal.result_cal_factor;
    doc["error"]      = cal.error;

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_calibrate_post() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, s_server.arg("plain"))) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }

    const float mm    = doc["extrude_mm"] | CAL_EXTRUDE_MM;
    const float speed = doc["speed_mmpm"] | CAL_EXTRUDE_SPEED_MMPM;

    if (mm < 10.0f || mm > 200.0f) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"extrude_mm out of range 10-200\"}");
        return;
    }
    if (speed < 60.0f || speed > 600.0f) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"speed_mmpm out of range 60-600\"}");
        return;
    }

    // Verify Klippy is ready before sending extrusion GCODE
    bool mr_ready = false;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        mr_ready = s_status->moonraker_connected &&
                   (strcmp(s_status->klippy_state, "ready") == 0);
        xSemaphoreGive(s_status_mutex);
    }
    if (!mr_ready) {
        s_server.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"Klippy not ready \u2013 connect Moonraker first\"}");
        return;
    }

    if (!fault_detector_start_calibration(mm, speed)) {
        s_server.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"calibration already running\"}");
        return;
    }

    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_sensor_get() {
  bool enabled = true;
  if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    enabled = s_config->sensor_enabled;
    xSemaphoreGive(s_config_mutex);
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = enabled;
  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

static void handle_sensor_post() {
  if (!s_server.hasArg("plain")) {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  if (!doc["enabled"].is<bool>()) {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"enabled must be bool\"}");
    return;
  }

  const bool enabled = doc["enabled"].as<bool>();
  const bool persist = doc["persist"].is<bool>() ? doc["persist"].as<bool>() : true;
  SensorConfig snap{};

  if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    s_server.send(503, "application/json", "{\"ok\":false,\"error\":\"mutex timeout\"}");
    return;
  }
  s_config->sensor_enabled = enabled;
  snap = *s_config;
  xSemaphoreGive(s_config_mutex);

  if (!enabled) {
    fault_detector_reset();
  }
  if (persist) {
    nvs_save(snap);
  }

  JsonDocument rsp;
  rsp["ok"] = true;
  rsp["enabled"] = enabled;
  rsp["persisted"] = persist;
  String out;
  serializeJson(rsp, out);
  s_server.send(200, "application/json", out);
}

static void handle_ota_get() {
    JsonDocument doc;
    doc["version"]    = FIRMWARE_VERSION;
    doc["latest_tag"] = ota_get_latest_tag();
    doc["status"]     = ota_get_status();
  doc["github_ota"] = (bool)ENABLE_GITHUB_OTA;

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_ota_check() {
#if ENABLE_GITHUB_OTA
    ota_github_check_request();
    s_server.send(200, "application/json", "{\"ok\":true}");
#else
  s_server.send(200, "application/json", "{\"ok\":false,\"error\":\"github ota disabled\"}");
#endif
}

static void handle_ota_update() {
#if ENABLE_GITHUB_OTA
    ota_github_update_request();
    s_server.send(200, "application/json", "{\"ok\":true}");
#else
  s_server.send(200, "application/json", "{\"ok\":false,\"error\":\"github ota disabled\"}");
#endif
}

static void handle_diag() {
  MoonrakerDiag mr{};
  moonraker_get_diag(&mr);

  const uint32_t now_ms = millis();
  uint32_t mr_age_ms = 0;
  if (mr.last_ext_rx_ms > 0 && now_ms >= mr.last_ext_rx_ms) {
    mr_age_ms = now_ms - mr.last_ext_rx_ms;
  }

  JsonDocument doc;
  doc["uptime_s"] = now_ms / 1000UL;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["wifi_disc_reason"] = wifi_last_disconnect_reason();
  doc["mr_ws_init"] = mr.ws_initialized;
  doc["mr_ws_conn"] = mr.ws_connected;
  doc["mr_sub"] = mr.subscribed;
  doc["mr_stale"] = mr.stale;
  doc["mr_last_ok"] = mr.last_connect_ok;
  doc["mr_fail_streak"] = mr.consecutive_connect_failures;
  doc["mr_backoff_ms"] = mr.reconnect_backoff_ms;
  doc["mr_last_host"] = mr.last_connect_host;
  doc["mr_last_port"] = mr.last_connect_port;
  doc["mr_probe_attempts"] = mr.ws_probe_attempts;
  doc["mr_probe_101"] = mr.ws_probe_101;
  doc["mr_probe_last_ms"] = mr.ws_probe_last_ms;
  doc["mr_probe_last_ok"] = mr.ws_probe_last_ok;
  doc["mr_probe_status_line"] = mr.ws_probe_status_line;
  doc["mr_age_ms"] = mr_age_ms;
  doc["klippy_state"] = mr.klippy_state;
  doc["mr_conn_evt"] = mr.ws_connect_events;
  doc["mr_disc_evt"] = mr.ws_disconnect_events;
  doc["mr_start_evt"] = mr.ws_start_attempts;
  doc["mr_info_req"] = mr.info_requests;
  doc["mr_sub_req"] = mr.subscribe_requests;
  doc["mr_query_req"] = mr.query_requests;
  doc["mr_json_err"] = mr.json_errors;

  char mr_log_buf[700];
  moonraker_copy_log(mr_log_buf, sizeof(mr_log_buf));
  doc["mr_log"] = mr_log_buf;

  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

static void handle_not_found() {
  const String uri = s_server.uri();
  if (uri.startsWith("/api/")) {
    s_server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }

  // Try to serve the requested path from LittleFS (CSS, JS, images, etc.)
  if (LittleFS.exists(uri)) {
    File f = LittleFS.open(uri, "r");
    if (f) {
      s_server.streamFile(f, get_content_type(uri));
      f.close();
      return;
    }
  }

  s_server.sendHeader("Location", "/", true);
  s_server.send(302, "text/plain", "");
}

// ─── Public API ───────────────────────────────────────────────────────────────
void web_init(SemaphoreHandle_t status_mutex,
              SemaphoreHandle_t config_mutex,
              SensorStatus     *status,
              SensorConfig     *config) {
    s_status_mutex = status_mutex;
    s_config_mutex = config_mutex;
    s_status       = status;
    s_config       = config;

    // Mount LittleFS – don't format on failure so we don't destroy user data.
    // If mount fails the fallback PROGMEM page will be served instead.
    if (!LittleFS.begin(false)) {
        DBG_PRINTLN("[WEB] LittleFS mount failed – web UI will use fallback page");
    } else {
        DBG_PRINTLN("[WEB] LittleFS mounted");
    }

    s_server.on("/",               HTTP_GET,  handle_root);
    s_server.on("/api/status",     HTTP_GET,  handle_status);
    s_server.on("/api/config",     HTTP_GET,  handle_get_config);
    s_server.on("/api/config",     HTTP_POST, handle_post_config);
    s_server.on("/api/sensor",     HTTP_GET,  handle_sensor_get);
    s_server.on("/api/sensor",     HTTP_POST, handle_sensor_post);
    s_server.on("/api/reset",      HTTP_POST, handle_reset);
    s_server.on("/api/reboot",     HTTP_POST, handle_reboot);
    s_server.on("/api/calibrate",  HTTP_GET,  handle_calibrate_get);
    s_server.on("/api/calibrate",  HTTP_POST, handle_calibrate_post);
    s_server.on("/api/ota",        HTTP_GET,  handle_ota_get);
    s_server.on("/api/ota/check",  HTTP_POST, handle_ota_check);
    s_server.on("/api/ota/update", HTTP_POST, handle_ota_update);
    s_server.on("/api/diag",       HTTP_GET,  handle_diag);
    s_server.onNotFound(handle_not_found);

    s_server.begin();
    DBG_PRINTLN("[WEB] HTTP server started on port " + String(WEB_SERVER_PORT));
}

void web_handle_client() {
    s_server.handleClient();
}
