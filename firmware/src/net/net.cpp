// Tarea de red: WiFi (portal «Pana-Setup» si no conecta), WebSocket con reconexión
// exponencial, protocolo (ver protocol/PROTOCOL.md), envío del micro y OTA.
// Toda llamada al WebSocket ocurre en esta tarea: la librería no es thread-safe.
#include "net.h"
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <sys/time.h>
#include "audio/audio.h"
#include "board.h"
#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"                    // tu WiFi y servidor (copia de secrets.example.h, sin subir)
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#define SERVER_HOST "pana.local"
#define SERVER_PORT 8770
#endif
#include "ui/model.h"

static WebSocketsClient ws;
static QueueHandle_t outQ;               // String* para enviar
static Preferences prefs;
static String host;
static uint16_t port;
static volatile bool micActive = false, micStopPending = false;
static uint32_t backoff = 1000;
static uint32_t lastStatus = 0;
static String otaUrl;

void netSend(const String &json) {
    String *s = new String(json);
    if (xQueueSend(outQ, &s, 0) != pdTRUE) delete s;
}

void netMicStart() {
    netSend("{\"type\":\"audio_start\",\"id\":\"mic\",\"sample_rate\":16000}");
    audioMic(true);
    micActive = true;
}

void netMicStop() {
    micActive = false;
    micStopPending = true;               // la tarea de red vacía lo que queda y luego manda audio_end
}

void netSetHost(const String &h) {
    prefs.putString("host", h);
    Serial.printf("servidor → %s (reinicia para aplicar)\n", h.c_str());
}

void netSetPort(uint16_t p) {
    prefs.putUShort("port", p);
    Serial.printf("puerto → %u (reinicia para aplicar)\n", p);
}

void netResetWifi() {
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}

