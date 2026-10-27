// Composición redonda de 466x466 (Waveshare AMOLED 1.75): una página a la vez, se cambia deslizando
//   0 · el avatar en grande con su cartel («Buscando», «¿Me autorizas?»…) y el anillo de estado en el borde
//   1 · consola: lo que dice Hermes, la tarjeta, las herramientas o la confirmación; en reposo, el panel
//   2 · grafo de Obsidian
// Lo nuevo de Hermes salta solo a la consola y, si nadie la toca, vuelve con el avatar a los 8 s (salvo con
// herramientas en marcha o una confirmación). La cuenta atrás de la confirmación es el anillo del borde.
// Un sprite de pantalla completa en PSRAM; flush() manda al CO5300 solo el trozo que cambió.
#ifdef ROUND_UI
#include <ArduinoJson.h>
#include "audio/audio.h"
#include "character/avatar.h"
#include "common.h"
#include "config.h"
#include "fonts/t43l.h"
#include "fonts/t43m.h"
#include "fonts/t43mono.h"
#include "fonts/t43s.h"
#include "fonts/t43xs.h"
#include "fonts/t175clk.h"
#include "model.h"
#include "net/net.h"
#include "ui.h"
#include <WiFi.h>
#include <algorithm>

static LGFX *lcd;
static LGFX_Sprite scr;
static Avatar face;
static const int CX = SCR_W / 2, CY = SCR_H / 2;
static const uint32_t BG = 0x05080C, INK = 0xE8EEF2, DIM = 0x3A4854, MUTED = 0x8090A0;
static const uint32_t AMBER = 0xFFB020, GREEN = 0x5CF08A, RED = 0xFF6A6A, BLUE = 0x7AA8FF;
static const uint32_t TEAL = 0x40E0C0, SEG = 0x111A22, SEG2 = 0x1A2630, LINE = 0x1E2A34, VIOLET = 0xA88BFA;
static const uint32_t CONSOLE_RUN = 0x16202A;

// la zona útil de la consola: un rectángulo que cabe en el círculo
static const int X0 = 78, X1 = SCR_W - 78, CW = X1 - X0, Y0 = 112, Y1 = 396;
// el avatar (×2,5 = 262x305) y su cartel
static const int AV_Y0 = 60, AV_H = 375;                    // avatar43 lo deja 64 px encima del final
static const int LABEL_W = 236, LABEL_H = 42, LABEL_X = CX - LABEL_W / 2, LABEL_Y = 380;
static const int DOTS_Y = 446;

static const uint8_t *fontNow = nullptr;
static void useFont(const uint8_t *f) {
    if (fontNow == f) return;
    scr.unloadFont();
    scr.loadFont(f);
    fontNow = f;
}

static void flushAll() { lcd->flush(scr, 0, 0, SCR_W, SCR_H); }

// Parte por letras (el texto mono no tiene por qué tener espacios: rutas, URLs, JSON)
static int wrapChars(const String &text, int perLine, String *out, int maxLines) {
    int n = 0;
    size_t i = 0;
    while (i < text.length() && n < maxLines) {
        size_t j = i;
        int c = 0;
        while (j < text.length() && c < perLine) {
            j++;
            while (j < text.length() && (text[j] & 0xC0) == 0x80) j++;
            c++;
        }
        if (j < text.length()) {
            int sp = text.lastIndexOf(' ', j);
            if (sp > (int)i + perLine / 2) j = sp + 1;
        }
        out[n++] = text.substring(i, j);
        i = j;
    }
    if (i < text.length() && n) {
        String &l = out[n - 1];
        while (utf8Len(l) > perLine - 1) {
            l.remove(l.length() - 1);
            while (l.length() && (l[l.length() - 1] & 0xC0) == 0x80) l.remove(l.length() - 1);
        }
        l += "…";
    }
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

static String clockText() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "--:--";
    struct tm t;
    localtime_r(&now, &t);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    return b;
}

static String withoutHermes(String t) {
    if (t.startsWith("Hermes · ")) t = t.substring(9);
    else if (t.startsWith("Hermes ")) t = t.substring(7);
    if (t.length()) t.setCharAt(0, toupper(t[0]));
    return t;
}

struct Box { int x, y, w, h; };
static bool inside(Box b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static Box rowButton(int i, int n) {             // botones al pie de la consola
    int w = (CW - (n - 1) * 10) / n;
    return {X0 + i * (w + 10), Y1 - 60, w, 58};
}

// --- estado ------------------------------------------------------------------------------
static int page = 0;                       // 0 el avatar · 1 consola · 2 grafo
static bool autoPage = false;              // la consola la abrió algo nuevo, no un dedo
static uint32_t tAuto = 0;
static bool pageDirty = true;
static int scroll = 0, drawnScroll = -1;
static uint32_t drawnVersion = UINT32_MAX, lastPanel = 0;
static int graphSel = -1;
static bool graphDirty = true;

static const float LETTERS_PER_S = 32;
static float revealed = 0;
static String animBubble, animCard, animConfirm;
static uint32_t tCard = 0, tConfirm = 0;

struct LogEntry { String hhmm, text; uint8_t st; };   // st: 0 empieza · 1 hecho · 2 error · 3 permiso
static LogEntry logRing[8];
static int logN = 0, tasksDone = 0;
static uint32_t logVersion = 0;

static void logPush(uint8_t st, const String &text) {
    if (logN && logRing[(logN - 1) % 8].text == text) return;
    logRing[logN % 8] = {clockText(), text, st};
    logN++;
    logVersion++;
    if (st == 1) tasksDone++;
}

static void setPage(int p, bool byHand) {
    p = (p + 3) % 3;
    if (p != page) { page = p; pageDirty = true; scroll = 0; graphSel = -1; graphDirty = true; }
    autoPage = !byHand && p == 1;
}

// --- marco común: anillo de estado, cápsula de título arriba y puntos de página abajo ----------
static uint32_t linkColor(Model &m) { return m.server ? TEAL : m.wifi ? AMBER : 0xE05050u; }

static uint32_t mix(uint32_t a, uint32_t b, float t) {      // de a a b, t en 0..1
    t = constrain(t, 0.0f, 1.0f);
    auto ch = [&](int sh) { return (uint32_t)(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

static const int RING_OUT = CX - 2, RING_IN = CX - 15;       // la banda del anillo (radios)

// el anillo de las páginas de consola: fino, y con la parte que queda (cuenta atrás) en su color
static void ring(Model &m, float frac, uint32_t col) {
    scr.fillArc(CX, CY, RING_OUT, RING_IN, 0, 360, BG);
    scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - 4, 0, 360, m.recording ? 0x40141Cu : mix(BG, col, 0.22f));
    if (frac > 0) scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - 4, 270, 270 + 360 * min(frac, 1.0f), col);
}

// el de la página de el avatar se mueve con lo que hace: cometa si trabaja, latido si escucha o espera,
// grosor con la voz si habla
enum RingMode : uint8_t { R_STATIC, R_COMET, R_PULSE, R_VOICE };
static void ringLive(RingMode mode, uint32_t col, uint32_t now, float level) {
    scr.fillArc(CX, CY, RING_OUT, RING_IN, 0, 360, BG);
    uint32_t dim = mix(BG, col, 0.22f);
    if (mode == R_STATIC) {
        scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - 4, 0, 360, col);
    } else if (mode == R_COMET) {
        scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - 4, 0, 360, dim);
        float head = fmodf(now * 0.24f, 360);
        for (int k = 0; k < 12; k++) {                         // cola de 96° que se apaga
            float a0 = head - (k + 1) * 8, a1 = head - k * 8;
            uint32_t c = mix(col, dim, k / 12.0f);
            int th = k < 3 ? 7 : k < 7 ? 5 : 4;
            if (a1 <= 0) { a0 += 360; a1 += 360; }
            if (a0 < 0) {                                       // cruza los 0°: en dos trozos
                scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - th, a0 + 360, 360, c);
                scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - th, 0, a1, c);
            } else scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - th, a0, a1, c);
        }
        float r = (head) * PI / 180, rr = RING_OUT - 4;
        scr.fillCircle(CX + cosf(r) * rr, CY + sinf(r) * rr, 5, mix(col, 0xFFFFFF, 0.35f));
    } else if (mode == R_PULSE) {
        float t = 0.5f + 0.5f * sinf(now / 260.0f);
        scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - 3 - (int)(6 * t), 0, 360, mix(dim, col, 0.35f + 0.65f * t));
    } else {
        int th = 3 + (int)(4 * level + 0.5f);          // respira con la voz: poco y sin cambiar de brillo
        scr.fillArc(CX, CY, RING_OUT - 1, RING_OUT - th, 0, 360, col);
    }
}

