#include "wifi_handler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>

#include "config.h"
#include "fault_detector.h"
#include "debug_log.h"

// Shared references from main.
static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorStatus     *s_status = nullptr;
static SensorConfig     *s_config = nullptr;

// Non-blocking reconnect state.
static bool     s_connecting         = false;
static bool     s_sta_connected_flag = false;
static uint32_t s_connect_started_ms = 0;
static uint32_t s_next_attempt_ms    = 0;
static uint32_t s_backoff_ms         = 1000;
static bool     s_ap_active          = false;
static uint8_t  s_last_disc_reason   = 0;

// Phase state for the non-blocking STA-reset sequence.
// start_sta_attempt() initiates the sequence; wifi_tick() advances it using
// timestamps instead of vTaskDelay() so the Core 0 loop is never blocked.
enum class StaPhase : uint8_t {
    IDLE,            // no active sequence
    WAIT_AFTER_DISC, // WiFi.disconnect() done – waiting 60 ms before WIFI_OFF
    WAIT_AFTER_OFF,  // WiFi.mode(WIFI_OFF) done – waiting 60 ms before WIFI_STA
};
static StaPhase  s_sta_phase    = StaPhase::IDLE;
static uint32_t  s_sta_phase_ms = 0;  // millis() timestamp when phase was entered

static DNSServer s_dns;

struct WifiTelemetry {
    uint32_t connect_attempts;
    uint32_t connect_successes;
    uint32_t connect_timeouts;
    uint32_t disconnect_events;
    uint32_t ap_starts;
    uint32_t last_log_ms;
};

static WifiTelemetry s_tm{};

static constexpr uint32_t WIFI_LOG_INTERVAL_MS = 30000UL;

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        s_last_disc_reason = (uint8_t)info.wifi_sta_disconnected.reason;
        s_tm.disconnect_events++;
    }
}

static void telemetry_log_periodic(wl_status_t wl) {
    const uint32_t now = millis();
    if ((uint32_t)(now - s_tm.last_log_ms) < WIFI_LOG_INTERVAL_MS) {
        return;
    }
    s_tm.last_log_ms = now;

    DBG_PRINTF("[WiFi][TLM] wl=%d sta=%u ap=%u attempts=%lu ok=%lu timeouts=%lu disc=%lu reason=%u backoff=%lu\n",
               (int)wl,
               s_sta_connected_flag ? 1u : 0u,
               s_ap_active ? 1u : 0u,
               s_tm.connect_attempts,
               s_tm.connect_successes,
               s_tm.connect_timeouts,
               s_tm.disconnect_events,
               s_last_disc_reason,
               s_backoff_ms);
}

static void start_captive_portal_ap() {
    if (s_ap_active) {
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS)) {
        Serial.println("[WiFi] Failed to start AP captive portal");
        return;
    }

    const IPAddress ap_ip = WiFi.softAPIP();
    s_dns.start(53, "*", ap_ip);
    s_ap_active = true;
    s_tm.ap_starts++;
    DBG_PRINTF("[WiFi] Captive portal AP started: ssid=%s ip=%s\n",
               WIFI_AP_SSID,
               ap_ip.toString().c_str());
}

static void stop_captive_portal_ap() {
    if (!s_ap_active) {
        return;
    }

    s_dns.stop();
    WiFi.softAPdisconnect(true);
    s_ap_active = false;
    Serial.println("[WiFi] Captive portal AP stopped (mode change or station connected)");
}

void disconnect_WiFi(bool wifi_off = true)
{
  // lets do this twice.... seems to be not working all the time
  // and if it did not work it seems we miss the update...
  uint8_t disconnect_counter = 3;
  bool disconnected = false;
  while(!disconnected && disconnect_counter--)
  {
    Serial.println("[WiFi] Disconnecting WiFi ...");
    disconnected = WiFi.disconnect(false);
    delay(5);
  }
  if(wifi_off) 
  {
    Serial.println("[WiFi] Turning off WiFi ...");
    WiFi.disconnect(wifi_off);
    delay(5);
    WiFi.mode(WIFI_OFF);  //redundant but who cares....
  }
}
static void update_status(bool connected, const char *ip_text) {
    if (!s_status || !s_status_mutex) {
        return;
    }

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    s_status->wifi_connected = connected;
    strncpy(s_status->ip_address, ip_text ? ip_text : "0.0.0.0", sizeof(s_status->ip_address) - 1);
    s_status->ip_address[sizeof(s_status->ip_address) - 1] = '\0';

    if (!connected) {
        if (s_status->state != SystemState::FAULT) {
            s_status->state = SystemState::WIFI_FAIL;
        }
    } else if (s_status->state != SystemState::FAULT &&
               s_status->state != SystemState::PRINTING &&
               s_status->state != SystemState::IDLE) {
        s_status->state = SystemState::READY;
    }

    xSemaphoreGive(s_status_mutex);
}

