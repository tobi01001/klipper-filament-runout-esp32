#pragma once

#include <stdint.h>
#include "config.h"

// ─── Runtime-configurable GPIO pin assignments ────────────────────────────────
// Defaults are the compile-time constants from config.h.
// pin_config_load() overwrites these from NVS if entries exist.
struct PinConfig {
    uint8_t enc_a_pin   = PIN_ENCODER_CHA;
    uint8_t enc_b_pin   = PIN_ENCODER_CHB;
    uint8_t enc_btn_pin = PIN_ENCODER_BTN;
    uint8_t runout_pin  = PIN_RUNOUT;
#ifdef ENABLE_DHT
    uint8_t dht_pin     = DHT_PIN;
#endif
};

// ─── NVS namespace for pin config (separate from "filsns") ───────────────────
#define PIN_CFG_NVS_NS "pin_cfg"

/**
 * @brief Load pin assignments from NVS into @p pins.
 *
 * If the NVS namespace does not exist yet (first boot) or a key is missing,
 * the corresponding field in @p pins is left at its default value
 * (i.e. the compile-time constant from config.h).
 * Call once during setup() before starting tasks.
 *
 * @param pins  Struct to fill; initialise with defaults before calling.
 */
void pin_config_load(PinConfig &pins);

/**
 * @brief Persist pin assignments from @p pins to NVS.
 *
 * Safe to call from any core; Preferences is internally mutex-protected.
 *
 * @param pins  Pin assignments to save.
 */
void pin_config_save(const PinConfig &pins);

/**
 * @brief Return true if the pin number is a valid, usable GPIO on ESP32.
 *
 * Rejects: > 39, strapping/SPI-flash pins 6–11.
 */
bool pin_config_valid_gpio(uint8_t pin);

/**
 * @brief Return true if the pin can be used as a digital output.
 *
 * GPIO 34–39 are input-only; all others that pass pin_config_valid_gpio() are
 * output-capable.
 */
bool pin_config_output_capable(uint8_t pin);