// manda a la pantalla solo la banda del anillo: por franjas de 16 filas, los dos trozos de cada lado
static void flushRing() {
    for (int y = 0; y < SCR_H; y += 16) {
        int y1 = min(SCR_H, y + 16);
        int near = (y <= CY && CY < y1) ? 0 : min(abs(y - CY), abs(y1 - 1 - CY));
        int far = max(abs(y - CY), abs(y1 - 1 - CY));
        int ro = RING_OUT + 1, ri = RING_IN - 1;
        if (near >= ro) continue;
        int outer = (int)sqrtf((float)(ro * ro - near * near)) + 1;
        int inner = far < ri ? (int)sqrtf((float)(ri * ri - far * far)) - 1 : 0;
        if (inner <= 0) { lcd->flush(scr, CX - outer, y, 2 * outer, y1 - y); continue; }
        lcd->flush(scr, CX - outer, y, outer - inner, y1 - y);
        lcd->flush(scr, CX + inner, y, outer - inner, y1 - y);
    }
}

static void pageDots() {
    scr.fillRect(CX - 40, DOTS_Y - 6, 80, 13, BG);
    for (int i = 0; i < 3; i++) {
        int x = CX + (i - 1) * 18;
        if (i == page) scr.fillRoundRect(x - 9, DOTS_Y - 3, 18, 7, 3, INK);
        else scr.fillCircle(x, DOTS_Y, 3, DIM);
    }
}

// medio ancho útil del círculo entre las filas y0 e y1 (lo que no pisa el anillo)
static int halfAt(int y0, int y1) {
    const int R = RING_IN - 8;
    int dy = max(abs(y0 - CY), abs(y1 - CY));
    return dy >= R ? 0 : (int)sqrtf((float)(R * R - dy * dy));
}

// cápsula de arriba: punto de color (o barras si suena la voz) y el título; el dato debajo, pequeño
static const uint32_t PANEL = 0x0C141B, PANEL2 = 0x141E28;
static void chip(const String &title, uint32_t accent, uint32_t now, bool speaking = false, int y = 38) {
    useFont(t43mono);
    int dy = abs(y + 16 - CY), R = RING_IN - 2;               // lo que deja el círculo a la altura del centro
    String t = ellipsize(scr, title, 2 * (int)sqrtf((float)max(0, R * R - dy * dy)) - 74);
    int w = scr.textWidth(t) + 50, x = CX - w / 2, h = 32;
    scr.fillRoundRect(x, y, w, h, h / 2, PANEL2);
    if (speaking) {
        for (int b = 0; b < 4; b++) {
            int hh = 4 + (int)(8 * (0.5f + 0.5f * sinf(now / 90.0f + b * 1.3f)));
            scr.fillRoundRect(x + 13 + b * 5, y + h / 2 - hh / 2, 3, hh, 1, accent);
        }
    } else {
        scr.fillCircle(x + 19, y + h / 2, 5, accent);
        scr.fillCircle(x + 19, y + h / 2, 8, mix(PANEL2, accent, 0.18f));
        scr.fillCircle(x + 19, y + h / 2, 4, accent);
    }
    scr.setTextDatum(textdatum_t::middle_left);
    scr.setTextColor(INK, PANEL2);
    scr.drawString(t, x + (speaking ? 38 : 34), y + h / 2 + 1);
    scr.setTextDatum(textdatum_t::top_left);
}

static void header(const String &title, uint32_t accent, const String &meta = "", uint32_t now = 0, bool speaking = false) {
    chip(title, accent, now, speaking);
    if (meta.length()) {
        useFont(t43xs);
        scr.setTextDatum(textdatum_t::middle_center);
        scr.setTextColor(MUTED, BG);
        scr.drawString(ellipsize(scr, meta, 300), CX, 86);
        scr.setTextDatum(textdatum_t::top_left);
    }
}

