#pragma once

#include <freertos/semphr.h>
#include "types.h"

/**
 * @brief Initialise the HTTP web server (port 80).
 *
 * Registers all REST API routes and the single-page application HTML.
 * Call once from the Core 0 task after WiFi is connected.
 *
 * @param status_mutex  Mutex protecting g_status.
 * @param config_mutex  Mutex protecting g_config.
 * @param status        Pointer to live SensorStatus.
 * @param config        Pointer to live SensorConfig.
 */
void web_init(SemaphoreHandle_t status_mutex,
              SemaphoreHandle_t config_mutex,
              SensorStatus     *status,
              SensorConfig     *config);

/**
 * @brief Process pending HTTP client connections (non-blocking).
 *
 * Call frequently (every 10 ms) from the Core 0 task loop.
 */
void web_handle_client();
