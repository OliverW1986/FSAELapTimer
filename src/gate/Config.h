#pragma once

// Gate node hardware/config pinout for the ESP32-C3-DevKitC-02.
//
// Adjust these to match your actual wiring. Pins were chosen to avoid the
// C3's strapping pins (2, 8, 9) and USB pins (18, 19) where possible, but
// double check against your specific board silkscreen before wiring.

#include <cstdint>

// --- Identity -----------------------------------------------------------
// Each physical gate must be flashed with a unique GATE_ID (1..MAX_GATES).
// Override per-build with `-D GATE_ID=<n>` in a platformio.ini env
// (see the [env:gate-1] style examples in platformio.ini) instead of editing
// this file for every gate you flash.
#ifndef GATE_ID
#define GATE_ID 1
#endif

// --- NRF24L01 (SPI) -------------------------------------------------------
constexpr uint8_t PIN_RADIO_CE = 3;
constexpr uint8_t PIN_RADIO_CSN = 7;
constexpr uint8_t PIN_SPI_SCK = 4;
constexpr uint8_t PIN_SPI_MISO = 5;
constexpr uint8_t PIN_SPI_MOSI = 6;

// --- IR break-beam receiver ----------------------------------------------
// Output of a 38kHz-tuned IR receiver module (e.g. TSOP38238). Idle HIGH,
// pulled LOW while the beam is broken/demodulated signal is present -
// invert GATE_TRIGGER_ON_RISING below if your receiver's polarity differs.
constexpr uint8_t PIN_IR_RECEIVER = 10;
constexpr bool GATE_TRIGGER_ON_RISING = false; // false = trigger on FALLING edge

// --- Battery monitor -------------------------------------------------------
// Analog pin sampling a resistor divider across the battery so the gate can
// report battery_mv in its event/heartbeat packets.
constexpr uint8_t PIN_BATTERY_ADC = 0;
// Divider ratio: battery_mv = adc_mv * BATTERY_DIVIDER_RATIO. Adjust to your
// resistor values (e.g. two equal resistors => ratio of 2.0).
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;

// --- Status LED -------------------------------------------------------
constexpr uint8_t PIN_STATUS_LED = 8;
