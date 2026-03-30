/**
 * @file main.cpp
 * @brief ESP32 Intelligent Filament Runout Sensor – main entry point.
 *
 * Architecture
 * ─────────────────────────────────────────────────────────────────────────────
 *  Core 1 (encoder_task)                Core 0 (core0_task)
 *  ─────────────────────────────────    ─────────────────────────────────────
 *  · ISR: quadrature decode (GPIO 25/26)  · WiFi management + auto-reconnect
 *  · Speed calculation at 50 Hz          · Moonraker HTTP polling at 5 Hz
 *  · Push EncoderData → encoder_queue    · Fault detection state machine
 *                                         · Web server (port 80)
 *                                         · NVS config persistence
 *
 * Communication
 * ─────────────────────────────────────────────────────────────────────────────
 *  encoder_queue  – single-slot FreeRTOS queue (Core 1 → Core 0)
 *  g_last_motion_ms – volatile uint32_t updated by ISR (no mutex needed)
 *  g_config       – SensorConfig protected by config_mutex
 *  g_status       – SensorStatus protected by status_mutex
 *
 * MIT License – Copyright (c) 2026 tobi01001
 */

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "config.h"
#include "types.h"
#include "encoder.h"
#include "moonraker_client.h"
#include "fault_detector.h"
#include "nvs_config.h"
#include "web_handler.h"

// ─── Global shared state ──────────────────────────────────────────────────────
static SensorConfig    g_config{};
static SensorStatus    g_status{};

static QueueHandle_t   g_encoder_queue = nullptr;
static SemaphoreHandle_t g_status_mutex = nullptr;
static SemaphoreHandle_t g_config_mutex = nullptr;

// ─── WiFi helpers (Core 0) ────────────────────────────────────────────────────
static bool wifi_connect(const SensorConfig &cfg) {
    if (cfg.wifi_ssid[0] == '\0') {
        Serial.println("[WiFi] No SSID configured – starting AP mode");
        return false;
    }

    Serial.printf("[WiFi] Connecting to '%s' …\n", cfg.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

    const uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("[WiFi] Connection timeout");
    return false;
}

static void wifi_start_ap() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    Serial.printf("[WiFi] AP mode – SSID: %s  IP: %s\n",
                  WIFI_AP_SSID,
                  WiFi.softAPIP().toString().c_str());
}

// ─── Core 0 Task – Protocol, Fault Detection, Web Server ─────────────────────
static void core0_task(void * /*param*/) {
    Serial.println("[C0] Core 0 task started");

    // ── WiFi connection with exponential backoff ───────────────────────────
    uint32_t backoff_ms = 1000;
    bool connected = false;

    {
        if (xSemaphoreTake(g_status_mutex, portMAX_DELAY) == pdTRUE) {
            g_status.state = SystemState::WIFI_CONN;
            xSemaphoreGive(g_status_mutex);
        }

        SensorConfig cfg_snap{};
        if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            cfg_snap = g_config;
            xSemaphoreGive(g_config_mutex);
        }

        connected = wifi_connect(cfg_snap);

        if (!connected) {
            if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_status.state = SystemState::WIFI_FAIL;
                xSemaphoreGive(g_status_mutex);
            }
            wifi_start_ap();
            // AP mode still allows web-based configuration
            connected = true; // Web server works over AP too
        }
    }

    // ── Update IP address in status ────────────────────────────────────────
    String ip = (WiFi.status() == WL_CONNECTED)
                    ? WiFi.localIP().toString()
                    : WiFi.softAPIP().toString();
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(g_status.ip_address, ip.c_str(), sizeof(g_status.ip_address) - 1);
        g_status.ip_address[sizeof(g_status.ip_address) - 1] = '\0';
        g_status.wifi_connected = (WiFi.status() == WL_CONNECTED);
        if (g_status.state != SystemState::FAULT &&
            g_status.state != SystemState::WIFI_FAIL) {
            g_status.state = SystemState::READY;
        }
        xSemaphoreGive(g_status_mutex);
    }

    // ── Start web server ───────────────────────────────────────────────────
    web_init(g_status_mutex, g_config_mutex, &g_status, &g_config);

    // ── Main protocol loop ─────────────────────────────────────────────────
    TickType_t last_mr_poll = xTaskGetTickCount();
    TickType_t last_reconnect_check = xTaskGetTickCount();
    backoff_ms = 1000;

    while (true) {
        // Handle HTTP requests (non-blocking when no client pending)
        web_handle_client();

        // Moonraker polling at 5 Hz
        if ((xTaskGetTickCount() - last_mr_poll) >= pdMS_TO_TICKS(MOONRAKER_POLL_MS)) {
            last_mr_poll = xTaskGetTickCount();

            SensorConfig cfg_snap{};
            if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                cfg_snap = g_config;
                xSemaphoreGive(g_config_mutex);
            }

            const float ext_vel = moonraker_poll(cfg_snap);
            fault_detector_update(ext_vel);
        }

        // WiFi reconnect check (station mode only) with exponential backoff
        if ((xTaskGetTickCount() - last_reconnect_check) >=
            pdMS_TO_TICKS(backoff_ms)) {
            last_reconnect_check = xTaskGetTickCount();

            if (WiFi.getMode() == WIFI_STA &&
                WiFi.status() != WL_CONNECTED) {
                Serial.printf("[WiFi] Reconnecting (backoff %lu ms) …\n",
                              backoff_ms);

                SensorConfig cfg_snap{};
                if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    cfg_snap = g_config;
                    xSemaphoreGive(g_config_mutex);
                }

                if (wifi_connect(cfg_snap)) {
                    backoff_ms = 1000; // Reset backoff on success
                    String new_ip = WiFi.localIP().toString();
                    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        strncpy(g_status.ip_address, new_ip.c_str(),
                                sizeof(g_status.ip_address) - 1);
                        g_status.wifi_connected = true;
                        xSemaphoreGive(g_status_mutex);
                    }
                } else {
                    // Exponential backoff capped at WIFI_RECONNECT_MAX_MS
                    backoff_ms = min(backoff_ms * 2, (uint32_t)WIFI_RECONNECT_MAX_MS);
                    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        g_status.wifi_connected = false;
                        xSemaphoreGive(g_status_mutex);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CORE0_LOOP_MS));
    }
}