// --- mensajes del servidor -----------------------------------------------------------
static void onText(const char *txt, size_t len) {
    JsonDocument d;
    if (deserializeJson(d, txt, len)) return;
    const char *t = d["type"] | "";
    if (!strcmp(t, "ping")) {
        // la hora sale del servidor: así no hace falta SNTP
        struct timeval tv = {(time_t)(d["t"] | 0), 0};
        if (tv.tv_sec > 1700000000) settimeofday(&tv, nullptr);
        String r = "{\"type\":\"pong\",\"t\":" + String((long)tv.tv_sec) + "}";
        ws.sendTXT(r);
        return;
    }
    if (!strcmp(t, "mood")) return;
    modelLock();
    Model &m = model;
    if (!strcmp(t, "state")) {
        m.emotion = d["emotion"] | "idle";
        m.animation = d["animation"] | "";
        if (d["look"].is<JsonArray>()) {
            m.lookX = d["look"][0] | 0.0f;
            m.lookY = d["look"][1] | 0.0f;
            m.lookNew = true;
        }
    } else if (!strcmp(t, "say")) {
        String id = d["id"] | "";
        if (id != m.bubbleId && (d["audio"] | false)) audioOpen();
        m.bubble = true;
        m.bubbleId = id;
        m.bubbleText = d["text"] | "";
        m.version++;
    } else if (!strcmp(t, "audio_end")) {
        audioClose();
    } else if (!strcmp(t, "card")) {
        m.card = true;
        m.cardId = d["id"] | "";
        m.cardKind = d["kind"] | "system";
        m.cardTitle = d["title"] | "";
        m.cardIcon = d["icon"] | "";
        m.nLines = 0;
        for (JsonVariant l : d["lines"].as<JsonArray>()) if (m.nLines < 4) m.cardLines[m.nLines++] = l.as<String>();
        ToolRow prev[MAX_TOOLS];
        int nPrev = m.nTools;
        for (int i = 0; i < nPrev; i++) prev[i] = m.tools[i];
        m.nTools = 0;
        m.toolsTotal = d["tools_total"] | 0;
        for (JsonObject r : d["tools"].as<JsonArray>()) {
            if (m.nTools == MAX_TOOLS) break;
            ToolRow &t = m.tools[m.nTools++];
            const char *st = r["st"] | "ok";
            t.st = !strcmp(st, "run") ? 0 : !strcmp(st, "err") ? 2 : 1;
            t.name = r["name"] | "";
            t.detail = r["detail"] | "";
            t.t0 = r["t0"] | 0.0;
            t.secs = r["secs"].isNull() ? -1.0f : r["secs"].as<float>();
            t.seenMs = millis();
            for (int i = 0; i < nPrev; i++)          // la misma llamada que ya estaba: conserva su reloj
                if (prev[i].t0 == t.t0 && prev[i].name == t.name) { t.seenMs = prev[i].seenMs; break; }
        }
        m.nActions = 0;
        for (JsonObject a : d["actions"].as<JsonArray>())
            if (m.nActions < 3) m.cardActions[m.nActions++] = {a["id"] | "", a["label"] | ""};
        m.version++;
    } else if (!strcmp(t, "clear")) {
        String w = d["what"] | "all";
        if (w != "bubble") m.card = false;
        if (w != "card") m.bubble = false;
        m.version++;
    } else if (!strcmp(t, "confirm")) {
        m.confirm = true;
        m.confirmId = d["id"] | "";
        m.question = d["question"] | "";
        m.preview = d["preview"] | "";
        m.nOptions = 0;
        for (JsonVariant o : d["options"].as<JsonArray>()) if (m.nOptions < 3) m.options[m.nOptions++] = o.as<String>();
        m.confirmTotal = (uint32_t)(d["expires_in"] | 300) * 1000;
        m.confirmUntil = millis() + m.confirmTotal;
        m.version++;
    } else if (!strcmp(t, "confirm_close")) {
        if (m.confirmId == (d["id"] | "")) { m.confirm = false; m.version++; }
    } else if (!strcmp(t, "graph")) {
        m.gTitle = d["title"] | "";
        m.gNotes = d["notes"] | 0;
        m.gLinks = d["links"] | 0;
        m.gN = m.gE = m.gG = 0;
        for (JsonObject n : d["nodes"].as<JsonArray>()) {
            if (m.gN == MAX_GNODES) break;
            GraphNode &g = m.gNodes[m.gN++];
            g.x = n["x"] | 0; g.y = n["y"] | 0;
            g.deg = min(255, (int)(n["deg"] | 0));
            g.group = n["group"] | -1;
            g.label = n["label"] | "";
        }
        for (JsonArray e : d["edges"].as<JsonArray>()) {
            if (m.gE == MAX_GEDGES) break;
            int a = e[0] | 0, b = e[1] | 0;
            if (a < m.gN && b < m.gN) { m.gEdges[m.gE][0] = a; m.gEdges[m.gE][1] = b; m.gE++; }
        }
        for (JsonVariant g : d["groups"].as<JsonArray>()) if (m.gG < MAX_GGROUPS) m.gGroups[m.gG++] = g.as<String>();
        m.gVersion++;
    } else if (!strcmp(t, "dash")) {
        Dash &x = m.dash;
        JsonObject s = d["sys"];
        x.cpu = s["cpu"] | 0.0f;
        x.gpu = s["gpu"].isNull() ? -1.0f : s["gpu"].as<float>();
        x.ram = s["ram"] | 0.0f;
        x.ramGb = s["ram_gb"] | 0.0f;
        x.ramTotal = s["ram_total"] | 0.0f;
        x.temp = s["temp"].isNull() ? -1.0f : s["temp"].as<float>();
        x.up = s["up"] | "";
        x.host = s["host"] | "";
        x.nSvc = 0;
        for (JsonArray v : d["svc"].as<JsonArray>()) {
            if (x.nSvc == 6) break;
            x.svcName[x.nSvc] = v[0] | "";
            x.svcOk[x.nSvc++] = (v[1] | 0) != 0;
        }
        JsonObject a = d["agenda"];
        x.agendaOk = a["ok"] | false;
        x.agendaWhy = a["why"] | "";
        x.agendaLeft = a["left"] | 0;
        x.nAgenda = 0;
        for (JsonObject e : a["items"].as<JsonArray>()) {
            if (x.nAgenda == 6) break;
            AgendaItem &it = x.agenda[x.nAgenda++];
            it.h = e["h"] | "";
            it.fin = e["fin"] | "";
            it.title = e["t"] | "";
            it.now = e["now"] | false;
        }
        JsonObject yt = d["redes"]["yt"], ig = d["redes"]["ig"];
        x.yt = !yt.isNull();
        if (x.yt) {
            x.ytSubs = yt["subs"] | 0L; x.ytViews = yt["views"] | 0L; x.ytD = yt["d"] | 0L;
            x.ytLast = yt["last"]["t"] | ""; x.ytLastViews = yt["last"]["views"] | 0L;
        }
        x.ig = !ig.isNull();
        if (x.ig) { x.igFollowers = ig["followers"] | 0L; x.igPosts = ig["posts"] | 0L; x.igD = ig["d"] | 0L; }
        x.have = true;
        x.version++;
    } else if (!strcmp(t, "ota")) {
        otaUrl = d["url"] | "";
        m.splash1 = "Actualizando…";
        m.splash2 = String("firmware ") + (d["version"] | "?");
    }
    modelUnlock();
}

static void hello() {
    JsonDocument d;
    d["type"] = "hello";
    d["device_id"] = DEVICE_ID;
    d["fw_version"] = FW_VERSION;
    d["kind"] = "board";
    d["has_mic"] = HAS_AUDIO;
    d["has_speaker"] = HAS_AUDIO;
    JsonArray s = d["screen"].to<JsonArray>();
    s.add(SCR_W);
    s.add(SCR_H);
    String out;
    serializeJson(d, out);
    ws.sendTXT(out);
}

