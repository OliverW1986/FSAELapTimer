#pragma once

// Owns all lap/sector timing logic (see FSAE-LapTimer-Plan.md section 2:
// gates are "dumb" sensors, the master is the single source of truth for
// timing math). Consumes corrected GateEventPacket timestamps and produces
// lap/sector records for the current session.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

#include "LapTimerProtocol.h"

class LapEngine {
public:
    struct LapRecord {
        uint32_t lapNumber;
        uint64_t lapTimeUs;
        std::vector<uint64_t> sectorTimesUs; // one entry per intermediate gate, in course order
        bool isBestLap;
    };

    enum class EventOutcome {
        Ignored,      // event from a gate not in the configured order, or no-op
        SectorSplit,  // an intermediate sector time was recorded
        LapCompleted, // a full lap was recorded (start/finish crossed again)
        LapStarted,   // the very first start/finish crossing of a session (no lap time yet)
    };

    // Sets the ordered list of gate IDs: index 0 = start/finish, the rest are
    // intermediate sectors in course order. Resets the in-progress lap (but
    // not the completed-lap history).
    void configureGateOrder(const std::vector<uint8_t> &order) {
        gateOrder_ = order;
        resetInProgressLap();
    }

    const std::vector<uint8_t> &gateOrder() const { return gateOrder_; }

    // Clears all recorded laps and starts a fresh session.
    void resetSession() {
        laps_.clear();
        bestLapTimeUs_ = UINT64_MAX;
        nextLapNumber_ = 1;
        resetInProgressLap();
    }

    // Feeds one corrected gate-event timestamp into the state machine.
    // On LapCompleted, `outRecord` is filled in with the finished lap.
    EventOutcome handleEvent(uint8_t gateId, uint64_t correctedTimestampUs, LapRecord &outRecord) {
        if (gateOrder_.empty()) {
            return EventOutcome::Ignored;
        }

        uint8_t startFinishGateId = gateOrder_[0];

        if (gateId == startFinishGateId) {
            if (lastStartFinishUs_ == 0) {
                // First crossing of the session/run: arms the timer, no lap yet.
                lastStartFinishUs_ = correctedTimestampUs;
                nextSectorIndex_ = 0;
                currentSectors_.clear();
                return EventOutcome::LapStarted;
            }

            uint64_t lapTimeUs = correctedTimestampUs - lastStartFinishUs_;

            LapRecord record;
            record.lapNumber = nextLapNumber_++;
            record.lapTimeUs = lapTimeUs;
            record.sectorTimesUs = currentSectors_;
            record.isBestLap = lapTimeUs < bestLapTimeUs_;
            if (record.isBestLap) {
                bestLapTimeUs_ = lapTimeUs;
            }
            laps_.push_back(record);
            outRecord = record;

            // This same crossing arms the next lap.
            lastStartFinishUs_ = correctedTimestampUs;
            nextSectorIndex_ = 0;
            currentSectors_.clear();
            return EventOutcome::LapCompleted;
        }

        // Check whether this gate is the next expected intermediate sector.
        if (gateOrder_.size() > 1 && lastStartFinishUs_ != 0) {
            size_t sectorCount = gateOrder_.size() - 1;
            if (nextSectorIndex_ < sectorCount && gateOrder_[nextSectorIndex_ + 1] == gateId) {
                uint64_t previousMarkUs = currentSectors_.empty() ? lastStartFinishUs_ : lastSectorAbsoluteUs_;
                uint64_t sectorTimeUs = correctedTimestampUs - previousMarkUs;
                currentSectors_.push_back(sectorTimeUs);
                lastSectorAbsoluteUs_ = correctedTimestampUs;
                nextSectorIndex_++;
                return EventOutcome::SectorSplit;
            }
        }

        return EventOutcome::Ignored;
    }

    const std::vector<LapRecord> &laps() const { return laps_; }
    uint64_t bestLapTimeUs() const { return bestLapTimeUs_; }

    void toSessionJson(JsonObject &obj) const {
        JsonArray order = obj["gateOrder"].to<JsonArray>();
        for (uint8_t g : gateOrder_) {
            order.add(g);
        }
        obj["bestLapTimeUs"] = (bestLapTimeUs_ == UINT64_MAX) ? 0 : bestLapTimeUs_;

        JsonArray lapsArray = obj["laps"].to<JsonArray>();
        for (const LapRecord &lap : laps_) {
            JsonObject lapObj = lapsArray.add<JsonObject>();
            lapRecordToJson(lap, lapObj);
        }
    }

    static void lapRecordToJson(const LapRecord &lap, JsonObject &lapObj) {
        lapObj["lapNumber"] = lap.lapNumber;
        lapObj["lapTimeUs"] = lap.lapTimeUs;
        lapObj["isBestLap"] = lap.isBestLap;
        JsonArray sectors = lapObj["sectorTimesUs"].to<JsonArray>();
        for (uint64_t s : lap.sectorTimesUs) {
            sectors.add(s);
        }
    }

    // Formats microseconds as "M:SS.mmm" for CSV/log output.
    static String formatLapTime(uint64_t us) {
        uint64_t ms = us / 1000ULL;
        uint32_t minutes = static_cast<uint32_t>(ms / 60000ULL);
        uint32_t seconds = static_cast<uint32_t>((ms / 1000ULL) % 60ULL);
        uint32_t millisPart = static_cast<uint32_t>(ms % 1000ULL);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u:%02u.%03u", minutes, seconds, millisPart);
        return String(buf);
    }

private:
    void resetInProgressLap() {
        lastStartFinishUs_ = 0;
        lastSectorAbsoluteUs_ = 0;
        nextSectorIndex_ = 0;
        currentSectors_.clear();
    }

    std::vector<uint8_t> gateOrder_;
    std::vector<LapRecord> laps_;
    uint64_t bestLapTimeUs_ = UINT64_MAX;
    uint32_t nextLapNumber_ = 1;

    uint64_t lastStartFinishUs_ = 0;
    uint64_t lastSectorAbsoluteUs_ = 0;
    size_t nextSectorIndex_ = 0;
    std::vector<uint64_t> currentSectors_;
};
