#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "types.h"

// ─── Shared with fault detector (ISR-updated, volatile) ──────────────────────
// Timestamp of the most recent encoder edge; readable from any core.
extern volatile uint32_t g_last_motion_ms;

/**
 * @brief Initialise GPIO pins and attach quadrature interrupts.
 *
 * Must be called from a Core 1 context so the ISRs are pinned there.
 *
 * @param queue  Single-slot overwriting queue; encoder_task writes latest
 *               EncoderData here every ENCODER_UPDATE_MS milliseconds.
 * @param cfg    Pointer to live SensorConfig (cal_factor is read every cycle).
 */
void encoder_init(QueueHandle_t queue, const SensorConfig *cfg);

/**
 * @brief Core 1 real-time task – speed calculation at 50 Hz.
 *
 * Reads ISR-updated tick counter, calculates EMA velocity, and pushes an
 * EncoderData snapshot to the queue. Pin this task to Core 1.
 *
 * @param param  Unused (pass nullptr).
 */
void encoder_task(void *param);
