#include "moonraker_client.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

float moonraker_poll(const SensorConfig &cfg) {
    if (WiFi.status() != WL_CONNECTED) {
        return 0.0f;
    }

    // Build URL: http://<ip>:<port>/printer/objects/query?extruder
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s:%u/printer/objects/query?extruder",
             cfg.moonraker_ip, cfg.moonraker_port);

    HTTPClient http;
    http.begin(url);           // NOLINT(bugprone-unused-return-value)
    http.setTimeout(3000);     // 3-second timeout so we don't block Core 0

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[MR] HTTP %d from %s\n", httpCode, url);
        http.end();
        return 0.0f;
    }

    // Parse JSON – ArduinoJson 7 uses JsonDocument
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[MR] JSON parse error: %s\n", err.c_str());
        return 0.0f;
    }

    // result.status.extruder.velocity (default 0.0f if key absent)
    const float velocity = doc["result"]["status"]["extruder"]["velocity"] | 0.0f;
    return velocity;
}
