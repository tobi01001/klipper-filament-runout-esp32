#include "encoder.h"
#include "config.h"

// ─── ISR-visible globals ──────────────────────────────────────────────────────
volatile uint32_t g_last_motion_ms = 0;

static volatile int32_t  s_tick_count = 0;
static volatile uint8_t  s_prev_state = 0;
static portMUX_TYPE      s_mux        = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_last_isr_us = 0;
static volatile uint32_t s_rejected_edges = 0;
static volatile uint32_t s_valid_edges = 0;
static volatile uint32_t s_invalid_transitions = 0;
static volatile int8_t   s_step_accum = 0;

static QueueHandle_t     s_queue = nullptr;
static const SensorConfig *s_cfg = nullptr;

#if !ENCODER_USE_PULSE_SERVICE
// ─── Gray-code Quadrature Decoder Table ────────────────────────────────────
// index = (prev_state << 2) | curr_state
// state encoding: bit1 = ChB, bit0 = ChA
// Forward (CW) sequence: 0b00 → 0b01 → 0b11 → 0b10 → 0b00 → … (+1 each step)
// Reverse (CCW) sequence: 0b00 → 0b10 → 0b11 → 0b01 → 0b00 → … (-1 each step)
static const int8_t QEM[16] = {
     0, +1, -1,  0,  // prev = 0b00
    -1,  0,  0, +1,  // prev = 0b01
    +1,  0,  0, -1,  // prev = 0b10
     0, -1, +1,  0   // prev = 0b11
};
#endif

// ─── ISR ─────────────────────────────────────────────────────────────────────
#if ENCODER_USE_PULSE_SERVICE
static void IRAM_ATTR encoder_isr() {
    const uint32_t now_us = static_cast<uint32_t>(micros());

    if ((uint32_t)(now_us - s_last_isr_us) < ENCODER_ISR_DEBOUNCE_US) {
        s_rejected_edges++;
        return;
    }
    s_last_isr_us = now_us;

    portENTER_CRITICAL_ISR(&s_mux);
    s_tick_count += 1;
    s_valid_edges++;
    g_last_motion_ms = static_cast<uint32_t>(millis());
    portEXIT_CRITICAL_ISR(&s_mux);
}
#else
static void IRAM_ATTR encoder_isr() {
    const uint32_t now_us = static_cast<uint32_t>(micros());

    // Reject very closely spaced edges (mechanical bounce / ringing).
    if ((uint32_t)(now_us - s_last_isr_us) < ENCODER_ISR_DEBOUNCE_US) {
        s_rejected_edges++;
        return;
    }
    s_last_isr_us = now_us;

    // Read both channels atomically (both reads < 100 ns, encoder << 1 MHz)
    const uint8_t chA  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHA));
    const uint8_t chB  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHB));
    const uint8_t curr = (chB << 1) | chA;

    int8_t delta = 0;

    portENTER_CRITICAL_ISR(&s_mux);
    const uint8_t prev = s_prev_state;
    delta = QEM[(prev << 2) | curr];
    s_prev_state = curr;

    if (delta == 0 && curr != prev) {
        // Non-adjacent state transition indicates bounce/skipped edge.
        s_invalid_transitions++;
    }

#if ENCODER_USE_FULL_STEP
    if (delta != 0) {
        s_step_accum += delta;
        s_valid_edges++;
        if (s_step_accum >= 3) {
            s_tick_count += 1;
            s_step_accum = 0;
            g_last_motion_ms = static_cast<uint32_t>(millis());
        } else if (s_step_accum <= -3) {
            s_tick_count -= 1;
            s_step_accum = 0;
            g_last_motion_ms = static_cast<uint32_t>(millis());
        }
    }
#else
    s_tick_count += delta;
    if (delta != 0) {
        s_valid_edges++;
        g_last_motion_ms = static_cast<uint32_t>(millis());
    }
#endif
    portEXIT_CRITICAL_ISR(&s_mux);
}
#endif

// ─── Public API ──────────────────────────────────────────────────────────────
void encoder_init(QueueHandle_t queue, const SensorConfig *cfg) {
    s_queue = queue;
    s_cfg   = cfg;

    pinMode(PIN_ENCODER_CHA, INPUT_PULLUP);
    pinMode(PIN_ENCODER_CHB, INPUT_PULLUP);
    pinMode(PIN_ENCODER_BTN, INPUT_PULLUP);

#if ENCODER_USE_PULSE_SERVICE
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHA), encoder_isr, RISING);
    Serial.printf("[ENC] Pulse service initialised (ChA=%d RISING, Btn=%d)\n",
                  PIN_ENCODER_CHA, PIN_ENCODER_BTN);
