#pragma once

// Shared wire protocol between gate nodes and the master node.
//
// Both the gate firmware (src/gate) and master firmware (src/master) include
// this header so the packet layout can never drift between the two ends -
// they are compiled from the exact same struct definitions.

#include <cstdint>
#include <cstring>

namespace LapTimer {

// Maximum number of gates supported. The NRF24L01 has 6 RX pipes; pipe 0 is
// reserved on every node for receiving auto-ack replies while transmitting,
// so pipes 1-5 are available for dedicated gate->master event pipes.
constexpr uint8_t MAX_GATES = 5;

// Reserved gate id meaning "not configured" / invalid.
constexpr uint8_t INVALID_GATE_ID = 0xFF;

// NRF24 RF channel (0-125, each unit is 1 MHz above 2.400 GHz). Channel 90
// (2.490 GHz) sits above the standard WiFi 2.4 GHz band (channels 1-13 top
// out around 2.472 GHz), reducing self-interference with the master's own
// WiFi access point.
constexpr uint8_t RADIO_CHANNEL = 90;

// NRF24 air data rate. RF24_250KBPS gives the longest range, which matters
// more for track-side gates than raw throughput (payloads are tiny).
constexpr uint8_t RADIO_RETRY_DELAY_MULTIPLIER = 15; // ~1.5ms retry delay, RF24 units of 250us
constexpr uint8_t RADIO_RETRY_COUNT = 15;            // max auto-retries per packet

// Gate-side minimum time between accepted beam-break events, used to filter
// beam flutter (e.g. a wheel or mudflap re-triggering the same crossing).
constexpr uint32_t RETRIGGER_DEBOUNCE_MS = 700;

// How often the master broadcasts a time-sync beacon to all gates.
constexpr uint32_t SYNC_BEACON_INTERVAL_MS = 3000;

// How often a gate sends a heartbeat when idle (no trigger) so the master
// dashboard can show it as online with a fresh battery reading.
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 2000;

// If the master hasn't heard from a gate (event or heartbeat) within this
// window, the dashboard marks it offline.
constexpr uint32_t GATE_OFFLINE_TIMEOUT_MS = 8000;

// Per-gate NRF24 RX pipe addresses used by the master, and matching TX
// addresses used by each gate. Index == (gateId - 1), gateId is 1..MAX_GATES.
// `static` (rather than C++17 `inline`) gives each translation unit its own
// copy with internal linkage, since the project targets C++11/14 by default.
static const uint8_t GATE_PIPE_ADDRESSES[MAX_GATES][5] = {
    {'G', 'a', 't', 'e', '1'},
    {'G', 'a', 't', 'e', '2'},
    {'G', 'a', 't', 'e', '3'},
    {'G', 'a', 't', 'e', '4'},
    {'G', 'a', 't', 'e', '5'},
};

// Shared broadcast address the master transmits sync beacons on, and that
// every gate listens to (in addition to its own dedicated pipe).
static const uint8_t BROADCAST_PIPE_ADDRESS[5] = {'B', 'c', 'a', 's', 't'};

enum class PacketType : uint8_t {
    Event = 1,
    SyncBeacon = 2,
    Heartbeat = 3,
};

// Sent by a gate the instant its IR beam-break interrupt fires.
struct __attribute__((packed)) GateEventPacket {
    PacketType type;        // always PacketType::Event
    uint8_t gateId;         // 1..MAX_GATES
    uint32_t seqNo;         // per-gate monotonically increasing sequence number
    uint64_t timestampUs;   // gate-local micros(), corrected by the gate's current sync offset
    uint16_t batteryMillivolts;
};

// Sent by the master periodically, broadcast to all gates, so each gate can
// compute (and keep updated) its offset from the master's clock.
struct __attribute__((packed)) SyncBeaconPacket {
    PacketType type;        // always PacketType::SyncBeacon
    uint64_t masterTimeUs;
};

// Sent by a gate on an idle timer so the master knows it is alive even when
// there is no crossing to report.
struct __attribute__((packed)) HeartbeatPacket {
    PacketType type;        // always PacketType::Heartbeat
    uint8_t gateId;
    uint16_t batteryMillivolts;
    uint32_t uptimeSec;
};

// Fixed-size radio frame used for every NRF24 transmission. RF24 static
// payloads are simplest when every packet on the wire is the same size, so
// all three packet types are carried inside one union sized to fit the
// 32-byte NRF24 payload limit.
union RadioPacket {
    PacketType type;
    GateEventPacket event;
    SyncBeaconPacket syncBeacon;
    HeartbeatPacket heartbeat;
    uint8_t raw[32];

    RadioPacket() { std::memset(raw, 0, sizeof(raw)); }
};

static_assert(sizeof(RadioPacket) <= 32,
              "RadioPacket exceeds the 32-byte NRF24 payload limit");

// Returns the RX pipe address for a given 1-based gate id, or nullptr if the
// id is out of range.
inline const uint8_t *gatePipeAddress(uint8_t gateId) {
    if (gateId < 1 || gateId > MAX_GATES) {
        return nullptr;
    }
    return GATE_PIPE_ADDRESSES[gateId - 1];
}

} // namespace LapTimer
