#include "fault_detector.h"
#include "encoder.h"
#include "config.h"
#include "debug_log.h"

#include <Arduino.h>
#include <math.h>

// ─── Module-private references ────────────────────────────────────────────────────────────
static SemaphoreHandle_t s_status_mutex  = nullptr;
static QueueHandle_t     s_encoder_queue = nullptr;
static SensorConfig     *s_cfg           = nullptr;
static SensorStatus     *s_status        = nullptr;
static gcode_send_fn_t   s_gcode_fn      = nullptr;
static uint8_t           s_runout_pin    = PIN_RUNOUT;

// ─── Calibration state ───────────────────────────────────────────────────────────────────
static CalState    s_cal_state         = CalState::IDLE;
static float       s_cal_mm            = 0.0f;
static int32_t     s_cal_tick_baseline = 0;   // tick count at GCODE send time
static int32_t     s_cal_tick_start    = 0;   // tick count when motion confirmed
static int32_t     s_cal_tick_end      = 0;
static uint32_t    s_cal_start_ms   = 0;
static uint32_t    s_cal_settle_ms  = 0;
static float       s_cal_result     = 0.0f;
static char        s_cal_error[32]  = "";

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void set_state(SystemState next) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_status->state != next) {
            DBG_PRINTF("[FSM] %s -> %s\n",
                       state_name(s_status->state), state_name(next));
            s_status->state = next;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

static void trigger_fault() {
    digitalWrite(s_runout_pin, LOW);  // Active-LOW signal to Klipper
    DBG_PRINTLN("[FAULT] Filament runout detected! GPIO " +
                   String(s_runout_pin) + " → LOW");
    set_state(SystemState::FAULT);

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_status->fault_active = true;
        xSemaphoreGive(s_status_mutex);
    }

    // Send fault GCODE via WebSocket if configured
    if (s_gcode_fn != nullptr && s_cfg != nullptr && s_cfg->fault_gcode[0] != '\0') {
        s_gcode_fn(s_cfg->fault_gcode);
        DBG_PRINTLN("[FD] Fault GCODE sent: " + String(s_cfg->fault_gcode));
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void fault_detector_init(SemaphoreHandle_t status_mutex,
                         QueueHandle_t     encoder_queue,
                         SensorConfig     *config,
                         SensorStatus     *status,
                         gcode_send_fn_t   gcode_fn,
                         uint8_t           runout_pin) {
    s_status_mutex  = status_mutex;
    s_encoder_queue = encoder_queue;
    s_cfg           = config;
    s_status        = status;
    s_gcode_fn      = gcode_fn;
    s_runout_pin    = runout_pin;

    // Runout pin: default HIGH (no fault)
    pinMode(s_runout_pin, OUTPUT);
    digitalWrite(s_runout_pin, HIGH);

    DBG_PRINTLN("[FD] Fault detector initialised (runout pin " +
                   String(s_runout_pin) + ")");
}

void fault_detector_update(float ext_vel_mm_s) {
    if (s_status == nullptr || s_cfg == nullptr) {
        return;
    }

    // Always capture latest encoder data from queue (non-blocking peek)
    EncoderData enc{};
    const bool  got_enc = (xQueuePeek(s_encoder_queue, &enc, 0) == pdTRUE);

    const uint32_t now_ms     = millis();
    const uint32_t idle_ms    = now_ms - g_last_motion_ms;
    SystemState    cur_state;

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        cur_state              = s_status->state;
        s_status->extruder_vel = ext_vel_mm_s;
        if (got_enc) {
            s_status->encoder = enc;
        }
        xSemaphoreGive(s_status_mutex);
    } else {
        return;
    }

    // If monitoring is disabled, keep runout output inactive and clear FAULT.
    if (!s_cfg->sensor_enabled) {
        if (cur_state == SystemState::FAULT) {
            fault_detector_reset();
        }
        return;
    }

    // ── Calibration overlay state machine (runs on Core 0, not in an ISR) ─────────
    if (s_cal_state != CalState::IDLE &&
        s_cal_state != CalState::DONE &&
        s_cal_state != CalState::FAILED) {

        const uint32_t cal_elapsed = now_ms - s_cal_start_ms;

        if (s_cal_state == CalState::SENT) {
            // Detect motion via tick count change – reliable even before the
            // velocity filter window fills up (velocity reads 0 initially).
            const bool motion_seen = got_enc &&
                (abs(enc.tick_count - s_cal_tick_baseline) >= 2);

            if (motion_seen) {
                s_cal_state      = CalState::MOVING;
                s_cal_tick_start = enc.tick_count;
                DBG_PRINTLN("[CAL] Motion detected, recording ticks");
            } else if (cal_elapsed > CAL_WAIT_START_MS) {
                s_cal_state = CalState::FAILED;
                strncpy(s_cal_error, "No motion detected", sizeof(s_cal_error) - 1);
                s_cal_error[sizeof(s_cal_error) - 1] = '\0';
                DBG_PRINTLN("[CAL] FAILED: no motion within timeout");
            }

        } else if (s_cal_state == CalState::MOVING) {
            // Keep updating the end-tick while motion continues.
            if (got_enc) {
                s_cal_tick_end = enc.tick_count;
            }

            // Stop condition based on ISR-level silence, NOT tick_delta or velocity.
            // tick_delta is 0 in most 20 ms windows at slow extrusion (e.g. 12 ticks/s
            // on a 0.4 nozzle / 1.75 mm filament), so it would fire mid-extrusion
            // and produce a different tick count on every run.
            // g_last_motion_ms is updated by the ISR on every encoder edge –
            // reliable at any tick rate.
            const uint32_t enc_idle_ms = (uint32_t)(now_ms - g_last_motion_ms);
            const bool encoder_stopped  = (enc_idle_ms >= CAL_ENC_IDLE_MS);
            // If Moonraker is stale, ext_vel_mm_s == 0 (set in moonraker_tick),
            // so this condition is safe when the WebSocket link is lost.
            const bool extruder_stopped = (ext_vel_mm_s < 0.15f);

            if (encoder_stopped && extruder_stopped) {
                s_cal_state     = CalState::SETTLING;
                s_cal_settle_ms = now_ms;
                DBG_PRINTLN("[CAL] Motion stopped, settling");
            } else if (cal_elapsed > CAL_WAIT_STOP_MS) {
                // Safety timeout – compute from what we have
                if (got_enc) { s_cal_tick_end = enc.tick_count; }
                const int32_t dticks = abs(s_cal_tick_end - s_cal_tick_baseline);
                if (dticks > 0) {
                    s_cal_result = s_cal_mm / static_cast<float>(dticks);
                    s_cal_state  = CalState::DONE;
                    DBG_PRINTF("[CAL] Timeout-done: %.1f mm / %ld ticks = %.4f mm/tick\n",
                                  s_cal_mm, (long)dticks, s_cal_result);
                } else {
                    s_cal_state = CalState::FAILED;
                    strncpy(s_cal_error, "Zero ticks (timeout)", sizeof(s_cal_error) - 1);
                    s_cal_error[sizeof(s_cal_error) - 1] = '\0';
                }
            }

        } else if (s_cal_state == CalState::SETTLING) {
            if ((now_ms - s_cal_settle_ms) >= CAL_SETTLE_MS) {
                // Use baseline (pre-GCODE, encoder at rest) as reference.
                // This captures every tick the extrude command produced,
                // including the first 1-2 ticks before s_cal_tick_start.
                const int32_t dticks = abs(s_cal_tick_end - s_cal_tick_baseline);
                if (dticks > 0) {
                    s_cal_result = s_cal_mm / static_cast<float>(dticks);
                    s_cal_state  = CalState::DONE;
                    DBG_PRINTF("[CAL] Done: %.1f mm / %ld ticks = %.4f mm/tick\n",
                                  s_cal_mm, (long)dticks, s_cal_result);
                } else {
                    s_cal_state = CalState::FAILED;
                    strncpy(s_cal_error, "Zero ticks measured", sizeof(s_cal_error) - 1);
                    s_cal_error[sizeof(s_cal_error) - 1] = '\0';
                    DBG_PRINTLN("[CAL] FAILED: zero ticks");
                }
            }
        }
    }

    // Do not interfere with FAULT state – wait for explicit reset
    if (cur_state == SystemState::FAULT) {
        return;
    }

    if (ext_vel_mm_s >= s_cfg->min_ext_vel) {
        // ── Extruder is actively pushing filament ───────────────────────────
        if (cur_state == SystemState::READY ||
            cur_state == SystemState::IDLE) {
            // Reset motion timer when we first enter PRINTING so fresh
            // filament has time_ms to start moving before the fault window.
            // g_last_motion_ms is set by the ISR; we reset it here to "now"
            // only if the encoder has not moved recently (cold start).
            if (idle_ms > s_cfg->timeout_ms) {
                // Suppress immediate false fault on print start by refreshing
                // the baseline. The ISR will update this as soon as filament
                // actually moves.
                // NOTE: g_last_motion_ms is volatile; single 32-bit write is
                //       atomic on ARM Cortex-M / Xtensa LX6.
                g_last_motion_ms = now_ms;
            }
            set_state(SystemState::PRINTING);
        } else if (cur_state == SystemState::PRINTING) {
            // Check for stuck / broken filament
            if (idle_ms >= s_cfg->timeout_ms) {
                trigger_fault();
            }
        }
    } else {
        // ── Extruder is idle ────────────────────────────────────────────────
        if (cur_state == SystemState::PRINTING) {
            set_state(SystemState::IDLE);
        } else if (cur_state == SystemState::WIFI_CONN ||
                   cur_state == SystemState::WIFI_FAIL) {
            // Still connecting – do nothing
        } else if (cur_state == SystemState::INIT) {
            set_state(SystemState::READY);
        }
    }
}

void fault_detector_reset() {
    digitalWrite(s_runout_pin, HIGH);
    DBG_PRINTLN("[FD] Fault cleared, GPIO " + String(s_runout_pin) + " → HIGH");

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_status->fault_active = false;
        xSemaphoreGive(s_status_mutex);
    }

    // Refresh motion baseline so printing can resume immediately
    g_last_motion_ms = millis();
    set_state(SystemState::READY);

    // Abort any in-progress calibration
    if (s_cal_state != CalState::IDLE &&
        s_cal_state != CalState::DONE &&
        s_cal_state != CalState::FAILED) {
        s_cal_state = CalState::FAILED;
        strncpy(s_cal_error, "Aborted (fault reset)", sizeof(s_cal_error) - 1);
        s_cal_error[sizeof(s_cal_error) - 1] = '\0';
    }
}

