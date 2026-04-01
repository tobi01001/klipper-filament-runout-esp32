#include "moonraker.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "fault_detector.h"

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorStatus     *s_status = nullptr;
static SensorConfig     *s_config = nullptr;

static TickType_t s_last_poll = 0;

static bool snapshot_config(SensorConfig *cfg_out) {
    if (!cfg_out || !s_config || !s_config_mutex) {
        return false;
    }
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
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

static float moonraker_poll_once(const SensorConfig &cfg) {
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s:%u/printer/objects/query?extruder",
             cfg.moonraker_ip, cfg.moonraker_port);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(3000);

    const int http_code = http.GET();
    if (http_code != HTTP_CODE_OK) {
        Serial.printf("[MR] HTTP %d from %s\n", http_code, url);
        http.end();
        return 0.0f;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[MR] JSON parse error: %s\n", err.c_str());
        return 0.0f;
    }

    return doc["result"]["status"]["extruder"]["velocity"] | 0.0f;
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
}

void moonraker_tick() {
    if ((xTaskGetTickCount() - s_last_poll) < pdMS_TO_TICKS(MOONRAKER_POLL_MS)) {
        return;
    }
    s_last_poll = xTaskGetTickCount();

    SensorConfig cfg{};
    if (!snapshot_config(&cfg)) {
        return;
    }

    const float ext_vel = moonraker_poll_once(cfg);
    set_extruder_velocity(ext_vel);
    fault_detector_update(ext_vel);
}

void moonraker_set_disconnected() {
    set_extruder_velocity(0.0f);
    fault_detector_update(0.0f);
}
