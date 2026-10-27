// Esfera viva de 720x720 (Waveshare ESP32-P4 4C). Cuatro páginas que se cambian deslizando:
//   0 · esfera: la hora arriba, el avatar (×2,5) en el centro, CPU/GPU a la izquierda y RAM/temperatura a la
//       derecha en arcos del borde con escala, y debajo un panel fijo: lo que dice o hace Hermes o, en
//       reposo, la agenda. Tocar el panel lo abre a pantalla completa
//   1 · homelab: cuatro relojes grandes del servidor y el estado de los servicios
//   2 · hoy: la agenda del día y las redes (YouTube, Instagram)
//   3 · grafo de Obsidian
// Las confirmaciones tapan todo, con la cuenta atrás en el anillo. El anillo del borde dice qué hace el avatar
// (cometa si trabaja, latido si escucha o espera permiso, grosor con la voz).
// Cada cuadro se pinta entero en un sprite de PSRAM y present() lo pasa a la pantalla con el PPA.
#ifdef BOARD_P4
#include <ArduinoJson.h>
#include "audio/audio.h"
#include "character/avatar.h"
#include "common.h"
#include "config.h"
#include "fonts/t72clk.h"
#include "fonts/t72l.h"
#include "fonts/t72m.h"
#include "fonts/t72mono.h"
#include "fonts/t72num.h"
#include "fonts/t72s.h"
#include "fonts/t72xs.h"
#include "model.h"
#include "net/net.h"
#include "ui.h"
#include <WiFi.h>
#include <algorithm>

static LGFX *lcd;
static LGFX_Sprite scr;
static Avatar face;
static const int CX = SCR_W / 2, CY = SCR_H / 2;
static const uint32_t BG = 0x05080C, INK = 0xE8EEF2, DIM = 0x3A4854, MUTED = 0x8090A0, SOFT = 0xC4CED6;
static const uint32_t AMBER = 0xFFB020, GREEN = 0x5CF08A, RED = 0xFF6A6A, BLUE = 0x7AA8FF;
static const uint32_t TEAL = 0x40E0C0, SEG = 0x111A22, SEG2 = 0x1A2630, LINE = 0x1E2A34, VIOLET = 0xA88BFA;
static const uint32_t PANEL = 0x0C141B, PANEL2 = 0x141E28, CONSOLE_RUN = 0x16202A;
static const uint32_t YT_RED = 0xFF3B3B, IG_PINK = 0xF05A9A;

// el anillo de estado (borde) y los arcos de medida, por dentro
static const int RING_OUT = CX - 2, RING_IN = CX - 20;
static const int GA_OUT = 333, GA_IN = 317, GB_OUT = 310, GB_IN = 304;
// el avatar (×2,5 = 262x305) y el panel de Hermes debajo
static const int AV_TOP = 136, AV_CLIP0 = 126, AV_CLIPH = AV_TOP + 305 + 64 - AV_CLIP0;
static const int DOTS_Y = 690;
// la tarjeta de Hermes sobre el avatar y la consola a pantalla completa
struct Box { int x, y, w, h; };
static const Box CARD = {142, 454, 436, 178};       // el panel de Hermes
static const int X0 = 120, X1 = SCR_W - 120, CW = X1 - X0, Y0 = 150, Y1 = 604;

static const uint8_t *fontNow = nullptr;
static void useFont(const uint8_t *f) {
    if (fontNow == f) return;
    scr.unloadFont();
    scr.loadFont(f);
    fontNow = f;
}

// texto sobre lo que ya hay pintado (las fuentes suaves mezclan con el fondo del sprite)
static void text(const String &s, int x, int y, uint32_t col, textdatum_t d = textdatum_t::top_left) {
    scr.setTextColor(col);
    scr.setTextDatum(d);
    scr.drawString(s, x, y);
    scr.setTextDatum(textdatum_t::top_left);
}

