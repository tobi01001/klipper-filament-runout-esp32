#include "moonraker.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <string.h>
#include <WiFi.h>

#include "config.h"
#include "fault_detector.h"
#include "debug_log.h"

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorStatus     *s_status = nullptr;
static SensorConfig     *s_config = nullptr;

static TickType_t s_last_poll = 0;
static WebSocketsClient s_ws;

static bool     s_ws_initialized = false;
static bool     s_ws_connected   = false;
static bool     s_subscribed     = false;
static bool     s_klippy_ready   = false;
static bool     s_have_extruder  = false;
static bool     s_stale          = true;

static float    s_last_extruder_vel = 0.0f;
static float    s_last_nozzle_temp  = 0.0f;
static float    s_last_nozzle_target = 0.0f;
static uint32_t s_last_ext_rx_ms    = 0;
static uint32_t s_last_info_req_ms  = 0;
static uint32_t s_last_sub_req_ms   = 0;
static uint32_t s_last_query_req_ms = 0;
static uint32_t s_reconnect_backoff_ms = MOONRAKER_WS_RECONNECT_MIN_MS;

static char     s_cfg_host[40] = {0};
static uint16_t s_cfg_port = 0;
static char     s_klippy_state[16] = "disconnected";
static uint32_t s_jsonrpc_id = 1;
static char     s_last_connect_host[40] = {0};
static uint16_t s_last_connect_port = 0;
static bool     s_last_connect_ok = false;
static uint32_t s_last_connect_attempt_ms = 0;
static uint32_t s_connect_fail_streak = 0;
static uint32_t s_ws_probe_attempts = 0;
static uint32_t s_ws_probe_101 = 0;
static uint32_t s_ws_probe_last_ms = 0;
static bool     s_ws_probe_last_ok = false;
static char     s_ws_probe_status_line[64] = "n/a";

// ─── Diagnostic event log ────────────────────────────────────────────────────
#define MR_LOG_N   8
#define MR_LOG_LEN 72
struct MrLogEntry { uint32_t ms; char msg[MR_LOG_LEN]; };
static MrLogEntry s_log_buf[MR_LOG_N];
static uint8_t    s_log_head  = 0;
static uint8_t    s_log_count = 0;

struct MoonrakerTelemetry {
    uint32_t ws_connect_events;
    uint32_t ws_disconnect_events;
    uint32_t ws_start_attempts;
    uint32_t info_requests;
    uint32_t subscribe_requests;
    uint32_t query_requests;
    uint32_t json_errors;
};

static MoonrakerTelemetry s_tm{};

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    const char *beg = src;
    while (*beg && is_ws(*beg)) {
        ++beg;
    }

    const char *end = beg + strlen(beg);
    while (end > beg && is_ws(*(end - 1))) {
        --end;
    }

    const size_t n = (size_t)(end - beg);
    const size_t copy_n = (n < (dst_len - 1)) ? n : (dst_len - 1);
    if (copy_n > 0) {
        memcpy(dst, beg, copy_n);
    }
    dst[copy_n] = '\0';
}

static void mr_log(const char *fmt, ...) {
    MrLogEntry &e = s_log_buf[s_log_head];
    e.ms = millis();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, MR_LOG_LEN, fmt, ap);
    va_end(ap);
    s_log_head = (s_log_head + 1) % MR_LOG_N;
    if (s_log_count < MR_LOG_N) { s_log_count++; }
}

static void set_klippy_state(const char *state) {
    if (!state || state[0] == '\0') {
        return;
    }
    strncpy(s_klippy_state, state, sizeof(s_klippy_state) - 1);
    s_klippy_state[sizeof(s_klippy_state) - 1] = '\0';
    s_klippy_ready = (strcmp(s_klippy_state, "ready") == 0);
}

