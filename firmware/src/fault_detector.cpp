#include "fault_detector.h"
#include "encoder.h"
#include "config.h"

#include <Arduino.h>

// ─── Module-private references ───────────────────────────────────────────────
static SemaphoreHandle_t s_status_mutex  = nullptr;
static QueueHandle_t     s_encoder_queue = nullptr;
static SensorConfig     *s_cfg           = nullptr;
static SensorStatus     *s_status        = nullptr;

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void set_state(SystemState next) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_status->state != next) {
            Serial.printf("[FSM] %s → %s\n",
                          state_name(s_status->state), state_name(next));
            s_status->state = next;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

static void trigger_fault() {
    digitalWrite(PIN_RUNOUT, LOW);  // Active-LOW signal to Klipper
    Serial.println("[FAULT] Filament runout detected! GPIO " +
                   String(PIN_RUNOUT) + " → LOW");
    set_state(SystemState::FAULT);

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_status->fault_active = true;
        xSemaphoreGive(s_status_mutex);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void fault_detector_init(SemaphoreHandle_t status_mutex,
                         QueueHandle_t     encoder_queue,
                         SensorConfig     *config,
                         SensorStatus     *status) {
    s_status_mutex  = status_mutex;
    s_encoder_queue = encoder_queue;
    s_cfg           = config;
    s_status        = status;

    // Runout pin: default HIGH (no fault)
    pinMode(PIN_RUNOUT, OUTPUT);
    digitalWrite(PIN_RUNOUT, HIGH);

    Serial.println("[FD] Fault detector initialised (runout pin " +
                   String(PIN_RUNOUT) + ")");
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

    // Do not interfere with FAULT state – wait for explicit reset.
    // However, a physical button press always clears the fault.
    if (cur_state == SystemState::FAULT) {
        if (g_button_pressed) {
            g_button_pressed = false;
            fault_detector_reset();
        }
        return;
    }

    // Consume button press outside FAULT state (no-op; clears the flag)
    if (g_button_pressed) {
        g_button_pressed = false;
        Serial.println("[FD] Button pressed (no active fault – ignored)");
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
    digitalWrite(PIN_RUNOUT, HIGH);
    Serial.println("[FD] Fault cleared, GPIO " + String(PIN_RUNOUT) + " → HIGH");

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_status->fault_active = false;
        xSemaphoreGive(s_status_mutex);
    }

    // Refresh motion baseline so printing can resume immediately
    g_last_motion_ms = millis();
    set_state(SystemState::READY);
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