// --- página 0: el avatar y su cartel ---------------------------------------------------------------
static void drawLabel(const PackAnim &a, int phase) {
    uint32_t accent = !strcmp(a.name, "error") ? 0xFFAD8Eu : !strcmp(a.name, "approval") ? 0xFFD780u : 0x96E6C4u;
    scr.fillRect(LABEL_X - 2, LABEL_Y - 2, LABEL_W + 4, LABEL_H + 4, BG);
    scr.fillRoundRect(LABEL_X, LABEL_Y, LABEL_W, LABEL_H, LABEL_H / 2, SEG);
    const int K = 3, ix = LABEL_X + 22 + (!strcmp(a.name, "searching") ? (phase == 2 ? 2 : phase & 1) * K : 0);
    const int iy = LABEL_Y + (LABEL_H - 8 * K) / 2;
    if (!strcmp(a.name, "talking") || !strcmp(a.name, "listening")) {
        static const int hs[] = {3, 6, 4, 7};
        for (int b = 0; b < 4; b++) {
            int hh = hs[(b + phase) % 4] * K;
            scr.fillRect(ix + b * 2 * K, iy + 8 * K / 2 - hh / 2, K, hh, accent);
        }
    } else {
        bool turn = !strcmp(a.name, "working") || !strcmp(a.name, "executing");
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                if (!(a.icon[yy] & (128 >> xx))) continue;
                if (!strcmp(a.name, "reconnecting") && yy < 2 && phase < 1) continue;
                int px = xx, py = yy;
                if (turn) for (int r = 0; r < phase; r++) { int t = px; px = 7 - py; py = t; }
                scr.fillRect(ix + px * K, iy + py * K, K, K, accent);
            }
    }
    useFont(t43s);
    scr.setTextColor(accent, SEG);
    scr.setTextDatum(textdatum_t::middle_left);
    scr.drawString(ellipsize(scr, a.label, LABEL_W - 100), LABEL_X + 58, LABEL_Y + LABEL_H / 2 + 1);
    scr.setTextDatum(textdatum_t::top_left);
    if (strcmp(a.name, "success") && strcmp(a.name, "error") && strcmp(a.name, "approval") && strcmp(a.name, "sleeping"))
        for (int p = 0; p < 3; p++)
            scr.fillCircle(LABEL_X + LABEL_W - 44 + p * 11, LABEL_Y + LABEL_H / 2, 3, p == phase % 3 ? accent : 0x35545Eu);
}

static bool hasNews(Model &m) { return m.bubble || m.card || m.confirm; }

// arriba, la misma cápsula: «HERMES · 14:32»; con algo nuevo en la consola, un punto ámbar al lado
static void topLine(Model &m) {
    scr.fillRect(100, 18, SCR_W - 200, 44, BG);
    // en línea basta la hora (el punto ya dice el estado); si no, qué pasa
    String a = m.recording ? "te escucho" : m.server ? clockText() : m.wifi ? "sin servidor" : "sin WiFi";
    uint32_t col = m.recording ? 0xFF4060u : linkColor(m);
    chip(a, col, 0, false, 26);
    if (hasNews(m) && page == 0) scr.fillCircle(CX, 61, 3, AMBER);   // algo nuevo en la consola
}

// --- página 1: la consola -------------------------------------------------------------------
static void drawBubble(Model &m, uint32_t now) {
    bool speaking = audioPlaying();
    header(speaking ? "habla" : "dice", TEAL, "", now, speaking);
    useFont(t43m);
    const int LH = 34, x0 = X0 + 16;
    static String lines[40];
    int n = wrap(scr, m.bubbleText, CW - 20, lines, 40);
    const int maxL = (Y1 - Y0) / LH;
    int left = (int)revealed, last = 0;
    for (int i = 0; i < n; i++) { if (left > 0) last = i; left -= utf8Len(lines[i]) + 1; }
    scroll = constrain(max(scroll, last - maxL + 1), 0, max(0, n - maxL));
    left = (int)revealed;
    // el texto, centrado en vertical si es corto
    int shown = min(n, maxL), y0 = Y0 + max(0, (maxL - shown) * LH / 2 - 10);
    int cx = x0, cy = y0;
    for (int i = 0; i < n; i++) {
        String vis = utf8Prefix(lines[i], left);
        left -= utf8Len(lines[i]) + 1;
        if (i >= scroll && i < scroll + maxL && vis.length()) {
            int yy = y0 + (i - scroll) * LH;
            scr.setTextColor(INK, BG);
            scr.drawString(vis, x0, yy);
            cx = x0 + scr.textWidth(vis) + 4;
            cy = yy;
        }
    }
    scr.fillRoundRect(X0, y0 + 2, 4, max(24, shown * LH - 8), 2, TEAL);
    if ((int)revealed <= utf8Len(m.bubbleText) || (now / 500) % 2) scr.fillRoundRect(cx, cy + 5, 10, 22, 2, TEAL);
}

static void button(Box b, int oy, const String &label, uint32_t fill, uint32_t ink) {
    scr.fillRoundRect(b.x, b.y + oy, b.w, b.h, b.h / 2, fill);
    useFont(t43m);
    if (scr.textWidth(label) > b.w - 20) useFont(t43s);
    scr.setTextColor(ink);
    scr.setTextDatum(textdatum_t::middle_center);
    scr.drawString(ellipsize(scr, label, b.w - 16), b.x + b.w / 2, b.y + oy + b.h / 2 + 1);
    scr.setTextDatum(textdatum_t::top_left);
}

static void drawCard(Model &m, int oy, uint32_t now) {
    uint32_t col = kindColor(m.cardKind);
    header(m.cardTitle.startsWith("Hermes") ? "resultado" : "aviso", col, "", now);
    // la tarjeta: panel redondeado con franja de color arriba
    int bottom = m.nActions ? Y1 - 70 : Y1 + 4;
    int top = Y0 - 8 + oy;
    scr.fillRoundRect(X0 - 12, top, CW + 24, bottom - top + oy, 16, PANEL);
    scr.fillRoundRect(X0 + 20, top, CW - 40, 4, 2, col);
    // icono en un círculo del color, y el título
    int ty = top + 22;
    scr.fillCircle(X0 + 14, ty + 14, 17, mix(PANEL, col, 0.22f));
    if (!drawIcon(scr, m.cardIcon, X0 + 2, ty + 2, 1.5f)) {
        scr.fillCircle(X0 + 14, ty + 14, 6, col);
    }
    useFont(t43m);
    scr.setTextColor(col, PANEL);
    scr.drawString(ellipsize(scr, withoutHermes(m.cardTitle), CW - 44), X0 + 42, ty + 1);
    useFont(t43s);
    scr.setTextColor(0xD8E0E6u, PANEL);
    int y = ty + 46;
    for (int i = 0; i < m.nLines; i++) {
        String part[3];
        int k = wrap(scr, m.cardLines[i], CW - 8, part, 3);
        for (int j = 0; j < k && y + 26 <= bottom + oy - 8; j++, y += 26) scr.drawString(ellipsize(scr, part[j], CW - 8), X0 + 2, y);
        y += 6;
    }
    for (int i = 0; i < m.nActions; i++)
        button(rowButton(i, m.nActions), oy, m.cardActions[i].label, i == 0 ? col : PANEL2, i == 0 ? BG : INK);
}

