#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "types.h"
#include "pin_config.h"

/** Callback type for sending GCODE via WebSocket (implemented by moonraker module). */
typedef void (*gcode_send_fn_t)(const char *script);

/**
 * @brief Initialise the fault detector subsystem.
 *
 * Configures the runout GPIO pin (active-LOW output) and stores references
 * to the shared state and queue.
 *
 * @param status_mutex  Mutex protecting g_status reads/writes.
 * @param encoder_queue Single-slot encoder queue produced by Core 1.
 * @param config        Pointer to live SensorConfig.
 * @param status        Pointer to live SensorStatus.
 * @param gcode_fn      Callback to send a GCODE script via WebSocket; may be nullptr.
 * @param runout_pin    GPIO to drive LOW on fault (must be output-capable). Defaults
 *                      to the compile-time PIN_RUNOUT constant if not specified.
 */
void fault_detector_init(SemaphoreHandle_t status_mutex,
                         QueueHandle_t     encoder_queue,
                         SensorConfig     *config,
                         SensorStatus     *status,
                         gcode_send_fn_t   gcode_fn,
                         uint8_t           runout_pin = PIN_RUNOUT);

/**
 * @brief Update the fault detector with the latest extruder velocity.
 *
 * Call this every MOONRAKER_POLL_MS from the Core 0 task.
 * Reads the latest EncoderData from the queue, advances the state machine,
 * and drives GPIO 27 LOW if a stuck-filament fault is detected.
 *
 * @param ext_vel_mm_s  Extruder velocity returned by moonraker_poll().
 */
void fault_detector_update(float ext_vel_mm_s);

/**
 * @brief Clear an active fault and restore normal monitoring.
 *
 * Releases GPIO 27 HIGH, resets the state to READY, and resets the
 * motion timer so printing can resume.  Also aborts any in-progress
 * calibration run.
 */
void fault_detector_reset();

/**
 * @brief Start an encoder auto-calibration run.
 *
 * Sends a relative-extrusion GCODE sequence via the registered gcode_fn,
 * then measures the encoder tick delta until the motion stops.  The result
 * (mm / ticks) is available via fault_detector_get_cal_status() once the
 * state reaches CalState::DONE.
 *
 * @param extrude_mm   Distance to extrude (mm).  Valid range: 10 – 200 mm.
 * @param speed_mmpm   Extrude speed (mm/min).  Valid range: 60 – 600 mm/min.
 * @return true  if calibration was started successfully.
 * @return false if already running or gcode_fn is not set.
 */
bool fault_detector_start_calibration(float extrude_mm, float speed_mmpm);

/**
 * @brief Read the current calibration state and result.
 *
 * Thread-safe: reads module-private variables only (no mutex needed as
 * calibration state is only written from Core 0 / fault_detector_update).
 */
void fault_detector_get_cal_status(CalibrationStatus *out);

/** @brief Return a human-readable string for the given state. */
const char *state_name(SystemState s);
