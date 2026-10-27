// El avatar: las 16 animaciones pixel art del paquete (sprites43.h, generado con tools/make_avatar.py).
// Cada cuadro de 120x160 va en flash con RLE sobre una paleta de 128 o 256 colores. Al decodificar se
// compara con el cuadro anterior y solo se repinta lo que cambió, ampliado con vecino más cercano
// (×3 alto y ×3·15/16 ancho en la 4.3"; ×2,5 en la 1.75, ver AV_* en avatar.h). El cartel de abajo («Leyendo»…) lo pinta ui43.cpp.
#ifdef PACK_AVATAR
#pragma GCC optimize("O2")
#include "avatar.h"

static const uint32_t BG = 0x05080C;
static uint8_t idxBuf[PACK_W * PACK_H];      // el cuadro decodificado (índices de paleta)
static uint16_t palBE[256];                  // la paleta del estado que se ve, en big-endian
static int palOf = -1;                  // la paleta en big-endian, como el sprite
static uint8_t colMap[AV_OUT_W];           // columna de pantalla → columna del cuadro
static bool ready = false;
static bool fullNext = true;                 // el próximo draw() repinta todo el rectángulo

SpriteState Avatar::fromEmotion(const char *e) const {
    for (int i = 0; i < S_COUNT; i++)
        if (!strcmp(e, PACK_ANIMS[i].name)) return (SpriteState)i;
    // las del ánimo en reposo (happy, excited, love, sad, bored) no tienen animación propia: si
    // happy fuera el pulgar arriba se quedaría así horas; el pulgar es "success", al acabar una tarea
    if (!strcmp(e, "alert")) return S_CONFUSED;
    return S_IDLE;
}

// lo que se ve manda en este orden: sin conexión, confirm, micro, puntual y la emoción
SpriteState Avatar::shown() const {
    if (disconnected) return S_RECONNECTING;
    if (confirming) return S_APPROVAL;
    if (listening) return S_LISTENING;
    if (once != S_COUNT) return once;
    return state;
}

void Avatar::set(const char *emotion, const char *animation) {
    state = fromEmotion(emotion);
    if (animation && *animation) { gesture = animation; tGesture = 0; }
}

void Avatar::play(SpriteState s, uint8_t loops) {
    const PackAnim &a = PACK_ANIMS[s];
    once = s;
    onceUntil = t + (uint32_t)loops * a.count * 1000 / a.fps;
    shownPrev = S_COUNT;                     // que empiece desde el primer cuadro
}

void Avatar::step(uint32_t dt, uint32_t) {
    t += dt;
    if (once != S_COUNT && (int32_t)(t - onceUntil) >= 0) once = S_COUNT;
    SpriteState s = shown();
    if (s != shownPrev) { shownPrev = s; tStart = t; }
    // el cuadro sale del tiempo: si el bucle se retrasa, se salta cuadros en vez de ir lento
    const PackAnim &a = PACK_ANIMS[s];
    uint32_t f = (uint64_t)(t - tStart) * a.fps / 1000;
    idx = a.loop ? f % a.count : min<uint32_t>(f, a.count - 1);   // las de una vez se quedan en la última
    gx = gy = 0;
    if (gesture.length()) {
        tGesture += dt;
        float tt = tGesture / 1000.0f;
        if (gesture == "bounce") gy = -fabsf(sinf(tt * 10)) * 12 * expf(-tt * 3);
        else if (gesture == "nod") gy = sinf(tt * 14) * 6 * expf(-tt * 4);
        else if (gesture == "shake") gx = sinf(tt * 18) * 9 * expf(-tt * 4);
        else if (gesture == "look_left") gx = -6 * expf(-tt * 3);
        else if (gesture == "look_right") gx = 6 * expf(-tt * 3);
        if (tt > 1.2f) gesture = "";
    }
}

uint8_t Avatar::cellNow() const { return idx; }

bool Avatar::dirty() const {
    return fullNext || drawn.st != shown() || drawn.cell != cellNow() || drawn.x != offX() || drawn.y != offY();
}

void Avatar::redrawAll() { fullNext = true; }

