#include "encoder.h"
#include "config.h"

// ─── ISR-visible globals ──────────────────────────────────────────────────────
volatile uint32_t g_last_motion_ms = 0;

static volatile int32_t  s_tick_count = 0;
static volatile int8_t   s_direction  = 0;
static volatile uint8_t  s_prev_state = 0;
static portMUX_TYPE      s_mux        = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t     s_queue = nullptr;
static const SensorConfig *s_cfg = nullptr;

// ─── Gray-code Quadrature Decoder Table ──────────────────────────────────────
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

// ─── ISR ─────────────────────────────────────────────────────────────────────
static void IRAM_ATTR encoder_isr() {
    // Read both channels atomically (both reads < 100 ns, encoder << 1 MHz)
    const uint8_t chA  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHA));
    const uint8_t chB  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHB));
    const uint8_t curr = (chB << 1) | chA;

    const int8_t delta = QEM[(s_prev_state << 2) | curr];

    portENTER_CRITICAL_ISR(&s_mux);
    s_tick_count += delta;
    if (delta != 0) {
        s_direction    = (delta > 0) ? 1 : -1;
        g_last_motion_ms = static_cast<uint32_t>(millis());
    }
    portEXIT_CRITICAL_ISR(&s_mux);

    s_prev_state = curr;
}

// ─── Public API ──────────────────────────────────────────────────────────────
void encoder_init(QueueHandle_t queue, const SensorConfig *cfg) {
    s_queue = queue;
    s_cfg   = cfg;

    pinMode(PIN_ENCODER_CHA, INPUT_PULLUP);
    pinMode(PIN_ENCODER_CHB, INPUT_PULLUP);
    pinMode(PIN_ENCODER_BTN, INPUT_PULLUP);

    // Capture initial state so first ISR transition is interpreted correctly
    const uint8_t chA  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHA));
    const uint8_t chB  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHB));
    s_prev_state = (chB << 1) | chA;

    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHA), encoder_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHB), encoder_isr, CHANGE);

    Serial.printf("[ENC] Encoder initialised (ChA=%d, ChB=%d, Btn=%d)\n",
                  PIN_ENCODER_CHA, PIN_ENCODER_CHB, PIN_ENCODER_BTN);
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
        int8_t  dir;
        portENTER_CRITICAL(&s_mux);
        ticks = s_tick_count;
        dir   = s_direction;
        portEXIT_CRITICAL(&s_mux);

        const int32_t delta = ticks - last_ticks;
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
    }
}