static bool snapshot_config(SensorConfig *out_cfg) {
    if (!out_cfg || !s_config || !s_config_mutex) {
        return false;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    *out_cfg = *s_config;
    xSemaphoreGive(s_config_mutex);
    return true;
}

static void start_sta_attempt(const SensorConfig &cfg) {
    if (cfg.wifi_ssid[0] == '\0') {
        Serial.println("[WiFi] No SSID configured - station connect skipped");
        s_connecting      = false;
        s_next_attempt_ms = millis() + s_backoff_ms;
        update_status(false, "0.0.0.0");
        return;
    }

    DBG_PRINTF("[WiFi] Connecting to '%s' ...\n", cfg.wifi_ssid);
    Serial.println("[WiFi] Resetting WiFi state ...");
    s_tm.connect_attempts++;

    // Stop the captive portal AP before changing WiFi mode so that s_ap_active
    // stays consistent with the actual hardware state.  Without this, the flag
    // would remain true while the AP is effectively off, causing subsequent
    // calls to start_captive_portal_ap() to silently skip the restart.
    stop_captive_portal_ap();

    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.disconnect(false, false);

    // Kick off the non-blocking 2-phase reset sequence.
    // wifi_tick() will advance through WAIT_AFTER_DISC → WAIT_AFTER_OFF using
    // timestamps (60 ms each) so the Core 0 loop is never delayed here.
    s_sta_phase    = StaPhase::WAIT_AFTER_DISC;
    s_sta_phase_ms = millis();

    update_status(false, "0.0.0.0");

    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_status->state != SystemState::FAULT) {
            s_status->state = SystemState::WIFI_CONN;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

void wifi_init(SemaphoreHandle_t status_mutex,
               SemaphoreHandle_t config_mutex,
               SensorStatus     *status,
               SensorConfig     *config) {
    s_status_mutex = status_mutex;
    s_config_mutex = config_mutex;
    s_status       = status;
    s_config       = config;

    s_connecting         = false;
    s_sta_connected_flag = false;
    s_connect_started_ms = 0;
    s_next_attempt_ms    = 0;
    s_backoff_ms         = 1000;
    s_ap_active          = false;
    s_last_disc_reason   = 0;
    s_sta_phase          = StaPhase::IDLE;
    s_sta_phase_ms       = 0;
    memset(&s_tm, 0, sizeof(s_tm));

    WiFi.onEvent(on_wifi_event);

    update_status(false, "0.0.0.0");
}

void wifi_tick() {
    SensorConfig cfg{};
    if (!snapshot_config(&cfg)) {
        return;
    }

    const uint32_t now_ms = millis();
    const wl_status_t wl = WiFi.status();

    if (s_ap_active) {
        s_dns.processNextRequest();
    }

    if (wl == WL_CONNECTED) {
        if (!s_sta_connected_flag) {
            s_sta_connected_flag = true;
            s_connecting = false;
            s_sta_phase  = StaPhase::IDLE;  // abort any pending phase
            s_backoff_ms = 1000;
            s_tm.connect_successes++;
            const String ip = WiFi.localIP().toString();
            DBG_PRINTF("[WiFi] Connected, IP: %s\n", ip.c_str());
            update_status(true, ip.c_str());
            stop_captive_portal_ap();
        }
        telemetry_log_periodic(wl);
        return;
    }

    if (s_sta_connected_flag) {
        s_sta_connected_flag = false;
        update_status(false, "0.0.0.0");
        DBG_PRINTF("[WiFi] Disconnected (status=%d)\n", (int)wl);
    }

    // ── Non-blocking STA-reset phase sequence ─────────────────────────────────
    // start_sta_attempt() initiates this; each wifi_tick() call advances through
    // WAIT_AFTER_DISC → WAIT_AFTER_OFF → (s_connecting=true) without blocking.
    //
    // It is safe to return early here because:
    //  • WL_CONNECTED is already handled above (sets s_sta_phase=IDLE), so a
    //    connection completing mid-phase is not missed.
    //  • s_connecting is false during the phase (WiFi.begin() has not been called
    //    yet), so the timeout logic below would be a no-op anyway.
    //  • Returning early prevents a concurrent call to start_sta_attempt() from
    //    starting a second overlapping attempt while the reset is in progress.
    if (s_sta_phase != StaPhase::IDLE) {
        const uint32_t elapsed = (uint32_t)(now_ms - s_sta_phase_ms);
        if (s_sta_phase == StaPhase::WAIT_AFTER_DISC && elapsed >= 60U) {
            Serial.println("[WiFi] Starting station mode in WIFI_OFF ...");
            WiFi.mode(WIFI_OFF);
            s_sta_phase    = StaPhase::WAIT_AFTER_OFF;
            s_sta_phase_ms = now_ms;
        } else if (s_sta_phase == StaPhase::WAIT_AFTER_OFF && elapsed >= 60U) {
            Serial.println("[WiFi] Enabling station mode ...");
            if (!WiFi.mode(WIFI_STA)) {
                Serial.println("[WiFi] Failed to switch to WIFI_STA, retrying later");
                s_sta_phase       = StaPhase::IDLE;
                s_connecting      = false;
                s_next_attempt_ms = now_ms + s_backoff_ms;
            } else {
                WiFi.setAutoReconnect(true);
                WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
                s_sta_phase          = StaPhase::IDLE;
                s_connect_started_ms = now_ms;
                s_connecting         = true;
            }
        }
        telemetry_log_periodic(wl);
        return;
    }

    if (s_connecting) {
        if ((uint32_t)(now_ms - s_connect_started_ms) >= WIFI_CONNECT_TIMEOUT_MS) {
            s_connecting = false;
            s_next_attempt_ms = now_ms + s_backoff_ms;
            s_tm.connect_timeouts++;
            DBG_PRINTF("[WiFi] Connect timeout, retry in %lu ms\n", s_backoff_ms);
            s_backoff_ms = min<uint32_t>(s_backoff_ms * 2U,
                                         (uint32_t)WIFI_RECONNECT_MAX_MS);
            start_captive_portal_ap();
        }
        telemetry_log_periodic(wl);
        return;
    }

    if ((int32_t)(now_ms - s_next_attempt_ms) < 0) {
        start_captive_portal_ap();
        telemetry_log_periodic(wl);
        return;
    }

    start_sta_attempt(cfg);
    telemetry_log_periodic(wl);
}

bool wifi_is_connected() {
    return s_sta_connected_flag;
}

bool wifi_is_network_ready() {
    return s_sta_connected_flag || s_ap_active;
}

uint8_t wifi_last_disconnect_reason() {
    return s_last_disc_reason;
}
