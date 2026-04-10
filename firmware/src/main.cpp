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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>   // esp32 low level WiFi control
// Bluetooth is never started in this application.  When DISABLE_BT is defined
// (set in platformio.ini) the BT header and all BT symbol references are
// compiled out so the linker can garbage-collect the BT library objects,
// saving ~100–200 KB of flash.  The btStop()/esp_bt_controller_disable() calls
// below are no-ops at runtime (BT was never enabled) but would otherwise keep
// every BT library object alive in the binary.
#ifndef DISABLE_BT
#include <esp_bt.h>
#endif

#include "config.h"
#include "debug_log.h"
#include "types.h"
#include "encoder.h"
#include "fault_detector.h"
#include "nvs_config.h"
#include "web_handler.h"
#include "display_handler.h"
#include "wifi_handler.h"
#include "moonraker.h"
#include "ota_runtime.h"
#include "dht_sensor.h"

// ─── Global shared state ──────────────────────────────────────────────────────
static SensorConfig    g_config{};
static SensorStatus    g_status{};

static QueueHandle_t   g_encoder_queue = nullptr;
static SemaphoreHandle_t g_status_mutex = nullptr;
static SemaphoreHandle_t g_config_mutex = nullptr;

// RTC_DATA_ATTR persists across deep-sleep wake cycles but is reset to its
// initialiser on a true power-on (cold boot / power loss).  We use this flag
// as a one-shot first-boot sentinel: the very first time the ESP32 is powered
// on, the flag is true and we perform a deliberate 1-second deep sleep before
// doing anything else.  On wake from that sleep, firstrun is false (RTC
// memory is intact) so normal initialisation proceeds.
//
// Rationale: the ESP32 RF subsystem can be left in an indeterminate state
// immediately after a cold power-on.  A short deep sleep and wake cycle fully
// resets the WiFi/BT hardware and ensures the radio stack comes up cleanly on
// the subsequent boot, preventing the device from being unreachable after a
// power cycle.  This adds only ~1 second to the very first boot and does not
// affect any subsequent reboots or wake cycles.
RTC_DATA_ATTR bool        firstrun = true;


void goto_sleep(uint16_t seconds)
{
  // Strange but the ESP will fail to reconnect on the next wake if WiFi is not 
  // explicitely turned off.
  // further, this will save additional energy.
  DBG_PRINTLN("Turning off WiFi and Bluetooth before sleeping...");
  disconnect_WiFi(true);

  // we also explicitly turn off Bluetooth before sleep when it was used.
  // With DISABLE_BT defined (default build), BT was never started so these
  // calls are omitted entirely – both to avoid the ESP_ERR_INVALID_STATE
  // return they would produce and to allow the linker to GC the BT libraries.
#ifndef DISABLE_BT
  DBG_PRINTLN("Turning off Bluetooth...");
  btStop();

  // and also low level ESP framework functions to turn off BT finally
  DBG_PRINTLN("Disabling Bluetooth controller...");
  esp_bt_controller_disable();
#endif

  // Guard against 32-bit integer overflow in the microsecond conversion.
  // The product 1000000 × seconds must fit inside a uint32_t
  // (UINT32_MAX = 4 294 967 295), so the safe upper bound is
  // UINT32_MAX / 1 000 000 = 4 294 seconds (~71 minutes).
  // Values above that are clamped rather than using a 64-bit literal.
  static constexpr uint32_t MAX_SLEEP_SECONDS = 4294UL;
  if (seconds > MAX_SLEEP_SECONDS) {
    DBG_PRINTF("[WARN] goto_sleep: clamping %u s to %u s to avoid overflow\n",
                  (unsigned)seconds, (unsigned)MAX_SLEEP_SECONDS);
    seconds = MAX_SLEEP_SECONDS;
  }
  const uint32_t sleep_us = (uint32_t)seconds * 1000000UL;
  esp_sleep_enable_timer_wakeup(sleep_us);

  // Go to sleep now.

  DBG_PRINTLN("Going to sleep for " + String(seconds) + " seconds.\n");
  
  esp_deep_sleep_start();
  // this line will never be reached!
}


