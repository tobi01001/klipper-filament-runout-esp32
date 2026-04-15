#include "mqtt_handler.h"
#include "fault_detector.h"
#include "nvs_config.h"
#include "config.h"
#include "debug_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <string.h>

// ─── Module state ─────────────────────────────────────────────────────────────
static WiFiClient        s_wifi_client;
static PubSubClient      s_mqtt_client(s_wifi_client);

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorStatus     *s_status       = nullptr;
static SensorConfig     *s_config       = nullptr;

static MqttConfig        s_cfg{};

// Reconnect backoff state
static uint32_t          s_last_reconnect_ms  = 0;
static uint32_t          s_reconnect_backoff  = MQTT_RECONNECT_MIN_MS;

// Periodic publish state
static uint32_t          s_last_publish_ms    = 0;
static SystemState       s_last_pub_state     = SystemState::INIT;
static bool              s_last_pub_fault     = false;
static bool              s_last_pub_enabled   = true;

// Protect s_cfg from concurrent web-handler and Core0 accesses
static SemaphoreHandle_t s_cfg_mutex = nullptr;

// ─── NVS helpers ─────────────────────────────────────────────────────────────
void mqtt_config_load(MqttConfig &cfg) {
    Preferences prefs;
    if (!prefs.begin(MQTT_NVS_NS, /*readOnly=*/true)) {
        // No namespace yet – first boot, use defaults
        cfg.broker[0]       = '\0';
        cfg.port            = MQTT_DEFAULT_PORT;
        cfg.username[0]     = '\0';
        cfg.password[0]     = '\0';
        strncpy(cfg.topic_prefix, MQTT_DEFAULT_PREFIX, sizeof(cfg.topic_prefix) - 1);
        cfg.topic_prefix[sizeof(cfg.topic_prefix) - 1] = '\0';
        cfg.enabled         = MQTT_DEFAULT_ENABLED;
        return;
    }

    String broker = prefs.getString(MQTT_NVS_BROKER, MQTT_DEFAULT_BROKER);
    strncpy(cfg.broker, broker.c_str(), sizeof(cfg.broker) - 1);
    cfg.broker[sizeof(cfg.broker) - 1] = '\0';

    cfg.port = static_cast<uint16_t>(
        prefs.getUInt(MQTT_NVS_PORT, MQTT_DEFAULT_PORT));

    String user = prefs.getString(MQTT_NVS_USER, MQTT_DEFAULT_USER);
    strncpy(cfg.username, user.c_str(), sizeof(cfg.username) - 1);
    cfg.username[sizeof(cfg.username) - 1] = '\0';

    String pass = prefs.getString(MQTT_NVS_PASS, MQTT_DEFAULT_PASS);
    strncpy(cfg.password, pass.c_str(), sizeof(cfg.password) - 1);
    cfg.password[sizeof(cfg.password) - 1] = '\0';

    String prefix = prefs.getString(MQTT_NVS_PREFIX, MQTT_DEFAULT_PREFIX);
    strncpy(cfg.topic_prefix, prefix.c_str(), sizeof(cfg.topic_prefix) - 1);
    cfg.topic_prefix[sizeof(cfg.topic_prefix) - 1] = '\0';

    cfg.enabled = prefs.getBool(MQTT_NVS_EN, MQTT_DEFAULT_ENABLED);

    prefs.end();
    DBG_PRINTLN("[MQTT] Configuration loaded from NVS");
}

