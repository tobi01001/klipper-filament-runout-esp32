#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "types.h"

/**
 * @brief Initialise Moonraker polling runtime.
 */
void moonraker_init(SemaphoreHandle_t status_mutex,
                    SemaphoreHandle_t config_mutex,
                    SensorStatus     *status,
                    SensorConfig     *config);

/**
 * @brief Poll Moonraker on configured interval and update fault detector.
 *
 * Must only be called while WiFi station is connected.
 */
void moonraker_tick();

/**
 * @brief Mark Moonraker path as disconnected and keep detector in safe idle.
 */
void moonraker_set_disconnected();
