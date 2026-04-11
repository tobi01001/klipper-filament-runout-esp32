#include "pin_config.h"
#include "debug_log.h"

#include <Preferences.h>

// ─── Validation helpers ───────────────────────────────────────────────────────

bool pin_config_valid_gpio(uint8_t pin) {
    if (pin > 39) return false;
    // GPIO 6–11 are connected to the internal SPI flash on most ESP32 modules.
    if (pin >= 6 && pin <= 11) return false;
    return true;
}

bool pin_config_output_capable(uint8_t pin) {
    if (!pin_config_valid_gpio(pin)) return false;
    // GPIO 34–39 are input-only (no output driver).
    if (pin >= 34) return false;
    return true;
}

// ─── NVS helpers ─────────────────────────────────────────────────────────────

void pin_config_load(PinConfig &pins) {
    Preferences prefs;
    if (!prefs.begin(PIN_CFG_NVS_NS, /*readOnly=*/true)) {
        // Namespace not found – first boot, keep compile-time defaults.
        DBG_PRINTLN("[PINS] No pin config in NVS – using compile-time defaults");
        return;
    }

    uint8_t v;

    v = prefs.getUChar("enc_a", pins.enc_a_pin);
    if (pin_config_valid_gpio(v)) pins.enc_a_pin = v;

    v = prefs.getUChar("enc_b", pins.enc_b_pin);
    if (pin_config_valid_gpio(v)) pins.enc_b_pin = v;

    v = prefs.getUChar("enc_btn", pins.enc_btn_pin);
    if (pin_config_valid_gpio(v)) pins.enc_btn_pin = v;

    v = prefs.getUChar("runout", pins.runout_pin);
    if (pin_config_output_capable(v)) pins.runout_pin = v;

#ifdef ENABLE_DHT
    v = prefs.getUChar("dht", pins.dht_pin);
    if (pin_config_valid_gpio(v)) pins.dht_pin = v;
#endif

    prefs.end();
    DBG_PRINTLN("[PINS] Pin config loaded from NVS");
}

void pin_config_save(const PinConfig &pins) {
    Preferences prefs;
    if (!prefs.begin(PIN_CFG_NVS_NS, /*readOnly=*/false)) {
        DBG_PRINTLN("[PINS] ERROR: Failed to open pin config NVS for writing");
        return;
    }

    prefs.putUChar("enc_a",   pins.enc_a_pin);
    prefs.putUChar("enc_b",   pins.enc_b_pin);
    prefs.putUChar("enc_btn", pins.enc_btn_pin);
    prefs.putUChar("runout",  pins.runout_pin);
#ifdef ENABLE_DHT
    prefs.putUChar("dht",     pins.dht_pin);
#endif

    prefs.end();
    DBG_PRINTLN("[PINS] Pin config saved to NVS");
}
