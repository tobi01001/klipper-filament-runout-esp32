#pragma once

#include "config.h"

#ifdef ENABLE_DHT

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "types.h"

/**
 * @brief Initialise the DHT22 sensor driver.
 *
 * Sets up the Adafruit DHT library on DHT_PIN and stores the shared-state
 * pointers used by dht_sensor_tick().  Call once from the Core 0 task after
 * WiFi init so that all sensor I/O stays on a single core.
 *
 * @param status_mutex  Mutex protecting g_status.
 * @param config_mutex  Mutex protecting g_config.
 * @param status        Pointer to live SensorStatus.
 * @param config        Pointer to live SensorConfig.
 */
void dht_sensor_init(SemaphoreHandle_t status_mutex,
                     SemaphoreHandle_t config_mutex,
                     SensorStatus     *status,
                     SensorConfig     *config);

/**
 * @brief Poll the DHT22 sensor (non-blocking, respects DHT_READ_INTERVAL_MS).
 *
 * Reads temperature and humidity at most every DHT_READ_INTERVAL_MS.  On a
 * successful read the SensorStatus fields dht_temperature, dht_humidity, and
 * dht_valid are updated under the status_mutex.  On failure dht_valid is set
 * to false and the previous temperature/humidity values are preserved.
 *
 * When config->dht_enabled is false the function returns immediately without
 * touching the sensor or the status struct.
 *
 * Call every CORE0_LOOP_MS (10 ms) from the Core 0 task loop.
 */
void dht_sensor_tick();

#endif // ENABLE_DHT
