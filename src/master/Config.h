#pragma once

// Master node hardware/network configuration (classic dual-core ESP32,
// e.g. an ESP32-WROOM-32 dev board).

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// --- NRF24L01 (SPI, default VSPI pins on ESP32-WROOM-32) ------------------
constexpr uint8_t PIN_RADIO_CE = 4;
constexpr uint8_t PIN_RADIO_CSN = 5;
constexpr uint8_t PIN_SPI_SCK = 18;
constexpr uint8_t PIN_SPI_MISO = 19;
constexpr uint8_t PIN_SPI_MOSI = 23;

// --- WiFi Access Point ------------------------------------------------------
constexpr const char *WIFI_AP_SSID = "FSAE-LapTimer";
constexpr const char *WIFI_AP_PASSWORD = "laptimer123"; // >= 8 chars required by WiFi spec
constexpr uint8_t WIFI_AP_CHANNEL = 6; // 2.437 GHz; kept below LapTimer::RADIO_CHANNEL's range
constexpr const char *MDNS_HOSTNAME = "laptimer"; // reachable at http://laptimer.local

// --- Web server ---------------------------------------------------------
constexpr uint16_t HTTP_PORT = 80;

// --- Default lap-timing configuration ------------------------------------
// Ordered gate IDs: index 0 is the start/finish gate, subsequent entries are
// intermediate sector gates in course order. This is also changeable at
// runtime via POST /api/config/gate-order once the dashboard is up.
constexpr uint8_t DEFAULT_GATE_ORDER[] = {1};
constexpr size_t DEFAULT_GATE_ORDER_LEN = sizeof(DEFAULT_GATE_ORDER) / sizeof(DEFAULT_GATE_ORDER[0]);

// FreeRTOS task/queue sizing for the radio task (see RadioLink.h).
constexpr uint32_t RADIO_TASK_STACK_SIZE = 4096;
constexpr UBaseType_t RADIO_TASK_PRIORITY = 2;
constexpr BaseType_t RADIO_TASK_CORE = 0; // radio pinned to core 0, WiFi/web on core 1
constexpr size_t RADIO_QUEUE_LENGTH = 32;
