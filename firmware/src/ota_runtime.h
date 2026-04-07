#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "types.h"

/**
 * @brief Initialise OTA runtime wrapper.
 */
void ota_runtime_init(SemaphoreHandle_t config_mutex, SensorConfig *config);

/**
 * @brief Service OTA only when WiFi is connected.
 */
void ota_runtime_tick(bool wifi_connected);