static bool toolsRunning(Model &m) {
    if (!m.card || m.cardKind != "tools") return false;
    for (int i = 0; i < m.nTools; i++) if (m.tools[i].st == 0) return true;
    for (int i = 0; i < m.nLines; i++) if (m.cardLines[i].startsWith("run\t")) return true;
    return false;
}

static bool isTerminal(const String &n) {
    return n.indexOf("terminal") >= 0 || n.indexOf("process") >= 0 || n.indexOf("execute") >= 0;
}

static void statusMark(int cx, int cy, uint8_t st, uint32_t now) {
    if (st == 0) {
        int head = (now / 90) % 8;
        for (int k = 0; k < 8; k++) {
            int age = (head - k + 8) % 8;
            float a = k * PI / 4 - PI / 2;
            uint32_t c = age == 0 ? AMBER : age == 1 ? 0xC08418u : age == 2 ? 0x805810u : 0x3A3020u;
            scr.fillCircle(cx + cosf(a) * 7, cy + sinf(a) * 7, age < 2 ? 3 : 2, c);
        }
    } else if (st == 2) {
        scr.fillCircle(cx, cy, 10, mix(BG, RED, 0.25f));
        for (int d = -1; d <= 1; d++) {
            scr.drawLine(cx - 4 + d, cy - 4, cx + 4 + d, cy + 4, RED);
            scr.drawLine(cx - 4 + d, cy + 4, cx + 4 + d, cy - 4, RED);
        }
    } else {
        scr.fillCircle(cx, cy, 10, mix(BG, GREEN, 0.22f));
        for (int d = 0; d < 2; d++) {
            scr.drawLine(cx - 5, cy + d, cx - 2, cy + 3 + d, GREEN);
            scr.drawLine(cx - 2, cy + 3 + d, cx + 5, cy - 4 + d, GREEN);
        }
    }
}

static void drawTools(Model &m, uint32_t now) {
    static ToolRow rows[MAX_TOOLS];
    int n = 0, total = m.toolsTotal;
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
    bool running = false;
    int done = 0, errs = 0;
    float sum = 0;
    for (int i = 0; i < n; i++) {
        running |= rows[i].st == 0;
        done += rows[i].st != 0;
        errs += rows[i].st == 2;
        if (rows[i].secs > 0) sum += rows[i].secs;
    }
    String title = withoutHermes(m.cardTitle);
    int dot = title.lastIndexOf(" · ");
    if (dot > 0 && title.substring(dot + 3).toInt() > 0) title = title.substring(0, dot);
    title.toLowerCase();
    uint32_t col = running ? AMBER : errs ? RED : GREEN;
    header(title, col, "", now);
    // barra de progreso: hechas sobre el total, y el dato al lado
    int hidden0 = total - n;
    float frac = total ? (float)(hidden0 + done) / total : 1;
    const int bw = 150, bx = CX - bw / 2 - 30, by = 86;
    scr.fillRoundRect(bx, by - 3, bw, 6, 3, PANEL2);
    scr.fillRoundRect(bx, by - 3, max(6, (int)(bw * frac)), 6, 3, col);
    useFont(t43xs);
    scr.setTextColor(MUTED, BG);
    scr.setTextDatum(textdatum_t::middle_left);
    scr.drawString(String(hidden0 + done) + "/" + total + (sum > 0 ? " · " + secsText(sum) : ""), bx + bw + 10, by);
    scr.setTextDatum(textdatum_t::top_left);

    const int CH = 22, perLine = (CW - 34) / 11;     // JetBrains Mono 18: 11 px por letra
    static String det[MAX_TOOLS][3];
    int nd[MAX_TOOLS], hgt[MAX_TOOLS];
    for (int i = 0; i < n; i++) {
        String d = rows[i].detail;
        if (isTerminal(rows[i].name) && d.length()) d = "$ " + d;
        nd[i] = d.length() ? wrapChars(d, perLine, det[i], rows[i].st == 0 || i == n - 1 ? 3 : 1) : 0;
        hgt[i] = 38 + nd[i] * CH + 8;
    }
    int first = n, used = 0;
    while (first > 0 && used + hgt[first - 1] <= Y1 - Y0) used += hgt[--first];
    int hidden = total - (n - first);
    int y = Y0;
    if (hidden > 0) {
        useFont(t43xs);
        scr.setTextColor(DIM, BG);
        scr.setTextDatum(textdatum_t::top_center);
        scr.drawString("· · ·  " + String(hidden) + (hidden == 1 ? " anterior" : " anteriores"), CX, y - 2);
        scr.setTextDatum(textdatum_t::top_left);
        y += 22;
        while (first < n && y + used > Y1) used -= hgt[first++];
    }
    for (int i = first; i < n; i++) {
        const ToolRow &r = rows[i];
        uint32_t ic = r.st == 0 ? AMBER : r.st == 2 ? RED : BLUE;
        uint32_t bg = r.st == 0 ? CONSOLE_RUN : PANEL;
        // cada fila tan ancha como deja el círculo a su altura
        int hw = min(CW / 2 + 10, halfAt(y, y + hgt[i] - 6));
        int rx0 = CX - hw, rx1 = CX + hw;
        scr.fillRoundRect(rx0, y, rx1 - rx0, hgt[i] - 6, 12, bg);
        if (r.st == 0) scr.fillRoundRect(rx0, y + 8, 4, hgt[i] - 22, 2, AMBER);
        statusMark(rx0 + 20, y + 18, r.st, now);
        toolIcon(scr, r.name, rx0 + 38, y + 9, ic, 1.4f);
        String tt;
        if (r.st != 0 && r.secs >= 0) tt = secsText(r.secs);
        else if (r.st == 0 && r.seenMs) tt = secsText((millis() - r.seenMs) / 1000.0f);
        useFont(t43mono);
        int tw = tt.length() ? scr.textWidth(tt) : 0;
        if (tt.length()) {
            scr.setTextColor(r.st == 0 ? AMBER : r.st == 2 ? RED : MUTED, bg);
            scr.setTextDatum(textdatum_t::middle_right);
            scr.drawString(tt, rx1 - 12, y + 19);
        }
        useFont(t43s);
        scr.setTextColor(r.st == 0 ? INK : 0xC4CED6u, bg);
        scr.setTextDatum(textdatum_t::middle_left);
        scr.drawString(ellipsize(scr, r.name, rx1 - 12 - (rx0 + 64) - tw - 10), rx0 + 64, y + 19);
        scr.setTextDatum(textdatum_t::top_left);
        useFont(t43mono);
        bool term = isTerminal(r.name);
        static String fit[3];
        int nf = 0;
        if (nd[i]) {                                   // se vuelve a partir con el ancho de esta fila
            String d = r.detail;
            if (term && d.length()) d = "$ " + d;
            nf = wrapChars(d, (rx1 - rx0 - 28) / 11, fit, nd[i]);
        }
        for (int k = 0; k < nf; k++) {
            uint32_t c = r.st == 2 ? 0xE0A0A0u : term ? (r.st == 0 ? 0xA8F0C0u : 0x8CC8A0u) : MUTED;
            scr.setTextColor(c, bg);
            scr.drawString(fit[k], rx0 + 14, y + 38 + k * CH);
        }
        y += hgt[i];
    }
}

