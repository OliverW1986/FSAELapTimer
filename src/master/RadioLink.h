#pragma once

// Wraps the NRF24L01 radio and runs it on a dedicated FreeRTOS task pinned to
// core 0, so servicing the radio (and sending the periodic sync beacon)
// never gets delayed by WiFi/web-server work on core 1 (see
// FSAE-LapTimer-Plan.md section 3.2).

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "LapTimerProtocol.h"

namespace RadioLink {

// Initializes SPI + the NRF24L01 and the inter-task queue. Call once from
// setup() before startTask(). Returns false if the radio did not respond.
bool begin();

// Starts the radio task (pinned to Config::RADIO_TASK_CORE).
void startTask();

// Queue of decoded LapTimer::RadioPacket structs (fixed-size, POD) placed by
// the radio task as they arrive. Drain this frequently from the main loop.
extern QueueHandle_t incomingQueue;

} // namespace RadioLink
