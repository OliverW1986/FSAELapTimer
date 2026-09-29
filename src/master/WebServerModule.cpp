#include "WebServerModule.h"

#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "Config.h"

namespace WebServerModule {

namespace {

AsyncWebServer server(HTTP_PORT);
AsyncWebSocket ws("/ws");

LapEngine *g_engine = nullptr;
GateRegistry *g_gates = nullptr;

// Body buffer for small JSON POST payloads (gate-order config changes).
// AsyncWebServer delivers POST bodies in chunks via this callback; we
// accumulate into a String and parse once the final chunk arrives.
void handleGateOrderBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                          size_t total) {
    static String body;
    if (index == 0) {
        body = "";
    }
    body.concat(reinterpret_cast<const char *>(data), len);
    if (index + len != total) {
        return; // wait for the rest of the body
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc["order"].is<JsonArray>()) {
        request->send(400, "application/json", "{\"error\":\"expected {\\\"order\\\":[gateId,...]}\"}");
        return;
    }

    std::vector<uint8_t> order;
    for (JsonVariant v : doc["order"].as<JsonArray>()) {
        order.push_back(v.as<uint8_t>());
    }
    if (order.empty() || order.size() > LapTimer::MAX_GATES) {
        request->send(400, "application/json", "{\"error\":\"order must have 1..MAX_GATES entries\"}");
        return;
    }

    g_engine->configureGateOrder(order);
    request->send(200, "application/json", "{\"ok\":true}");

    JsonDocument evt;
    evt["type"] = "gateOrderChanged";
    JsonArray arr = evt["order"].to<JsonArray>();
    for (uint8_t g : order) {
        arr.add(g);
    }
    String out;
    serializeJson(evt, out);
    broadcastJson(out);
}

void handleSessionGet(AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    g_engine->toSessionJson(obj);
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handleGatesGet(AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    g_gates->toJson(arr);
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handleSessionReset(AsyncWebServerRequest *request) {
    g_engine->resetSession();
    request->send(200, "application/json", "{\"ok\":true}");
    broadcastJson("{\"type\":\"sessionReset\"}");
}

void handleSessionCsv(AsyncWebServerRequest *request) {
    String csv = "lap,lap_time,lap_time_us";
    size_t sectorCount = g_engine->gateOrder().empty() ? 0 : g_engine->gateOrder().size() - 1;
    for (size_t i = 0; i < sectorCount; ++i) {
        csv += ",sector" + String(i + 1) + "_time,sector" + String(i + 1) + "_time_us";
    }
    csv += ",is_best_lap\n";

    for (const LapEngine::LapRecord &lap : g_engine->laps()) {
        csv += String(lap.lapNumber) + "," + LapEngine::formatLapTime(lap.lapTimeUs) + "," +
               String(static_cast<unsigned long long>(lap.lapTimeUs));
        for (uint64_t s : lap.sectorTimesUs) {
            csv += "," + LapEngine::formatLapTime(s) + "," + String(static_cast<unsigned long long>(s));
        }
        csv += String(",") + (lap.isBestLap ? "1" : "0") + "\n";
    }

    AsyncWebServerResponse *response = request->beginResponse(200, "text/csv", csv);
    response->addHeader("Content-Disposition", "attachment; filename=session.csv");
    request->send(response);
}

void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *,
               size_t) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[master] websocket client #%u connected\n", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[master] websocket client #%u disconnected\n", client->id());
    }
}

} // namespace

void begin(LapEngine &engine, GateRegistry &gates) {
    g_engine = &engine;
    g_gates = &gates;

    if (!LittleFS.begin(true)) {
        Serial.println(F("[master] ERROR: LittleFS mount failed"));
    }

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.on("/api/session", HTTP_GET, handleSessionGet);
    server.on("/api/gates", HTTP_GET, handleGatesGet);
    server.on("/api/session/csv", HTTP_GET, handleSessionCsv);
    server.on("/api/session/reset", HTTP_POST, handleSessionReset);
    server.on("/api/config/gate-order", HTTP_POST, [](AsyncWebServerRequest *) { /* handled in body cb */ },
              nullptr, handleGateOrderBody);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("max-age=3600");

    server.begin();
    Serial.println(F("[master] web server started"));
}

void broadcastJson(const String &json) { ws.textAll(json); }

void loopCleanup() { ws.cleanupClients(); }

} // namespace WebServerModule