static String uptimeText() {
    uint32_t m = millis() / 60000;
    if (m < 60) return String(m) + " min";
    return String(m / 60) + " h " + String(m % 60) + " min";
}

static String dateText() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "sin hora todavía";
    static const char *dias[] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
    static const char *meses[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
    struct tm t;
    localtime_r(&now, &t);
    return String(dias[t.tm_wday]) + " " + t.tm_mday + " " + meses[t.tm_mon];
}

// cápsula pequeña con punto de color, para el estado en reposo
static int pill(int x, int y, const String &text, uint32_t col) {
    useFont(t43xs);
    int w = scr.textWidth(text) + 34;
    scr.fillRoundRect(x, y, w, 28, 14, PANEL2);
    scr.fillCircle(x + 15, y + 14, 4, col);
    scr.setTextColor(0xC4CED6u, PANEL2);
    scr.setTextDatum(textdatum_t::middle_left);
    scr.drawString(text, x + 26, y + 15);
    scr.setTextDatum(textdatum_t::top_left);
    return w;
}

static int cursorX = 0, cursorY = 0;

// reposo: esfera de reloj con el estado y lo último que hizo Hermes
static void drawIdle(Model &m) {
    useFont(t175clk);
    scr.setTextDatum(textdatum_t::middle_center);
    scr.setTextColor(INK, BG);
    scr.drawString(clockText(), CX, 112);
    useFont(t43s);
    scr.setTextColor(MUTED, BG);
    scr.drawString(dateText(), CX, 170);
    scr.setTextDatum(textdatum_t::top_left);
    // estado en dos cápsulas centradas
    useFont(t43xs);
    String a = m.server ? "en línea" : m.wifi ? "sin servidor" : "sin red";
    String b = String(tasksDone) + (tasksDone == 1 ? " tarea" : " tareas");
    int wa = scr.textWidth(a) + 34, wb = scr.textWidth(b) + 34, x = CX - (wa + 8 + wb) / 2;
    x += pill(x, 196, a, m.server ? TEAL : m.wifi ? AMBER : RED) + 8;
    pill(x, 196, b, GREEN);
    // actividad: las tres últimas
    int y = 244;
    scr.drawFastHLine(X0 + 30, y - 8, CW - 60, LINE);
    useFont(t43mono);
    if (logN == 0) {
        scr.setTextColor(DIM, BG);
        scr.setTextDatum(textdatum_t::top_center);
        scr.drawString("sin actividad todavía", CX, y + 6);
        scr.setTextDatum(textdatum_t::top_left);
    }
    for (int i = max(0, logN - 3); i < logN; i++, y += 28) {
        const LogEntry &e = logRing[i % 8];
        uint32_t c = e.st == 0 ? AMBER : e.st == 1 ? GREEN : e.st == 2 ? RED : 0xFFD780u;
        scr.setTextColor(DIM, BG);
        scr.drawString(e.hhmm, X0 + 4, y + 4);
        scr.fillCircle(X0 + 72, y + 13, 4, c);
        scr.setTextColor(e.st == 1 ? 0xC4CED6u : c, BG);
        scr.drawString(ellipsize(scr, e.text, CW - 92), X0 + 86, y + 4);
    }
    // el prompt al pie, con cursor que parpadea
    int py = Y1 - 26;
    String pr = "hermes --watch";
    int pw = scr.textWidth("$ " + pr);
    int px = CX - pw / 2 - 6;
    scr.setTextColor(GREEN, BG);
    scr.drawString("$", px, py);
    scr.setTextColor(DIM, BG);
    scr.drawString(" " + pr, px + scr.textWidth("$"), py);
    cursorX = px + pw + 6;
    cursorY = py;
}

static void drawConfirm(Model &m, uint32_t now) {
    header("permiso", AMBER, String((int)((m.confirmUntil > now ? m.confirmUntil - now : 0) / 1000)) + " s para decidir", now);
    useFont(t43m);
    static String q[3];
    int nq = wrap(scr, m.question, CW, q, 2);
    scr.setTextColor(AMBER, BG);
    scr.setTextDatum(textdatum_t::top_center);
    for (int i = 0; i < nq; i++) scr.drawString(q[i], CX, Y0 + i * 32);
    scr.setTextDatum(textdatum_t::top_left);
    int py = Y0 + nq * 32 + 8;
    const int bottom = Y1 - 72;
    scr.fillRoundRect(X0 - 10, py, CW + 20, bottom - py, 12, PANEL);
    useFont(t43mono);
    static String lines[40];
    int n = 0;
    const int perLine = (CW - 30) / 11;
    int from = 0;
    String p = m.preview;
    while (from <= (int)p.length() && n < 40) {
        int nl = p.indexOf('\n', from);
        if (nl < 0) nl = p.length();
        String part = p.substring(from, nl);
        if (!part.length()) lines[n++] = "";
        else n += wrapChars(part, perLine, lines + n, 40 - n);
        from = nl + 1;
    }
    const int maxL = (bottom - py - 16) / 22;
    scroll = constrain(scroll, 0, max(0, n - maxL));
    for (int i = 0; i < maxL && scroll + i < n; i++) {
        if (scroll + i == 0) { scr.setTextColor(GREEN, PANEL); scr.drawString("$", X0 + 2, py + 8); }
        scr.setTextColor(INK, PANEL);
        scr.drawString(lines[scroll + i], X0 + 20, py + 8 + i * 22);
    }
    for (int i = 0; i < m.nOptions; i++) button(rowButton(i, m.nOptions), 0, m.options[i], optionColor(m.options[i]), 0xFFFFFFu);
}

