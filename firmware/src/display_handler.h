#pragma once

#include "config.h"

#ifdef ENABLE_OLED

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "types.h"

/**
 * @brief Initialise the SSD1306 OLED display over I²C.
 *
 * Sets up the U8g2 driver on OLED_SDA_PIN / OLED_SCL_PIN and draws a
 * boot splash.  Call once from the Core 0 task after WiFi init so that
 * I²C traffic stays on a single core.
 *
 * @param status_mutex  Mutex protecting g_status.
 * @param config_mutex  Mutex protecting g_config.
 * @param status        Pointer to live SensorStatus.
 * @param config        Pointer to live SensorConfig.
 */
void display_init(SemaphoreHandle_t status_mutex,
                  SemaphoreHandle_t config_mutex,
                  SensorStatus     *status,
                  SensorConfig     *config);

/**
 * @brief Refresh the OLED display with the latest sensor data.
 *
 * Reads SensorStatus (under mutex) and renders:
 *   · System state (title bar, inverted + blinking when FAULT)
 *   · Encoder velocity (mm/s)
 *   · Extruder velocity reported by Moonraker (mm/s)
 *   · Cumulative tick count and motion direction
 *   · Sensor IP address / WiFi status
 *
 * When config->display_enabled is false the display is blanked (power-save
 * mode) and the function returns immediately on subsequent calls.
 *
 * Call every OLED_UPDATE_MS (500 ms) from the Core 0 task loop.
 */
void display_update();

#endif // ENABLE_OLED