static void ws_probe_upgrade(const char *host, uint16_t port) {
    if (!host || host[0] == '\0' || port == 0) {
        return;
    }

    s_ws_probe_attempts++;
    s_ws_probe_last_ms = millis();
    s_ws_probe_last_ok = false;
    strncpy(s_ws_probe_status_line, "tcp-fail", sizeof(s_ws_probe_status_line) - 1);
    s_ws_probe_status_line[sizeof(s_ws_probe_status_line) - 1] = '\0';

    WiFiClient client;
    client.setTimeout(1200);
    if (!client.connect(host, port)) {
        return;
    }

    client.print("GET /websocket HTTP/1.1\r\n");
    client.print("Host: ");
    client.print(host);
    client.print(":");
    client.print(port);
    client.print("\r\n");
    client.print("Upgrade: websocket\r\n");
    client.print("Connection: Upgrade\r\n");
    client.print("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    client.print("Sec-WebSocket-Version: 13\r\n\r\n");

    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
        strncpy(s_ws_probe_status_line, "no-response", sizeof(s_ws_probe_status_line) - 1);
        s_ws_probe_status_line[sizeof(s_ws_probe_status_line) - 1] = '\0';
        client.stop();
        return;
    }

    const size_t max_copy = sizeof(s_ws_probe_status_line) - 1;
    strncpy(s_ws_probe_status_line, line.c_str(), max_copy);
    s_ws_probe_status_line[max_copy] = '\0';

    if (line.startsWith("HTTP/1.1 101")) {
        s_ws_probe_last_ok = true;
        s_ws_probe_101++;
    }

    client.stop();
}

static bool snapshot_config(SensorConfig *cfg_out) {
    if (!cfg_out || !s_config || !s_config_mutex) {
        mr_log("cfg-snap: null ptr");
        return false;
    }
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        mr_log("cfg-snap: mutex tmo");
        return false;
    }
    *cfg_out = *s_config;
    xSemaphoreGive(s_config_mutex);
    return true;
}

static void set_extruder_velocity(float vel) {
    if (!s_status || !s_status_mutex) {
        return;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_status->extruder_vel = vel;
        xSemaphoreGive(s_status_mutex);
    }
}

static void update_status_flags() {
    if (!s_status || !s_status_mutex) {
        return;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_status->moonraker_connected  = s_ws_connected;
        s_status->moonraker_subscribed = s_subscribed;
        s_status->moonraker_stale      = s_stale;
        s_status->nozzle_temp          = s_last_nozzle_temp;
        s_status->nozzle_target        = s_last_nozzle_target;
        strncpy(s_status->klippy_state, s_klippy_state, sizeof(s_status->klippy_state) - 1);
        s_status->klippy_state[sizeof(s_status->klippy_state) - 1] = '\0';
        xSemaphoreGive(s_status_mutex);
    }
}

static void reset_transport_state() {
    s_ws_connected = false;
    s_subscribed = false;
    s_klippy_ready = false;
    s_have_extruder = false;
    s_stale = true;
    s_last_extruder_vel = 0.0f;
    s_last_nozzle_temp = 0.0f;
    s_last_nozzle_target = 0.0f;
    s_last_ext_rx_ms = 0;
    s_last_connect_ok = false;
    set_klippy_state("disconnected");
    update_status_flags();
}

static void request_server_info() {
    JsonDocument req;
    req["jsonrpc"] = "2.0";
    req["method"] = "server.info";
    req["id"] = s_jsonrpc_id++;

    String out;
    serializeJson(req, out);
    s_ws.sendTXT(out);
    s_tm.info_requests++;
}

static void request_subscribe() {
    JsonDocument req;
    req["jsonrpc"] = "2.0";
    req["method"] = "printer.objects.subscribe";
    req["id"] = s_jsonrpc_id++;
    JsonObject objects = req["params"]["objects"].to<JsonObject>();
    JsonArray motion_report = objects["motion_report"].to<JsonArray>();
    JsonArray print_stats = objects["print_stats"].to<JsonArray>();
    JsonArray webhooks = objects["webhooks"].to<JsonArray>();
    JsonArray extruder = objects["extruder"].to<JsonArray>();
    motion_report.add("live_extruder_velocity");
    print_stats.add("state");
    webhooks.add("state");
    extruder.add("temperature");
    extruder.add("target");

    String out;
    serializeJson(req, out);
    s_ws.sendTXT(out);
    s_tm.subscribe_requests++;
}

