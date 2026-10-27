#pragma once
#include "board.h"

void uiBegin(LGFX &lcd);
void uiFrame(uint32_t now);      // un cuadro: toques + cara + paneles
void uiDump();                   // vuelca la pantalla por serie (tools/shot.py)
void uiKey(char c);              // atajos por serie para probar sin tocar (g: página, n: nota del grafo, e: consola)
void uiSplash(const char *line1, const char *line2);
