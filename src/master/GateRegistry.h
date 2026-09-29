#pragma once

// Tracks the last-known status of every gate (online/offline, battery,
// last-seen time) so the dashboard can show a live connectivity panel.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "LapTimerProtocol.h"

class GateRegistry {
public:
    struct Status {
        bool everSeen = false;
        bool online = false;
        uint32_t lastSeenMs = 0;
        uint16_t batteryMillivolts = 0;
        uint32_t lastEventSeq = 0;
    };

    // Records that a gate was just heard from (event or heartbeat). Returns
    // true if this changed the gate's online/offline state (useful for
    // deciding whether to push a websocket update).
    bool noteSeen(uint8_t gateId, uint16_t batteryMillivolts, bool isEvent, uint32_t seqNo) {
        if (gateId < 1 || gateId > LapTimer::MAX_GATES) {
            return false;
        }
        Status &s = status_[gateId - 1];
        bool wasOnline = s.online;
        s.everSeen = true;
        s.online = true;
        s.lastSeenMs = millis();
        s.batteryMillivolts = batteryMillivolts;
        if (isEvent) {
            s.lastEventSeq = seqNo;
        }
        return !wasOnline;
    }

    // Call periodically (e.g. once a second) from the main loop. Returns
    // true if any gate's online/offline state changed.
    bool refreshOfflineStates() {
        bool changed = false;
        uint32_t now = millis();
        for (uint8_t i = 0; i < LapTimer::MAX_GATES; ++i) {
            Status &s = status_[i];
            if (s.everSeen && s.online && (now - s.lastSeenMs) > LapTimer::GATE_OFFLINE_TIMEOUT_MS) {
                s.online = false;
                changed = true;
            }
        }
        return changed;
    }

    const Status &get(uint8_t gateId) const { return status_[gateId - 1]; }

    void toJson(JsonArray &array) const {
        for (uint8_t i = 0; i < LapTimer::MAX_GATES; ++i) {
            const Status &s = status_[i];
            if (!s.everSeen) {
                continue;
            }
            JsonObject obj = array.add<JsonObject>();
            obj["gateId"] = i + 1;
            obj["online"] = s.online;
            obj["batteryMillivolts"] = s.batteryMillivolts;
            obj["lastSeenMsAgo"] = millis() - s.lastSeenMs;
        }
    }

private:
    Status status_[LapTimer::MAX_GATES];
};