#else
    // Capture initial state so first ISR transition is interpreted correctly
    const uint8_t chA  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHA));
    const uint8_t chB  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHB));
    s_prev_state = (chB << 1) | chA;

    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHA), encoder_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHB), encoder_isr, CHANGE);

    Serial.printf("[ENC] Quadrature service initialised (ChA=%d, ChB=%d, Btn=%d)\n",
                  PIN_ENCODER_CHA, PIN_ENCODER_CHB, PIN_ENCODER_BTN);
#endif
}

void encoder_task(void * /*param*/) {
    int32_t  last_ticks  = 0;
    uint32_t last_time   = millis();
    float    vel_ema     = 0.0f;

    // Button debounce state (task-level, not ISR)
    uint8_t  btn_history  = 0xFF;  // shift register; all-high → not pressed
    bool     btn_state    = false;
    int32_t  last_reported_ticks = 0;
    bool     last_reported_btn   = false;
    uint32_t last_edge_log_ms    = millis();

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ENCODER_UPDATE_MS));

        const uint32_t now_ms = millis();
        const uint32_t dt_ms  = now_ms - last_time;
        if (dt_ms == 0) {
            continue;
        }

        // Snapshot tick counter and direction under critical section
        int32_t ticks;
        portENTER_CRITICAL(&s_mux);
        ticks = s_tick_count;
        portEXIT_CRITICAL(&s_mux);

        const int32_t delta = ticks - last_ticks;
        const int8_t dir = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
        last_ticks = ticks;
        last_time  = now_ms;

        // Calculate instantaneous velocity, apply EMA filter
        const float cal_factor = s_cfg ? s_cfg->cal_factor : DEFAULT_CAL_FACTOR;
        const float raw_vel    = (static_cast<float>(delta) * cal_factor) /
                                 (static_cast<float>(dt_ms) / 1000.0f);
        vel_ema = EMA_ALPHA * raw_vel + (1.0f - EMA_ALPHA) * vel_ema;

        // Button debounce: shift in current reading; stable only when all 8 bits agree
        btn_history = static_cast<uint8_t>((btn_history << 1) |
                      (digitalRead(PIN_ENCODER_BTN) == LOW ? 1 : 0));
        if (btn_history == 0xFF) {
            btn_state = true;   // 8 consecutive LOW readings → pressed
        } else if (btn_history == 0x00) {
            btn_state = false;  // 8 consecutive HIGH readings → released
        }

        // Push latest state to single-slot queue (overwrites previous if unread)
        EncoderData data;
        data.tick_count    = ticks;
        data.tick_delta    = delta;
        data.direction     = dir;
        data.timestamp_ms  = now_ms;
        data.velocity_mm_s = vel_ema;
        data.btn_pressed   = btn_state;

        if (s_queue != nullptr) {
            xQueueOverwrite(s_queue, &data);
        }

        // Event-driven serial debug output
        if ((ticks != last_reported_ticks) || (btn_state != last_reported_btn)) {
            last_reported_ticks = ticks;
            last_reported_btn   = btn_state;
            const char *dir_str = (dir > 0) ? "FWD" : (dir < 0) ? "REV" : "---";
            Serial.printf("[ENC] ticks=%7ld  delta=%+4ld  dir=%s  "
                          "vel=%+7.2f mm/s  btn=%s\n",
                          (long)ticks, (long)delta, dir_str,
                          vel_ema, btn_state ? "PRESS" : "open");
        }

        // Periodic edge-quality telemetry helps diagnose wiring/noise issues.
        if ((uint32_t)(now_ms - last_edge_log_ms) >= 1000UL) {
            uint32_t valid = 0;
            uint32_t rejected = 0;
            uint32_t invalid = 0;
            portENTER_CRITICAL(&s_mux);
            valid = s_valid_edges;
            rejected = s_rejected_edges;
            invalid = s_invalid_transitions;
            s_valid_edges = 0;
            s_rejected_edges = 0;
            s_invalid_transitions = 0;
            portEXIT_CRITICAL(&s_mux);

            Serial.printf("[ENC][QUAL] mode=%s valid=%lu rejected=%lu invalid=%lu debounce_us=%lu ticks=%ld\n",
#if ENCODER_USE_PULSE_SERVICE
                          "pulse",
#elif ENCODER_USE_FULL_STEP
                          "full",
#else
                          "x4",
#endif
                          valid, rejected, invalid,
                          (unsigned long)ENCODER_ISR_DEBOUNCE_US, (long)ticks);
            last_edge_log_ms = now_ms;
        }
    }
}
