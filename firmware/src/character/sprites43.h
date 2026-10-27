// Generado por tools/make_avatar.py — no editar a mano
#pragma once
#include <stdint.h>

#define PACK_W 120
#define PACK_H 160
#define PACK_X0 11           // rectángulo que ocupa el personaje en el cuadro
#define PACK_Y0 30
#define PACK_CW 99
#define PACK_CH 121

extern const uint8_t PACK_RLE[];      // RLE: c&128 → (c&127)+1 copias; si no, (c&127)+1 literales
extern const uint32_t PACK_OFFS[];    // cuadro → posición en PACK_RLE (+1 al final)
#define PACK_COLORS 256
extern const uint16_t PACK_PAL[];     // paleta de 256 en RGB565 nativo; el 0 es el fondo

struct PackAnim { const char *name, *label; uint16_t first, count; uint8_t fps; bool loop; uint8_t pal; uint8_t icon[8]; };
enum SpriteState : uint8_t { S_IDLE, S_LISTENING, S_THINKING, S_TALKING, S_WORKING, S_SUCCESS, S_WAVE, S_CONFUSED, S_READING, S_TYPING, S_SEARCHING, S_EXECUTING, S_APPROVAL, S_ERROR, S_RECONNECTING, S_SLEEPING, S_COUNT };
static const PackAnim PACK_ANIMS[] = {
    {"idle", "Listo", 0, 24, 12, true, 0, {16, 84, 56, 254, 56, 84, 16, 0}},
    {"listening", "Te escucho", 24, 16, 12, true, 0, {60, 66, 153, 133, 137, 17, 34, 28}},
    {"thinking", "Pensando", 40, 20, 12, true, 0, {60, 66, 129, 129, 66, 60, 8, 16}},
    {"talking", "Hablando", 60, 16, 12, true, 0, {16, 50, 245, 245, 245, 50, 16, 0}},
    {"working", "Trabajando", 76, 16, 12, true, 0, {36, 126, 219, 102, 102, 219, 126, 36}},
    {"success", "Hecho", 92, 16, 12, true, 0, {1, 3, 6, 140, 216, 112, 32, 0}},
    {"wave", "Hola", 108, 18, 12, false, 0, {40, 170, 170, 170, 254, 124, 56, 56}},
    {"confused", "Tengo una duda", 126, 16, 12, true, 0, {60, 66, 2, 12, 16, 16, 0, 16}},
    {"reading", "Leyendo", 142, 20, 12, true, 0, {231, 153, 153, 187, 153, 187, 153, 255}},
    {"typing", "Escribiendo", 162, 12, 12, true, 0, {0, 255, 129, 213, 171, 213, 153, 255}},
    {"searching", "Buscando", 174, 20, 12, true, 0, {56, 68, 130, 130, 68, 60, 6, 3}},
    {"executing", "Ejecutando", 194, 16, 12, true, 0, {64, 96, 112, 120, 112, 96, 64, 0}},
    {"approval", "¿Me autorizas?", 210, 16, 12, true, 0, {60, 66, 2, 12, 16, 16, 0, 16}},
    {"error", "Revisar error", 226, 12, 12, true, 0, {24, 60, 36, 102, 66, 219, 129, 255}},
    {"reconnecting", "Conectando", 238, 16, 12, true, 0, {126, 129, 60, 66, 24, 36, 0, 24}},
    {"sleeping", "Descansando", 254, 24, 12, true, 0, {254, 4, 8, 16, 32, 64, 254, 0}},
};
