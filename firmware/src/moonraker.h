#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "types.h"

struct MoonrakerDiag {
    bool     ws_initialized;
    bool     ws_connected;
    bool     subscribed;
    bool     klippy_ready;
    bool     stale;
    bool     last_connect_ok;
    uint32_t last_ext_rx_ms;
    uint32_t last_connect_attempt_ms;
    uint32_t consecutive_connect_failures;
    uint32_t reconnect_backoff_ms;
    uint32_t ws_connect_events;
    uint32_t ws_disconnect_events;
    uint32_t ws_start_attempts;
    uint32_t info_requests;
    uint32_t subscribe_requests;
    uint32_t query_requests;
    uint32_t json_errors;
    uint32_t ws_probe_attempts;
    uint32_t ws_probe_101;
    uint32_t ws_probe_last_ms;
    bool     ws_probe_last_ok;
    uint16_t last_connect_port;
    char     last_connect_host[40];
    char     ws_probe_status_line[64];
    char     klippy_state[16];
};

/**
 * @brief Initialise Moonraker polling runtime.
 */
void moonraker_init(SemaphoreHandle_t status_mutex,
                    SemaphoreHandle_t config_mutex,
                    SensorStatus     *status,
                    SensorConfig     *config);

/**
 * @brief Poll Moonraker on configured interval and update fault detector.
 *
 * Must only be called while WiFi station is connected.
 */
void moonraker_tick();

/**
 * @brief Mark Moonraker path as disconnected and keep detector in safe idle.
 */
void moonraker_set_disconnected();

/**
 * @brief Get a snapshot of Moonraker transport diagnostics.
 */
void moonraker_get_diag(MoonrakerDiag *out_diag);

/** @brief Copy the last 8 WS events as newline-separated text. buf >= 700 bytes. */
void moonraker_copy_log(char *out, size_t out_len);

/**
 * @brief Send a raw GCODE script to Moonraker via the active WebSocket.
 *
 * Sends a printer.gcode.script JSON-RPC call.  No-op if the WebSocket is
 * not connected or Klippy is not ready.
 *
 * @param script  GCODE string; multiple commands separated by '\n'.
 */
void moonraker_send_gcode(const char *script);