// ─── Core 0 Task – Protocol, Fault Detection, Web Server ─────────────────────
static void core0_task(void * /*param*/) {
    DBG_PRINTLN("[C0] Core 0 task started");
    wifi_init(g_status_mutex, g_config_mutex, &g_status, &g_config);
    moonraker_init(g_status_mutex, g_config_mutex, &g_status, &g_config);
    ota_runtime_init(g_config_mutex, &g_config);
#ifdef ENABLE_OLED
    // ── Initialise OLED display (Core 0 only – I²C stays on one core) ─────
    display_init(g_status_mutex, g_config_mutex, &g_status, &g_config);
#endif
#ifdef ENABLE_DHT
    // ── Initialise DHT22 temperature/humidity sensor (Core 0) ─────────────
    dht_sensor_init(g_status_mutex, g_config_mutex, &g_status, &g_config);
#endif

    // ── Main protocol loop ─────────────────────────────────────────────────
#ifdef ENABLE_OLED
    TickType_t last_display_update = xTaskGetTickCount();
#endif

    bool web_started = false;

    while (true) {
        wifi_tick();
        const bool wifi_connected = wifi_is_connected();
        const bool network_ready = wifi_is_network_ready();

        if (network_ready && !web_started) {
            web_init(g_status_mutex, g_config_mutex, &g_status, &g_config);
            web_started = true;
        }

        // Web UI is available over STA or AP captive portal.
        if (network_ready) {
            web_handle_client();
        }

        // Moonraker and OTA are serviced only while station mode is connected.
        if (wifi_connected) {
            ota_runtime_tick(true);
            moonraker_tick();
        } else {
            moonraker_set_disconnected();
        }

#ifdef ENABLE_DHT
        // DHT22 tick: reads at most every DHT_READ_INTERVAL_MS (non-blocking
        // except for the ~20 ms single-shot sensor read itself).
        dht_sensor_tick();
#endif

#ifdef ENABLE_OLED
        if ((xTaskGetTickCount() - last_display_update) >=
            pdMS_TO_TICKS(OLED_UPDATE_MS)) {
            last_display_update = xTaskGetTickCount();
            display_update();
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(CORE0_LOOP_MS));
    }
}

// ─── Core 1 Task – Encoder real-time processing ───────────────────────────────
static void core1_task(void * /*param*/) {
    DBG_PRINTLN("[C1] Core 1 task started");

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
#if DEBUG_LOG_ENABLED
    Serial.begin(SERIAL_BAUD);
    delay(200); // Let serial settle
#endif
    DBG_PRINTLN("\n[MAIN] ESP32 Filament Runout Sensor booting…");

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

    // Initialise fault detector (configures runout GPIO); wire GCODE sender
    fault_detector_init(g_status_mutex, g_encoder_queue, &g_config, &g_status,
                        moonraker_send_gcode);

    // ── First-boot WiFi-radio safeguard ───────────────────────────────────────
    // On a cold power-on, firstrun is true (RTC_DATA_ATTR is reset when power
    // is removed).  We immediately perform a 1-second deep sleep so that the
    // ESP32 RF hardware is fully reset and reinitialised on the subsequent wake.
    // This prevents the WiFi stack from being stale or unresponsive after a
    // power cycle.  On wake, firstrun remains false (RTC memory survives deep
    // sleep), so normal task initialisation proceeds without any further delay.
    if(firstrun) {
        DBG_PRINTLN("[C0] First run detected - Going to sleep for a second");
        firstrun = false;
        goto_sleep(1);
    }
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

    DBG_PRINTLN("[MAIN] Tasks launched – suspending Arduino loop task");
}

void loop() {
    // Arduino loop() runs on Core 1 at default priority.
    // All real work is done in the pinned FreeRTOS tasks above.
    vTaskDelay(portMAX_DELAY);
}