bool fault_detector_start_calibration(float extrude_mm, float speed_mmpm) {
    if (s_gcode_fn == nullptr) {
        return false;
    }
    // Allow restart from DONE or FAILED but not while actively running
    if (s_cal_state == CalState::SENT ||
        s_cal_state == CalState::MOVING ||
        s_cal_state == CalState::SETTLING) {
        return false;
    }

    s_cal_mm             = extrude_mm;
    s_cal_result         = 0.0f;
    s_cal_error[0]       = '\0';
    s_cal_tick_baseline  = 0;
    s_cal_tick_start     = 0;
    s_cal_tick_end       = 0;
    s_cal_start_ms       = millis();
    s_cal_settle_ms      = 0;

    // Snapshot current tick count so we can detect motion vs. pre-existing ticks
    EncoderData enc_snap{};
    if (xQueuePeek(s_encoder_queue, &enc_snap, 0) == pdTRUE) {
        s_cal_tick_baseline = enc_snap.tick_count;
    }

    // Build and send relative-extrusion GCODE
    char script[72];
    snprintf(script, sizeof(script), "M83\nG1 E%.1f F%.0f\nM82", extrude_mm, speed_mmpm);
    s_gcode_fn(script);

    s_cal_state = CalState::SENT;
    DBG_PRINTF("[CAL] Started: %.1f mm @ %.0f mm/min\n", extrude_mm, speed_mmpm);
    return true;
}

void fault_detector_get_cal_status(CalibrationStatus *out) {
    if (!out) { return; }
    out->state             = s_cal_state;
    out->result_cal_factor = s_cal_result;
    out->measured_ticks    = abs(s_cal_tick_end - s_cal_tick_baseline);
    out->requested_mm      = s_cal_mm;
    strncpy(out->error, s_cal_error, sizeof(out->error) - 1);
    out->error[sizeof(out->error) - 1] = '\0';
}

const char *state_name(SystemState s) {
    switch (s) {
        case SystemState::INIT:      return "INIT";
        case SystemState::WIFI_CONN: return "WIFI_CONN";
        case SystemState::WIFI_FAIL: return "WIFI_FAIL";
        case SystemState::READY:     return "READY";
        case SystemState::IDLE:      return "IDLE";
        case SystemState::PRINTING:  return "PRINTING";
        case SystemState::FAULT:     return "FAULT";
        default:                     return "UNKNOWN";
    }
}
