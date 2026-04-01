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
 * Skips rendering while an OTA update is active (see display_set_ota_active())
 * so that OTA progress screens are not overwritten.
 *
 * Call every OLED_UPDATE_MS (100 ms) from the Core 0 task loop.
 * The module redraws only when displayed values change.
 */
void display_update();

/**
 * @brief Signal the display module that an OTA update is active or has ended.
 *
 * While @p active is true, display_update() skips its normal sensor render so
 * that OTA progress screens are not overwritten.  Call with false when OTA ends
 * (failure or cancellation) to resume normal display updates.  On a successful
 * flash the device reboots so the flag need not be cleared.
 *
 * @param active  true when OTA starts; false when OTA ends without rebooting.
 */
void display_set_ota_active(bool active);

/**
 * @brief Render an OTA download progress bar on the display.
 *
 * Replaces the normal status screen with a centred progress bar and percentage
 * label.  Call repeatedly from the OTA task or ArduinoOTA progress callback as
 * each chunk is written; the display is redrawn on every call.
 *
 * @param percent  Download progress (0–100 %).
 */
void display_show_ota_progress(uint8_t percent);

/**
 * @brief Show an OTA-complete / rebooting message on the display.
 *
 * Replaces the progress screen with a static "OTA Complete – Rebooting…"
 * notice.  Call once after a successful OTA write, immediately before the
 * device reboots.
 */
void display_show_ota_reboot();

#endif // ENABLE_OLED