void mqtt_config_save(const MqttConfig &cfg) {
    Preferences prefs;
    if (!prefs.begin(MQTT_NVS_NS, /*readOnly=*/false)) {
        DBG_PRINTLN("[MQTT] ERROR: failed to open NVS for writing");
        return;
    }
    prefs.putString(MQTT_NVS_BROKER, cfg.broker);
    prefs.putUInt  (MQTT_NVS_PORT,   cfg.port);
    prefs.putString(MQTT_NVS_USER,   cfg.username);
    prefs.putString(MQTT_NVS_PASS,   cfg.password);
    prefs.putString(MQTT_NVS_PREFIX, cfg.topic_prefix);
    prefs.putBool  (MQTT_NVS_EN,     cfg.enabled);
    prefs.end();
    DBG_PRINTLN("[MQTT] Configuration saved to NVS");
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

// Build a full topic string: prefix + suffix (caller owns the buffer)
static void build_topic(char *buf, size_t buf_len,
                        const char *prefix, const char *suffix) {
    snprintf(buf, buf_len, "%s%s", prefix, suffix);
}

// Build a unique client ID from the MAC address
static void build_client_id(char *buf, size_t buf_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, buf_len, "filsns_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

// Snapshot current sensor state (takes both mutexes briefly)
static void snapshot_state(SystemState &out_state, bool &out_fault,
                             bool &out_enabled, float &out_enc_vel,
                             float &out_ext_vel, uint32_t &out_motion_ago,
                             float &out_nozzle_temp,
                             float &out_dht_temp, float &out_dht_hum,
                             bool &out_dht_valid) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        out_state      = s_status->state;
        out_fault      = s_status->fault_active;
        out_enc_vel    = s_status->encoder.velocity_mm_s;
        out_ext_vel    = s_status->extruder_vel;
        out_motion_ago = millis() - s_status->encoder.timestamp_ms;
        out_nozzle_temp = s_status->nozzle_temp;
#ifdef ENABLE_DHT
        out_dht_temp   = s_status->dht_temperature;
        out_dht_hum    = s_status->dht_humidity;
        out_dht_valid  = s_status->dht_valid;
#else
        out_dht_temp   = 0.0f;
        out_dht_hum    = 0.0f;
        out_dht_valid  = false;
#endif
        xSemaphoreGive(s_status_mutex);
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        out_enabled = s_config->sensor_enabled;
        xSemaphoreGive(s_config_mutex);
    }
}

// Publish the full sensor state JSON to <prefix>/state (retained)
static void publish_state() {
    if (!s_mqtt_client.connected()) {
        return;
    }

    SystemState cur_state = SystemState::INIT;
    bool        fault     = false;
    bool        enabled   = true;
    float       enc_vel   = 0.0f;
    float       ext_vel   = 0.0f;
    uint32_t    motion_ago = 0;
    float       nozzle_temp = 0.0f;
    float       dht_temp  = 0.0f;
    float       dht_hum   = 0.0f;
    bool        dht_valid = false;

    snapshot_state(cur_state, fault, enabled, enc_vel, ext_vel, motion_ago,
                   nozzle_temp, dht_temp, dht_hum, dht_valid);

    // Build JSON payload using a fixed-size buffer (no dynamic heap alloc)
    char payload[384];
    const char *state_str = "UNKNOWN";
    switch (cur_state) {
        case SystemState::INIT:      state_str = "INIT";      break;
        case SystemState::WIFI_CONN: state_str = "WIFI_CONN"; break;
        case SystemState::WIFI_FAIL: state_str = "WIFI_FAIL"; break;
        case SystemState::READY:     state_str = "READY";     break;
        case SystemState::IDLE:      state_str = "IDLE";      break;
        case SystemState::PRINTING:  state_str = "PRINTING";  break;
        case SystemState::FAULT:     state_str = "FAULT";     break;
    }

    int written = snprintf(payload, sizeof(payload),
        "{\"state\":\"%s\","
        "\"fault\":%s,"
        "\"sensor_enabled\":%s,"
        "\"enc_vel\":%.2f,"
        "\"ext_vel\":%.2f,"
        "\"motion_ago_ms\":%lu,"
        "\"nozzle_temp\":%.1f,"
        "\"dht_temp\":%.1f,"
        "\"dht_humidity\":%.1f,"
        "\"dht_valid\":%s}",
        state_str,
        fault   ? "true" : "false",
        enabled ? "true" : "false",
        enc_vel,
        ext_vel,
        (unsigned long)motion_ago,
        nozzle_temp,
        dht_temp,
        dht_hum,
        dht_valid ? "true" : "false");

    if (written <= 0 || (size_t)written >= sizeof(payload)) {
        DBG_PRINTLN("[MQTT] publish_state: payload buffer overflow");
        return;
    }

    char topic[96];
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        build_topic(topic, sizeof(topic), s_cfg.topic_prefix, MQTT_SUFFIX_STATE);
        xSemaphoreGive(s_cfg_mutex);
    } else {
        return;
    }

    const bool ok = s_mqtt_client.publish(topic, payload, /*retained=*/true);
    DBG_PRINTF("[MQTT] publish state %s\n", ok ? "ok" : "FAIL");

    s_last_pub_state   = cur_state;
    s_last_pub_fault   = fault;
    s_last_pub_enabled = enabled;
    s_last_publish_ms  = millis();
}