static bool inside(Box b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static uint32_t mix(uint32_t a, uint32_t b, float t) {      // de a a b, t en 0..1
    t = constrain(t, 0.0f, 1.0f);
    auto ch = [&](int sh) { return (uint32_t)(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

// medio ancho útil del círculo entre las filas y0 e y1 (lo que no pisa el anillo)
static int halfAt(int y0, int y1) {
    const int R = RING_IN - 14;
    int dy = max(abs(y0 - CY), abs(y1 - CY));
    return dy >= R ? 0 : (int)sqrtf((float)(R * R - dy * dy));
}

// Arco suavizado (el fillArc de LovyanGFX tiene el borde en escalera, y a 180 ppi se ve). Ángulos en
// grados, 0 = derecha y en sentido horario, aunque crucen los 0°; radios r0 (dentro) y r1 (fuera).
// Recorre solo las filas y los tramos de la corona dentro de la caja del arco y mezcla por cobertura:
// radial (distancia a los dos bordes) y angular (longitud de arco hasta los extremos).
static inline uint16_t blend565(uint16_t be, uint32_t rgb, float a) {
    uint16_t v = (be << 8) | (be >> 8);
    int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    r += (int)((((rgb >> 16) & 255) - r) * a);
    g += (int)((((rgb >> 8) & 255) - g) * a);
    b += (int)(((rgb & 255) - b) * a);
    uint16_t o = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (o << 8) | (o >> 8);
}

static void aaArc(int cx, int cy, float r0, float r1, float a0, float a1, uint32_t col) {
    float span = a1 - a0;
    if (span <= 0) return;
    bool full = span >= 360;
    const bool narrow = span <= 180;               // si no, se mira el hueco que queda (que es estrecho)
    const float u0x = cosf(a0 * PI / 180), u0y = sinf(a0 * PI / 180), u1x = cosf(a1 * PI / 180), u1y = sinf(a1 * PI / 180);
    // caja: los extremos y los puntos cardinales que caen dentro
    int bx0 = cx - r1 - 2, bx1 = cx + r1 + 2, by0 = cy - r1 - 2, by1 = cy + r1 + 2;
    if (!full) {
        float xs[6], ys[6];
        int n = 0;
        auto add = [&](float ang, float r) { float t = ang * PI / 180; xs[n] = cx + cosf(t) * r; ys[n] = cy + sinf(t) * r; n++; };
        add(a0, r1); add(a1, r1);
        float minx = min(xs[0], xs[1]), maxx = max(xs[0], xs[1]), miny = min(ys[0], ys[1]), maxy = max(ys[0], ys[1]);
        n = 0;
        add(a0, r0); add(a1, r0);
        for (int i = 0; i < 2; i++) { minx = min(minx, xs[i]); maxx = max(maxx, xs[i]); miny = min(miny, ys[i]); maxy = max(maxy, ys[i]); }
        for (int k = -1; k <= 4; k++) {
            float c = k * 90;
            float rel = fmodf(c - a0 + 720, 360);
            if (rel <= span) {
                float t = c * PI / 180;
                float x = cx + cosf(t) * r1, y = cy + sinf(t) * r1;
                minx = min(minx, x); maxx = max(maxx, x); miny = min(miny, y); maxy = max(maxy, y);
            }
        }
        bx0 = minx - 2; bx1 = maxx + 2; by0 = miny - 2; by1 = maxy + 2;
    }
    bx0 = max(bx0, 0); by0 = max(by0, 0); bx1 = min(bx1, SCR_W - 1); by1 = min(by1, SCR_H - 1);
    uint16_t *buf = (uint16_t *)scr.getBuffer();
    const float ro = r1 + 1, ri = r0 - 1;
    const float in2 = (r0 + 0.5f) * (r0 + 0.5f), out2 = r1 > 0.5f ? (r1 - 0.5f) * (r1 - 0.5f) : 0;
    for (int y = by0; y <= by1; y++) {
        float dy = y - cy;
        if (fabsf(dy) > ro) continue;
        int ox = (int)sqrtf(ro * ro - dy * dy) + 1;
        int ix = fabsf(dy) < ri ? (int)sqrtf(ri * ri - dy * dy) - 1 : -1;
        for (int side = 0; side < 2; side++) {
            int xa = side ? cx + max(ix, 0) : cx - ox, xb = side ? cx + ox : cx - max(ix, 0);
            if (ix < 0 && side) break;                     // sin agujero: un solo tramo
            if (ix < 0) xb = cx + ox;
            xa = max(xa, bx0); xb = min(xb, bx1);
            for (int x = xa; x <= xb; x++) {
                float dx = x - cx, d2 = dx * dx + dy * dy, cov = 1;
                if (d2 < in2 || d2 > out2) {                // solo los bordes necesitan la raíz
                    float d = sqrtf(d2);
                    cov = min(d - r0, r1 - d) + 0.5f;
                    if (cov <= 0) continue;
                    if (cov > 1) cov = 1;
                }
                if (!full) {
                    // distancia (en píxeles) a las rectas de los dos extremos, con signo: dentro si positiva
                    float c0 = u0x * dy - u0y * dx, c1 = dx * u1y - dy * u1x;
                    float edge = (narrow ? min(c0, c1) : max(c0, c1)) + 0.5f;
                    if (edge <= 0) continue;
                    if (edge < 1) cov *= edge;
                }
                uint16_t &px = buf[y * SCR_W + x];
                px = cov >= 0.999f ? (uint16_t)((((col >> 8) & 0xF800) | ((col >> 5) & 0x07E0) | ((col >> 3) & 0x1F)) << 8 |
                                                (((col >> 8) & 0xF800) | ((col >> 5) & 0x07E0) | ((col >> 3) & 0x1F)) >> 8)
                                   : blend565(px, col, cov);
            }
        }
    }
}

static void arc(int r0, int r1, float a0, float a1, uint32_t col) { aaArc(CX, CY, r0, r1, a0, a1, col); }

// extremos redondeados para los arcos de valor
static void arcCaps(float r0, float r1, float a0, float a1, uint32_t col) {
    float rm = (r0 + r1) / 2, w = (r1 - r0) / 2;
    for (float a : {a0, a1}) {
        float t = a * PI / 180;
        scr.drawSpot(CX + cosf(t) * rm, CY + sinf(t) * rm, w, col);
    }
}

// --- textos ---------------------------------------------------------------------------------
static int wrapChars(const String &t, int perLine, String *out, int maxLines) {   // por letras (rutas, JSON)
    int n = 0;
    size_t i = 0;
    while (i < t.length() && n < maxLines) {
        size_t j = i;
        int c = 0;
        while (j < t.length() && c < perLine) {
            j++;
            while (j < t.length() && (t[j] & 0xC0) == 0x80) j++;
            c++;
        }
        if (j < t.length()) {
            int sp = t.lastIndexOf(' ', j);
            if (sp > (int)i + perLine / 2) j = sp + 1;
        }
        out[n++] = t.substring(i, j);
        i = j;
    }
    if (i < t.length() && n) out[n - 1] = utf8Prefix(out[n - 1], perLine - 1) + "…";
    return n;
}

static String secsText(float s) {   // 0,4 s · 12 s · 2 min 5 s
    char b[24];
    if (s < 0.1f) return "<0,1 s";
    if (s < 10) snprintf(b, sizeof(b), "%.1f s", s);
    else if (s < 60) snprintf(b, sizeof(b), "%d s", (int)s);
    else snprintf(b, sizeof(b), "%d min %d s", (int)s / 60, (int)s % 60);
    String r = b;
    r.replace(".", ",");
    return r;
}

static bool timeOk() { return time(nullptr) > 1700000000; }
static struct tm localNow() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    return t;
}
static String clockText() {
    if (!timeOk()) return "--:--";
    struct tm t = localNow();
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    return b;
}
static String dateText(bool longDay = true) {
    if (!timeOk()) return "sin hora todavía";
    static const char *dias[] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
    static const char *meses[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
    struct tm t = localNow();
    return String(longDay ? dias[t.tm_wday] : "") + (longDay ? " " : "") + t.tm_mday + " " + meses[t.tm_mon];
}

static String withoutHermes(String t) {
    if (t.startsWith("Hermes · ")) t = t.substring(9);
    else if (t.startsWith("Hermes ")) t = t.substring(7);
    if (t.length()) t.setCharAt(0, toupper(t[0]));
    return t;
}

// 18.523 → «18,5 K» · 9.876 → «9.876» · 1.234.567 → «1,2 M»
static String fmtCount(long v) {
    char b[20];
    if (v >= 1000000) snprintf(b, sizeof(b), "%.1f M", v / 1e6);
    else if (v >= 10000) snprintf(b, sizeof(b), "%.1f K", v / 1e3);
    else if (v >= 1000) snprintf(b, sizeof(b), "%ld.%03ld", v / 1000, v % 1000);
    else snprintf(b, sizeof(b), "%ld", v);
    String r = b;
    if (v >= 10000) r.replace(".", ",");
    return r;
}

// --- estado ------------------------------------------------------------------------------
enum Page : uint8_t { P_SPHERE, P_LAB, P_DAY, P_GRAPH, NPAGES };
static int page = P_SPHERE;
static bool expanded = false;              // la consola de Hermes a pantalla completa
static uint32_t tExpanded = 0;
static int scroll = 0;
static int graphSel = -1;

static const float LETTERS_PER_S = 34;
static float revealed = 0;
static String animBubble, animCard, animConfirm;
static uint32_t tCard = 0, tNews = 0;
static bool collapsed = false;             // la tarjeta se recogió en el aviso de abajo

struct LogEntry { String hhmm, text; uint8_t st; };   // st: 0 empieza · 1 hecho · 2 error · 3 permiso
static LogEntry logRing[8];
static int logN = 0, tasksDone = 0;

static void logPush(uint8_t st, const String &t) {
    if (logN && logRing[(logN - 1) % 8].text == t) return;
    logRing[logN % 8] = {clockText(), t, st};
    logN++;
    if (st == 1) tasksDone++;
}

static void setPage(int p) {
    p = (p + NPAGES) % NPAGES;
    if (p != page) { page = p; scroll = 0; graphSel = -1; }
}

static bool hasNews(Model &m) { return m.bubble || m.card; }
static uint32_t linkColor(Model &m) { return m.server ? TEAL : m.wifi ? AMBER : 0xE05050u; }

static bool toolsRunning(Model &m) {
    if (!m.card || m.cardKind != "tools") return false;
    for (int i = 0; i < m.nTools; i++) if (m.tools[i].st == 0) return true;
    for (int i = 0; i < m.nLines; i++) if (m.cardLines[i].startsWith("run\t")) return true;
    return false;
}

// los valores que se ven persiguen a los de verdad: los arcos se deslizan en vez de saltar
struct Smooth { float v = 0; bool init = false; float step(float target, float k = 0.12f) {
    if (!init) { v = target; init = true; } v += (target - v) * k; return v; } };
static Smooth sCpu, sGpu, sRam, sTemp;

// --- marco común -----------------------------------------------------------------------------
enum RingMode : uint8_t { R_STATIC, R_COMET, R_PULSE, R_VOICE, R_COUNTDOWN };
static void ringLive(RingMode mode, uint32_t col, uint32_t now, float level, float frac = 1) {
    uint32_t dim = mix(BG, col, 0.22f);
    const int o = RING_OUT - 2;
    if (mode == R_STATIC) {
        arc(o - 3, o, 0, 360, mix(BG, col, 0.75f));
    } else if (mode == R_COUNTDOWN) {
        arc(o - 4, o, 0, 360, dim);
        arc(o - 6, o, 270, 270 + 360 * constrain(frac, 0.0f, 1.0f), col);
    } else if (mode == R_COMET) {
        arc(o - 3, o, 0, 360, dim);
        float head = fmodf(now * 0.22f, 360);
        for (int k = 0; k < 14; k++) {                         // cola de ~100° que se apaga
            float a0 = head - (k + 1) * 7, a1 = head - k * 7;
            int th = k < 3 ? 10 : k < 7 ? 7 : 5;
            arc(o - th, o, a0 - 0.8f, a1, mix(col, dim, k / 14.0f));   // solapados: sin costuras entre tramos
        }
        float r = head * PI / 180, rr = o - 5;
        scr.drawSpot(CX + cosf(r) * rr, CY + sinf(r) * rr, 7, mix(col, 0xFFFFFF, 0.4f));
    } else if (mode == R_PULSE) {
        float t = 0.5f + 0.5f * sinf(now / 260.0f);
        arc(o - 4 - (int)(9 * t), o, 0, 360, mix(dim, col, 0.35f + 0.65f * t));
    } else {
        arc(o - 4 - (int)(8 * level + 0.5f), o, 0, 360, col);   // respira con la voz
    }
}

// manda solo la banda del anillo (de radio ri a RING_OUT): por franjas, los dos trozos de cada lado
static void flushRing(int ri) {
    const int ro = RING_OUT + 2;
    ri -= 2;
    for (int y = CY - ro; y < CY + ro; y += 16) {
        int y1 = min(CY + ro, y + 16);
        int near = (y <= CY && CY < y1) ? 0 : min(abs(y - CY), abs(y1 - 1 - CY));
        int far = max(abs(y - CY), abs(y1 - 1 - CY));
        if (near >= ro) continue;
        int outer = (int)sqrtf((float)(ro * ro - near * near)) + 1;
        int inner = far < ri ? (int)sqrtf((float)(ri * ri - far * far)) - 1 : 0;
        if (inner <= 0) { lcd->flush(scr, CX - outer, y, 2 * outer, y1 - y); continue; }
        lcd->flush(scr, CX - outer, y, outer - inner, y1 - y);
        lcd->flush(scr, CX + inner, y, outer - inner, y1 - y);
    }
}

static void pageDots() {
    for (int i = 0; i < NPAGES; i++) {
        int x = CX + (i * 2 - (NPAGES - 1)) * 13;
        if (i == page) scr.fillSmoothRoundRect(x - 12, DOTS_Y - 4, 24, 9, 4, INK);
        else scr.drawSpot(x, DOTS_Y, 4, DIM);
    }
}

// cápsula de título (arriba en las páginas): punto de color o barras si suena la voz
static void chip(const String &title, uint32_t accent, uint32_t now, bool speaking = false, int y = 52) {
    useFont(t72s);
    int dy = abs(y + 20 - CY), R = RING_IN - 4;
    String t = ellipsize(scr, title, 2 * (int)sqrtf((float)max(0, R * R - dy * dy)) - 100);
    int w = scr.textWidth(t) + 64, x = CX - w / 2, h = 42;
    scr.fillSmoothRoundRect(x, y, w, h, h / 2, PANEL2);
    if (speaking) {
        for (int b = 0; b < 4; b++) {
            int hh = 6 + (int)(12 * (0.5f + 0.5f * sinf(now / 90.0f + b * 1.3f)));
            scr.fillSmoothRoundRect(x + 16 + b * 7, y + h / 2 - hh / 2, 4, hh, 2, accent);
        }
    } else {
        scr.drawSpot(x + 25, y + h / 2, 11, mix(PANEL2, accent, 0.18f));
        scr.drawSpot(x + 25, y + h / 2, 6, accent);
    }
    text(t, x + (speaking ? 50 : 44), y + h / 2 + 1, INK, textdatum_t::middle_left);
}

static void subtitle(const String &s, int y = 108) {
    useFont(t72xs);
    text(ellipsize(scr, s, 420), CX, y, MUTED, textdatum_t::middle_center);
}

static void pill(int x, int y, const String &t, uint32_t col, int h = 34) {
    useFont(t72xs);
    int w = scr.textWidth(t) + 44;
    scr.fillSmoothRoundRect(x, y, w, h, h / 2, PANEL2);
    scr.drawSpot(x + 19, y + h / 2, 5, col);
    text(t, x + 32, y + h / 2 + 1, SOFT, textdatum_t::middle_left);
}
static int pillW(const String &t) { useFont(t72xs); return scr.textWidth(t) + 44; }

static void button(Box b, const String &label, uint32_t fill, uint32_t ink) {
    scr.fillSmoothRoundRect(b.x, b.y, b.w, b.h, b.h / 2, fill);
    useFont(t72m);
    if (scr.textWidth(label) > b.w - 24) useFont(t72s);
    text(ellipsize(scr, label, b.w - 20), b.x + b.w / 2, b.y + b.h / 2 + 1, ink, textdatum_t::middle_center);
}

static void statusMark(int cx, int cy, uint8_t st, uint32_t now, float k = 1.3f) {
    if (st == 0) {
        int head = (now / 90) % 8;
        for (int i = 0; i < 8; i++) {
            int age = (head - i + 8) % 8;
            float a = i * PI / 4 - PI / 2;
            uint32_t c = age == 0 ? AMBER : age == 1 ? 0xC08418u : age == 2 ? 0x805810u : 0x3A3020u;
            scr.drawSpot(cx + cosf(a) * 7 * k, cy + sinf(a) * 7 * k, (age < 2 ? 3 : 2) * k, c);
        }
    } else if (st == 2) {
        scr.drawSpot(cx, cy, 10 * k, mix(BG, RED, 0.25f));
        int d = 4 * k;
        for (int o = -1; o <= 1; o++) {
            scr.drawWideLine(cx - d + o, cy - d, cx + d + o, cy + d, 0.7f, RED);
            scr.drawWideLine(cx - d + o, cy + d, cx + d + o, cy - d, 0.7f, RED);
        }
    } else {
        scr.drawSpot(cx, cy, 10 * k, mix(BG, GREEN, 0.22f));
        for (int o = 0; o < 3; o++) {
            scr.drawWideLine(cx - 6, cy + o, cx - 2, cy + 4 + o, 0.7f, GREEN);
            scr.drawWideLine(cx - 2, cy + 4 + o, cx + 6, cy - 4 + o, 0.7f, GREEN);
        }
    }
}

static bool isTerminal(const String &n) {
    return n.indexOf("terminal") >= 0 || n.indexOf("process") >= 0 || n.indexOf("execute") >= 0;
}

// filas de herramientas sacadas de la tarjeta (Card.tools o el formato viejo «st\tnombre\tdetalle»)
static int toolRows(Model &m, ToolRow *rows, int &total) {
    int n = 0;
    total = m.toolsTotal;
    if (m.nTools) {
        for (int i = 0; i < m.nTools; i++) rows[n++] = m.tools[i];
    } else {
        for (int i = 0; i < m.nLines; i++) {
            const String &l = m.cardLines[i];
            int a = l.indexOf('\t'), b = a < 0 ? -1 : l.indexOf('\t', a + 1);
            if (b < 0) continue;
            String st = l.substring(0, a);
            rows[n] = ToolRow();
            rows[n].st = st == "run" ? 0 : st == "err" ? 2 : 1;
            rows[n].name = l.substring(a + 1, b);
            rows[n].detail = l.substring(b + 1);
            n++;
        }
    }
    if (total < n) total = n;
    return n;
}
static String rowTime(const ToolRow &r) {
    if (r.st != 0 && r.secs >= 0) return secsText(r.secs);
    if (r.st == 0 && r.seenMs) return secsText((millis() - r.seenMs) / 1000.0f);
    return "";
}
static String toolsTitle(Model &m) {
    String t = withoutHermes(m.cardTitle);
    int dot = t.lastIndexOf(" · ");
    if (dot > 0 && t.substring(dot + 3).toInt() > 0) t = t.substring(0, dot);
    return t;
}

// --- página 0: la esfera ------------------------------------------------------------------------
// icono de 8x8 del estado de el avatar (los de su cartel), a escala K
static void stateIcon(const PackAnim &a, int phase, int ix, int iy, int K, uint32_t accent) {
    if (!strcmp(a.name, "talking") || !strcmp(a.name, "listening")) {
        static const int hs[] = {3, 6, 4, 7};
        for (int b = 0; b < 4; b++) {
            int hh = hs[(b + phase) % 4] * K;
            scr.fillRect(ix + b * 2 * K, iy + 8 * K / 2 - hh / 2, K, hh, accent);
        }
        return;
    }
    bool turn = !strcmp(a.name, "working") || !strcmp(a.name, "executing");
    if (!strcmp(a.name, "searching")) ix += (phase == 2 ? 2 : phase & 1) * K;
    for (int yy = 0; yy < 8; yy++)
        for (int xx = 0; xx < 8; xx++) {
            if (!(a.icon[yy] & (128 >> xx))) continue;
            if (!strcmp(a.name, "reconnecting") && yy < 2 && phase < 1) continue;
            int px = xx, py = yy;
            if (turn) for (int r = 0; r < phase; r++) { int t = px; px = 7 - py; py = t; }
            scr.fillRect(ix + px * K, iy + py * K, K, K, accent);
        }
}
static uint32_t stateColor(const PackAnim &a) {
    return !strcmp(a.name, "error") ? 0xFFAD8Eu : !strcmp(a.name, "approval") ? 0xFFD780u : 0x96E6C4u;
}

static uint32_t loadColor(float pct) { return pct < 60 ? GREEN : pct < 85 ? AMBER : RED; }
static uint32_t tempColor(float c) { return c < 60 ? TEAL : c < 80 ? AMBER : RED; }

// arcos del borde, de abajo arriba: a la izquierda CPU (fuera) y GPU (dentro), a la derecha RAM y temperatura
static const float L0 = 142, SPAN = 92, R0 = 38;
static void gaugeArcs(Model &m) {
    Dash &d = m.dash;
    float cpu = sCpu.step(d.cpu), gpu = sGpu.step(max(0.0f, d.gpu)), ram = sRam.step(d.ram);
    float tf = sTemp.step(d.temp > 0 ? constrain((d.temp - 30) / 60, 0.0f, 1.0f) * 100 : 0);
    arc(GA_IN, GA_OUT, L0, L0 + SPAN, SEG2);
    arc(GB_IN, GB_OUT, L0, L0 + SPAN, SEG);
    arc(GA_IN, GA_OUT, R0 - SPAN, R0, SEG2);
    arc(GB_IN, GB_OUT, R0 - SPAN, R0, SEG);
    for (int k = 1; k < 4; k++) {                        // escala: 25, 50 y 75 %
        float al = L0 + SPAN * k / 4, ar = R0 - SPAN * k / 4;
        arc(GA_OUT + 1, GA_OUT + 4, al - 0.6f, al + 0.6f, DIM);
        arc(GA_OUT + 1, GA_OUT + 4, ar - 0.6f, ar + 0.6f, DIM);
    }
    if (!d.have) return;
    auto val = [](int r0, int r1, float a0, float a1, uint32_t c) {
        if (a1 - a0 < 0.5f) return;
        arc(r0, r1, a0, a1, c);
        arcCaps(r0, r1, a0, a1, c);
    };
    val(GA_IN, GA_OUT, L0, L0 + SPAN * cpu / 100, loadColor(cpu));
    if (d.gpu >= 0) val(GB_IN, GB_OUT, L0, L0 + SPAN * gpu / 100, VIOLET);
    val(GA_IN, GA_OUT, R0 - SPAN * ram / 100, R0, BLUE);
    if (d.temp > 0) val(GB_IN, GB_OUT, R0 - SPAN * tf / 100, R0, tempColor(d.temp));
}

static const Box NUM_L = {84, 222, 132, 206}, NUM_R = {SCR_W - 84 - 132, 222, 132, 206};
static void gaugeLabels(Model &m) {
    Dash &d = m.dash;
    auto pair = [&](int x, int y, textdatum_t dt, const char *lab, const String &val, uint32_t col, bool big) {
        useFont(t72xs);
        text(lab, x, y, MUTED, dt);
        useFont(big ? t72num : t72l);
        text(val, x, y + 20, col, dt);
    };
    String na = "—";
    int xl = NUM_L.x + 4, xr = NUM_R.x + NUM_R.w - 4;
    pair(xl, 232, textdatum_t::top_left, "CPU", d.have ? String((int)(d.cpu + 0.5f)) + "%" : na, d.have ? loadColor(d.cpu) : DIM, true);
    pair(xl + 6, 334, textdatum_t::top_left, "GPU", d.have && d.gpu >= 0 ? String((int)(d.gpu + 0.5f)) + "%" : na, d.have ? VIOLET : DIM, false);
    pair(xr, 232, textdatum_t::top_right, "RAM", d.have ? String((int)(d.ram + 0.5f)) + "%" : na, d.have ? BLUE : DIM, true);
    pair(xr - 6, 334, textdatum_t::top_right, "TEMP", d.have && d.temp > 0 ? String((int)(d.temp + 0.5f)) + "°" : na,
         d.have && d.temp > 0 ? tempColor(d.temp) : DIM, false);
    if (d.have && d.ramTotal > 0) {
        char gb[16];
        snprintf(gb, sizeof(gb), "%.0f/%.0f GB", d.ramGb, d.ramTotal);
        useFont(t72xs);
        text(gb, xr, 304, DIM, textdatum_t::top_right);
    }
}

static void drawTop(Model &m, uint32_t now) {
    useFont(t72s);
    String a = m.recording ? "te escucho" : !m.wifi ? "sin WiFi" : !m.server ? "sin servidor" : dateText();
    uint32_t col = m.recording ? 0xFF4060u : !m.server ? linkColor(m) : MUTED;
    int w = scr.textWidth(a);
    text(a, CX + 9, 44, col, textdatum_t::middle_center);
    scr.drawSpot(CX - w / 2 - 6, 44, 5, m.recording ? 0xFF4060u : linkColor(m));
    useFont(t72clk);
    text(clockText(), CX, 96, INK, textdatum_t::middle_center);
}

String uiTestLabel;                          // prueba guiada (serie T): qué fase es

// el panel de Hermes: cabecera con el estado de el avatar y, debajo, lo que dice, lo que hace o la agenda
static void drawPanel(Model &m, uint32_t now) {
    const Box b = CARD;
    const int px = b.x + 26, pw = b.w - 52;
    bool news = hasNews(m) && !collapsed;
    bool speaking = m.bubble && audioPlaying();
    const PackAnim &st = face.anim();
    uint32_t accent = news ? (m.bubble ? TEAL : m.cardKind == "tools" ? (toolsRunning(m) ? AMBER : GREEN) : kindColor(m.cardKind))
                           : stateColor(st);
    scr.fillSmoothRoundRect(b.x, b.y, b.w, b.h, 28, PANEL);
    scr.fillSmoothRoundRect(b.x + 40, b.y, b.w - 80, 3, 1, mix(PANEL, accent, 0.7f));
    // cabecera: el estado de el avatar con su icono (o barras si habla) y, a la derecha, qué hay
    int hy = b.y + 30;
    if (speaking) {
        for (int k = 0; k < 4; k++) {
            int hh = 6 + (int)(14 * (0.5f + 0.5f * sinf(now / 90.0f + k * 1.3f)));
            scr.fillSmoothRoundRect(px + k * 7, hy - hh / 2, 4, hh, 2, accent);
        }
    } else stateIcon(st, face.phase(), px, hy - 12, 3, stateColor(st));
    useFont(t72s);
    text(m.recording ? String("Te escucho") : String(st.label), px + 38, hy + 1, stateColor(st), textdatum_t::middle_left);
    useFont(t72xs);
    String right = uiTestLabel.length() ? uiTestLabel
                 : news ? String(m.bubble ? (speaking ? "HERMES HABLA" : "HERMES DICE") : m.cardKind == "tools" ? "TRABAJANDO" : "AVISO")
                 : hasNews(m) ? String("1 NUEVO · TOCA") : String("HOY");
    uint32_t rc = uiTestLabel.length() ? AMBER : news ? MUTED : hasNews(m) ? AMBER : DIM;
    if (hasNews(m) && !news) scr.drawSpot(px + pw - scr.textWidth(right) - 12, hy, 4, AMBER);
    text(right, px + pw, hy + 1, rc, textdatum_t::middle_right);
    scr.drawFastHLine(px, b.y + 54, pw, LINE);

    int y = b.y + 66;
    const int LH = 34, maxL = 3;
    if (news && m.bubble) {
        useFont(t72m);
        static String lines[40];
        int n = wrap(scr, m.bubbleText, pw, lines, 40);
        int left = (int)revealed, last = 0;
        for (int i = 0; i < n; i++) { if (left > 0) last = i; left -= utf8Len(lines[i]) + 1; }
        int first = max(0, last - maxL + 1);
        left = (int)revealed;
        int cx = px, cy = y;
        for (int i = 0; i < n; i++) {
            String vis = utf8Prefix(lines[i], left);
            left -= utf8Len(lines[i]) + 1;
            if (i >= first && i < first + maxL && vis.length()) {
                int yy = y + (i - first) * LH;
                text(vis, px, yy, INK);
                cx = px + scr.textWidth(vis) + 4;
                cy = yy;
            }
        }
        if ((int)revealed <= utf8Len(m.bubbleText) || (now / 500) % 2) scr.fillSmoothRoundRect(cx, cy + 5, 11, 25, 3, TEAL);
    } else if (news && m.cardKind == "tools") {
        static ToolRow rows[MAX_TOOLS];
        int total, n = toolRows(m, rows, total);
        int done = 0;
        for (int i = 0; i < n; i++) done += rows[i].st != 0;
        String cnt = String(total - n + done) + "/" + total;
        useFont(t72s);
        int cw = scr.textWidth(cnt);
        text(cnt, px + pw, y + 2, MUTED, textdatum_t::top_right);
        useFont(t72m);
        text(ellipsize(scr, toolsTitle(m), pw - cw - 16), px, y - 2, INK);
        float frac = total ? (float)(total - n + done) / total : 1;
        scr.fillSmoothRoundRect(px, y + 32, pw, 5, 2, PANEL2);
        scr.fillSmoothRoundRect(px, y + 32, max(5, (int)(pw * frac)), 5, 2, accent);
        int ry = y + 46;
        for (int i = max(0, n - 2); i < n; i++, ry += 32) {
            const ToolRow &r = rows[i];
            statusMark(px + 10, ry + 12, r.st, now, 0.95f);
            String tt = rowTime(r);
            useFont(t72mono);
            int tw = tt.length() ? scr.textWidth(tt) : 0;
            if (tt.length()) text(tt, px + pw, ry + 13, r.st == 0 ? AMBER : r.st == 2 ? RED : MUTED, textdatum_t::middle_right);
            useFont(t72s);
            String nm = r.name;
            if (r.st == 0 && r.detail.length()) nm += "  " + r.detail;
            text(ellipsize(scr, nm, pw - 34 - tw - 12), px + 30, ry + 13, r.st == 0 ? INK : SOFT, textdatum_t::middle_left);
        }
    } else if (news) {
        useFont(t72m);
        text(ellipsize(scr, withoutHermes(m.cardTitle), pw), px, y - 2, accent);
        useFont(t72s);
        int yy = y + 36;
        for (int i = 0; i < m.nLines && yy < b.y + b.h - 30; i++, yy += 30) text(ellipsize(scr, m.cardLines[i], pw), px, yy, 0xD8E0E6u);
    } else {
        // reposo: lo que queda de la agenda; si no hay, lo último que hizo Hermes
        Dash &d = m.dash;
        int shown = 0;
        if (d.have && d.agendaOk)
            for (int i = 0; i < d.nAgenda && shown < 3; i++, shown++) {
                const AgendaItem &e = d.agenda[i];
                int yy = y + shown * LH;
                useFont(t72s);
                String h = e.now ? "ahora" : e.h == "todo el día" ? "día" : e.h;
                text(h, px, yy + 3, e.now ? TEAL : MUTED);
                useFont(t72m);
                text(ellipsize(scr, e.title, pw - 86), px + 86, yy, e.now ? INK : SOFT);
            }
        for (int i = logN - 1; i >= max(0, logN - 8) && shown < 3; i--, shown++) {
            const LogEntry &e = logRing[i % 8];
            int yy = y + shown * LH;
            uint32_t c = e.st == 0 ? AMBER : e.st == 1 ? GREEN : e.st == 2 ? RED : 0xFFD780u;
            useFont(t72s);
            text(e.hhmm, px, yy + 3, DIM);
            scr.drawSpot(px + 74, yy + 15, 4, c);
            text(ellipsize(scr, e.text, pw - 92), px + 90, yy + 3, SOFT);
        }
        if (!shown) {
            useFont(t72s);
            text("mantén pulsado a Pana para hablar", CX, y + 14, SOFT, textdatum_t::top_center);
            useFont(t72xs);
            if (d.have && !d.agendaOk) text("agenda sin conectar", CX, y + 56, DIM, textdatum_t::top_center);
        }
    }
}
static bool cardShown(Model &m) { return page == P_SPHERE && !expanded && !m.confirm; }   // el panel está siempre

static void drawFace(Model &m) {
    face.clipBottom = CARD.y - 6;
    face.draw(scr, AV_CLIP0, AV_CLIPH);
}

static void drawSphere(Model &m, uint32_t now) {
    drawTop(m, now);
    gaugeLabels(m);
    face.redrawAll();
    drawFace(m);
    drawPanel(m, now);
}

// --- consola a pantalla completa ----------------------------------------------------------------
static void drawBubbleFull(Model &m, uint32_t now) {
    bool speaking = audioPlaying();
    chip(speaking ? "Hermes habla" : "Hermes dice", TEAL, now, speaking);
    useFont(t72m);
    const int LH = 38, x0 = X0 + 22;
    static String lines[60];
    int n = wrap(scr, m.bubbleText, CW - 30, lines, 60);
    const int maxL = (Y1 - Y0) / LH;
    int left = (int)revealed, last = 0;
    for (int i = 0; i < n; i++) { if (left > 0) last = i; left -= utf8Len(lines[i]) + 1; }
    scroll = constrain(max(scroll, last - maxL + 1), 0, max(0, n - maxL));
    left = (int)revealed;
    int shown = min(n, maxL), y0 = Y0 + max(0, (maxL - shown) * LH / 2 - 10);
    int cx = x0, cy = y0;
    for (int i = 0; i < n; i++) {
        String vis = utf8Prefix(lines[i], left);
        left -= utf8Len(lines[i]) + 1;
        if (i >= scroll && i < scroll + maxL && vis.length()) {
            int yy = y0 + (i - scroll) * LH;
            text(vis, x0, yy, INK);
            cx = x0 + scr.textWidth(vis) + 4;
            cy = yy;
        }
    }
    scr.fillSmoothRoundRect(X0, y0 + 2, 5, max(30, shown * LH - 8), 2, TEAL);
    if ((int)revealed <= utf8Len(m.bubbleText) || (now / 500) % 2) scr.fillSmoothRoundRect(cx, cy + 6, 12, 28, 3, TEAL);
}

static Box fullButton(int i, int n) {
    int w = (CW - (n - 1) * 12) / n;
    return {X0 + i * (w + 12), Y1 - 64, w, 60};
}

static void drawCardFull(Model &m, uint32_t now) {
    uint32_t col = kindColor(m.cardKind);
    chip(m.cardTitle.startsWith("Hermes") ? "resultado" : "aviso", col, now);
    int bottom = m.nActions ? Y1 - 80 : Y1;
    scr.fillSmoothRoundRect(X0 - 14, Y0, CW + 28, bottom - Y0, 22, PANEL);
    scr.fillSmoothRoundRect(X0 + 30, Y0, CW - 60, 5, 2, col);
    int ty = Y0 + 28;
    scr.drawSpot(X0 + 20, ty + 18, 24, mix(PANEL, col, 0.22f));
    if (!drawIcon(scr, m.cardIcon, X0 + 2, ty, 2.2f)) scr.drawSpot(X0 + 20, ty + 18, 8, col);
    useFont(t72l);
    text(ellipsize(scr, withoutHermes(m.cardTitle), CW - 60), X0 + 58, ty + 2, col);
    useFont(t72m);
    int y = ty + 60;
    for (int i = 0; i < m.nLines; i++) {
        String part[4];
        int k = wrap(scr, m.cardLines[i], CW - 10, part, 4);
        for (int j = 0; j < k && y + 34 <= bottom - 10; j++, y += 36) text(ellipsize(scr, part[j], CW - 10), X0 + 4, y, 0xD8E0E6u);
        y += 8;
    }
    for (int i = 0; i < m.nActions; i++)
        button(fullButton(i, m.nActions), m.cardActions[i].label, i == 0 ? col : PANEL2, i == 0 ? BG : INK);
}

static void drawToolsFull(Model &m, uint32_t now) {
    static ToolRow rows[MAX_TOOLS];
    int total, n = toolRows(m, rows, total);
    bool running = false;
    int done = 0, errs = 0;
    float sum = 0;
    for (int i = 0; i < n; i++) {
        running |= rows[i].st == 0;
        done += rows[i].st != 0;
        errs += rows[i].st == 2;
        if (rows[i].secs > 0) sum += rows[i].secs;
    }
    String title = toolsTitle(m);
    title.toLowerCase();
    uint32_t col = running ? AMBER : errs ? RED : GREEN;
    chip(title, col, now);
    int hidden0 = total - n;
    float frac = total ? (float)(hidden0 + done) / total : 1;
    const int bw = 220, bx = CX - bw / 2 - 50, by = 118;
    scr.fillSmoothRoundRect(bx, by - 4, bw, 8, 4, PANEL2);
    scr.fillSmoothRoundRect(bx, by - 4, max(8, (int)(bw * frac)), 8, 4, col);
    useFont(t72xs);
    text(String(hidden0 + done) + "/" + total + (sum > 0 ? " · " + secsText(sum) : ""), bx + bw + 14, by + 1, MUTED, textdatum_t::middle_left);

    useFont(t72mono);
    const int CH = 26, charW = max(1, (int)scr.textWidth("0"));
    static String det[MAX_TOOLS][3];
    int nd[MAX_TOOLS], hgt[MAX_TOOLS];
    for (int i = 0; i < n; i++) {
        String d = rows[i].detail;
        if (isTerminal(rows[i].name) && d.length()) d = "$ " + d;
        nd[i] = d.length() ? wrapChars(d, (CW - 40) / charW, det[i], rows[i].st == 0 || i == n - 1 ? 3 : 1) : 0;
        hgt[i] = 50 + nd[i] * CH + 10;
    }
    int first = n, used = 0;
    const int top = Y0 + 6;
    while (first > 0 && used + hgt[first - 1] <= Y1 - top) used += hgt[--first];
    int hidden = total - (n - first);
    int y = top;
    if (hidden > 0) {
        useFont(t72xs);
        text("· · ·  " + String(hidden) + (hidden == 1 ? " anterior" : " anteriores"), CX, y - 4, DIM, textdatum_t::top_center);
        y += 26;
        while (first < n && y + used > Y1) used -= hgt[first++];
    }
    for (int i = first; i < n; i++) {
        const ToolRow &r = rows[i];
        uint32_t ic = r.st == 0 ? AMBER : r.st == 2 ? RED : BLUE;
        uint32_t bg = r.st == 0 ? CONSOLE_RUN : PANEL;
        int hw = min(CW / 2 + 20, halfAt(y, y + hgt[i] - 8));
        int rx0 = CX - hw, rx1 = CX + hw;
        scr.fillSmoothRoundRect(rx0, y, rx1 - rx0, hgt[i] - 8, 16, bg);
        if (r.st == 0) scr.fillSmoothRoundRect(rx0, y + 10, 5, hgt[i] - 28, 2, AMBER);
        statusMark(rx0 + 28, y + 25, r.st, now);
        toolIcon(scr, r.name, rx0 + 52, y + 12, ic, 1.6f);
        String tt = rowTime(r);
        useFont(t72mono);
        int tw = tt.length() ? scr.textWidth(tt) : 0;
        if (tt.length()) text(tt, rx1 - 16, y + 26, r.st == 0 ? AMBER : r.st == 2 ? RED : MUTED, textdatum_t::middle_right);
        useFont(t72m);
        text(ellipsize(scr, r.name, rx1 - 16 - (rx0 + 86) - tw - 12), rx0 + 86, y + 26, r.st == 0 ? INK : SOFT, textdatum_t::middle_left);
        useFont(t72mono);
        bool term = isTerminal(r.name);
        static String fit[3];
        int nf = 0;
        if (nd[i]) {
            String d = r.detail;
            if (term && d.length()) d = "$ " + d;
            nf = wrapChars(d, (rx1 - rx0 - 36) / charW, fit, nd[i]);
        }
        for (int k = 0; k < nf; k++) {
            uint32_t c = r.st == 2 ? 0xE0A0A0u : term ? (r.st == 0 ? 0xA8F0C0u : 0x8CC8A0u) : MUTED;
            text(fit[k], rx0 + 20, y + 50 + k * CH, c);
        }
        y += hgt[i];
    }
}

static void drawConfirm(Model &m, uint32_t now) {
    chip("permiso", AMBER, now);
    subtitle(String((int)((m.confirmUntil > now ? m.confirmUntil - now : 0) / 1000)) + " s para decidir", 116);
    useFont(t72l);
    static String q[3];
    int nq = wrap(scr, m.question, CW, q, 2);
    for (int i = 0; i < nq; i++) text(q[i], CX, Y0 + i * 40, AMBER, textdatum_t::top_center);
    int py = Y0 + nq * 40 + 12;
    const int bottom = Y1 - 82;
    scr.fillSmoothRoundRect(X0 - 12, py, CW + 24, bottom - py, 16, PANEL);
    useFont(t72mono);
    const int charW = max(1, (int)scr.textWidth("0"));
    static String lines[60];
    int n = 0, from = 0;
    const int perLine = (CW - 40) / charW;
    String p = m.preview;
    while (from <= (int)p.length() && n < 60) {
        int nl = p.indexOf('\n', from);
        if (nl < 0) nl = p.length();
        String part = p.substring(from, nl);
        if (!part.length()) lines[n++] = "";
        else n += wrapChars(part, perLine, lines + n, 60 - n);
        from = nl + 1;
    }
    const int maxL = (bottom - py - 20) / 26;
    scroll = constrain(scroll, 0, max(0, n - maxL));
    for (int i = 0; i < maxL && scroll + i < n; i++) {
        if (scroll + i == 0) text("$", X0 + 4, py + 10, GREEN);
        text(lines[scroll + i], X0 + 26, py + 10 + i * 26, INK);
    }
    for (int i = 0; i < m.nOptions; i++) button(fullButton(i, m.nOptions), m.options[i], optionColor(m.options[i]), 0xFFFFFFu);
}

// --- página 1: homelab --------------------------------------------------------------------------
static void dial(int cx, int cy, int r, float pct, uint32_t col, const String &val, const String &lab, const String &sub) {
    const float a0 = 135, span = 270;
    aaArc(cx, cy, r - 16, r, a0, a0 + span, SEG2);
    float a1 = a0 + span * constrain(pct, 0.0f, 100.0f) / 100;
    if (pct > 0.5f) {
        aaArc(cx, cy, r - 16, r, a0, a1, col);
        for (float a : {a0, a1}) {
            float t = a * PI / 180;
            scr.drawSpot(cx + cosf(t) * (r - 8), cy + sinf(t) * (r - 8), 8, col);
        }
        float t = a1 * PI / 180;
        scr.drawSpot(cx + cosf(t) * (r - 8), cy + sinf(t) * (r - 8), 4.5f, mix(col, 0xFFFFFF, 0.5f));
    }
    useFont(t72num);
    text(val, cx, cy - 4, INK, textdatum_t::middle_center);
    useFont(t72xs);
    text(lab, cx, cy + 34, MUTED, textdatum_t::middle_center);
    if (sub.length()) text(sub, cx, cy + r - 6, DIM, textdatum_t::middle_center);
}

static void drawLab(Model &m, uint32_t now, bool dialsOnly = false) {
    Dash &d = m.dash;
    if (!dialsOnly) {
        chip(d.host.length() ? d.host : String("homelab"), d.have ? GREEN : DIM, now);
        if (d.up.length()) subtitle("encendida " + d.up, 116);
    }
    if (!d.have) {
        useFont(t72m);
        text("esperando datos del servidor…", CX, CY, DIM, textdatum_t::middle_center);
        return;
    }
    float cpu = sCpu.step(d.cpu), gpu = sGpu.step(max(0.0f, d.gpu)), ram = sRam.step(d.ram);
    float tf = sTemp.step(d.temp > 0 ? constrain((d.temp - 30) / 60, 0.0f, 1.0f) * 100 : 0);
    char gb[24];
    snprintf(gb, sizeof(gb), "%.0f / %.0f GB", d.ramGb, d.ramTotal);
    dial(246, 256, 96, cpu, loadColor(cpu), String((int)(d.cpu + 0.5f)) + "%", "CPU", "");
    dial(474, 256, 96, gpu, VIOLET, d.gpu >= 0 ? String((int)(d.gpu + 0.5f)) + "%" : "—", "GPU", "");
    dial(246, 464, 96, ram, BLUE, String((int)(d.ram + 0.5f)) + "%", "RAM", gb);
    dial(474, 464, 96, tf, d.temp > 0 ? tempColor(d.temp) : DIM, d.temp > 0 ? String((int)(d.temp + 0.5f)) + "°C" : "—", "TEMP", "");
    if (dialsOnly) return;
    // servicios: dos filas de cápsulas centradas
    int rows[2][6], nr[2] = {0, 0};
    for (int i = 0; i < d.nSvc; i++) { int r = i < 3 ? 0 : 1; rows[r][nr[r]++] = i; }
    for (int r = 0; r < 2; r++) {
        int w = 0;
        for (int k = 0; k < nr[r]; k++) w += pillW(d.svcName[rows[r][k]]) + (k ? 10 : 0);
        int x = CX - w / 2, y = 584 + r * 44;
        for (int k = 0; k < nr[r]; k++) {
            int i = rows[r][k];
            pill(x, y, d.svcName[i], d.svcOk[i] ? GREEN : RED);
            x += pillW(d.svcName[i]) + 10;
        }
    }
}

// --- página 2: hoy (agenda y redes) ---------------------------------------------------------------
static void ytLogo(int x, int y, float k) {
    scr.fillSmoothRoundRect(x, y, 26 * k, 18 * k, 5 * k, YT_RED);
    scr.fillTriangle(x + 10 * k, y + 5 * k, x + 10 * k, y + 13 * k, x + 18 * k, y + 9 * k, 0xFFFFFF);
}

static void tile(Box b, uint32_t accent, bool have, const char *name, long big, const char *unit, long delta,
                 const String &foot, bool yt) {
    scr.fillSmoothRoundRect(b.x, b.y, b.w, b.h, 24, PANEL);
    scr.fillSmoothRoundRect(b.x + 30, b.y, b.w - 60, 4, 2, accent);
    if (yt) ytLogo(b.x + 22, b.y + 22, 1.2f);
    else if (!drawIcon(scr, "instagram", b.x + 22, b.y + 18, 1.8f)) scr.drawSpot(b.x + 36, b.y + 34, 10, accent);
    useFont(t72xs);
    text(name, b.x + 64, b.y + 34, MUTED, textdatum_t::middle_left);
    if (!have) {
        useFont(t72s);
        text("sin conectar", b.x + b.w / 2, b.y + b.h / 2 + 10, DIM, textdatum_t::middle_center);
        return;
    }
    useFont(t72num);
    text(fmtCount(big), b.x + 22, b.y + 62, INK);
    useFont(t72xs);
    text(unit, b.x + 24, b.y + 116, MUTED);
    if (delta) {                                       // en la cabecera, a la derecha: abajo no cabe junto a la unidad
        useFont(t72xs);
        text((delta > 0 ? "+" : "") + fmtCount(delta) + " hoy", b.x + b.w - 18, b.y + 34, delta > 0 ? GREEN : RED, textdatum_t::middle_right);
    }
    if (foot.length()) {
        useFont(t72xs);
        text(ellipsize(scr, foot, b.w - 40), b.x + 22, b.y + b.h - 34, SOFT);
    }
}

static void drawDay(Model &m, uint32_t now) {
    Dash &d = m.dash;
    chip("hoy · " + dateText(), BLUE, now);
    int y = 124;
    if (!d.have) {
        useFont(t72s);
        text("esperando datos…", CX, 240, DIM, textdatum_t::middle_center);
    } else if (!d.agendaOk) {
        useFont(t72m);
        text("agenda sin conectar", CX, 220, DIM, textdatum_t::middle_center);
        useFont(t72xs);
        text(d.agendaWhy, CX, 256, DIM, textdatum_t::middle_center);
    } else if (!d.nAgenda) {
        useFont(t72m);
        text("nada en la agenda hoy", CX, 240, SOFT, textdatum_t::middle_center);
    } else {
        const int RH = 58;
        for (int i = 0; i < min(d.nAgenda, 4); i++, y += RH) {
            const AgendaItem &e = d.agenda[i];
            int hw = halfAt(y, y + RH - 8) - 10;
            int x0 = CX - hw, x1 = CX + hw;
            uint32_t bg = e.now ? mix(BG, TEAL, 0.14f) : PANEL;
            scr.fillSmoothRoundRect(x0, y, x1 - x0, RH - 8, 16, bg);
            if (e.now) scr.fillSmoothRoundRect(x0, y + 10, 5, RH - 28, 2, TEAL);
            useFont(t72s);
            String h = e.h == "todo el día" ? "día" : e.h;
            text(h, x0 + 22, y + (RH - 8) / 2 + 1, e.now ? TEAL : MUTED, textdatum_t::middle_left);
            useFont(t72m);
            text(ellipsize(scr, e.title, x1 - x0 - 120), x0 + 104, y + (RH - 8) / 2 + 1, e.now ? INK : SOFT, textdatum_t::middle_left);
        }
        if (d.agendaLeft > 4) {
            useFont(t72xs);
            text("+" + String(d.agendaLeft - 4) + " más", CX, y + 2, DIM, textdatum_t::top_center);
        }
    }
    String ytFoot = d.ytLast.length() ? "último: " + fmtCount(d.ytLastViews) + " vistas" : "";
    tile({138, 402, 214, 200}, YT_RED, d.yt, "YOUTUBE", d.ytSubs, "suscriptores", d.ytD, ytFoot, true);
    tile({368, 402, 214, 200}, IG_PINK, d.ig, "INSTAGRAM", d.igFollowers, "seguidores", d.igD,
         d.ig ? String(d.igPosts) + " publicaciones" : "", false);
}

// --- página 3: el grafo de Obsidian ---------------------------------------------------------------
static const uint32_t GROUP_COL[] = {0x7AA8FF, 0x5CF08A, 0xFFB020, 0xFF7AA8, 0x40E0C0, 0xC4CED6, 0xFFD780, 0xFF8C5A};
static int16_t gsx[MAX_GNODES], gsy[MAX_GNODES];
static const int G_X0 = 130, G_X1 = SCR_W - 130, G_Y0 = 150, G_Y1 = 580;
static uint32_t groupCol(int g) { return g < 0 ? 0x3A4854u : GROUP_COL[g % 8]; }
static int nodeR(const GraphNode &n) { return 4 + min((int)n.deg, 14) * 2 / 3; }

static void drawGraph(Model &m, uint32_t now) {
    chip("obsidian", VIOLET, now);
    if (m.gN) subtitle(String(m.gNotes) + " notas · " + m.gLinks + " enlaces", 116);
    if (!m.gN) {
        useFont(t72m);
        text("esperando el grafo…", CX, CY, DIM, textdatum_t::middle_center);
        return;
    }
    for (int i = 0; i < m.gN; i++) {
        gsx[i] = G_X0 + (int32_t)m.gNodes[i].x * (G_X1 - G_X0) / 1000;
        gsy[i] = G_Y0 + (int32_t)m.gNodes[i].y * (G_Y1 - G_Y0) / 1000;
    }
    static bool nb[MAX_GNODES];
    for (int i = 0; i < m.gN; i++) nb[i] = false;
    if (graphSel >= 0)
        for (int e = 0; e < m.gE; e++) {
            if (m.gEdges[e][0] == graphSel) nb[m.gEdges[e][1]] = true;
            if (m.gEdges[e][1] == graphSel) nb[m.gEdges[e][0]] = true;
        }
    for (int pass = 0; pass < 2; pass++)
        for (int e = 0; e < m.gE; e++) {
            int a = m.gEdges[e][0], b = m.gEdges[e][1];
            bool hot = graphSel >= 0 && (a == graphSel || b == graphSel);
            if (hot != (pass == 1)) continue;
            uint32_t c = hot ? VIOLET : graphSel >= 0 ? 0x141E28u : 0x2A3A48u;
            scr.drawWideLine(gsx[a], gsy[a], gsx[b], gsy[b], 0.7f, c);
            if (hot) scr.drawWideLine(gsx[a] + 1, gsy[a], gsx[b] + 1, gsy[b], 0.7f, c);
        }
    for (int i = 0; i < m.gN; i++) {
        int r = nodeR(m.gNodes[i]);
        bool dim = graphSel >= 0 && i != graphSel && !nb[i];
        if (i == graphSel) scr.drawSpot(gsx[i], gsy[i], r + 6, 0x3A2E5Cu);
        scr.drawSpot(gsx[i], gsy[i], r, dim ? 0x26323Cu : groupCol(m.gNodes[i].group));
    }
    static int order[MAX_GNODES];
    for (int i = 0; i < m.gN; i++) order[i] = i;
    std::sort(order, order + m.gN, [&](int a, int b) { return m.gNodes[a].deg > m.gNodes[b].deg; });
    if (graphSel >= 0) std::stable_partition(order, order + m.gN, [&](int i) { return i == graphSel; });
    useFont(t72xs);
    static Box placed[MAX_GNODES];
    int np = 0, shown = 0;
    for (int k = 0; k < m.gN; k++) {
        int i = order[k];
        bool want = graphSel >= 0 ? (i == graphSel || nb[i]) : shown < 14 && m.gNodes[i].deg > 0;
        if (!want) continue;
        String l = m.gNodes[i].label;
        if (utf8Len(l) > 20) l = utf8Prefix(l, 19) + "…";
        int w = scr.textWidth(l) + 12, h = 24, r = nodeR(m.gNodes[i]);
        int lx = constrain(gsx[i] - w / 2, X0 - 10, X1 + 10 - w);
        Box cand[2] = {{lx, gsy[i] + r + 3, w, h}, {lx, gsy[i] - r - 3 - h, w, h}};
        int pick = -1;
        for (int c = 0; c < 2 && pick < 0; c++) {
            Box b = cand[c];
            if (b.y < 134 || b.y + b.h > 600) continue;
            bool clash = false;
            for (int q = 0; q < np && !clash; q++)
                clash = b.x < placed[q].x + placed[q].w && placed[q].x < b.x + b.w && b.y < placed[q].y + placed[q].h &&
                        placed[q].y < b.y + b.h;
            if (!clash) pick = c;
        }
        if (pick < 0 && i == graphSel) pick = 0;
        if (pick < 0) continue;
        Box b = cand[pick];
        placed[np++] = b;
        shown++;
        scr.fillSmoothRoundRect(b.x, b.y, b.w, b.h, 6, i == graphSel ? 0x2A2248u : 0x0D151Cu);
        text(l, b.x + 6, b.y + 3, i == graphSel ? INK : 0xAEBAC4u);
    }
    if (graphSel >= 0) {
        useFont(t72m);
        text(ellipsize(scr, m.gNodes[graphSel].label, 380), CX, 628, INK, textdatum_t::middle_center);
    }
}

static int graphHit(Model &m, int px, int py) {
    int best = -1, bd = 34 * 34;
    for (int i = 0; i < m.gN; i++) {
        int dx = gsx[i] - px, dy = gsy[i] - py, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// --- splash ---------------------------------------------------------------------------------
void uiSplash(const char *l1, const char *l2) {
    scr.fillScreen(BG);
    face.disconnected = true;
    face.step(33, millis());
    face.redrawAll();
    face.draw(scr, AV_CLIP0, AV_CLIPH);
    arc(RING_OUT - 6, RING_OUT - 2, 0, 360, SEG2);
    useFont(t72l);
    text(ellipsize(scr, l1, 400), CX, 96, TEAL, textdatum_t::middle_center);
    useFont(t72s);
    static String ls[3];
    int n = wrap(scr, l2, 420, ls, 3);
    for (int i = 0; i < n; i++) text(ls[i], CX, CARD.y + 30 + i * 30, MUTED, textdatum_t::top_center);
    lcd->present(scr);
}

// --- toques y botón BOOT ---------------------------------------------------------------------
static struct { bool down = false; int x0, y0, x, y; uint32_t t0; bool ptt = false; bool swallow = false; } tch;
static bool bootPtt = false;

// --- pantalla que duerme: baja el brillo a los 45 s sin nada, se apaga a los 2 min --------------
enum Screen : uint8_t { SCR_ON, SCR_DIM, SCR_OFF };
static Screen screen = SCR_ON;
static const uint8_t BRIGHT = 210, DIMMED = 40;
static uint32_t lastActive = 0;

static bool wake() {                 // devuelve true si estaba apagada (ese toque no cuenta)
    bool wasOff = screen == SCR_OFF;
    if (screen != SCR_ON) lcd->setBrightness(BRIGHT);
    screen = SCR_ON;
    lastActive = millis();
    return wasOff;
}

static void send(JsonDocument &d) {
    String s;
    serializeJson(d, s);
    netSend(s);
}

static void sendTouch(const char *target, int x, int y, const char *gesture) {
    JsonDocument d;
    d["type"] = "touch"; d["target"] = target; d["x"] = x; d["y"] = y; d["gesture"] = gesture;
    send(d);
}

static bool onFace(Model &m, int x, int y) {
    if (page != P_SPHERE || expanded || m.confirm) return false;
    return y > AV_TOP - 10 && y < CARD.y - 4 && abs(x - CX) < 150;
}

static void startPtt(Model &m) {
    m.recording = true;
    audioChirp(true);
    netMicStart();
}
static void stopPtt(Model &m) {
    m.recording = false;
    netMicStop();
    audioChirp(false);
}

static void handleTouch(Model &m, uint32_t now) {
    bool boot = digitalRead(0) == LOW;                   // BOOT también es pulsar para hablar
    if (boot && !bootPtt && !m.recording && !m.confirm) {
        wake();
        bootPtt = true;
        startPtt(m);
        setPage(P_SPHERE);
        expanded = false;
    } else if (!boot && bootPtt) {
        bootPtt = false;
        stopPtt(m);
    }
    lgfx::touch_point_t tp;
    if (lcd->getTouch(&tp)) {
        if (!tch.down) {
            bool woke = wake();
            tch = {true, tp.x, tp.y, tp.x, tp.y, now, false, woke};
        }
        tch.x = tp.x; tch.y = tp.y;
        lastActive = millis();
        if (!tch.swallow && onFace(m, tch.x0, tch.y0) && !tch.ptt && !bootPtt && now - tch.t0 > PTT_MS &&
            abs(tch.x - tch.x0) < 24 && abs(tch.y - tch.y0) < 24) {
            tch.ptt = true;
            startPtt(m);
            face.set(m.emotion.c_str(), "look_left");
        }
        return;
    }
    if (!tch.down) return;
    tch.down = false;
    int x = tch.x, y = tch.y, dx = x - tch.x0, dy = y - tch.y0;
    uint32_t dt = now - tch.t0;
    if (tch.ptt) { stopPtt(m); return; }
    if (tch.swallow) return;                             // solo despertó la pantalla
    if (abs(dx) > 70 && abs(dx) > abs(dy)) {            // desliza: otra página (o cierra la consola)
        if (m.confirm) return;
        if (expanded) { expanded = false; return; }
        setPage(page + (dx < 0 ? 1 : -1));
        return;
    }
    if (abs(dy) > 50) {                                  // en vertical: scroll; hacia abajo desde arriba, cierra
        if (expanded && dy > 0 && tch.y0 < 200 && scroll == 0) { expanded = false; return; }
        scroll = max(0, scroll + (dy < 0 ? 3 : -3));
        return;
    }
    if (m.confirm) {
        for (int i = 0; i < m.nOptions; i++)
            if (inside(fullButton(i, m.nOptions), x, y)) {
                JsonDocument d;
                d["type"] = "confirm_reply"; d["id"] = m.confirmId; d["choice"] = m.options[i];
                send(d);
                return;
            }
        return;
    }
    if (expanded) {
        if (m.card && !m.bubble && m.cardKind != "tools")
            for (int i = 0; i < m.nActions; i++)
                if (inside(fullButton(i, m.nActions), x, y)) {
                    String t = "card:" + m.cardId + ":" + m.cardActions[i].id;
                    sendTouch(t.c_str(), x, y, "tap");
                    return;
                }
        expanded = false;                                // tocar en cualquier otro sitio la cierra
        return;
    }
    if (page == P_GRAPH) {
        int hit = graphHit(m, x, y);
        graphSel = hit == graphSel ? -1 : hit;
        return;
    }
    if (page != P_SPHERE) return;
    const char *gesture = dt > 600 ? "long_press" : "tap";
    if (inside(CARD, x, y)) {                            // el panel: abre la consola si hay algo de Hermes
        if (hasNews(m)) {
            collapsed = false;
            expanded = true;
            tExpanded = now;
            scroll = 0;
        }
        return;
    }
    if (onFace(m, tch.x0, tch.y0)) {
        face.play(S_WAVE, 1);
        sendTouch("face", x, y, gesture);
    }
}

// --- bucle ------------------------------------------------------------------------------------
void uiBegin(LGFX &l) {
    lcd = &l;
    pinMode(0, INPUT_PULLUP);
    scr.setPsram(true);
    scr.setColorDepth(16);
    scr.createSprite(SCR_W, SCR_H);
    scr.fillScreen(BG);
}

bool uiFrozen = false;          // diagnóstico (serie x): no se manda nada a la pantalla
static uint32_t frameMsSum = 0, frames = 0;
static uint32_t tm[6] = {};

void uiFrame(uint32_t now) {
    if (uiFrozen) return;
    static uint32_t last = now;
    static bool wasServer = false;
    uint32_t dtFrame = now - last;
    uint32_t t0 = micros();
    modelLock();
    Model &m = model;
    handleTouch(m, now);
    face.listening = m.recording;
    face.confirming = m.confirm;
    face.set(m.emotion.c_str(), m.animation.c_str());
    m.lookNew = false;
    m.animation = "";
    face.disconnected = !m.server;
    if (m.server && !wasServer) face.play(S_WAVE, 2);
    wasServer = m.server;
    face.step(now - last, now);
    last = now;

    if (m.splash1.length()) {
        String a = m.splash1, b = m.splash2;
        modelUnlock();
        uiSplash(a.c_str(), b.c_str());
        return;
    }
    // actividad: algo nuevo, micro, voz, herramientas o confirmación
    static uint32_t seenVersion = 0;
    bool active = m.version != seenVersion || m.recording || audioPlaying() || m.confirm || toolsRunning(m) || tch.down;
    if (m.version != seenVersion) { tNews = now; collapsed = false; }
    seenVersion = m.version;
    if (active) wake();
    uint32_t quieta = millis() - lastActive;     // con millis(): wake() la pone por delante de now
    if (screen == SCR_ON && quieta > 45000) { screen = SCR_DIM; lcd->setBrightness(DIMMED); }
    if (screen == SCR_DIM && quieta > 120000) {
        screen = SCR_OFF;
        lcd->setBrightness(0);
        setPage(P_SPHERE);                              // al despertar, con el avatar
        expanded = false;
    }
    if (screen == SCR_OFF) { modelUnlock(); return; }

    // lo nuevo de Hermes sube como tarjeta en la esfera
    bool fresh = false;
    if (m.bubble && m.bubbleId != animBubble) { animBubble = m.bubbleId; revealed = 0; scroll = 0; fresh = true; }
    if (m.card && m.cardId != animCard) { animCard = m.cardId; fresh = true; }
    if (m.confirm && m.confirmId != animConfirm) { animConfirm = m.confirmId; scroll = 0; }
    if (fresh) { tCard = now; collapsed = false; if (!m.recording && !expanded) setPage(P_SPHERE); }
    if (m.bubble) revealed += dtFrame / 1000.0f * LETTERS_PER_S;
    if (m.confirm && now > m.confirmUntil) { m.confirm = false; m.version++; }
    bool typing = m.bubble && (int)revealed <= utf8Len(m.bubbleText) + 1;
    // la tarjeta se recoge en el aviso de abajo a los 12 s quieta; la consola abierta se cierra a los 25 s
    if (!collapsed && hasNews(m) && !typing && !toolsRunning(m) && !audioPlaying() && now - tNews > 12000) collapsed = true;
    if (expanded && (!hasNews(m) || (!typing && !toolsRunning(m) && now - max(tNews, tExpanded) > 25000))) expanded = false;

    static String loggedCard, loggedConfirm;
    if (m.card && m.cardId != "oido" && m.cardId + m.cardKind + m.cardTitle != loggedCard) {   // «Te entendí» no es una tarea
        loggedCard = m.cardId + m.cardKind + m.cardTitle;
        bool err = false;
        for (int i = 0; i < m.nTools; i++) err |= m.tools[i].st == 2;
        String t = withoutHermes(m.cardTitle);
        if (m.cardKind == "tools") logPush(0, toolsTitle(m));
        else if (t.indexOf("listo") >= 0) logPush(err ? 2 : 1, t);
        else logPush(1, t);
    }
    if (m.confirm && m.confirmId != loggedConfirm) { loggedConfirm = m.confirmId; logPush(3, "permiso: " + m.question); }

    // Por capas, para no mover 1 MB de PSRAM en cada cuadro (el DPI lee de la misma PSRAM y, si no le
    // llega a tiempo, pinta rayas): todo entero solo cuando cambia lo que se ve; en cada cuadro, el
    // anillo (y los arcos de la esfera), el avatar si se movió, su cartel y la tarjeta si está animándose.
    uint32_t a0 = micros();
    bool sphere = page == P_SPHERE && !expanded && !m.confirm;
    bool typing2 = m.bubble && (int)revealed <= utf8Len(m.bubbleText) + 1;
    bool speaking = m.bubble && audioPlaying();
    bool anim = typing2 || speaking || toolsRunning(m) || now - tCard < 420;
    static String key;
    // el tablero llega cada 5 s: solo la página «hoy» se repinta entera si cambió lo que enseña
    String dayKey;
    if (page == P_DAY) {
        Dash &d = m.dash;
        dayKey = String(d.have) + d.agendaOk + d.agendaWhy + d.nAgenda + d.yt + d.ytSubs + d.ytD + d.ytLastViews + d.ig + d.igFollowers + d.igD;
        for (int i = 0; i < d.nAgenda; i++) dayKey += d.agenda[i].h + d.agenda[i].title + d.agenda[i].now;
    }
    if (page == P_LAB) {
        Dash &d = m.dash;
        dayKey = String(d.have) + d.up + d.host;
        for (int i = 0; i < d.nSvc; i++) dayKey += d.svcName[i] + d.svcOk[i];
    }
    static uint32_t seenDash = 0;
    bool dashNew = m.dash.version != seenDash;
    seenDash = m.dash.version;
    String k = String(page) + expanded + m.confirm + m.version + "," + dayKey + "," + m.gVersion + "," + graphSel +
               "," + scroll + uiTestLabel + collapsed + cardShown(m) + m.recording + m.wifi + m.server + logN + clockText();
    bool full = k != key;
    static uint32_t lastAnim = 0, lastBlink = 0;
    if (!sphere && (anim || m.confirm) && now - lastAnim >= 50) full = true;         // consola y permiso: enteros
    if (!sphere && expanded && now / 500 != lastBlink) full = true;                  // el cursor
    bool cardAnim = sphere && hasNews(m) && !collapsed && (anim || now / 500 != lastBlink);
    lastBlink = now / 500;
    if (full) { key = k; lastAnim = now; }
    bool faceDrew = false, labelDrew = false, labDrew = false, numsDrew = false;

    if (full) {
        scr.fillScreen(BG);
        if (m.confirm) drawConfirm(m, now);
        else if (expanded && hasNews(m)) {
            if (m.bubble) drawBubbleFull(m, now);
            else if (m.cardKind == "tools") drawToolsFull(m, now);
            else drawCardFull(m, now);
        } else {
            if (page == P_SPHERE) drawSphere(m, now);
            else if (page == P_LAB) drawLab(m, now);
            else if (page == P_DAY) drawDay(m, now);
            else drawGraph(m, now);
            pageDots();
        }
    } else if (sphere) {
        if (dashNew) {                                   // los números de los lados
            scr.fillRect(NUM_L.x, NUM_L.y, NUM_L.w, NUM_L.h, BG);
            scr.fillRect(NUM_R.x, NUM_R.y, NUM_R.w, NUM_R.h, BG);
            gaugeLabels(m);
            numsDrew = true;
        }
        if (face.dirty()) { drawFace(m); faceDrew = face.changed.w > 0; }
        static int panelSt = -1, panelPh = -1;
        bool stChanged = face.current() != panelSt || face.phase() != panelPh;
        if (cardAnim || stChanged) {
            panelSt = face.current();
            panelPh = face.phase();
            scr.fillRect(CARD.x, CARD.y, CARD.w, CARD.h, BG);
            drawPanel(m, now);
            labelDrew = true;
        }
    } else if (page == P_LAB && !expanded && !m.confirm) {
        // los relojes se deslizan hacia el valor nuevo: solo mientras se mueven
        static uint32_t lastLab = 0;
        Dash &d = m.dash;
        bool moving = d.have && (fabsf(sCpu.v - d.cpu) > 0.3f || fabsf(sRam.v - d.ram) > 0.3f || fabsf(sGpu.v - max(0.0f, d.gpu)) > 0.3f);
        if ((moving || dashNew) && now - lastLab >= 66) { lastLab = now; scr.fillRect(140, 150, 440, 420, BG); drawLab(m, now, true); labDrew = true; }
    }
    uint32_t a1 = micros();
    // capa del anillo: la banda del borde y, en la esfera, los tramos de los arcos de medida
    RingMode mode = R_STATIC;
    uint32_t rcol = linkColor(m);
    float frac = 1;
    static float level = 0;
    level += (audioMouth() - level) * (audioMouth() > level ? 0.15f : 0.06f);
    if (m.confirm) {
        mode = R_COUNTDOWN;
        rcol = AMBER;
        frac = m.confirmUntil > now ? (float)(m.confirmUntil - now) / m.confirmTotal : 0;
    } else {
        SpriteState st = face.current();
        if (m.recording || st == S_LISTENING) { mode = R_PULSE; rcol = 0xFF4060u; }
        else if (st == S_APPROVAL) { mode = R_PULSE; rcol = AMBER; }
        else if (st == S_TALKING || audioPlaying()) { mode = R_VOICE; rcol = TEAL; }
        else if (st == S_THINKING || st == S_WORKING || st == S_READING || st == S_TYPING || st == S_SEARCHING ||
                 st == S_EXECUTING || toolsRunning(m)) { mode = R_COMET; rcol = st == S_THINKING ? VIOLET : AMBER; }
        else if (st == S_ERROR) rcol = RED;
        else if (st == S_SUCCESS) rcol = GREEN;
    }
    static String ringKey;
    String rk = String(mode) + rcol;
    bool gaugesMoving = sphere && m.dash.have && (fabsf(sCpu.v - m.dash.cpu) > 0.3f || fabsf(sRam.v - m.dash.ram) > 0.3f ||
                                                  fabsf(sGpu.v - max(0.0f, m.dash.gpu)) > 0.3f);
    static uint32_t lastRing = 0;
    bool ringDirty = full || rk != ringKey || (mode != R_STATIC && now - lastRing >= 33);
    bool gaugesDirty = sphere && (full || gaugesMoving);
    if (gaugesDirty) {
        static uint32_t lastG = 0;
        if (full || now - lastG >= 50) {               // se deslizan a 20 fps, de sobra
            lastG = now;
            arc(GB_IN - 4, GA_OUT + 5, L0 - 6, L0 + SPAN + 6, BG);
            arc(GB_IN - 4, GA_OUT + 5, R0 - SPAN - 6, R0 + 6, BG);
            gaugeArcs(m);
        } else gaugesDirty = false;
    }
    if (ringDirty) {
        ringKey = rk;
        lastRing = now;
        arc(RING_IN - 1, RING_OUT + 2, 0, 360, BG);
        ringLive(mode, rcol, now, level, frac);
    }
    uint32_t a2 = micros();
    modelUnlock();
    // a la pantalla solo lo que cambió
    if (full) lcd->present(scr);
    else {
        if (ringDirty || gaugesDirty) flushRing(gaugesDirty ? GB_IN - 4 : RING_IN - 1);
        if (faceDrew) lcd->flush(scr, face.changed.x, face.changed.y, face.changed.w, face.changed.h);
        if (labelDrew) lcd->flush(scr, CARD.x, CARD.y, CARD.w, CARD.h);
        if (labDrew) lcd->flush(scr, 140, 150, 440, 420);
        if (numsDrew) {
            lcd->flush(scr, NUM_L.x, NUM_L.y, NUM_L.w, NUM_L.h);
            lcd->flush(scr, NUM_R.x, NUM_R.y, NUM_R.w, NUM_R.h);
        }
    }
    uint32_t a3 = micros();
    tm[0] += full; tm[1] += a1 - a0; tm[2] += a2 - a1; tm[3] += a3 - a2;
    frameMsSum += (micros() - t0) / 1000;
    if (++frames == 300) {
        Serial.printf("ui: %u ms por cuadro · %u enteros · pintar %u · anillo %u · present %u us\n", frameMsSum / frames,
                      tm[0], tm[1] / frames, tm[2] / frames, tm[3] / frames);
        frames = frameMsSum = 0;
        memset(tm, 0, sizeof(tm));
    }
}

// carta de ajuste (serie P): rayas de 1, 2 y 4 píxeles, damero de 1 y texto pequeño; la UI se congela
void uiTestPattern() {
    extern bool uiFrozen;
    uiFrozen = true;
    scr.fillScreen(0x000000);
    for (int y = 120; y < 340; y++)
        for (int x = 120; x < 340; x++) {
            bool on = x & 1;                                       // 1 px vertical
            if (x >= 230) on = y & 1;                              // 1 px horizontal
            if (y >= 230) on = x >= 230 ? ((x / 2) & 1) : ((x ^ y) & 1);   // 2 px vertical · damero
            scr.drawPixel(x, y, on ? 0xFFFFFFu : 0x000000u);
        }
    for (int y = 120; y < 340; y++)
        for (int x = 380; x < 600; x++) {
            bool on = y < 230 ? ((x / 4) & 1) : (x >= 490 ? ((y / 4) & 1) : true);
            uint32_t c = y >= 230 && x < 490 ? ((x - 380) * 255 / 110) * 0x010101u : (on ? 0xFFFFFFu : 0);
            scr.drawPixel(x, y, c);
        }
    useFont(t72xs);
    text("1 px  |   1 px =", 230, 104, MUTED, textdatum_t::middle_center);
    text("4 px  |   degradado", 490, 104, MUTED, textdatum_t::middle_center);
    text("damero 1 px  |  2 px", 230, 356, MUTED, textdatum_t::middle_center);
    text("degradado  |  4 px =", 490, 356, MUTED, textdatum_t::middle_center);
    const uint8_t *fs[] = {t72xs, t72s, t72m, t72l};
    int y = 400;
    for (auto f : fs) {
        useFont(f);
        text("Hermes 0123456789 áéíóú", CX, y, INK, textdatum_t::top_center);
        y += scr.fontHeight() + 8;
    }
    for (int r = 0; r < 6; r++) scr.drawCircle(CX, 640, 8 + r * 6, r & 1 ? 0x40E0C0u : 0xFFFFFFu);
    lcd->present(scr);
}

void uiKey(char c) {
    if (c == 'g') setPage(page + 1);                     // otra página
    else if (c == 'n') {                                  // tocar la nota más conectada (o soltarla)
        if (page != P_GRAPH) setPage(P_GRAPH);
        int best = -1;
        for (int i = 0; i < model.gN; i++) if (best < 0 || model.gNodes[i].deg > model.gNodes[best].deg) best = i;
        graphSel = graphSel >= 0 ? -1 : best;
    } else if (c == 'e') expanded = !expanded;            // abrir o cerrar la consola
}

void uiDump() {
    // el sprite ya guarda big-endian, como espera tools/shot.py
    Serial.printf("SHOT %d %d\n", SCR_W, SCR_H);
    const uint8_t *p = (const uint8_t *)scr.getBuffer();
    for (int y = 0; y < SCR_H; y++) Serial.write(p + y * SCR_W * 2, SCR_W * 2);
    Serial.flush();
}
#endif
