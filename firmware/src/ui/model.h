#pragma once
// Lo que hay que pintar. La tarea de red lo escribe al llegar mensajes y la de UI lo lee
// en cada cuadro; siempre con el mutex tomado (lock()/unlock()).
#include <Arduino.h>

struct Action { String id, label; };

// una fila de la consola de herramientas (Card.tools del servidor)
struct ToolRow {
    uint8_t st = 1;                    // 0 run · 1 ok · 2 err
    String name, detail;
    double t0 = 0;                     // epoch de inicio
    float secs = -1;                   // duración; -1 = sigue corriendo
    uint32_t seenMs = 0;               // millis() al verla por primera vez: el reloj en vivo no depende de la hora
};
static const int MAX_TOOLS = 10;

// el grafo de Obsidian (mensaje "graph" del servidor, brain/obsidian.py)
struct GraphNode { int16_t x, y; uint8_t deg; int8_t group; String label; };   // x, y en 0..1000
static const int MAX_GNODES = 150, MAX_GEDGES = 400, MAX_GGROUPS = 8;

// el tablero del servidor, la agenda y las redes (mensaje "dash" del servidor, brain/tablero.py)
struct AgendaItem { String h, fin, title; bool now = false; };
struct Dash {
    bool have = false;
    float cpu = 0, gpu = -1, ram = 0, ramGb = 0, ramTotal = 0, temp = -1;
    String up, host;
    String svcName[6];
    bool svcOk[6] = {};
    int nSvc = 0;
    bool agendaOk = false;
    String agendaWhy;
    AgendaItem agenda[6];
    int nAgenda = 0, agendaLeft = 0;
    bool yt = false, ig = false;
    long ytSubs = 0, ytViews = 0, ytD = 0, ytLastViews = 0, igFollowers = 0, igPosts = 0, igD = 0;
    String ytLast;
    uint32_t version = 0;
};

struct Model {
    // cara
    String emotion = "idle";
    String animation;                  // gesto puntual pendiente ("" = ninguno)
    bool lookNew = false;
    float lookX = 0, lookY = 0;        // mirar a (-1..1)
    // burbuja
    bool bubble = false;
    String bubbleId, bubbleText;
    // tarjeta
    bool card = false;
    String cardId, cardKind, cardTitle, cardIcon;
    String cardLines[4];
    int nLines = 0;
    Action cardActions[3];
    int nActions = 0;
    ToolRow tools[MAX_TOOLS];          // solo kind "tools"
    int nTools = 0, toolsTotal = 0;
    // confirmación
    bool confirm = false;
    String confirmId, question, preview;
    String options[3];
    int nOptions = 0;
    uint32_t confirmUntil = 0;         // millis()
    uint32_t confirmTotal = 1;
    // grafo de Obsidian
    GraphNode gNodes[MAX_GNODES];
    uint8_t gEdges[MAX_GEDGES][2];
    String gGroups[MAX_GGROUPS], gTitle;
    int gN = 0, gE = 0, gG = 0, gNotes = 0, gLinks = 0;
    uint32_t gVersion = 0;
    Dash dash;
    // estado
    bool wifi = false, server = false, recording = false;
    String splash1, splash2;           // splash1 != "" → pantalla de aviso (OTA, portal WiFi)
    uint32_t version = 0;              // sube con cada cambio de burbuja/tarjeta/confirm
};

extern Model model;
void modelLock();
void modelUnlock();