// MQTT message callback – handles inbound cmd topics
static void on_message(char *topic, uint8_t *payload_bytes, unsigned int length) {
    if (length == 0 || length > 32) {
        return;
    }
    // Safe copy of payload into a null-terminated buffer
    char val[33];
    memcpy(val, payload_bytes, length);
    val[length] = '\0';

    // Determine which suffix matches
    char topic_prefix[48] = {};
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        strncpy(topic_prefix, s_cfg.topic_prefix, sizeof(topic_prefix) - 1);
        xSemaphoreGive(s_cfg_mutex);
    } else {
        return;
    }

    // Build expected topic strings for comparison
    char t_enable[96], t_reset[96];
    build_topic(t_enable, sizeof(t_enable), topic_prefix, MQTT_SUFFIX_CMD_ENABLE);
    build_topic(t_reset,  sizeof(t_reset),  topic_prefix, MQTT_SUFFIX_CMD_RESET);

    if (strcmp(topic, t_enable) == 0) {
        // Determine desired enable state: "1", "true", "on" → enable
        const bool enable = (val[0] == '1' ||
                             strncasecmp(val, "true", 4) == 0 ||
                             strncasecmp(val, "on",   2) == 0);
        DBG_PRINTF("[MQTT] cmd/enable → %d\n", (int)enable);

        if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_config->sensor_enabled = enable;
            // Persist change to NVS
            SensorConfig snap = *s_config;
            xSemaphoreGive(s_config_mutex);
            nvs_save(snap);
        }
        if (!enable) {
            fault_detector_reset();
        }
        // Publish updated state immediately
        publish_state();

    } else if (strcmp(topic, t_reset) == 0) {
        DBG_PRINTLN("[MQTT] cmd/reset received");
        fault_detector_reset();
        publish_state();
    }
}

// Attempt to connect/reconnect to the broker
static void do_connect() {
    MqttConfig cfg_snap{};
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        cfg_snap = s_cfg;
        xSemaphoreGive(s_cfg_mutex);
    } else {
        return;
    }

    if (!cfg_snap.enabled || cfg_snap.broker[0] == '\0') {
        return;
    }

    char client_id[24];
    build_client_id(client_id, sizeof(client_id));

    // Build LWT topic
    char lwt_topic[96];
    build_topic(lwt_topic, sizeof(lwt_topic), cfg_snap.topic_prefix, MQTT_SUFFIX_AVAIL);

    s_mqtt_client.setServer(cfg_snap.broker, cfg_snap.port);
    s_mqtt_client.setBufferSize(384);

    DBG_PRINTF("[MQTT] Connecting to %s:%u as %s\n",
               cfg_snap.broker, cfg_snap.port, client_id);

    const bool connected =
        (cfg_snap.username[0] != '\0')
            ? s_mqtt_client.connect(client_id,
                                    cfg_snap.username,
                                    cfg_snap.password,
                                    lwt_topic, 0, true, "offline")
            : s_mqtt_client.connect(client_id,
                                    nullptr, nullptr,
                                    lwt_topic, 0, true, "offline");

    if (connected) {
        DBG_PRINTLN("[MQTT] Connected");
        s_reconnect_backoff = MQTT_RECONNECT_MIN_MS;

        // Publish availability
        s_mqtt_client.publish(lwt_topic, "online", /*retained=*/true);

        // Subscribe to command topics
        char t_enable[96], t_reset[96];
        build_topic(t_enable, sizeof(t_enable), cfg_snap.topic_prefix, MQTT_SUFFIX_CMD_ENABLE);
        build_topic(t_reset,  sizeof(t_reset),  cfg_snap.topic_prefix, MQTT_SUFFIX_CMD_RESET);
        s_mqtt_client.subscribe(t_enable);
        s_mqtt_client.subscribe(t_reset);

        // Publish current state immediately on connect
        publish_state();
    } else {
        DBG_PRINTF("[MQTT] Connect failed (rc=%d), backoff %lu ms\n",
                   s_mqtt_client.state(), (unsigned long)s_reconnect_backoff);
        // Exponential backoff capped at max
        if (s_reconnect_backoff < MQTT_RECONNECT_MAX_MS / 2) {
            s_reconnect_backoff *= 2;
        } else {
            s_reconnect_backoff = MQTT_RECONNECT_MAX_MS;
        }
    }
    s_last_reconnect_ms = millis();
}

