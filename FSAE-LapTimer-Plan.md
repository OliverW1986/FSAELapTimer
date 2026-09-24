# FSAE Lap Timer — System Design Plan

## 1. Overview

A distributed lap-timing system for track testing/dynamic events:

- **Gate nodes** (1..N): standalone battery-powered units, each with an IR break-beam sensor, detect a vehicle crossing a line and report the event wirelessly.
- **Master node**: 1 ESP32, receives gate events over NRF24L01, computes lap/sector times, hosts a WiFi Access Point + web server so laptops/phones can view live timing data.
- **Clients**: any WiFi device (phone, laptop, pit-wall tablet) connects to the master's AP and views a live dashboard in a browser — no app install needed.

### Key assumptions (flag if wrong)
- One vehicle on course at a time (standard for FSAE autocross/skidpad/accel/endurance stints) — gates don't need to *identify which car*, only *that a crossing happened*. If multiple simultaneous cars must be distinguished, a transponder/RFID-per-car scheme would need to be added later (noted as a future extension).
- Track-side gates are outdoors, spread out (tens to a few hundred meters from the master), and need to run untethered on battery for a full test day.
- Minimum useful config is **1 gate** (start/finish only, lap time = time between consecutive crossings). The design scales to additional gates for sector splits (e.g., before/after skidpad, autocross sector splits) without redesign.
- Your tagged project (`platformio.ini`, board `esp32-c3-devkitc-02`) is a good fit for **gate nodes** (cheap, small, low power). The **master** should be a classic dual-core ESP32 (WROOM32) — reasoning below.

---

## 2. Where should lap times be calculated? → **On the Master**

This is the central design decision, and the answer is: **gates timestamp locally, master computes and owns all lap/sector logic.** Rationale:

| Option | Problem |
|---|---|
| Gate computes full lap time itself | Only works if there is exactly one gate (start=finish). Can't do sector splits since a gate has no visibility into other gates. |
| Master timestamps using its own RX-arrival time | Simplest, no clock sync needed, but adds NRF24 latency/jitter (roughly 1–5 ms, variable) directly into the lap time. Usually acceptable for sector splits, borderline for a "true" finish-line time. |
| Gate timestamps at the hardware interrupt (best), master does all math | Best accuracy: the timestamp is captured at the moment of the IR edge, before any radio/software latency is added. Requires the gates and master to share a common time base (see §4). |

**Decision:** Each gate captures a hardware-interrupt timestamp (`esp_timer_get_time()`/`micros()`) the instant the beam breaks, tags it with `gate_id` + monotonic sequence number, and transmits it over NRF24 as soon as possible. The **master is the single source of truth**: it maintains gate topology/order, corrects each incoming timestamp to a shared time base, and is the only place that computes lap time, sector time, best lap, deltas, etc. Gates are "dumb" sensors — this keeps gate firmware small/reliable and means all timing logic/config changes happen in one place (no re-flashing gates when you change the timing rules).

---

## 3. Hardware

### 3.1 Gate node (×N)
- **MCU:** ESP32-C3 (matches your existing `platformio.ini`) — single core is fine here since a gate only does: watch one GPIO interrupt, run a tiny NRF24 stack, sleep. No web server, no heavy concurrency needed.
- **IR sensor — recommend a true break-beam pair, not a short-range obstacle sensor:**
  - Emitter: 940 nm IR LED driven with a 38 kHz carrier (via MCU timer/PWM or a 555/dedicated driver).
  - Receiver: TSOP38238 (or similar) 38 kHz-tuned IR demodulator receiver module, mounted across the track from the emitter.
  - Modulating at 38 kHz + using a tuned receiver rejects ambient sunlight, which is the #1 failure mode of naive IR gates outdoors. This gives working range of several meters, enough to span most FSAE track widths/lanes.
  - Cheap short-range "IR obstacle avoidance" boards (the ones with onboard emitter+receiver in one housing, reflective) only work over a few cm–30 cm and are not suitable spanning a track — avoid these unless gate width is very narrow (e.g., a chute).
  - Wire the receiver output to a GPIO configured for a **falling/rising edge hardware interrupt** (not polling) to minimize timestamp jitter.
- **Radio:** NRF24L01+**PA+LNA** (external antenna version), not the bare NRF24L01. Track-side distances of 50–300 m need the extra link budget; bare modules are typically good for well under 50 m outdoors with any obstruction.
  - Configure at **250 kbps** air data rate (longest range mode) and a fixed channel chosen to sit away from the master's WiFi channel (2.4 GHz WiFi and NRF24 share spectrum — see §6.4).
