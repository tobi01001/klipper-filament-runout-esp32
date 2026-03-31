#include "web_handler.h"
#include "fault_detector.h"
#include "nvs_config.h"
#include "ota_handler.h"
#include "config.h"

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ─── Module state ─────────────────────────────────────────────────────────────
static WebServer          s_server(WEB_SERVER_PORT);
static SemaphoreHandle_t  s_status_mutex = nullptr;
static SemaphoreHandle_t  s_config_mutex = nullptr;
static SensorStatus      *s_status       = nullptr;
static SensorConfig      *s_config       = nullptr;

// ─── Embedded Single-Page Application ────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Filament Runout Sensor</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
         background:#111;color:#eee;padding:16px}
    h1{font-size:1.4rem;margin-bottom:16px;color:#f90}
    h2{font-size:1rem;margin-bottom:10px;color:#aaa}
    .card{background:#1e1e1e;border-radius:8px;padding:16px;margin-bottom:14px;
          border:1px solid #333}
    .badge{display:inline-block;padding:4px 12px;border-radius:20px;
           font-size:.85rem;font-weight:600;margin-bottom:12px}
    .badge-ready   {background:#0a5;color:#fff}
    .badge-printing{background:#06f;color:#fff}
    .badge-fault   {background:#c00;color:#fff;animation:pulse 1s infinite}
    .badge-idle    {background:#555;color:#fff}
    .badge-wifi    {background:#a60;color:#fff}
    @keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
    table{width:100%;border-collapse:collapse;font-size:.9rem}
    td{padding:5px 4px;border-bottom:1px solid #2a2a2a}
    td:first-child{color:#aaa;width:55%}
    td:last-child{text-align:right;font-family:monospace}
    label{display:block;font-size:.85rem;color:#aaa;margin:8px 0 2px}
    input[type=text],input[type=number]{width:100%;padding:6px 8px;
      background:#2a2a2a;border:1px solid #444;border-radius:4px;
      color:#eee;font-size:.9rem}
    .row{display:flex;gap:10px}
    .row>*{flex:1}
    button{width:100%;margin-top:10px;padding:8px;border:none;border-radius:5px;
           font-size:.9rem;cursor:pointer;font-weight:600}
    .btn-save  {background:#0a5;color:#fff}
    .btn-reset {background:#c00;color:#fff}
    .btn-save:hover{background:#0c6}
    .btn-reset:hover{background:#e00}
    #msg{font-size:.8rem;text-align:center;margin-top:6px;min-height:1.2em;color:#0f0}
    .vel-bar{height:8px;background:#2a2a2a;border-radius:4px;margin-top:4px;
             overflow:hidden}
    .vel-bar-inner{height:100%;background:#06f;border-radius:4px;
                   transition:width .3s}
    .toggle-row{display:flex;align-items:center;gap:10px;margin:8px 0 2px}
    .toggle-row label{margin:0;flex:1}
    input[type=checkbox]{width:18px;height:18px;accent-color:#06f;cursor:pointer}
  </style>
</head>
<body>
  <h1>&#x1F9F5; Filament Runout Sensor</h1>

  <!-- Status Card -->
  <div class="card">
    <h2>Live Status</h2>
    <div id="badge" class="badge badge-idle">…</div>
    <table>
      <tr><td>Encoder velocity</td><td id="enc-vel">-</td></tr>
      <tr><td>Extruder velocity</td><td id="ext-vel">-</td></tr>
      <tr><td>Tick count</td><td id="ticks">-</td></tr>
      <tr><td>Direction</td><td id="dir">-</td></tr>
      <tr><td>Last motion</td><td id="motion-ago">-</td></tr>
      <tr><td>Fault active</td><td id="fault">-</td></tr>
      <tr><td>IP address</td><td id="ip">-</td></tr>
    </table>
    <div class="vel-bar"><div class="vel-bar-inner" id="vel-bar" style="width:0%"></div></div>
    <button class="btn-reset" onclick="resetFault()">&#x26A0; Reset Fault</button>
  </div>

  <!-- Configuration Card -->
  <div class="card">
    <h2>Configuration</h2>
    <div class="row">
      <div>
        <label>Calibration factor (mm/tick)</label>
        <input type="number" id="cal" step="0.001" min="0.001" max="10"/>
      </div>
      <div>
        <label>Fault timeout (ms)</label>
        <input type="number" id="timeout" step="100" min="500" max="10000"/>
      </div>
    </div>
    <div class="row">
      <div>
        <label>Min extruder velocity (mm/s)</label>
        <input type="number" id="min-vel" step="0.1" min="0.1" max="20"/>
      </div>
      <div>
        <label>Motion threshold (ticks)</label>
        <input type="number" id="thresh" step="1" min="1" max="100"/>
      </div>
    </div>
    <div class="row">
      <div>
        <label>Moonraker IP / hostname</label>
        <input type="text" id="mr-ip" maxlength="39"/>
      </div>
      <div>
        <label>Moonraker port</label>
        <input type="number" id="mr-port" step="1" min="1" max="65535"/>
      </div>
    </div>
    <label>WiFi SSID</label>
    <input type="text" id="wifi-ssid" maxlength="63"/>
    <label>WiFi Password</label>
    <input type="text" id="wifi-pass" maxlength="63" placeholder="leave blank to keep current"/>
    <div class="row">
      <div>
        <label>OTA hostname (.local)</label>
        <input type="text" id="ota-host" maxlength="31" placeholder="filament-sensor"/>
      </div>
      <div>
        <label>OTA push password</label>
        <input type="password" id="ota-pass" maxlength="31" placeholder="leave blank to keep current"/>
      </div>
    <div class="toggle-row">
      <input type="checkbox" id="disp-en"/>
      <label for="disp-en">Enable OLED display (SSD1306 128&#x00D7;64)</label>
    </div>
    <button class="btn-save" onclick="saveConfig()">&#x1F4BE; Save &amp; Apply</button>
    <div id="msg"></div>
  </div>

  <!-- OTA Update Card -->
  <div class="card">
    <h2>Firmware Update</h2>
    <table>
      <tr><td>Installed version</td><td id="ota-ver">-</td></tr>
      <tr><td>Latest release</td>   <td id="ota-latest">-</td></tr>
      <tr><td>Status</td>           <td id="ota-status">-</td></tr>
    </table>
    <button class="btn-save" id="btn-check" onclick="checkForUpdates()" style="margin-top:10px">
      &#x1F50D; Check for Updates
    </button>
    <button class="btn-reset" id="btn-apply" onclick="applyUpdate()" style="margin-top:6px;display:none">
      &#x2B06; Apply Update
    </button>
  </div>

  <script>
    let maxVel = 10;

    function badge(state) {
      const b = document.getElementById('badge');
      b.textContent = state;
      if      (state === 'PRINTING')          b.className = 'badge badge-printing';
      else if (state === 'FAULT')             b.className = 'badge badge-fault';
      else if (state === 'IDLE')              b.className = 'badge badge-idle';
      else if (state === 'READY')             b.className = 'badge badge-ready';
      else if (state.startsWith('WIFI'))      b.className = 'badge badge-wifi';
      else                                    b.className = 'badge badge-idle';
    }

    function refresh() {
      fetch('/api/status')
        .then(r=>r.json())
        .then(d=>{
          badge(d.state);
          document.getElementById('enc-vel').textContent   = d.enc_vel.toFixed(2)+' mm/s';
          document.getElementById('ext-vel').textContent   = d.ext_vel.toFixed(2)+' mm/s';
          document.getElementById('ticks').textContent     = d.ticks;
          const dirs = {'-1':'◀ Reverse','0':'● Stopped','1':'▶ Forward'};
          document.getElementById('dir').textContent       = dirs[String(d.direction)] ?? d.direction;
          document.getElementById('motion-ago').textContent= d.motion_ago_ms + ' ms ago';
          document.getElementById('fault').textContent     = d.fault ? '⚠ YES' : 'No';
          document.getElementById('fault').style.color     = d.fault ? '#f44' : '#4f4';
          document.getElementById('ip').textContent        = d.ip;
          const pct = Math.min(100, Math.abs(d.enc_vel) / maxVel * 100);
          document.getElementById('vel-bar').style.width   = pct + '%';
        }).catch(()=>{});
    }

    function loadConfig() {
      fetch('/api/config')
        .then(r=>r.json())
        .then(d=>{
          document.getElementById('cal').value        = d.cal_factor;
          document.getElementById('timeout').value    = d.timeout_ms;
          document.getElementById('min-vel').value    = d.min_ext_vel;
          document.getElementById('thresh').value     = d.motion_threshold;
          document.getElementById('mr-ip').value      = d.moonraker_ip;
          document.getElementById('mr-port').value    = d.moonraker_port;
          document.getElementById('wifi-ssid').value  = d.wifi_ssid;
          document.getElementById('ota-host').value   = d.ota_hostname;
          document.getElementById('disp-en').checked  = d.display_enabled;
          maxVel = Math.max(10, d.min_ext_vel * 20);
        }).catch(()=>{});
    }

    function saveConfig() {
      const body = {
        cal_factor:       parseFloat(document.getElementById('cal').value),
        timeout_ms:       parseInt(document.getElementById('timeout').value),
        min_ext_vel:      parseFloat(document.getElementById('min-vel').value),
        motion_threshold: parseInt(document.getElementById('thresh').value),
        moonraker_ip:     document.getElementById('mr-ip').value,
        moonraker_port:   parseInt(document.getElementById('mr-port').value),
        wifi_ssid:        document.getElementById('wifi-ssid').value,
        wifi_pass:        document.getElementById('wifi-pass').value,
        ota_hostname:     document.getElementById('ota-host').value,
        ota_password:     document.getElementById('ota-pass').value,
        display_enabled:  document.getElementById('disp-en').checked,
      };
      fetch('/api/config', {method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify(body)})
        .then(r=>r.json())
        .then(d=>{ showMsg(d.ok ? '✓ Saved' : '✗ '+d.error, d.ok); })
        .catch(()=>showMsg('✗ Request failed', false));
    }

    function resetFault() {
      fetch('/api/reset', {method:'POST'})
        .then(r=>r.json())
        .then(d=>showMsg(d.ok ? '✓ Fault cleared' : '✗ '+d.error, d.ok))
        .catch(()=>showMsg('✗ Request failed', false));
    }

    function loadOta() {
      fetch('/api/ota')
        .then(r=>r.json())
        .then(d=>{
          document.getElementById('ota-ver').textContent    = d.version;
          document.getElementById('ota-latest').textContent = d.latest_tag || '—';
          document.getElementById('ota-status').textContent = d.status;
          // Show "Apply Update" button only when a newer version is confirmed available.
          document.getElementById('btn-apply').style.display =
            d.status === 'update-available' ? 'block' : 'none';
        }).catch(()=>{});
    }

    function checkForUpdates() {
      document.getElementById('ota-status').textContent = 'checking…';
      document.getElementById('btn-apply').style.display = 'none';
      fetch('/api/ota/check', {method:'POST'})
        .then(r=>r.json())
        .then(d=>{
          if (!d.ok) document.getElementById('ota-status').textContent = '✗ '+d.error;
        })
        .catch(()=>{ document.getElementById('ota-status').textContent = '✗ Request failed'; });
    }

    function applyUpdate() {
      const tag = document.getElementById('ota-latest').textContent;
      if (!confirm(`Apply firmware update ${tag}?\nThe device will reboot after flashing.`)) return;
      document.getElementById('ota-status').textContent = 'starting update…';
      document.getElementById('btn-apply').style.display = 'none';
      fetch('/api/ota/update', {method:'POST'})
        .then(r=>r.json())
        .then(d=>{
          if (!d.ok) document.getElementById('ota-status').textContent = '✗ '+d.error;
          else document.getElementById('ota-status').textContent = 'downloading…';
        })
        .catch(()=>{ document.getElementById('ota-status').textContent = '✗ Request failed'; });
    }

    function showMsg(text, ok) {
      const m = document.getElementById('msg');
      m.textContent  = text;
      m.style.color  = ok ? '#0f0' : '#f44';
      setTimeout(()=>{ m.textContent=''; }, 4000);
    }

    loadConfig();
    loadOta();
    refresh();
    setInterval(refresh, 1000);
    setInterval(loadOta, 5000);
  </script>
</body>
</html>
)rawhtml";

// ─── REST Handlers ────────────────────────────────────────────────────────────
static void handle_root() {
    s_server.send_P(200, "text/html", INDEX_HTML);
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
    doc["display_enabled"]  = snap.display_enabled;

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

static void handle_ota_get() {
    JsonDocument doc;
    doc["version"]    = FIRMWARE_VERSION;
    doc["latest_tag"] = ota_get_latest_tag();
    doc["status"]     = ota_get_status();

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_ota_check() {
    ota_github_check_request();
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_ota_update() {
    ota_github_update_request();
    s_server.send(200, "application/json", "{\"ok\":true}");
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

    s_server.on("/",               HTTP_GET,  handle_root);
    s_server.on("/api/status",     HTTP_GET,  handle_status);
    s_server.on("/api/config",     HTTP_GET,  handle_get_config);
    s_server.on("/api/config",     HTTP_POST, handle_post_config);
    s_server.on("/api/reset",      HTTP_POST, handle_reset);
    s_server.on("/api/ota",        HTTP_GET,  handle_ota_get);
    s_server.on("/api/ota/check",  HTTP_POST, handle_ota_check);
    s_server.on("/api/ota/update", HTTP_POST, handle_ota_update);

    s_server.begin();
    Serial.println("[WEB] HTTP server started on port " + String(WEB_SERVER_PORT));
}

void web_handle_client() {
    s_server.handleClient();
}
