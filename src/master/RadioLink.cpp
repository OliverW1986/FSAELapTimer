#include "RadioLink.h"

#include <RF24.h>
#include <SPI.h>

#include "Config.h"

namespace RadioLink {

QueueHandle_t incomingQueue = nullptr;

namespace {

RF24 radio(PIN_RADIO_CE, PIN_RADIO_CSN);
TaskHandle_t taskHandle = nullptr;

// 64-bit wraparound-safe local clock, mirrors the technique used in the gate
// firmware so the master's SyncBeacon timestamps never glitch across a
// micros() rollover (~71 min on a 32-bit counter).
uint64_t masterMicros64() {
    static uint32_t lastMicros = 0;
    static uint64_t overflowCount = 0;
    uint32_t now = micros();
    if (now < lastMicros) {
        overflowCount += 1ULL << 32;
    }
    lastMicros = now;
    return overflowCount + now;
}

void sendSyncBeacon() {
    LapTimer::RadioPacket pkt;
    pkt.syncBeacon.type = LapTimer::PacketType::SyncBeacon;
    pkt.syncBeacon.masterTimeUs = masterMicros64();

    // Broadcast: multiple gates share this pipe address, so no ack is
    // expected (auto-ack would only ever succeed for one listener).
    radio.stopListening();
    radio.openWritingPipe(LapTimer::BROADCAST_PIPE_ADDRESS);
    radio.write(&pkt, sizeof(pkt), true /* multicast: skip waiting on an ack */);
    radio.startListening();
}

void radioTask(void *) {
    uint32_t lastBeaconMs = 0;
    for (;;) {
        while (radio.available()) {
            LapTimer::RadioPacket pkt;
            radio.read(&pkt, sizeof(pkt));
            xQueueSend(incomingQueue, &pkt, 0);
        }

        uint32_t nowMs = millis();
        if (nowMs - lastBeaconMs >= LapTimer::SYNC_BEACON_INTERVAL_MS) {
            lastBeaconMs = nowMs;
            sendSyncBeacon();
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

} // namespace

bool begin() {
    incomingQueue = xQueueCreate(RADIO_QUEUE_LENGTH, sizeof(LapTimer::RadioPacket));
    if (!incomingQueue) {
        Serial.println(F("[master] ERROR: failed to create radio queue"));
        return false;
    }

    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_RADIO_CSN);
    if (!radio.begin(&SPI)) {
        Serial.println(F("[master] ERROR: NRF24L01 not responding - check wiring"));
        return false;
    }

    radio.setChannel(LapTimer::RADIO_CHANNEL);
    radio.setDataRate(RF24_250KBPS);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setRetries(LapTimer::RADIO_RETRY_DELAY_MULTIPLIER, LapTimer::RADIO_RETRY_COUNT);
    radio.setAutoAck(true);
    radio.setCRCLength(RF24_CRC_16);

    for (uint8_t gateId = 1; gateId <= LapTimer::MAX_GATES; ++gateId) {
        radio.openReadingPipe(gateId, LapTimer::gatePipeAddress(gateId));
    }
    radio.startListening();
    return true;
}

void startTask() {
    xTaskCreatePinnedToCore(radioTask, "radioTask", RADIO_TASK_STACK_SIZE, nullptr,
                             RADIO_TASK_PRIORITY, &taskHandle, RADIO_TASK_CORE);
}

} // namespace RadioLink