static void request_status_query() {
    JsonDocument req;
    req["jsonrpc"] = "2.0";
    req["method"] = "printer.objects.query";
    req["id"] = s_jsonrpc_id++;
    JsonObject objects = req["params"]["objects"].to<JsonObject>();
    objects["motion_report"] = nullptr;
    objects["print_stats"] = nullptr;
    objects["webhooks"] = nullptr;
    objects["extruder"] = nullptr;

    String out;
    serializeJson(req, out);
    s_ws.sendTXT(out);
    s_tm.query_requests++;
}

static void consume_status_obj(JsonVariantConst status) {
    if (status.isNull()) {
        return;
    }

    if (status["webhooks"]["state"].is<const char *>()) {
        set_klippy_state(status["webhooks"]["state"].as<const char *>());
    }

    if (status["motion_report"]["live_extruder_velocity"].is<float>() ||
        status["motion_report"]["live_extruder_velocity"].is<double>() ||
        status["motion_report"]["live_extruder_velocity"].is<int>()) {
        s_last_extruder_vel = status["motion_report"]["live_extruder_velocity"].as<float>();
        s_last_ext_rx_ms = millis();
        s_have_extruder = true;
        s_stale = false;
    }

    if (status["extruder"]["temperature"].is<float>() ||
        status["extruder"]["temperature"].is<double>() ||
        status["extruder"]["temperature"].is<int>()) {
        s_last_nozzle_temp = status["extruder"]["temperature"].as<float>();
    }

    if (status["extruder"]["target"].is<float>() ||
        status["extruder"]["target"].is<double>() ||
        status["extruder"]["target"].is<int>()) {
        s_last_nozzle_target = status["extruder"]["target"].as<float>();
    }
}

static void handle_ws_json(const char *payload, size_t len) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        DBG_PRINTF("[MR] WS JSON parse error: %s\n", err.c_str());
        s_tm.json_errors++;
        return;
    }

    if (doc["result"]["klippy_state"].is<const char *>()) {
        set_klippy_state(doc["result"]["klippy_state"].as<const char *>());
    }

    if (!doc["result"]["status"].isNull()) {
        consume_status_obj(doc["result"]["status"].as<JsonVariantConst>());
        s_subscribed = true;
    }

    if (doc["method"].is<const char *>()) {
        const char *method = doc["method"].as<const char *>();

        if (strcmp(method, "notify_status_update") == 0) {
            JsonVariantConst params0 = doc["params"][0];
            if (!params0.isNull()) {
                consume_status_obj(params0);
            }
        } else if (strcmp(method, "notify_klippy_disconnected") == 0) {
            set_klippy_state("disconnected");
            s_subscribed = false;
        } else if (strcmp(method, "notify_klippy_ready") == 0) {
            set_klippy_state("ready");
            s_subscribed = false;
        } else if (strcmp(method, "notify_klippy_shutdown") == 0) {
            set_klippy_state("shutdown");
        }
    }

    update_status_flags();
}

static void ws_opened() {
    mr_log("CONNECTED");
    s_ws_connected = true;
    s_subscribed = false;
    s_last_connect_ok = true;
    s_connect_fail_streak = 0;
    s_reconnect_backoff_ms = MOONRAKER_WS_RECONNECT_MIN_MS;
    s_tm.ws_connect_events++;
    DBG_PRINTLN("[MR] WebSocket connected");
    request_server_info();
    s_last_info_req_ms = millis();
    update_status_flags();
}

static void ws_closed() {
    if (!s_ws_connected) {
        s_connect_fail_streak++;
    }
    //mr_log("DISCONN/FAIL streak=%u", s_connect_fail_streak);
    DBG_PRINTLN("[MR] WebSocket disconnected/failed");
    s_tm.ws_disconnect_events++;
    reset_transport_state();
    // links2004 auto-reconnects via setReconnectInterval
}

static void ws_event(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            ws_opened();
            break;
        case WStype_DISCONNECTED:
            if (payload && length > 0) {
                mr_log("DISCONNECTED: %.*s", (int)length, (const char *)payload);
            } else {
                mr_log("DISCONNECTED");
            }
            ws_closed();
            break;
        case WStype_TEXT:
            handle_ws_json((const char *)payload, length);
            break;
        case WStype_ERROR:
            if (payload && length > 0) {
                mr_log("WS ERROR: %.*s", (int)length, (const char *)payload);
            } else {
                mr_log("WS ERROR");
            }
            break;
        default:
            break;
    }
}

