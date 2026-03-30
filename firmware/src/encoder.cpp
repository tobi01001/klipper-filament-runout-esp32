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

    // Capture initial state so first ISR transition is interpreted correctly
    const uint8_t chA  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHA));
    const uint8_t chB  = static_cast<uint8_t>(digitalRead(PIN_ENCODER_CHB));
    s_prev_state = (chB << 1) | chA;

    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHA), encoder_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CHB), encoder_isr, CHANGE);

    Serial.println("[ENC] Encoder initialised (ChA=" + String(PIN_ENCODER_CHA) +
                   ", ChB=" + String(PIN_ENCODER_CHB) + ")");
}

void encoder_task(void * /*param*/) {
    int32_t  last_ticks  = 0;
    uint32_t last_time   = millis();
    float    vel_ema     = 0.0f;

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

        // Push latest state to single-slot queue (overwrites previous if unread)
        EncoderData data;
        data.tick_count    = ticks;
        data.tick_delta    = delta;
        data.direction     = dir;
        data.timestamp_ms  = now_ms;
        data.velocity_mm_s = vel_ema;

        if (s_queue != nullptr) {
            xQueueOverwrite(s_queue, &data);
        }
    }
}