// --- página 2: el grafo de Obsidian --------------------------------------------------------
static const uint32_t GROUP_COL[] = {0x7AA8FF, 0x5CF08A, 0xFFB020, 0xFF7AA8, 0x40E0C0, 0xC4CED6, 0xFFD780, 0xFF8C5A};
static int16_t gsx[MAX_GNODES], gsy[MAX_GNODES];
static const int G_X0 = 88, G_X1 = SCR_W - 88, G_Y0 = 112, G_Y1 = 374;

static uint32_t groupCol(int g) { return g < 0 ? 0x3A4854u : GROUP_COL[g % 8]; }
static int nodeR(const GraphNode &n) { return 3 + min((int)n.deg, 14) / 2; }

static void drawGraph(Model &m) {
    header("obsidian", VIOLET, m.gN ? String(m.gNotes) + " notas · " + m.gLinks + " enlaces" : "");
    if (!m.gN) {
        useFont(t43mono);
        scr.setTextColor(DIM, BG);
        scr.setTextDatum(textdatum_t::middle_center);
        scr.drawString("esperando el grafo…", CX, CY);
        scr.setTextDatum(textdatum_t::top_left);
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
            scr.drawLine(gsx[a], gsy[a], gsx[b], gsy[b], hot ? VIOLET : graphSel >= 0 ? 0x141E28u : 0x2A3A48u);
        }
    for (int i = 0; i < m.gN; i++) {
        int r = nodeR(m.gNodes[i]);
        bool dim = graphSel >= 0 && i != graphSel && !nb[i];
        if (i == graphSel) scr.fillCircle(gsx[i], gsy[i], r + 4, 0x3A2E5Cu);
        scr.fillCircle(gsx[i], gsy[i], r, dim ? 0x26323Cu : groupCol(m.gNodes[i].group));
    }
    // nombres: los más conectados (o la nota tocada y sus vecinas), sin pisarse
    static int order[MAX_GNODES];
    for (int i = 0; i < m.gN; i++) order[i] = i;
    std::sort(order, order + m.gN, [&](int a, int b) { return m.gNodes[a].deg > m.gNodes[b].deg; });
    if (graphSel >= 0) std::stable_partition(order, order + m.gN, [&](int i) { return i == graphSel; });
    useFont(t43xs);
    static Box placed[MAX_GNODES];
    int np = 0, shown = 0;
    for (int k = 0; k < m.gN; k++) {
        int i = order[k];
        bool want = graphSel >= 0 ? (i == graphSel || nb[i]) : shown < 8 && m.gNodes[i].deg > 0;
        if (!want) continue;
        String l = m.gNodes[i].label;
        if (utf8Len(l) > 16) l = utf8Prefix(l, 15) + "…";
        int w = scr.textWidth(l) + 8, h = 18, r = nodeR(m.gNodes[i]);
        int lx = constrain(gsx[i] - w / 2, X0 - 10, X1 + 10 - w);
        Box cand[2] = {{lx, gsy[i] + r + 2, w, h}, {lx, gsy[i] - r - 2 - h, w, h}};
        int pick = -1;
        for (int c = 0; c < 2 && pick < 0; c++) {
            Box b = cand[c];
            if (b.y < 96 || b.y + b.h > 400) continue;
            bool clash = false;
            for (int q = 0; q < np && !clash; q++)
                clash = b.x < placed[q].x + placed[q].w && placed[q].x < b.x + b.w &&
                        b.y < placed[q].y + placed[q].h && placed[q].y < b.y + b.h;
            if (!clash) pick = c;
        }
        if (pick < 0 && i == graphSel) pick = 0;
        if (pick < 0) continue;
        Box b = cand[pick];
        placed[np++] = b;
        shown++;
        scr.fillRoundRect(b.x, b.y, b.w, b.h, 4, i == graphSel ? 0x2A2248u : 0x0D151Cu);
        scr.setTextColor(i == graphSel ? INK : 0xAEBAC4u);
        scr.drawString(l, b.x + 4, b.y + 1);
    }
    if (graphSel >= 0) {                              // pie: la nota tocada
        const GraphNode &n = m.gNodes[graphSel];
        useFont(t43s);
        scr.setTextColor(INK, BG);
        scr.setTextDatum(textdatum_t::middle_center);
        scr.drawString(ellipsize(scr, n.label, 250), CX, 410);
        scr.setTextDatum(textdatum_t::top_left);
    }
}

static int graphHit(Model &m, int px, int py) {
    int best = -1, bd = 26 * 26;
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
    face.draw(scr, AV_Y0, AV_H);
    scr.fillArc(CX, CY, CX - 1, CX - 5, 0, 360, SEG2);
    useFont(t43s);
    scr.setTextColor(TEAL, BG);
    scr.setTextDatum(textdatum_t::middle_center);
    scr.drawString(ellipsize(scr, l1, 260), CX, 40);
    scr.fillRect(60, LABEL_Y - 6, SCR_W - 120, 70, BG);
    useFont(t43xs);
    scr.setTextColor(MUTED, BG);
    static String ls[3];
    int n = wrap(scr, l2, 270, ls, 3);
    for (int i = 0; i < n; i++) scr.drawString(ls[i], CX, LABEL_Y + 8 + i * 20);
    scr.setTextDatum(textdatum_t::top_left);
    flushAll();
    drawnVersion = UINT32_MAX;
    pageDirty = true;
}

// --- toques y botón BOOT ---------------------------------------------------------------------
static struct { bool down = false; int x0, y0, x, y; uint32_t t0; bool ptt = false; bool swallow = false; } tch;
static bool bootPtt = false;

// --- pantalla que duerme: baja el brillo a los 45 s sin nada, se apaga a los 2 min --------------
enum Screen : uint8_t { SCR_ON, SCR_DIM, SCR_OFF };
static Screen screen = SCR_ON;
static const uint8_t BRIGHT = 200, DIMMED = 40;
static uint32_t lastActive = 0;

