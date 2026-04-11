#include "dht_sensor.h"

#ifdef ENABLE_DHT

#include <DHT.h>
#include <Arduino.h>
#include <math.h>

#include "config.h"
#include "debug_log.h"

// ─── Module state ─────────────────────────────────────────────────────────────
static DHT             *s_dht          = nullptr;

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorStatus     *s_status       = nullptr;
static SensorConfig     *s_config       = nullptr;

static uint32_t          s_last_read_ms = 0;
static bool              s_initialized  = false;

// ─── Public API ───────────────────────────────────────────────────────────────

void dht_sensor_init(SemaphoreHandle_t status_mutex,
                     SemaphoreHandle_t config_mutex,
                     SensorStatus     *status,
                     SensorConfig     *config,
                     uint8_t           dht_pin) {
    s_status_mutex = status_mutex;
    s_config_mutex = config_mutex;
    s_status       = status;
    s_config       = config;

    // Allocate DHT object with the runtime-configured pin.
    // Guard against accidental double-initialisation: free the old object first.
    if (s_dht != nullptr) {
        delete s_dht;
        s_dht = nullptr;
    }
    // This single allocation lives for the process lifetime under normal operation.
    s_dht = new DHT(dht_pin, DHT22);
    s_dht->begin();
    s_initialized = true;

    DBG_PRINTLN("[DHT] DHT22 initialised on GPIO " + String(dht_pin));
}

void dht_sensor_tick() {
    if (!s_initialized || !s_status || !s_config || !s_dht) {
        return;
    }

    // Respect runtime enable/disable setting.
    bool enabled = true;
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        enabled = s_config->dht_enabled;
        xSemaphoreGive(s_config_mutex);
    }
    if (!enabled) {
        return;
    }

    // Enforce minimum read interval (DHT22 requires ≥2 s between samples).
    const uint32_t now_ms = millis();
    if (now_ms - s_last_read_ms < DHT_READ_INTERVAL_MS) {
        return;
    }
    s_last_read_ms = now_ms;

    // Read sensor – these calls block for ~20 ms while the DHT22 sends its
    // 40-bit response.  This is acceptable on Core 0 given the 3-second
    // interval; the 10 ms loop tick will simply be late for that one cycle.
    const float temp = s_dht->readTemperature();  // °C
    const float hum  = s_dht->readHumidity();     // %RH
    const bool  valid = !isnan(temp) && !isnan(hum);

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (valid) {
            s_status->dht_temperature = temp;
            s_status->dht_humidity    = hum;
        }
        s_status->dht_valid = valid;
        xSemaphoreGive(s_status_mutex);
    }

    if (valid) {
        DBG_PRINTF("[DHT] T=%.1f°C H=%.1f%%\n", temp, hum);
    } else {
        DBG_PRINTLN("[DHT] Read failed – check wiring");
    }
}

#endif // ENABLE_DHT