static void onEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED:
        Serial.printf("ws conectado a %s:%u\n", host.c_str(), port);
        backoff = 1000;
        ws.setReconnectInterval(backoff);
        // el servidor repinta lo que haya en pantalla: se limpia lo viejo (p. ej. un confirm huérfano)
        modelLock();
        model.server = true;
        model.confirm = model.card = model.bubble = false;
        model.version++;
        modelUnlock();
        hello();
        break;
    case WStype_DISCONNECTED: {
        modelLock(); bool was = model.server; model.server = false; modelUnlock();
        if (was) Serial.println("ws desconectado");
        // reconexión exponencial: 1 s, 2 s, 4 s… hasta 30 s
        backoff = min<uint32_t>(backoff * 2, 30000);
        ws.setReconnectInterval(backoff);
        break;
    }
    case WStype_TEXT:
        onText((const char *)payload, len);
        break;
    case WStype_BIN:
        audioPush(payload, len);
        break;
    default:
        break;
    }
}

static void doOta() {
    Serial.printf("OTA desde %s\n", otaUrl.c_str());
    ws.disconnect();
    WiFiClient client;
    httpUpdate.rebootOnUpdate(true);
    t_httpUpdate_return r = httpUpdate.update(client, otaUrl);
    Serial.printf("OTA falló (%d): %s\n", r, httpUpdate.getLastErrorString().c_str());
    modelLock(); model.splash1 = ""; modelUnlock();
    otaUrl = "";
}

static void sendStatus() {
    JsonDocument d;
    d["type"] = "status";
    d["rssi"] = WiFi.RSSI();
    d["heap"] = ESP.getFreeHeap();
    d["psram"] = ESP.getFreePsram();
    d["uptime"] = millis() / 1000;
    d["fw_version"] = FW_VERSION;
    String out;
    serializeJson(d, out);
    ws.sendTXT(out);
}

static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                // menos latencia de audio
    WiFiManager wm;
    wm.setConnectTimeout(15);
    wm.setConfigPortalTimeout(300);
    // primera vez: prueba la WiFi de secrets.h antes de abrir el portal
    if (!wm.getWiFiIsSaved() && strlen(WIFI_SSID)) {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(250);
        if (WiFi.status() == WL_CONNECTED) return;
    }
    char hostBuf[64], portBuf[8];
    strlcpy(hostBuf, host.c_str(), sizeof(hostBuf));
    snprintf(portBuf, sizeof(portBuf), "%u", port);
    WiFiManagerParameter pHost("host", "Servidor (IP o nombre.local)", hostBuf, 63);
    WiFiManagerParameter pPort("port", "Puerto", portBuf, 7);
    wm.addParameter(&pHost);
    wm.addParameter(&pPort);
    wm.setSaveParamsCallback([&]() { netSetHost(pHost.getValue()); netSetPort(atoi(pPort.getValue())); });
    modelLock();
    model.splash1 = "Conéctame a tu WiFi";
    model.splash2 = "red «Pana-Setup» desde el móvil";
    modelUnlock();
    bool ok = wm.autoConnect("Pana-Setup");
    modelLock(); model.splash1 = ""; modelUnlock();
    if (!ok) ESP.restart();
    host = prefs.getString("host", SERVER_HOST);
    port = prefs.getUShort("port", SERVER_PORT);
}

static void netTask(void *) {
    connectWifi();
    modelLock(); model.wifi = true; modelUnlock();
    Serial.printf("WiFi %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    String ip = host;
    if (host.endsWith(".local")) {       // mDNS: el core no resuelve .local por sí solo
        MDNS.begin(DEVICE_ID);
        IPAddress a = MDNS.queryHost(host.substring(0, host.length() - 6));
        if (a != IPAddress()) ip = a.toString();
    }
    ws.begin(ip.c_str(), port, "/ws");
    ws.onEvent(onEvent);
    ws.setReconnectInterval(backoff);
    ws.enableHeartbeat(15000, 5000, 2);  // detecta una conexión muerta

    static uint8_t frame[2048];
    for (;;) {
        ws.loop();
        bool up = WiFi.status() == WL_CONNECTED;
        modelLock(); model.wifi = up; modelUnlock();
        String *s;
        while (xQueueReceive(outQ, &s, 0) == pdTRUE) {
            ws.sendTXT(*s);
            delete s;
        }
        // micro → servidor
        size_t n;
        while ((n = audioMicPop(frame, sizeof(frame))) > 0) ws.sendBIN(frame, n);
        if (micStopPending && !micActive) {
            audioMic(false);
            while ((n = audioMicPop(frame, sizeof(frame))) > 0) ws.sendBIN(frame, n);
            ws.sendTXT("{\"type\":\"audio_end\",\"id\":\"mic\"}");
            micStopPending = false;
        }
        if (millis() - lastStatus > 30000 && ws.isConnected()) {
            lastStatus = millis();
            sendStatus();
        }
        if (otaUrl.length()) doOta();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void netBegin() {
    prefs.begin("pana", false);
    host = prefs.getString("host", SERVER_HOST);
    port = prefs.getUShort("port", SERVER_PORT);
    outQ = xQueueCreate(32, sizeof(String *));
    setenv("TZ", "AST4", 1);             // República Dominicana, sin horario de verano
    tzset();
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 3, nullptr, 0);
}