static bool wake() {                 // devuelve true si estaba apagada (ese toque no cuenta)
    bool wasOff = screen == SCR_OFF;
    if (wasOff) pageDirty = true;                    // el panel nunca duerme: solo vuelve el brillo y se repinta
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

static bool onFace(int x, int y) { return page == 0 && y > 60 && y < LABEL_Y + LABEL_H && abs(x - CX) < 160; }

static void handleTouch(Model &m, uint32_t now) {
    // BOOT (GPIO 0) también es pulsar para hablar
    bool boot = digitalRead(0) == LOW;
    if (boot && !bootPtt && !m.recording && !m.confirm) {
        wake();
        bootPtt = true;
        m.recording = true;
        audioChirp(true);
        netMicStart();
        if (page != 0) setPage(0, true);
    } else if (!boot && bootPtt) {
        bootPtt = false;
        m.recording = false;
        netMicStop();
        audioChirp(false);
    }
    lgfx::touch_point_t tp;
    if (lcd->getTouch(&tp)) {
        if (!tch.down) {
            bool woke = wake();
            tch = {true, tp.x, tp.y, tp.x, tp.y, now, false, woke};
            Serial.printf("toque x=%d y=%d\n", tp.x, tp.y);
        }
        tch.x = tp.x; tch.y = tp.y;
        lastActive = millis();
        if (!tch.swallow && onFace(tch.x0, tch.y0) && !m.confirm && !tch.ptt && !bootPtt && now - tch.t0 > PTT_MS &&
            abs(tch.x - tch.x0) < 20 && abs(tch.y - tch.y0) < 20) {
            tch.ptt = true;
            m.recording = true;
            audioChirp(true);
            netMicStart();
            face.set(m.emotion.c_str(), "look_left");
        }
        return;
    }
    if (!tch.down) return;
    tch.down = false;
    int x = tch.x, y = tch.y, dx = x - tch.x0, dy = y - tch.y0;
    uint32_t dt = now - tch.t0;
    if (tch.ptt) { m.recording = false; netMicStop(); audioChirp(false); return; }
    if (tch.swallow) return;                             // solo despertó la pantalla
    if (abs(dx) > 60 && abs(dx) > abs(dy)) {            // desliza: otra página
        if (!m.confirm) setPage(page + (dx < 0 ? 1 : -1), true);
        return;
    }
    if (abs(dy) > 40) {                                  // en vertical: scroll de la consola
        scroll = max(0, scroll + (dy < 0 ? 3 : -3));
        return;
    }
    if (page == 1 && m.confirm) {
        for (int i = 0; i < m.nOptions; i++)
            if (inside(rowButton(i, m.nOptions), x, y)) {
                JsonDocument d;
                d["type"] = "confirm_reply"; d["id"] = m.confirmId; d["choice"] = m.options[i];
                send(d);
                return;
            }
        return;
    }
    if (page == 2) {
        int hit = graphHit(m, x, y);
        graphSel = hit == graphSel ? -1 : hit;
        graphDirty = true;
        return;
    }
    const char *gesture = dt > 600 ? "long_press" : "tap";
    if (page == 0) {
        if (onFace(tch.x0, tch.y0)) {
            face.play(S_WAVE, 1);
            sendTouch("face", x, y, gesture);
        } else if (y < 70 && hasNews(m)) setPage(1, true);   // el aviso de arriba lleva a la consola
        return;
    }
    if (m.bubble) { m.bubble = false; m.version++; sendTouch("bubble", x, y, gesture); return; }
    if (m.card)
        for (int i = 0; i < m.nActions; i++)
            if (inside(rowButton(i, m.nActions), x, y)) {
                String t = "card:" + m.cardId + ":" + m.cardActions[i].id;
                sendTouch(t.c_str(), x, y, "tap");
                return;
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

void uiFrame(uint32_t now) {
    if (uiFrozen) return;
    static uint32_t last = now;
    static bool wasServer = false;
    uint32_t dtFrame = now - last;
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
    // actividad: algo nuevo, micro, voz, herramientas, confirmación o moverla
    static uint32_t seenVersion = 0, lastImu = 0;
    bool active = m.version != seenVersion || m.recording || audioPlaying() || m.confirm || toolsRunning(m) || tch.down;
    seenVersion = m.version;
    if (now - lastImu >= 100) { lastImu = now; active |= imuMoved(); }
    if (active) wake();
    // con millis(), no con now: wake() acaba de poner lastActive = millis(), que va por delante de now,
    // y la resta sin signo daba la vuelta (se apagaba y encendía en cada cuadro mientras hablaba)
    uint32_t quieta = millis() - lastActive;
    if (screen == SCR_ON && quieta > 45000) { screen = SCR_DIM; lcd->setBrightness(DIMMED); }
    if (screen == SCR_DIM && quieta > 120000) {
        screen = SCR_OFF;
        lcd->setBrightness(0);                          // sin sleep(): el CO5300 pierde lo que llega en sus 120 ms de despertar
        setPage(0, false);                              // al despertar, con el avatar
    }
    if (screen == SCR_OFF) { modelUnlock(); return; }

    // lo nuevo de Hermes abre la consola
    bool fresh = false;
    if (m.bubble && m.bubbleId != animBubble) { animBubble = m.bubbleId; revealed = 0; scroll = 0; fresh = true; }
    if (m.card && m.cardId != animCard) { animCard = m.cardId; tCard = now; fresh = true; }
    if (m.confirm && m.confirmId != animConfirm) { animConfirm = m.confirmId; tConfirm = now; scroll = 0; fresh = true; }
    if (fresh && !m.recording) { setPage(1, false); tAuto = now; }
    if (m.version != drawnVersion) tAuto = now;
    if (m.bubble) revealed += dtFrame / 1000.0f * LETTERS_PER_S;
    if (m.confirm && now > m.confirmUntil) { m.confirm = false; m.version++; }
    if (m.confirm && page != 1) setPage(1, false);
    // vuelta sola con el avatar: sin nada que enseñar, o 8 s quieta (si no hay nada en marcha)
    bool typing = m.bubble && (int)revealed <= utf8Len(m.bubbleText) + 1;
    if (page == 1 && autoPage && !m.confirm && !toolsRunning(m) && !typing &&
        (!hasNews(m) || now - tAuto > 8000))
        setPage(0, false);

    static String loggedCard, loggedConfirm;
    if (m.card && m.cardId != "oido" && m.cardId + m.cardKind + m.cardTitle != loggedCard) {   // «Te entendí» no es una tarea
        loggedCard = m.cardId + m.cardKind + m.cardTitle;
        bool err = false;
        for (int i = 0; i < m.nTools; i++) err |= m.tools[i].st == 2;
        String t = withoutHermes(m.cardTitle);
        if (m.cardKind == "tools") {
            int dot = t.lastIndexOf(" · ");
            if (dot > 0 && t.substring(dot + 3).toInt() > 0) t = t.substring(0, dot);
            logPush(0, t);
        } else if (t.indexOf("listo") >= 0) logPush(err ? 2 : 1, t);
        else logPush(1, t);
    }
    if (m.confirm && m.confirmId != loggedConfirm) { loggedConfirm = m.confirmId; logPush(3, "permiso: " + m.question); }

    bool changed = m.version != drawnVersion || scroll != drawnScroll;
    bool full = pageDirty;
    if (page == 0) {
        // anillo y línea de arriba: al cambiar el enlace, el micro, el reloj o el aviso
        static String topKey;
        String key = String(m.server) + m.wifi + m.recording + clockText() + hasNews(m);
        if (full) {
            scr.fillScreen(BG);
            face.redrawAll();
        }
        bool topDirty = full || key != topKey;
        if (topDirty) {
            topKey = key;
            topLine(m);
            pageDots();
        }
        // el anillo según lo que hace el avatar: se repinta a ~25 fps si se mueve
        SpriteState st = face.current();
        RingMode mode = R_STATIC;
        uint32_t rcol = linkColor(m);
        if (m.recording || st == S_LISTENING) { mode = R_PULSE; rcol = 0xFF4060u; }
        else if (st == S_APPROVAL) { mode = R_PULSE; rcol = AMBER; }
        else if (st == S_TALKING || audioPlaying()) { mode = R_VOICE; rcol = TEAL; }
        else if (st == S_THINKING || st == S_WORKING || st == S_READING || st == S_TYPING || st == S_SEARCHING ||
                 st == S_EXECUTING) { mode = R_COMET; rcol = st == S_THINKING ? VIOLET : AMBER; }
        else if (st == S_ERROR) rcol = RED;
        else if (st == S_SUCCESS) rcol = GREEN;
        static float level = 0;
        level += (audioMouth() - level) * (audioMouth() > level ? 0.15f : 0.06f);   // sube y baja despacio
        static uint32_t lastRing = 0;
        static String ringKey;
        String rk = String(mode) + rcol;
        bool ringDirty = full || topDirty || rk != ringKey || (mode != R_STATIC && now - lastRing >= 40);
        if (ringDirty) {
            ringKey = rk;
            lastRing = now;
            ringLive(mode, rcol, now, level);
        }
        bool faceDirty = face.dirty() || full;
        if (faceDirty) face.draw(scr, AV_Y0, AV_H);
        static int labelSt = -1, labelPh = -1;
        bool labelDirty = full || face.current() != labelSt || face.phase() != labelPh;
        if (labelDirty) {
            labelSt = face.current();
            labelPh = face.phase();
            drawLabel(face.anim(), labelPh);
        }
        drawnVersion = m.version;
        modelUnlock();
        if (full) flushAll();
        else {
            if (faceDirty && face.changed.w) {
                const Avatar::Rect &r = face.changed;
                lcd->flush(scr, r.x, r.y, r.w, r.h);
            }
            if (labelDirty) lcd->flush(scr, LABEL_X - 2, LABEL_Y - 2, LABEL_W + 4, LABEL_H + 4);
            if (topDirty) {
                lcd->flush(scr, 100, 18, SCR_W - 200, 44);
                lcd->flush(scr, CX - 40, DOTS_Y - 6, 80, 13);
            }
            if (ringDirty) flushRing();
        }
        pageDirty = false;
        return;
    }

    bool dirty = full || changed, chipOnly = false;
    if (page == 2) {
        static uint32_t drawnGraph = UINT32_MAX;
        dirty |= graphDirty || m.gVersion != drawnGraph;
        if (dirty) { drawnGraph = m.gVersion; graphDirty = false; }
    } else if (m.confirm) {
        dirty |= now - lastPanel >= 250;                              // la cuenta atrás
    } else {
        bool sliding = m.card && !m.bubble && now - tCard < 400;
        bool spinning = !m.bubble && toolsRunning(m);
        static uint32_t drawnLog = UINT32_MAX;
        if (!hasNews(m) && (logVersion != drawnLog || now - lastPanel >= 5000)) { dirty = true; drawnLog = logVersion; }
        bool speaking = m.bubble && audioPlaying();                  // las barras de la cápsula
        static bool wasSpeaking = false;
        dirty |= ((typing || sliding) && now - lastPanel >= 60) || (spinning && now - lastPanel >= 100) ||
                 speaking != wasSpeaking;
        wasSpeaking = speaking;
        // mientras habla solo se mueven las barras de la cápsula: se repinta ella sola, no la pantalla
        static uint32_t lastChip = 0;
        if (speaking && !dirty && now - lastChip >= 80) {
            lastChip = now;
            chip("habla", TEAL, now, true);
            chipOnly = true;
        }
    }
    if (dirty) {
        scr.fillScreen(BG);
        float frac = 1;
        uint32_t ringCol = linkColor(m);
        if (page == 2) drawGraph(m);
        else if (m.confirm) {
            drawConfirm(m, now);
            frac = m.confirmUntil > now ? (float)(m.confirmUntil - now) / m.confirmTotal : 0;
            ringCol = AMBER;
        } else if (m.bubble) drawBubble(m, now);
        else if (m.card) {
            int oy = (int)(80 * (1 - easeOutBack((now - tCard) / 340.0f)));
            if (m.cardKind == "tools") drawTools(m, now);
            else drawCard(m, oy, now);
        } else drawIdle(m);
        ring(m, frac, ringCol);
        pageDots();
        lastPanel = now;
        drawnVersion = m.version;
        drawnScroll = scroll;
    }
    // el cursor del prompt en reposo parpadea solo
    bool cursorDirty = false;
    if (page == 1 && !hasNews(m) && cursorX) {
        static bool on = false;
        bool want = (now / 500) % 2 == 0;
        if (want != on || dirty) {
            on = want;
            scr.fillRect(cursorX, cursorY + 1, 10, 20, on ? GREEN : BG);
            cursorDirty = !dirty;
        }
    }
    modelUnlock();
    if (dirty) flushAll();
    else if (chipOnly) lcd->flush(scr, CX - 120, 36, 240, 36);
    else if (cursorDirty) lcd->flush(scr, cursorX, cursorY + 1, 10, 20);
    pageDirty = false;
}

void uiKey(char c) {
    if (c == 'g') setPage(page + 1, true);                // otra página
    else if (c == 'n') {                                  // tocar la nota más conectada (o soltarla)
        if (page != 2) setPage(2, true);
        int best = -1;
        for (int i = 0; i < model.gN; i++) if (best < 0 || model.gNodes[i].deg > model.gNodes[best].deg) best = i;
        graphSel = graphSel >= 0 ? -1 : best;
        graphDirty = true;
    }
}

void uiDump() {
    // el sprite ya guarda big-endian, como espera tools/shot.py
    Serial.printf("SHOT %d %d\n", SCR_W, SCR_H);
    const uint8_t *p = (const uint8_t *)scr.getBuffer();
    for (int y = 0; y < SCR_H; y++) Serial.write(p + y * SCR_W * 2, SCR_W * 2);
    Serial.flush();
}
#endif