// ─── Core 1 Task – Encoder real-time processing ───────────────────────────────
static void core1_task(void * /*param*/) {
    Serial.println("[C1] Core 1 task started");

    SensorConfig *cfg_ptr = nullptr;
    if (xSemaphoreTake(g_config_mutex, portMAX_DELAY) == pdTRUE) {
        cfg_ptr = &g_config;
        xSemaphoreGive(g_config_mutex);
    }

    // Initialise encoder ISRs – must run on Core 1 so ISRs are pinned here
    encoder_init(g_encoder_queue, cfg_ptr);

    // Run the 50 Hz encoder / speed calculation loop (never returns)
    encoder_task(nullptr);
}

// ─── Arduino Entry Points ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200); // Let serial settle
    Serial.println("\n[MAIN] ESP32 Filament Runout Sensor booting…");

    // Create synchronisation primitives
    g_status_mutex  = xSemaphoreCreateMutex();
    g_config_mutex  = xSemaphoreCreateMutex();
    g_encoder_queue = xQueueCreate(ENCODER_QUEUE_LEN, sizeof(EncoderData));

    configASSERT(g_status_mutex  != nullptr);
    configASSERT(g_config_mutex  != nullptr);
    configASSERT(g_encoder_queue != nullptr);

    // Initialise status / config structs
    memset(&g_status, 0, sizeof(g_status));
    g_status.state = SystemState::INIT;

    // Load persistent configuration from NVS
    nvs_load(g_config);

    // Initialise fault detector (configures runout GPIO)
    fault_detector_init(g_status_mutex, g_encoder_queue, &g_config, &g_status);

    // ── Launch FreeRTOS tasks ──────────────────────────────────────────────
    // Core 0: protocol, WiFi, Moonraker, fault detection, web server
    xTaskCreatePinnedToCore(
        core0_task,
        "Core0Task",
        CORE0_TASK_STACK,
        nullptr,
        CORE0_TASK_PRIO,
        nullptr,
        0   // Pin to Core 0
    );

    // Core 1: encoder ISR handling, speed calculation
    xTaskCreatePinnedToCore(
        core1_task,
        "Core1Task",
        CORE1_TASK_STACK,
        nullptr,
        CORE1_TASK_PRIO,
        nullptr,
        1   // Pin to Core 1
    );

    Serial.println("[MAIN] Tasks launched – suspending Arduino loop task");
}

void loop() {
    // Arduino loop() runs on Core 1 at default priority.
    // All real work is done in the pinned FreeRTOS tasks above.
    vTaskDelay(portMAX_DELAY);
}
