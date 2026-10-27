#pragma once
// Lo que comparten las interfaces de las dos placas (ui.cpp vertical 240x320, ui43.cpp
// horizontal 800x480): texto que se parte o se corta sin romper UTF-8, curvas, colores
// por tipo e iconos dibujados con primitivas. Los iconos aceptan una escala k.
#include <Arduino.h>
#include <LovyanGFX.hpp>

static uint32_t kindColor(const String &k) {
    if (k == "lead") return 0x5CF08A;
    if (k == "noshow") return 0xFFB020;
    if (k == "brief") return 0x7AA8FF;
    return 0xFF6A6A;
}

static uint32_t optionColor(const String &o) {
    if (o == "Enviar" || o == "Permitir") return 0x2E9E5B;
    if (o == "Editar" || o == "Esta sesión") return 0x3A6FD8;
    if (o == "Denegar") return 0xB03A3A;
    return 0x5A6470;
}

// Parte el texto en líneas que caben en `w` (la fuente ya cargada en el sprite)
static int wrap(LGFX_Sprite &s, const String &text, int w, String *out, int maxLines) {
    int n = 0;
    String line;
    int i = 0, len = text.length();
    while (i <= len && n < maxLines) {
        int j = text.indexOf(' ', i);
        int nl = text.indexOf('\n', i);
        bool brk = false;
        if (nl >= 0 && (j < 0 || nl < j)) { j = nl; brk = true; }
        if (j < 0) j = len;
        String word = text.substring(i, j);
        String test = line.length() ? line + " " + word : word;
        if (s.textWidth(test) > w && line.length()) {
            out[n++] = line;
            line = word;
        } else {
            line = test;
        }
        if (brk && n < maxLines) { out[n++] = line; line = ""; }
        i = j + 1;
    }
    if (line.length() && n < maxLines) out[n++] = line;
    return n;
}

static String ellipsize(LGFX_Sprite &s, String t, int w) {
    if (s.textWidth(t) <= w) return t;
    while (t.length() && s.textWidth(t + "...") > w) {
        t.remove(t.length() - 1);
        while (t.length() && (t[t.length() - 1] & 0xC0) == 0x80) t.remove(t.length() - 1);  // no partir UTF-8
    }
    return t + "...";
}

static float easeOutBack(float x) {
    const float c1 = 1.70158f, c3 = c1 + 1;
    x = constrain(x, 0.0f, 1.0f) - 1;
    return 1 + c3 * x * x * x + c1 * x * x;
}

static int utf8Len(const String &s) {
    int n = 0;
    for (size_t i = 0; i < s.length(); i++) if ((s[i] & 0xC0) != 0x80) n++;
    return n;
}

static String utf8Prefix(const String &s, int chars) {   // las primeras `chars` letras, sin partir UTF-8
    if (chars <= 0) return "";
    size_t i = 0;
    int n = 0;
    while (i < s.length()) {
        if ((s[i] & 0xC0) != 0x80) { if (n == chars) break; n++; }
        i++;
    }
    return s.substring(0, i);
}

// Logos de canal dibujados (16 px × k), igual que icono() en el simulador
static bool drawIcon(LGFX_Sprite &s, const String &name, int x, int y, float k = 1) {
    auto P = [k](float v) { return (int)lroundf(v * k); };
    String n = name;
    n.toLowerCase();
    if (n.indexOf("whatsapp") >= 0) {
        s.fillCircle(x + P(8), y + P(8), P(8), 0x25D366u);
        s.drawCircle(x + P(8), y + P(8), P(5), 0xFFFFFFu);
        s.drawCircle(x + P(8), y + P(8), P(4), 0xFFFFFFu);
        s.fillTriangle(x + P(3), y + P(14), x + P(4), y + P(10), x + P(7), y + P(12), 0xFFFFFFu);
    } else if (n.indexOf("instagram") >= 0) {
        s.fillRoundRect(x, y, P(16), P(16), P(5), 0xE1306Cu);
        s.drawRoundRect(x + P(3), y + P(3), P(10), P(10), P(3), 0xFFFFFFu);
        s.drawCircle(x + P(8), y + P(8), P(2), 0xFFFFFFu);
        s.fillRect(x + P(11), y + P(4), P(2), P(2), 0xFFFFFFu);
    } else if (n.indexOf("facebook") >= 0) {
        s.fillCircle(x + P(8), y + P(8), P(8), 0x1877F2u);
        s.fillRect(x + P(7), y + P(4), P(3), P(10), 0xFFFFFFu);
        s.fillRect(x + P(7), y + P(4), P(5), P(2), 0xFFFFFFu);
        s.fillRect(x + P(5), y + P(7), P(7), P(2), 0xFFFFFFu);
    } else {
        return false;
    }
    return true;
}

// Icono de 14 px × k según el tipo de herramienta, dibujado con primitivas
static void toolIcon(LGFX_Sprite &s, const String &name, int x, int y, uint32_t c, float k = 1) {
    auto P = [k](float v) { return (int)lroundf(v * k); };
    auto line = [&](float x0, float y0, float x1, float y1) {   // trazo de grosor ~k
        for (int t = 0; t < max(1, P(1)); t++) s.drawLine(x + P(x0), y + P(y0) + t, x + P(x1), y + P(y1) + t, c);
    };
    if (name.indexOf("terminal") >= 0 || name.indexOf("process") >= 0 || name.indexOf("execute") >= 0) {
        s.drawRoundRect(x, y, P(14), P(12), P(2), c);              // ventana con >_
        line(3, 4, 5, 6);
        line(5, 6, 3, 8);
        line(7, 8, 11, 8);
    } else if (name.indexOf("web") >= 0 || name.indexOf("browser") >= 0 || name.indexOf("http") >= 0) {
        s.drawCircle(x + P(7), y + P(6), P(6), c);                 // globo
        s.drawEllipse(x + P(7), y + P(6), P(2), P(6), c);
        s.drawFastHLine(x + P(1), y + P(6), P(12), c);
    } else if (name.indexOf("file") >= 0 || name.indexOf("patch") >= 0 || name.indexOf("read") >= 0 ||
               name.indexOf("write") >= 0) {
        s.drawRect(x + P(2), y, P(10), P(13), c);                  // hoja con renglones
        for (int i = 0; i < 3; i++) s.drawFastHLine(x + P(4), y + P(3 + i * 3), P(6), c);
    } else if (name.indexOf("memory") >= 0 || name.indexOf("skill") >= 0 || name.indexOf("session") >= 0) {
        s.fillTriangle(x + P(7), y, x + P(2), y + P(12), x + P(12), y + P(12), c);   // estrella simple
        s.fillTriangle(x + P(7), y + P(13), x + P(1), y + P(4), x + P(13), y + P(4), c);
    } else {
        s.drawCircle(x + P(7), y + P(6), P(4), c);                 // engranaje
        for (int i = 0; i < 8; i++) {
            float a = i * PI / 4;
            line(7 + cosf(a) * 4, 6 + sinf(a) * 4, 7 + cosf(a) * 6, 6 + sinf(a) * 6);
        }
    }
}
