#pragma once

// Hosts the master's HTTP REST API + WebSocket live-push channel and serves
// the dashboard single-page app from LittleFS (see FSAE-LapTimer-Plan.md
// section 6).

#include "GateRegistry.h"
#include "LapEngine.h"

namespace WebServerModule {

// Starts LittleFS, the HTTP server, and the WebSocket endpoint. Call once
// from setup() after LapEngine/GateRegistry are constructed.
void begin(LapEngine &engine, GateRegistry &gates);

// Pushes a JSON string to every connected WebSocket client. Used by the main
// loop to notify the dashboard of new laps/sectors/gate-status changes.
void broadcastJson(const String &json);

// Call periodically (e.g. every loop iteration) to reap disconnected
// WebSocket clients.
void loopCleanup();

} // namespace WebServerModule
