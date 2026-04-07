#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "types.h"

/**
 * @brief Initialise the WiFi manager.
 *
 * Stores references to shared status/config and prepares the non-blocking
 * station reconnect state machine.
 */
void wifi_init(SemaphoreHandle_t status_mutex,
               SemaphoreHandle_t config_mutex,
               SensorStatus     *status,
               SensorConfig     *config);

/**
 * @brief Progress the non-blocking station connect/reconnect state machine.
 *
 * Call from Core 0 loop at CORE0_LOOP_MS cadence.
 */
void wifi_tick();

/**
 * @brief Reliable station connection flag managed by wifi_tick().
 *
 * Returns true only when STA is connected and has a valid local IP.
 */
bool wifi_is_connected();

/**
 * @brief Returns true when either STA is connected or AP captive portal is active.
 */
bool wifi_is_network_ready();

/**
 * @brief Return last station disconnect reason code from WiFi events.
 */
uint8_t wifi_last_disconnect_reason();

void disconnect_WiFi(bool wifi_off);
