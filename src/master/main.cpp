// Master node firmware (dual-core ESP32).
//
// Ties together:
//  - RadioLink: NRF24L01 servicing on a task pinned to core 0 (events,
//    heartbeats in; periodic sync beacons out).
//  - LapEngine: the single source of truth for lap/sector timing math.
//  - GateRegistry: online/offline + battery tracking per gate.
//  - WebServerModule: WiFi AP + REST API + WebSocket live push + dashboard
//    static assets, running on core 1 via the normal Arduino loop().

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "Config.h"
#include "GateRegistry.h"
#include "LapEngine.h"
#include "LapTimerProtocol.h"
#include "RadioLink.h"
#include "WebServerModule.h"

namespace {

LapEngine g_engine;
GateRegistry g_gates;

uint32_t g_lastOfflineCheckMs = 0;

void broadcastEventOutcome(uint8_t gateId, LapEngine::EventOutcome outcome,
                            const LapEngine::LapRecord &record) {
    JsonDocument doc;
    switch (outcome) {
        case LapEngine::EventOutcome::LapCompleted: {
            doc["type"] = "lap";
            JsonObject lapObj = doc["lap"].to<JsonObject>();
            LapEngine::lapRecordToJson(record, lapObj);
            break;
        }
        case LapEngine::EventOutcome::SectorSplit:
            doc["type"] = "sector";
            doc["gateId"] = gateId;
            break;
        case LapEngine::EventOutcome::LapStarted:
            doc["type"] = "lapStarted";
            doc["gateId"] = gateId;
            break;
        case LapEngine::EventOutcome::Ignored:
        default:
            return; // nothing worth pushing to clients
    }
    String out;
    serializeJson(doc, out);
    WebServerModule::broadcastJson(out);
}

void broadcastGateStatus() {
    JsonDocument doc;
    doc["type"] = "gateStatus";
    JsonArray arr = doc["gates"].to<JsonArray>();
    g_gates.toJson(arr);
    String out;
    serializeJson(doc, out);
    WebServerModule::broadcastJson(out);
}

void handleIncomingPacket(const LapTimer::RadioPacket &pkt) {
    switch (pkt.type) {
        case LapTimer::PacketType::Event: {
            const auto &ev = pkt.event;
            bool statusChanged = g_gates.noteSeen(ev.gateId, ev.batteryMillivolts, true, ev.seqNo);

            LapEngine::LapRecord record{};
            LapEngine::EventOutcome outcome = g_engine.handleEvent(ev.gateId, ev.timestampUs, record);
            broadcastEventOutcome(ev.gateId, outcome, record);

            if (statusChanged) {
                broadcastGateStatus();
            }
            break;
        }
        case LapTimer::PacketType::Heartbeat: {
            const auto &hb = pkt.heartbeat;
            bool statusChanged = g_gates.noteSeen(hb.gateId, hb.batteryMillivolts, false, 0);
            if (statusChanged) {
                broadcastGateStatus();
            }
            break;
        }
        case LapTimer::PacketType::SyncBeacon:
        default:
            // The master originates sync beacons; it should never receive one.
            break;
    }
}

void setupWiFiAccessPoint() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL);
    Serial.printf("[master] AP \"%s\" started, IP: %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        Serial.printf("[master] mDNS: http://%s.local\n", MDNS_HOSTNAME);
    } else {
        Serial.println(F("[master] WARNING: mDNS init failed"));
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("[master] starting"));

    std::vector<uint8_t> defaultOrder(DEFAULT_GATE_ORDER, DEFAULT_GATE_ORDER + DEFAULT_GATE_ORDER_LEN);
    g_engine.configureGateOrder(defaultOrder);

    setupWiFiAccessPoint();
    WebServerModule::begin(g_engine, g_gates);

    if (!RadioLink::begin()) {
        Serial.println(F("[master] radio init failed - gate events will not be received"));
    } else {
        RadioLink::startTask();
    }

    Serial.println(F("[master] ready"));
}

void loop() {
    LapTimer::RadioPacket pkt;
    while (xQueueReceive(RadioLink::incomingQueue, &pkt, 0) == pdTRUE) {
        handleIncomingPacket(pkt);
    }

    uint32_t nowMs = millis();
    if (nowMs - g_lastOfflineCheckMs >= 1000) {
        g_lastOfflineCheckMs = nowMs;
        if (g_gates.refreshOfflineStates()) {
            broadcastGateStatus();
        }
    }

    WebServerModule::loopCleanup();
}
