#include "nvs_config.h"
#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// ─── Helper macros ────────────────────────────────────────────────────────────
#define PREF_OPEN_RO()  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)
#define PREF_OPEN_RW()  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)

void nvs_load(SensorConfig &cfg) {
    Preferences prefs;

    if (!PREF_OPEN_RO()) {
        // No namespace yet – first boot, fill with defaults and save them
        Serial.println("[NVS] First boot – using defaults");
        cfg.cal_factor       = DEFAULT_CAL_FACTOR;
        cfg.timeout_ms       = DEFAULT_TIMEOUT_MS;
        cfg.min_ext_vel      = DEFAULT_MIN_EXT_VEL;
        cfg.motion_threshold = DEFAULT_MOTION_THRESH;
        strncpy(cfg.moonraker_ip,   DEFAULT_MOONRAKER_IP, sizeof(cfg.moonraker_ip) - 1);
        cfg.moonraker_ip[sizeof(cfg.moonraker_ip) - 1] = '\0';
        cfg.moonraker_port   = DEFAULT_MOONRAKER_PORT;
        cfg.wifi_ssid[0]     = '\0';
        cfg.wifi_pass[0]     = '\0';
#ifdef ENABLE_OLED
        cfg.display_enabled  = OLED_DEFAULT_EN;
#else
        cfg.display_enabled  = false;
#endif
        nvs_save(cfg);
        return;
    }

    cfg.cal_factor       = prefs.getFloat(NVS_KEY_CAL,     DEFAULT_CAL_FACTOR);
    cfg.timeout_ms       = prefs.getULong(NVS_KEY_TIMEOUT,  DEFAULT_TIMEOUT_MS);
    cfg.min_ext_vel      = prefs.getFloat(NVS_KEY_MIN_VEL,  DEFAULT_MIN_EXT_VEL);
    cfg.motion_threshold = prefs.getInt  (NVS_KEY_THRESH,   DEFAULT_MOTION_THRESH);
    cfg.moonraker_port   = static_cast<uint16_t>(
        prefs.getUInt(NVS_KEY_MR_PORT, DEFAULT_MOONRAKER_PORT));

    String mr_ip = prefs.getString(NVS_KEY_MR_IP, DEFAULT_MOONRAKER_IP);
    strncpy(cfg.moonraker_ip, mr_ip.c_str(), sizeof(cfg.moonraker_ip) - 1);
    cfg.moonraker_ip[sizeof(cfg.moonraker_ip) - 1] = '\0';

    String ssid = prefs.getString(NVS_KEY_SSID, "");
    strncpy(cfg.wifi_ssid, ssid.c_str(), sizeof(cfg.wifi_ssid) - 1);
    cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';

    String pass = prefs.getString(NVS_KEY_PASS, "");
    strncpy(cfg.wifi_pass, pass.c_str(), sizeof(cfg.wifi_pass) - 1);
    cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';

#ifdef ENABLE_OLED
    cfg.display_enabled = prefs.getBool(NVS_KEY_DISP_EN, OLED_DEFAULT_EN);
#else
    cfg.display_enabled = false;
#endif

    prefs.end();
    Serial.println("[NVS] Configuration loaded from NVS");
}

void nvs_save(const SensorConfig &cfg) {
    Preferences prefs;
    if (!PREF_OPEN_RW()) {
        Serial.println("[NVS] ERROR: Failed to open NVS for writing");
        return;
    }

    prefs.putFloat (NVS_KEY_CAL,     cfg.cal_factor);
    prefs.putULong (NVS_KEY_TIMEOUT, cfg.timeout_ms);
    prefs.putFloat (NVS_KEY_MIN_VEL, cfg.min_ext_vel);
    prefs.putInt   (NVS_KEY_THRESH,  cfg.motion_threshold);
    prefs.putUInt  (NVS_KEY_MR_PORT, cfg.moonraker_port);
    prefs.putString(NVS_KEY_MR_IP,   cfg.moonraker_ip);
    prefs.putString(NVS_KEY_SSID,    cfg.wifi_ssid);
    prefs.putString(NVS_KEY_PASS,    cfg.wifi_pass);
    prefs.putBool  (NVS_KEY_DISP_EN, cfg.display_enabled);

    prefs.end();
    Serial.println("[NVS] Configuration saved to NVS");
}
