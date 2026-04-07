#include "ota_runtime.h"

#include <Arduino.h>

#include "config.h"
#include "ota_handler.h"

static SemaphoreHandle_t s_config_mutex = nullptr;
static SensorConfig     *s_config = nullptr;
static bool              s_initialized = false;

static void snapshot_ota_credentials(char *host_out, size_t host_len,
                                     char *pass_out, size_t pass_len) {
    if (!host_out || !pass_out || host_len == 0 || pass_len == 0) {
        return;
    }

    if (s_config && s_config_mutex && xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        strncpy(host_out, s_config->ota_hostname, host_len - 1);
        host_out[host_len - 1] = '\0';
        strncpy(pass_out, s_config->ota_password, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
        xSemaphoreGive(s_config_mutex);
    } else {
        strncpy(host_out, OTA_HOSTNAME, host_len - 1);
        host_out[host_len - 1] = '\0';
        strncpy(pass_out, OTA_PASSWORD, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
    }

    if (host_out[0] == '\0') {
        strncpy(host_out, OTA_HOSTNAME, host_len - 1);
        host_out[host_len - 1] = '\0';
    }
    if (pass_out[0] == '\0') {
        strncpy(pass_out, OTA_PASSWORD, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
    }
}

void ota_runtime_init(SemaphoreHandle_t config_mutex, SensorConfig *config) {
    s_config_mutex = config_mutex;
    s_config = config;
    s_initialized = false;
}

void ota_runtime_tick(bool wifi_connected) {
    if (!wifi_connected) {
        return;
    }

    if (!s_initialized) {
        char ota_host[32] = {0};
        char ota_pass[32] = {0};
        snapshot_ota_credentials(ota_host, sizeof(ota_host), ota_pass, sizeof(ota_pass));
        // ArduinoOTA is initialised once per WiFi connection.  If the user
        // changes ota_hostname or ota_password via the web UI, the new values
        // will not take effect until the device is rebooted (or WiFi
        // reconnects).  Use the "Restart Device" button in the web UI after
        // saving new OTA credentials to apply the change.
        ota_init(ota_host, ota_pass);
        s_initialized = true;
    }

    ota_handle();
}