// decodifica y devuelve el rectángulo (en el cuadro) que cambió respecto al anterior
static bool decode(int id, int &x0, int &y0, int &x1, int &y1) {
    const uint8_t *p = PACK_RLE + PACK_OFFS[id], *end = PACK_RLE + PACK_OFFS[id + 1];
    x0 = PACK_W; y0 = PACK_H; x1 = y1 = -1;
    int i = 0;
    while (p < end && i < PACK_W * PACK_H) {
        uint8_t c = *p++;
        int n = (c & 127) + 1;
        bool run = c & 128;
        for (int j = 0; j < n && i < PACK_W * PACK_H; j++, i++) {
            uint8_t v = run ? *p : p[j];
            if (idxBuf[i] != v) {
                idxBuf[i] = v;
                int x = i % PACK_W, y = i / PACK_W;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
        p += run ? 1 : n;
    }
    return x1 >= 0;
}

void Avatar::draw(LGFX_Sprite &s, int y0, int h) {
    if (!ready) {
        for (int c = 0; c < AV_OUT_W; c++) colMap[c] = PACK_X0 + AV_COL(c);
        ready = true;
    }
    SpriteState st = shown();
    uint8_t cell = cellNow();
    drawn = {(int8_t)st, cell, offX(), offY(), disconnected};
    // cada estado lleva su paleta (hablar viene de otro paquete): al cambiar, se repinta entero
    const PackAnim &pa = PACK_ANIMS[st];
    if (pa.pal != palOf) {
        const uint16_t *p = PACK_PAL + pa.pal * 256;
        for (int i = 0; i < 256; i++) palBE[i] = (p[i] << 8) | (p[i] >> 8);
        palOf = pa.pal;
        fullNext = true;
    }
    int nx0, ny0, nx1, ny1;
    bool any = decode(PACK_ANIMS[st].first + cell, nx0, ny0, nx1, ny1);

    const int W = AV_OUT_W, H = AV_OUT_H, sw = s.width();
    int x = (sw - W) / 2 + offX();
    int y = y0 + h - 64 - H + offY();                  // encima del cartel
    Rect cur{x, y, W, H};
    bool moved = last.x != x || last.y != y || !last.w;
    if (moved && last.w) s.fillRect(last.x, last.y, last.w, min(last.h, clipBottom - last.y), BG);
    // qué filas y columnas de pantalla repintar
    int c0 = 0, c1 = W, r0 = 0, r1 = H;
    if (!(moved || fullNext)) {
        if (!any) { changed = {0, 0, 0, 0}; last = cur; return; }
        c0 = AV_FWD_X(nx0 - PACK_X0);
        c1 = AV_FWD_X(nx1 + 1 - PACK_X0);
        r0 = AV_FWD_Y(ny0 - PACK_Y0);
        r1 = AV_FWD_Y(ny1 + 1 - PACK_Y0);
    }
    c0 = max(c0, max(0, -x)); c1 = min(c1, min(W, sw - x));
    r0 = max(r0, max(0, y0 - y)); r1 = min(r1, min(H, min(y0 + h, clipBottom) - y));
    fullNext = false;
    if (c0 >= c1 || r0 >= r1) { changed = {0, 0, 0, 0}; last = cur; return; }
    static uint16_t line[AV_OUT_W];
    uint16_t *dst = (uint16_t *)s.getBuffer();
    for (int r = r0; r < r1; r++) {
        if (r == r0 || AV_ROW(r) != AV_ROW(r - 1)) {   // cada fila del cuadro da varias de pantalla
            const uint8_t *row = idxBuf + (PACK_Y0 + AV_ROW(r)) * PACK_W;
            for (int c = c0; c < c1; c++) line[c] = palBE[row[colMap[c]]];
        }
        memcpy(dst + (y + r) * sw + x + c0, line + c0, (c1 - c0) * 2);
    }
    changed = {x + c0, y + r0, c1 - c0, r1 - r0};
    if (moved && last.w) {                             // la copia incluye lo que se borró
        int ax = min(last.x, x), ay = min(last.y, y);
        int bx = max(last.x + last.w, x + W), by = max(last.y + last.h, y + H);
        ax = max(0, ax); ay = max(y0, ay); bx = min(sw, bx); by = min(y0 + h, by);
        changed = {ax, ay, bx - ax, by - ay};
    }
    last = cur;
}
#endif
