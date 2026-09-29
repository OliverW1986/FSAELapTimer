// Gate node firmware (ESP32-C3).
//
// Responsibilities (see FSAE-LapTimer-Plan.md section 2/5):
//  - Watch the IR break-beam receiver via a hardware interrupt and capture a
//    timestamp the instant the beam breaks (before any radio/software
//    latency is added).
//  - Debounce re-triggers (beam flutter) before ever transmitting.
//  - Maintain a running offset to the master's clock via periodic sync
//    beacons, so timestamps sent to the master are already corrected onto a
//    shared time base.
//  - Send a GateEventPacket for every accepted crossing, and a periodic
//    HeartbeatPacket otherwise, so the master's dashboard can show this gate
//    as online with a live battery reading.
//
// The gate does NOT compute lap or sector times - it only reports tagged,
// timestamped events. All timing math lives on the master.

#include <Arduino.h>
#include <RF24.h>
#include <SPI.h>

#include "Config.h"
#include "LapTimerProtocol.h"

using LapTimer::GateEventPacket;
using LapTimer::HeartbeatPacket;
using LapTimer::PacketType;
using LapTimer::RadioPacket;
using LapTimer::SyncBeaconPacket;

namespace {

RF24 radio(PIN_RADIO_CE, PIN_RADIO_CSN);

// --- Beam-break interrupt state ------------------------------------------
volatile bool g_beamEventPending = false;
volatile uint64_t g_beamEventTimeUs = 0;

// --- Clock sync state ------------------------------------------------------
// Offset such that (local micros64() + g_syncOffsetUs) approximates the
// master's clock. Updated every time a SyncBeacon is received.
int64_t g_syncOffsetUs = 0;
bool g_syncEverReceived = false;

// --- Debounce / sequencing --------------------------------------------------
uint64_t g_lastAcceptedLocalUs = 0;
uint32_t g_seqNo = 0;

uint32_t g_lastHeartbeatMs = 0;

// micros() overflows (wraps) after ~71 minutes on a 32-bit counter. Track a
// 64-bit local clock so long test days don't produce a wraparound glitch in
// debounce or sync-offset math.
uint64_t micros64() {
    static uint32_t lastMicros = 0;
    static uint64_t overflowCount = 0;
    uint32_t now = micros();
    if (now < lastMicros) {
        overflowCount += 1ULL << 32;
    }
    lastMicros = now;
    return overflowCount + now;
}

void IRAM_ATTR onBeamInterrupt() {
    // Kept minimal: just latch the timestamp and a flag. All heavier work
    // (debounce decision, radio TX) happens in loop().
    g_beamEventTimeUs = micros64();
    g_beamEventPending = true;
}

uint16_t readBatteryMillivolts() {
    // ESP32-C3 ADC default reference is ~3.3V over a 12-bit (0-4095) range;
    // analogReadMilliVolts() applies the SoC's calibrated attenuation curve.
    uint32_t adcMv = analogReadMilliVolts(PIN_BATTERY_ADC);
    return static_cast<uint16_t>(adcMv * BATTERY_DIVIDER_RATIO);
}

void setupRadio() {
    if (!radio.begin(&SPI)) {
        Serial.println(F("[gate] ERROR: NRF24L01 not responding - check wiring"));
    }
    radio.setChannel(LapTimer::RADIO_CHANNEL);
    radio.setDataRate(RF24_250KBPS);
    radio.setPALevel(RF24_PA_HIGH);
    radio.setRetries(LapTimer::RADIO_RETRY_DELAY_MULTIPLIER, LapTimer::RADIO_RETRY_COUNT);
    radio.enableDynamicAck();
    radio.setAutoAck(true);
    radio.setCRCLength(RF24_CRC_16);

    // Pipe 1: this gate's dedicated event pipe (also used for TX to master).
    // Pipe 2: shared broadcast pipe for the master's sync beacons.
    const uint8_t *ownAddress = LapTimer::gatePipeAddress(GATE_ID);
    radio.openWritingPipe(ownAddress);
    radio.openReadingPipe(1, LapTimer::BROADCAST_PIPE_ADDRESS);
    radio.startListening();
}

// Sends one radio packet: leaves listening mode briefly, transmits, then
// resumes listening for sync beacons. This is the standard RF24 pattern for
// a node that is normally an RX "gateway" but occasionally needs to TX.
bool sendPacket(const RadioPacket &pkt) {
    radio.stopListening();
    bool ok = radio.write(&pkt, sizeof(pkt));
    radio.startListening();
    return ok;
}

void sendEvent(uint64_t correctedTimestampUs) {
    RadioPacket pkt;
    pkt.event.type = PacketType::Event;
    pkt.event.gateId = GATE_ID;
    pkt.event.seqNo = ++g_seqNo;
    pkt.event.timestampUs = correctedTimestampUs;
    pkt.event.batteryMillivolts = readBatteryMillivolts();

    bool ok = sendPacket(pkt);
    Serial.printf("[gate] event seq=%lu t=%llu ok=%d\n",
                  static_cast<unsigned long>(pkt.event.seqNo),
                  static_cast<unsigned long long>(correctedTimestampUs), ok);
}

void sendHeartbeat() {
    RadioPacket pkt;
    pkt.heartbeat.type = PacketType::Heartbeat;
    pkt.heartbeat.gateId = GATE_ID;
    pkt.heartbeat.batteryMillivolts = readBatteryMillivolts();
    pkt.heartbeat.uptimeSec = static_cast<uint32_t>(millis() / 1000);
    sendPacket(pkt);
}

void handleIncomingRadio() {
    while (radio.available()) {
        RadioPacket pkt;
        radio.read(&pkt, sizeof(pkt));
        if (pkt.type == PacketType::SyncBeacon) {
            uint64_t localNow = micros64();
            g_syncOffsetUs = static_cast<int64_t>(pkt.syncBeacon.masterTimeUs) -
                             static_cast<int64_t>(localNow);
            g_syncEverReceived = true;
        }
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("[gate] starting, GATE_ID=%d\n", GATE_ID);

    static_assert(GATE_ID >= 1 && GATE_ID <= LapTimer::MAX_GATES,
                  "GATE_ID must be between 1 and LapTimer::MAX_GATES");

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    pinMode(PIN_IR_RECEIVER, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_IR_RECEIVER), onBeamInterrupt,
                     GATE_TRIGGER_ON_RISING ? RISING : FALLING);

    analogReadResolution(12);

    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_RADIO_CSN);
    setupRadio();

