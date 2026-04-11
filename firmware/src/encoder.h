#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "types.h"
#include "pin_config.h"

// ─── Shared with fault detector (ISR-updated, volatile) ──────────────────────
// Timestamp of the most recent encoder edge; readable from any core.
extern volatile uint32_t g_last_motion_ms;

/**
 * @brief Set by the SW-pin ISR when the KY-040 push-button is pressed.
 *
 * Cleared by the consumer (fault_detector_update) after each read.
 * Single 8-bit write/read is atomic on Xtensa LX6 – no mutex needed.
 */
extern volatile bool g_button_pressed;

/**
 * @brief Initialise GPIO pins and attach quadrature interrupts.
 *
 * Must be called from a Core 1 context so the ISRs are pinned there.
 * Configures CLK (ChA), DT (ChB), and SW (push-button) pins of the KY-040.
 *
 * @param queue  Single-slot overwriting queue; encoder_task writes latest
 *               EncoderData here every ENCODER_UPDATE_MS milliseconds.
 * @param cfg    Pointer to live SensorConfig (cal_factor is read every cycle).
 * @param pins   Pointer to PinConfig (enc_a_pin, enc_b_pin, enc_btn_pin used).
 */
void encoder_init(QueueHandle_t queue, const SensorConfig *cfg, const PinConfig *pins);

/**
 * @brief Core 1 real-time task – speed calculation at 50 Hz.
 *
 * Reads ISR-updated tick counter, calculates EMA velocity, and pushes an
 * EncoderData snapshot to the queue. Pin this task to Core 1.
 *
 * @param param  Unused (pass nullptr).
 */
void encoder_task(void *param);