- **Power:** 18650 Li-ion cell + TP4056 charge/protect module + 3.3 V buck/LDO (or an ESP32 dev board with onboard battery charging, e.g. an "ESP32 LiPo" variant), sized for a full test day. Use light sleep between triggers with the IR receiver pin as an RTC/GPIO wakeup source to conserve power; wake, timestamp, transmit, sleep again.
- **Enclosure:** weatherproof (IP54+) box, tripod/stake mount, emitter and receiver as a matched pair per gate (2 physical posts, 1 "brain" — put the ESP32+radio+battery in the receiver post since that's the side with the interrupt-driven event).
- **Status LED:** simple on-board LED for "powered/linked/low battery" visual feedback without needing the web UI.

### 3.2 Master node (×1)
- **MCU:** classic dual-core ESP32 (e.g., ESP32-WROOM-32), *not* the C3. Reasoning: it must simultaneously run a WiFi AP + async web server + WebSocket push + SPI polling of the NRF24 + timekeeping, all with tight timing tolerances. The second core lets you pin the radio/timestamp-critical work to Core 0 and the WiFi/web stack to Core 1 (Arduino/FreeRTOS `xTaskCreatePinnedToCore`), avoiding WiFi-stack jitter delaying radio reception.
- **Radio:** NRF24L01+PA+LNA as well, so it has adequate range to all gates. If gates are very spread out and one radio can't reach them all reliably, add a "repeater" gate node that just forwards packets — not needed for a typical single-track layout.
- **Storage:** onboard flash via LittleFS for persisting session data/config across reboots; SD card is an optional future add-on if long multi-day logging is wanted.
- **Power:** USB power bank or vehicle/pit 12 V-to-5 V supply; this node lives at the pit wall/timing table, so battery constraints are looser than gates.

---

## 4. Time synchronization across gates

Needed only once you have more than one gate (sector splits). Approach:

1. Master periodically **broadcasts a sync beacon** over NRF24 (e.g., every 2–5 s) containing its own `micros()`/epoch counter.
2. Each gate, on receiving a beacon, computes and stores an **offset** = `master_time - local_time` (filtered/averaged across a few beacons to smooth radio jitter).
3. When a gate timestamps a beam-break event locally, it applies its current offset before sending the event, OR sends the raw local timestamp + the offset it used, so the master can reconstruct a corrected time.
4. Master uses these corrected timestamps to compute deltas between different gates (sector times) and between two events on the same gate (lap time on a single-gate finish line, which incidentally needs *no* sync at all since it's the same clock).

This keeps single-gate (start/finish only) setups perfectly simple (zero sync error, since it's one clock measuring its own deltas), while still enabling multi-gate sector splits with typical sub-few-ms correction error — plenty for FSAE dynamic event purposes (not FIA-grade timing, but well beyond stopwatch/human-reaction-time accuracy).

---

## 5. Communication protocol (NRF24L01)

- **Topology:** all gates as NRF24 "transmitters," master as the single "receiver" listening on multiple pipes (NRF24 supports up to 6 RX pipes on one radio) — one pipe per gate (or a shared pipe with `gate_id` in the payload if you expect >5 gates).
- **Packet types** (small fixed-size structs, defined once in a **shared library** used by both gate and master firmware):
  - `EVENT` — `{gate_id, seq_no, timestamp, battery_mv}` sent on beam break.
  - `SYNC_BEACON` — master → gates, `{master_time}`.
  - `HEARTBEAT` — gate → master periodically even without a trigger, so the dashboard can show "gate online/offline" and battery level.
- **Reliability:** enable NRF24 auto-ack + auto-retry; use `seq_no` per gate so the master can detect/report dropped or duplicate packets. Each gate keeps a small local ring buffer of unacknowledged events and retries so a transient link hiccup near the finish line doesn't lose a lap.
- **Debounce:** apply a minimum-lap-time / minimum-retrigger-interval (e.g., ignore a second trigger on the same gate within ~0.5–1 s) in the *gate* firmware to filter beam flutter (e.g., from a car's wheel/mudflap or wind-blown debris breaking the beam twice) before it ever reaches the master.

---

## 6. Master: WiFi + Web Application

- **WiFi mode:** ESP32 as a WiFi **Access Point** ("FSAE-LapTimer" SSID), so pit crew connect directly with no external router dependency. (Optionally support AP+STA if you also want internet access for OTA updates back at the shop, but AP-only is simpler and more robust at the track.)
- **Web server:** `ESPAsyncWebServer` + `AsyncTCP` libraries — async so the web stack doesn't block radio servicing.
- **Live updates:** WebSocket channel pushes new lap/split events to connected browsers instantly (no polling needed); regular HTTP endpoints (or a REST-ish JSON API) for session history, CSV export, and configuration.
- **Frontend:** a single-page app served from LittleFS (plain HTML/CSS/vanilla JS or a very light framework — avoid heavy frameworks given flash constraints), showing:
  - Live lap table (lap #, lap time, delta to best, sector times if multiple gates).
  - Best lap highlight, current session name/driver field.
  - Gate status panel: online/offline, last-seen time, battery voltage per gate.
  - Session controls: start new session/run, reset, download CSV of the current or past session.
  - Basic config page: number of gates, gate role/order (which is "start/finish" vs "intermediate N"), minimum lap time threshold.
- **Data persistence:** keep the active session in RAM for speed; on session end (or periodically) flush to LittleFS as JSON/CSV so data survives a reboot and can be downloaded later.
- **Discovery:** enable mDNS (`laptimer.local`) so users don't need to remember an IP address.

---

## 7. Firmware / repository structure (PlatformIO)

Recommend a single PlatformIO project with two environments (gate vs master) sharing a common protocol library, rather than duplicating code:

```
LapTimer/
├── platformio.ini          # [env:gate-c3] and [env:master-wroom32]
├── include/
├── lib/
│   └── protocol/            # shared: packet structs, CRC/seq helpers, NRF24 pipe addresses
├── src/
│   ├── gate/                # gate firmware (compiled only in env:gate-c3)
│   │   └── main.cpp
│   └── master/               # master firmware (compiled only in env:master-wroom32)
│       └── main.cpp
├── data/                    # LittleFS web assets (html/css/js) for the master
└── test/
```

Use PlatformIO's per-environment `build_src_filter` to compile only the relevant `src/gate` or `src/master` folder for each environment, while both link against the shared `lib/protocol`. This lets you `pio run -e gate-c3 -t upload` for a gate board and `pio run -e master-wroom32 -t upload` for the master from the same repo, with the wire protocol guaranteed to match on both ends since it's literally the same struct definitions.

---

## 8. Suggested libraries

- `RF24` (TMRh20) — mature NRF24L01 driver, supports multiple RX pipes, ack payloads, and the data rates/power levels needed here.
- `ESPAsyncWebServer` + `AsyncTCP` — async HTTP + WebSocket server for the master.
- `ArduinoJson` — packet/config (de)serialization for the web API.
- `LittleFS` (built into the ESP32 Arduino core) — flash storage for web assets + session logs.

---

## 9. Build-out / validation plan

1. **Bench proof-of-concept:** one gate + master on the bench, hand-trigger the IR beam, confirm event round-trips over NRF24 and shows up on the web dashboard with correct debounce.
2. **Timestamp precision check:** verify interrupt-to-timestamp latency and jitter (oscilloscope/logic analyzer on the IR receiver output vs. a GPIO toggled at capture time) to confirm sub-millisecond capture jitter at the gate.
3. **Radio range test:** place a gate at realistic track distances with the PA+LNA modules, confirm reliable delivery (check `seq_no` gaps) at max expected range and in the presence of the master's WiFi AP running simultaneously (checks for 2.4 GHz self-interference).
4. **Multi-gate sync validation:** with 2+ gates, verify sector-time math against a stopwatch/known reference to confirm the sync-beacon correction is within acceptable error.
5. **Battery run-time test:** full-day discharge test on a gate with expected trigger frequency to size the battery correctly.
6. **Field trial:** run at an actual test day, monitor gate connectivity/battery from the dashboard, validate against a reference timing method (e.g., video) for a session.

---

## 10. Future extensions (not required now, noted for design headroom)

- Per-car identification (RFID/BLE transponder on the car) if multiple simultaneous vehicles need to be told apart.
- OTA firmware updates for gates (saves needing to physically retrieve each unit to reflash).
- Cloud/offline sync of session data after the event (export to a laptop, or STA mode to upload once back on shop WiFi).
- SD card logging on the master for long-term archives across many test days.
