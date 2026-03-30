#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "types.h"

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
 */
void fault_detector_init(SemaphoreHandle_t status_mutex,
                         QueueHandle_t     encoder_queue,
                         SensorConfig     *config,
                         SensorStatus     *status);

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
 * motion timer so printing can resume.
 */
void fault_detector_reset();

/** @brief Return a human-readable string for the given state. */
const char *state_name(SystemState s);