// ─── Public API ───────────────────────────────────────────────────────────────
void mqtt_init(SemaphoreHandle_t status_mutex,
               SemaphoreHandle_t config_mutex,
               SensorStatus     *status,
               SensorConfig     *config) {
    s_status_mutex = status_mutex;
    s_config_mutex = config_mutex;
    s_status       = status;
    s_config       = config;

    s_cfg_mutex = xSemaphoreCreateMutex();
    configASSERT(s_cfg_mutex != nullptr);

    mqtt_config_load(s_cfg);
    s_mqtt_client.setCallback(on_message);

    DBG_PRINTLN("[MQTT] Handler initialised");
}

void mqtt_tick() {
    if (s_status == nullptr || s_cfg_mutex == nullptr) {
        return;
    }

    // Take a config snapshot for the enabled/broker check
    bool enabled       = false;
    bool broker_set    = false;
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        enabled    = s_cfg.enabled;
        broker_set = (s_cfg.broker[0] != '\0');
        xSemaphoreGive(s_cfg_mutex);
    } else {
        return;
    }

    if (!enabled || !broker_set) {
        // If previously connected but now disabled, disconnect cleanly
        if (s_mqtt_client.connected()) {
            char lwt_topic[96];
            if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                build_topic(lwt_topic, sizeof(lwt_topic),
                            s_cfg.topic_prefix, MQTT_SUFFIX_AVAIL);
                xSemaphoreGive(s_cfg_mutex);
                s_mqtt_client.publish(lwt_topic, "offline", /*retained=*/true);
            }
            s_mqtt_client.disconnect();
        }
        return;
    }

    if (!s_mqtt_client.connected()) {
        const uint32_t now = millis();
        if ((now - s_last_reconnect_ms) >= s_reconnect_backoff) {
            do_connect();
        }
        return;
    }

    // Drive the MQTT library (processes inbound messages, keepalive ping)
    s_mqtt_client.loop();

    // Check whether state has changed since last publish
    SystemState cur_state   = SystemState::INIT;
    bool        cur_fault   = false;
    bool        cur_enabled = true;

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        cur_state = s_status->state;
        cur_fault = s_status->fault_active;
        xSemaphoreGive(s_status_mutex);
    }
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        cur_enabled = s_config->sensor_enabled;
        xSemaphoreGive(s_config_mutex);
    }

    const uint32_t now = millis();
    const bool state_changed = (cur_state   != s_last_pub_state  ||
                                 cur_fault   != s_last_pub_fault  ||
                                 cur_enabled != s_last_pub_enabled);
    const bool heartbeat_due = ((now - s_last_publish_ms) >= MQTT_PUBLISH_INTERVAL_MS);

    if (state_changed || heartbeat_due) {
        publish_state();
    }
}

bool mqtt_is_connected() {
    return s_mqtt_client.connected();
}

MqttConfig mqtt_config_snapshot() {
    MqttConfig snap{};
    if (s_cfg_mutex && xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        snap = s_cfg;
        xSemaphoreGive(s_cfg_mutex);
    }
    return snap;
}

void mqtt_config_update(const MqttConfig &cfg) {
    if (s_cfg_mutex == nullptr) {
        return;
    }

    // Disconnect before applying new config to avoid stale connection
    if (s_mqtt_client.connected()) {
        char lwt_topic[96];
        if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            build_topic(lwt_topic, sizeof(lwt_topic),
                        s_cfg.topic_prefix, MQTT_SUFFIX_AVAIL);
            xSemaphoreGive(s_cfg_mutex);
            s_mqtt_client.publish(lwt_topic, "offline", /*retained=*/true);
        }
        s_mqtt_client.disconnect();
    }

    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_cfg = cfg;
        xSemaphoreGive(s_cfg_mutex);
    }

    mqtt_config_save(cfg);

    // Reset backoff so the next tick attempts reconnection promptly
    s_reconnect_backoff = MQTT_RECONNECT_MIN_MS;
    s_last_reconnect_ms = 0;

    DBG_PRINTLN("[MQTT] Config updated");
}
