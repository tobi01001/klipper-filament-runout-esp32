#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "types.h"

// ─── MQTT Configuration Struct ───────────────────────────────────────────────

struct MqttConfig {
    char     broker[64];        // MQTT broker hostname or IP
    uint16_t port;              // MQTT broker port (default 1883)
    char     username[32];      // Optional broker username (empty = anonymous)
    char     password[32];      // Optional broker password
    char     topic_prefix[48];  // Topic prefix (default "esp32/filament_sensor")
    bool     enabled;           // Enable/disable MQTT client
};

// ─── Defaults ────────────────────────────────────────────────────────────────
#define MQTT_DEFAULT_BROKER  ""
#define MQTT_DEFAULT_PORT    1883
#define MQTT_DEFAULT_USER    ""
#define MQTT_DEFAULT_PASS    ""
#define MQTT_DEFAULT_PREFIX  "esp32/filament_sensor"
#define MQTT_DEFAULT_ENABLED false

// ─── NVS namespace & keys (max 15 chars each) ────────────────────────────────
#define MQTT_NVS_NS          "mqtt_cfg"
#define MQTT_NVS_BROKER      "mqtt_broker"
#define MQTT_NVS_PORT        "mqtt_port"
#define MQTT_NVS_USER        "mqtt_user"
#define MQTT_NVS_PASS        "mqtt_pass"
#define MQTT_NVS_PREFIX      "mqtt_prefix"
#define MQTT_NVS_EN          "mqtt_en"

// ─── Topic suffixes ───────────────────────────────────────────────────────────
// Published topics  (ESP32 → broker):
//   <prefix>/state          – JSON sensor state (retained)
//   <prefix>/availability   – "online" / LWT "offline" (retained)
// Subscribed topics (broker → ESP32):
//   <prefix>/cmd/enable     – payload "1"/"0"/"true"/"false"
//   <prefix>/cmd/reset      – payload "1" / any

#define MQTT_SUFFIX_STATE        "/state"
#define MQTT_SUFFIX_AVAIL        "/availability"
#define MQTT_SUFFIX_CMD_ENABLE   "/cmd/enable"
#define MQTT_SUFFIX_CMD_RESET    "/cmd/reset"

// ─── Timing ──────────────────────────────────────────────────────────────────
#define MQTT_RECONNECT_MIN_MS    5000UL   // Initial reconnect wait after failure
#define MQTT_RECONNECT_MAX_MS  120000UL   // Maximum reconnect backoff
#define MQTT_PUBLISH_INTERVAL_MS 5000UL   // Periodic heartbeat publish interval

/**
 * @brief Load MQTT configuration from NVS into @p cfg.
 *
 * Missing keys are filled with compile-time defaults.
 * Safe to call from setup() before tasks are started.
 */
void mqtt_config_load(MqttConfig &cfg);

/**
 * @brief Persist MQTT configuration to NVS.
 *
 * Safe to call from any core; Preferences is internally mutex-protected.
 */
void mqtt_config_save(const MqttConfig &cfg);

/**
 * @brief Initialise the MQTT handler.
 *
 * Stores references to shared state and loads config from NVS.
 * Call once from the Core 0 task before the main loop.
 *
 * @param status_mutex  Mutex protecting g_status.
 * @param config_mutex  Mutex protecting g_config.
 * @param status        Pointer to live SensorStatus.
 * @param config        Pointer to live SensorConfig.
 */
void mqtt_init(SemaphoreHandle_t status_mutex,
               SemaphoreHandle_t config_mutex,
               SensorStatus     *status,
               SensorConfig     *config);

/**
 * @brief Service the MQTT client (non-blocking).
 *
 * Handles reconnect with exponential backoff, drives the PubSubClient loop,
 * publishes state on change and on the periodic heartbeat interval.
 * Call from the Core 0 main loop every CORE0_LOOP_MS (10 ms).
 */
void mqtt_tick();

/**
 * @brief Return true if the MQTT client is currently connected to the broker.
 *
 * Thread-safe (reads a single volatile bool).
 */
bool mqtt_is_connected();

/**
 * @brief Return a snapshot of the current MQTT configuration.
 *
 * Thread-safe; the returned struct is a value copy.
 */
MqttConfig mqtt_config_snapshot();

/**
 * @brief Update MQTT config at runtime, persist to NVS, and reconnect.
 *
 * If MQTT is enabled in the new config and WiFi is up, the client
 * immediately attempts a fresh connection.
 */
void mqtt_config_update(const MqttConfig &cfg);