static void start_ws(const SensorConfig &cfg) {
    char host[40];
    trim_copy(host, sizeof(host), cfg.moonraker_ip);

    mr_log("start_ws %s:%u", host[0] ? host : "?", cfg.moonraker_port);
    if (host[0] == '\0' || cfg.moonraker_port == 0) {
        mr_log("start_ws: skip no cfg");
        return;
    }

    s_ws.onEvent(ws_event);
    s_ws.setExtraHeaders("");  // suppress default "Origin: file://" — Moonraker rejects it with 403
    s_ws.begin(host, cfg.moonraker_port, "/websocket");
    mr_log("ws.begin called %s:%u", host, cfg.moonraker_port);
    s_ws.setReconnectInterval(MOONRAKER_WS_RECONNECT_MIN_MS);

    s_ws_initialized = true;
    s_tm.ws_start_attempts++;
    s_last_connect_attempt_ms = millis();
    strncpy(s_last_connect_host, host, sizeof(s_last_connect_host) - 1);
    s_last_connect_host[sizeof(s_last_connect_host) - 1] = '\0';
    s_last_connect_port = cfg.moonraker_port;
    strncpy(s_cfg_host, host, sizeof(s_cfg_host) - 1);
    s_cfg_host[sizeof(s_cfg_host) - 1] = '\0';
    s_cfg_port = cfg.moonraker_port;

    DBG_PRINTF("[MR] WS begin -> %s:%u/websocket\n", host, cfg.moonraker_port);
    mr_log("WS STARTED");
}

static void restart_ws(const SensorConfig &cfg, const char *reason) {
    if (reason) {
        DBG_PRINTF("[MR] WS restart: %s\n", reason);
    }
    s_ws.disconnect();
    s_ws_initialized = false;
    reset_transport_state();
    start_ws(cfg);
}

static void service_transport(const SensorConfig &cfg) {
    char host[40];
    trim_copy(host, sizeof(host), cfg.moonraker_ip);

    if (!s_ws_initialized) {
        start_ws(cfg);
        return;
    }

    if (strncmp(host, s_cfg_host, sizeof(s_cfg_host)) != 0 ||
        cfg.moonraker_port != s_cfg_port) {
        restart_ws(cfg, "config changed");
        return;
    }

    s_ws.loop();

    if (!s_ws_connected) {
        return;
    }

    const uint32_t now = millis();

    if (!s_klippy_ready && (uint32_t)(now - s_last_info_req_ms) >= MOONRAKER_INFO_MS) {
        request_server_info();
        s_last_info_req_ms = now;
    }

    if (s_klippy_ready && !s_subscribed && (uint32_t)(now - s_last_sub_req_ms) >= MOONRAKER_SUB_RETRY_MS) {
        request_subscribe();
        s_last_sub_req_ms = now;
    }

    if (s_klippy_ready && (uint32_t)(now - s_last_query_req_ms) >= MOONRAKER_POLL_MS) {
        request_status_query();
        s_last_query_req_ms = now;
    }

    if (s_klippy_ready && s_have_extruder && (uint32_t)(now - s_last_ext_rx_ms) <= MOONRAKER_STALE_MS) {
        s_subscribed = true;
        s_stale = false;
    } else {
        s_stale = true;
    }

    update_status_flags();
}

void moonraker_send_gcode(const char *script) {
    if (!script || script[0] == '\0') {
        return;
    }
    if (!s_ws_connected || !s_klippy_ready) {
        mr_log("send_gcode: not ready");
        return;
    }
    JsonDocument req;
    req["jsonrpc"] = "2.0";
    req["method"]  = "printer.gcode.script";
    req["id"]      = s_jsonrpc_id++;
    req["params"]["script"] = script;
    String out;
    serializeJson(req, out);
    s_ws.sendTXT(out);
    mr_log("gcode: %.40s", script);
}

void moonraker_init(SemaphoreHandle_t status_mutex,
                    SemaphoreHandle_t config_mutex,
                    SensorStatus     *status,
                    SensorConfig     *config) {
    s_status_mutex = status_mutex;
    s_config_mutex = config_mutex;
    s_status       = status;
    s_config       = config;
    s_last_poll    = xTaskGetTickCount();

    reset_transport_state();
}

