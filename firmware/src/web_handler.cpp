#include "web_handler.h"
#include "fault_detector.h"
#include "nvs_config.h"
#include "ota_handler.h"
#include "moonraker.h"
#include "wifi_handler.h"
#include "config.h"

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>

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
    .row{display:flex;gap:10px;flex-wrap:wrap}
    .row>*{flex:1 1 220px;min-width:0}
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
    .toggle-row{display:flex;align-items:flex-start;gap:10px;margin:8px 0 2px;flex-wrap:wrap}
    .toggle-row label{margin:0;flex:1 1 220px}
    input[type=checkbox]{width:18px;height:18px;accent-color:#06f;cursor:pointer}
    @media (max-width: 560px){
      body{padding:10px}
      .card{padding:12px}
      .row{gap:8px}
      .row>*{flex-basis:100%}
      td:first-child{width:58%}
    }
  </style>
</head>
<body>
  <h1>Filament Runout Sensor</h1>

  <!-- Status Card -->
  <div class="card">
    <h2>Live Status</h2>
    <div id="badge" class="badge badge-idle">…</div>
    <table>
      <tr><td>Encoder velocity</td><td id="enc-vel">-</td></tr>
      <tr><td>Extruder velocity</td><td id="ext-vel">-</td></tr>
      <tr><td>Moonraker link</td><td id="mr-link">-</td></tr>
      <tr><td>Klippy state</td><td id="klippy">-</td></tr>
      <tr><td>Tick count</td><td id="ticks">-</td></tr>
      <tr><td>Direction</td><td id="dir">-</td></tr>
      <tr><td>Last motion</td><td id="motion-ago">-</td></tr>
      <tr><td>Fault active</td><td id="fault">-</td></tr>
      <tr><td>IP address</td><td id="ip">-</td></tr>
    </table>
    <div class="vel-bar"><div class="vel-bar-inner" id="vel-bar" style="width:0%"></div></div>
    <button class="btn-reset" onclick="resetFault()">Reset Fault</button>
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
    </div>
    <p style="font-size:.75rem;color:#a60;margin:4px 0 6px">&#9432; OTA hostname and password changes require a device restart to take effect.</p>
    <label>Fault GCODE <span style="color:#666;font-size:.75rem">(sent to Moonraker via WebSocket on runout)</span></label>
    <input type="text" id="fault-gcode" maxlength="63" placeholder="PAUSE"/>
    <div class="toggle-row">
      <input type="checkbox" id="sensor-en"/>
      <label for="sensor-en">Enable fault trigger sensor (runout output)</label>
    </div>
    <div class="toggle-row">
      <input type="checkbox" id="disp-en"/>
      <label for="disp-en">Enable OLED display (SSD1306 128&#x00D7;64)</label>
    </div>
    <button class="btn-save" onclick="saveConfig()">Save &amp; Apply</button>
    <button class="btn-reset" onclick="rebootDevice()" style="margin-top:6px">Restart Device</button>
    <div id="msg"></div>
  </div>

  <!-- Calibration Card -->
  <div class="card">
    <h2>Auto-Calibrate</h2>
    <p style="font-size:.8rem;color:#aaa;margin-bottom:8px">
      Extrudes filament and measures encoder ticks to compute the calibration factor.<br>
      <strong style="color:#fa0">&#9888; Nozzle must be at print temperature. No active print.</strong>
    </p>
    <p id="cal-nozzle" style="font-size:.82rem;color:#9ad;margin-bottom:8px">Nozzle: --.- &#176;C / target --.- &#176;C</p>
    <div class="row">
      <div>
        <label>Extrude distance (mm)</label>
        <input type="number" id="cal-mm" step="1" min="10" max="200" value="50"/>
      </div>
      <div>
        <label>Speed (mm/min)</label>
        <input type="number" id="cal-speed" step="10" min="60" max="600" value="300"/>
      </div>
    </div>
    <button class="btn-save" id="btn-cal" onclick="startCal()">Start Calibration</button>
    <div id="cal-status" style="font-size:.85rem;margin-top:8px;min-height:1.4em;color:#aaa"></div>
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
      Check for Updates
    </button>
    <button class="btn-reset" id="btn-apply" onclick="applyUpdate()" style="margin-top:6px;display:none">
      Apply Update
    </button>
  </div>

  <!-- Diagnostics Card -->
  <div class="card">
    <h2>Diagnostics</h2>
    <table>
      <tr><td>Uptime</td><td id="diag-uptime">-</td></tr>
      <tr><td>Heap free / min</td><td id="diag-heap">-</td></tr>
      <tr><td>WiFi disconnect reason</td><td id="diag-disc">-</td></tr>
      <tr><td>MR ws init/conn/sub</td><td id="diag-mr-state">-</td></tr>
      <tr><td>MR stale / age</td><td id="diag-mr-age">-</td></tr>
      <tr><td>MR target</td><td id="diag-mr-target">-</td></tr>
      <tr><td>MR last connect</td><td id="diag-mr-last">-</td></tr>
      <tr><td>MR raw WS probe</td><td id="diag-mr-probe">-</td></tr>
      <tr><td>MR probe status line</td><td id="diag-mr-probe-line">-</td></tr>
      <tr><td>MR conn/disc/start</td><td id="diag-mr-evt">-</td></tr>
      <tr><td>MR info/sub/query</td><td id="diag-mr-req">-</td></tr>
      <tr><td>MR JSON errors</td><td id="diag-mr-json">-</td></tr>
    </table>
    <p style="font-size:.75rem;color:#666;margin:10px 0 3px">Event log</p>
    <pre id="diag-mr-log" style="font-size:.7rem;background:#0a0a0a;padding:8px;border-radius:4px;white-space:pre-wrap;word-break:break-all;max-height:150px;overflow-y:auto;color:#7c7;margin:0">(no events)</pre>
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
          let mrText = 'DISCONNECTED';
          if (d.mr_connected && d.mr_subscribed && !d.mr_stale) mrText = 'LIVE';
          else if (d.mr_connected && d.mr_subscribed && d.mr_stale) mrText = 'STALE';
          else if (d.mr_connected) mrText = 'CONNECTING';
          document.getElementById('mr-link').textContent    = mrText;
          document.getElementById('mr-link').style.color    = (mrText === 'LIVE') ? '#4f4' : ((mrText === 'STALE') ? '#fa0' : '#f66');
          document.getElementById('klippy').textContent     = d.klippy_state;
          document.getElementById('ticks').textContent     = d.ticks;
          const dirs = {'-1':'◀ Reverse','0':'● Stopped','1':'▶ Forward'};
          document.getElementById('dir').textContent       = dirs[String(d.direction)] ?? d.direction;
          document.getElementById('motion-ago').textContent= d.motion_ago_ms + ' ms ago';
          document.getElementById('fault').textContent     = d.fault ? '⚠ YES' : 'No';
          document.getElementById('fault').style.color     = d.fault ? '#f44' : '#4f4';
          document.getElementById('ip').textContent        = d.ip;
          const noz = d.nozzle_temp ?? 0;
          const nozTarget = d.nozzle_target ?? 0;
          const calNozzle = document.getElementById('cal-nozzle');
          calNozzle.textContent = 'Nozzle: ' + noz.toFixed(1) + ' °C / target ' + nozTarget.toFixed(1) + ' °C';
          calNozzle.style.color = (nozTarget > 0 && noz >= nozTarget - 5.0) ? '#4f4' : '#9ad';
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
          document.getElementById('sensor-en').checked = (d.sensor_enabled !== false);
          document.getElementById('disp-en').checked  = d.display_enabled;
          document.getElementById('fault-gcode').value = d.fault_gcode || 'PAUSE';
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
        sensor_enabled:   document.getElementById('sensor-en').checked,
        display_enabled:  document.getElementById('disp-en').checked,
        fault_gcode:      document.getElementById('fault-gcode').value,
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

    function rebootDevice() {
      if (!confirm('Restart the device now?\nThe page will reload automatically after ~5 seconds.')) return;
      fetch('/api/reboot', {method:'POST'})
        .then(r=>r.json())
        .then(()=>{ showMsg('⟳ Restarting…', true); setTimeout(()=>location.reload(), 5000); })
        .catch(()=>showMsg('✗ Request failed', false));
    }

    function loadOta() {
      fetch('/api/ota')
        .then(r=>r.json())
        .then(d=>{
          document.getElementById('ota-ver').textContent    = d.version;
          document.getElementById('ota-latest').textContent = d.latest_tag || '—';
          document.getElementById('ota-status').textContent = d.status;
          if (!d.github_ota) {
            document.getElementById('btn-check').style.display = 'none';
            document.getElementById('btn-apply').style.display = 'none';
            return;
          }
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

    // ─── Calibration ───────────────────────────────────────────────────────────────
    let calPollTimer = null;

    function startCal() {
      const mm    = parseFloat(document.getElementById('cal-mm').value);
      const speed = parseFloat(document.getElementById('cal-speed').value);
      const btn   = document.getElementById('btn-cal');
      const stat  = document.getElementById('cal-status');
      stat.textContent = 'Starting…';
      stat.style.color = '#aaa';
      btn.disabled = true;
      fetch('/api/calibrate', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({extrude_mm: mm, speed_mmpm: speed})
      })
        .then(r=>r.json())
        .then(d=>{
          if (!d.ok) {
            stat.textContent = '\u2717 ' + d.error;
            stat.style.color = '#f44';
            btn.disabled = false;
          } else {
            pollCal();
          }
        })
        .catch(()=>{ stat.textContent = '\u2717 Request failed'; stat.style.color='#f44'; btn.disabled=false; });
    }

    function pollCal() {
      fetch('/api/calibrate')
        .then(r=>r.json())
        .then(d=>{
          const st    = d.state;
          const stat  = document.getElementById('cal-status');
          const btn   = document.getElementById('btn-cal');
          if (st === 'done') {
            btn.disabled = false;
            stat.style.color = '#4f4';
            stat.innerHTML = '\u2713 Done: <strong>' + d.cal_factor.toFixed(4) + ' mm/tick</strong>'
              + ' (' + d.ticks + ' ticks for ' + d.mm.toFixed(1) + ' mm)'
              + ' &mdash; <a href="#" style="color:#06f" onclick="applyCal(' + d.cal_factor + ');return false">Apply &amp; Save</a>';
          } else if (st === 'failed') {
            btn.disabled = false;
            stat.style.color = '#f44';
            stat.textContent = '\u2717 Failed: ' + (d.error || 'unknown error');
          } else if (st === 'idle') {
            btn.disabled = false;
            stat.textContent = '';
          } else {
            const labels = {sent:'Waiting for motion\u2026', moving:'Measuring ticks\u2026', settling:'Settling\u2026'};
            stat.style.color = '#aaa';
            stat.textContent = labels[st] || st;
            calPollTimer = setTimeout(pollCal, 400);
          }
        })
        .catch(()=>{ calPollTimer = setTimeout(pollCal, 1000); });
    }

    function applyCal(factor) {
      document.getElementById('cal').value = factor.toFixed(4);
      saveConfig();
      document.getElementById('cal-status').textContent = '\u2713 Applied and saved';
      document.getElementById('cal-status').style.color = '#4f4';
    }

    function refreshDiag() {
      fetch('/api/diag')
        .then(r=>r.json())
        .then(d=>{
          document.getElementById('diag-uptime').textContent   = d.uptime_s + ' s';
          document.getElementById('diag-heap').textContent     = d.heap_free + ' / ' + d.heap_min + ' B';
          document.getElementById('diag-disc').textContent     = d.wifi_disc_reason;
          document.getElementById('diag-mr-state').textContent = (d.mr_ws_init?1:0) + '/' + (d.mr_ws_conn?1:0) + '/' + (d.mr_sub?1:0) + ' (' + d.klippy_state + ')';
          document.getElementById('diag-mr-age').textContent   = (d.mr_stale ? 'stale' : 'live') + ' / ' + d.mr_age_ms + ' ms';
          document.getElementById('diag-mr-target').textContent = (d.mr_last_host || '-') + ':' + d.mr_last_port;
          document.getElementById('diag-mr-last').textContent   = (d.mr_last_ok ? 'ok' : 'failed') + ' / fails=' + d.mr_fail_streak + ' / backoff=' + d.mr_backoff_ms + ' ms';
          document.getElementById('diag-mr-probe').textContent  = (d.mr_probe_last_ok ? '101-ok' : 'not-101') + ' / ' + d.mr_probe_101 + '/' + d.mr_probe_attempts;
          document.getElementById('diag-mr-probe-line').textContent = d.mr_probe_status_line || '-';
          document.getElementById('diag-mr-evt').textContent   = d.mr_conn_evt + '/' + d.mr_disc_evt + '/' + d.mr_start_evt;
          document.getElementById('diag-mr-req').textContent   = d.mr_info_req + '/' + d.mr_sub_req + '/' + d.mr_query_req;
          document.getElementById('diag-mr-json').textContent  = d.mr_json_err;
          if (d.mr_log !== undefined) document.getElementById('diag-mr-log').textContent = d.mr_log;
        }).catch(()=>{});
    }

    loadConfig();
    loadOta();
    refresh();
    refreshDiag();
    setInterval(refresh, 1000);
    setInterval(refreshDiag, 2000);
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
    Serial.println("[WEB] HTTP server started on port " + String(WEB_SERVER_PORT));
}

void web_handle_client() {
    s_server.handleClient();
}