    Serial.println(F("[gate] ready"));
}

void loop() {
    handleIncomingRadio();

    if (g_beamEventPending) {
        noInterrupts();
        uint64_t capturedUs = g_beamEventTimeUs;
        g_beamEventPending = false;
        interrupts();

        uint64_t debounceUs = static_cast<uint64_t>(LapTimer::RETRIGGER_DEBOUNCE_MS) * 1000ULL;
        bool isFirstEvent = (g_lastAcceptedLocalUs == 0);
        bool pastDebounceWindow = (capturedUs - g_lastAcceptedLocalUs) >= debounceUs;

        if (isFirstEvent || pastDebounceWindow) {
            g_lastAcceptedLocalUs = capturedUs;

            uint64_t correctedUs = g_syncEverReceived
                                        ? static_cast<uint64_t>(static_cast<int64_t>(capturedUs) + g_syncOffsetUs)
                                        : capturedUs;

            // Brief visual confirmation independent of any web dashboard.
            digitalWrite(PIN_STATUS_LED, HIGH);
            sendEvent(correctedUs);
            digitalWrite(PIN_STATUS_LED, LOW);
        }
    }

    uint32_t nowMs = millis();
    if (nowMs - g_lastHeartbeatMs >= LapTimer::HEARTBEAT_INTERVAL_MS) {
        g_lastHeartbeatMs = nowMs;
        sendHeartbeat();
    }
}