void moonraker_tick() {
    SensorConfig cfg{};
    if (!snapshot_config(&cfg)) {
        return;
    }

    service_transport(cfg);

    if ((xTaskGetTickCount() - s_last_poll) < pdMS_TO_TICKS(MOONRAKER_POLL_MS)) {
        return;
    }
    s_last_poll = xTaskGetTickCount();

    const float ext_vel = (s_ws_connected && s_klippy_ready && !s_stale)
                        ? s_last_extruder_vel
                        : 0.0f;

    set_extruder_velocity(ext_vel);
    fault_detector_update(ext_vel);
}

void moonraker_set_disconnected() {
    s_ws.disconnect();
    s_ws_initialized = false;
    reset_transport_state();
    set_extruder_velocity(0.0f);
    fault_detector_update(0.0f);
}

void moonraker_get_diag(MoonrakerDiag *out_diag) {
    if (!out_diag) {
        return;
    }

    memset(out_diag, 0, sizeof(*out_diag));
    out_diag->ws_initialized = s_ws_initialized;
    out_diag->ws_connected = s_ws_connected;
    out_diag->subscribed = s_subscribed;
    out_diag->klippy_ready = s_klippy_ready;
    out_diag->stale = s_stale;
    out_diag->last_connect_ok = s_last_connect_ok;
    out_diag->last_ext_rx_ms = s_last_ext_rx_ms;
    out_diag->last_connect_attempt_ms = s_last_connect_attempt_ms;
    out_diag->consecutive_connect_failures = s_connect_fail_streak;
    out_diag->reconnect_backoff_ms = s_reconnect_backoff_ms;
    out_diag->ws_connect_events = s_tm.ws_connect_events;
    out_diag->ws_disconnect_events = s_tm.ws_disconnect_events;
    out_diag->ws_start_attempts = s_tm.ws_start_attempts;
    out_diag->info_requests = s_tm.info_requests;
    out_diag->subscribe_requests = s_tm.subscribe_requests;
    out_diag->query_requests = s_tm.query_requests;
    out_diag->json_errors = s_tm.json_errors;
    out_diag->ws_probe_attempts = s_ws_probe_attempts;
    out_diag->ws_probe_101 = s_ws_probe_101;
    out_diag->ws_probe_last_ms = s_ws_probe_last_ms;
    out_diag->ws_probe_last_ok = s_ws_probe_last_ok;
    out_diag->last_connect_port = s_last_connect_port;
    strncpy(out_diag->last_connect_host, s_last_connect_host, sizeof(out_diag->last_connect_host) - 1);
    out_diag->last_connect_host[sizeof(out_diag->last_connect_host) - 1] = '\0';
    strncpy(out_diag->ws_probe_status_line, s_ws_probe_status_line, sizeof(out_diag->ws_probe_status_line) - 1);
    out_diag->ws_probe_status_line[sizeof(out_diag->ws_probe_status_line) - 1] = '\0';
    strncpy(out_diag->klippy_state, s_klippy_state, sizeof(out_diag->klippy_state) - 1);
    out_diag->klippy_state[sizeof(out_diag->klippy_state) - 1] = '\0';
}

void moonraker_copy_log(char *out, size_t out_len) {
    if (!out || out_len == 0) { return; }
    if (s_log_count == 0) {
        strlcpy(out, "(no events)", out_len);
        return;
    }
    const uint8_t total     = s_log_count;
    const uint8_t start_idx = (s_log_count == MR_LOG_N) ? s_log_head : 0;
    size_t pos = 0;
    for (uint8_t i = 0; i < total && pos < out_len - 1; i++) {
        const uint8_t idx = (start_idx + i) % MR_LOG_N;
        const MrLogEntry &e = s_log_buf[idx];
        int n = snprintf(out + pos, out_len - pos,
                         "T+%lums %s\n", (unsigned long)e.ms, e.msg);
        if (n <= 0 || (size_t)n >= out_len - pos) { break; }
        pos += (size_t)n;
    }
    out[pos] = '\0';
}
