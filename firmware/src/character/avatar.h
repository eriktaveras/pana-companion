#pragma once
// El avatar: un paquete pixel art de 16 estados (sprites43.h + sprites_data43.cpp), que se genera con
// tools/make_avatar.py desde una carpeta de PNG. avatar43.cpp lo descomprime y lo amplía ×2,5.
#include <LovyanGFX.hpp>
#ifdef PACK_AVATAR
#include "sprites43.h"
#define SPRITE_ZOOM 1
// escala del personaje: AV_COL/AV_ROW llevan de pantalla al cuadro y AV_FWD del cuadro a pantalla
// (redondeando hacia arriba). Para otra escala, cambia el 5/2 (y el 2/5) de las seis líneas
#define AV_OUT_W (PACK_CW * 5 / 2)
#define AV_OUT_H (PACK_CH * 5 / 2)
#define AV_COL(c) ((c) * 2 / 5)
#define AV_ROW(r) ((r) * 2 / 5)
#define AV_FWD_X(n) (((n) * 5 + 1) / 2)
#define AV_FWD_Y(n) (((n) * 5 + 1) / 2)
#else
#error "Compila con -DPACK_AVATAR"
#endif

class Avatar {
public:
    void set(const char *emotion, const char *animation);
    void step(uint32_t dtMs, uint32_t nowMs);
    void draw(LGFX_Sprite &s, int y0, int h);   // pinta en el ancho del sprite a partir de la fila y0
    bool dirty() const;                         // ¿cambió algo desde el último draw()?
    bool listening = false;                     // push-to-talk local: manda sobre la emoción
    int clipBottom = 1 << 20;                   // no pintar por debajo de esta fila (sin mover a el avatar)
    bool disconnected = false;
#ifdef PACK_AVATAR
    bool confirming = false;                    // hay un confirm en pantalla: espera la decisión
    void play(SpriteState s, uint8_t loops);    // una animación puntual (saludo) y vuelve a la suya
    struct Rect { int x, y, w, h; } changed{};  // lo que tocó el último draw(), para copiar solo eso
    const PackAnim &anim() const { return PACK_ANIMS[shown()]; }
    SpriteState current() const { return shown(); }
    void redrawAll();                           // tras borrar el sprite por fuera (splash)
    uint8_t phase() const { return (cellNow() / 6) % 4; }   // fase del cartel: 4 por ciclo
#endif

private:
    SpriteState state = S_IDLE;
    uint8_t idx = 0;                            // posición en la secuencia del estado
    int32_t left = 0;                           // ms que le quedan al cuadro actual
    String gesture;
    uint32_t tGesture = 0;
    float gx = 0, gy = 0;                       // desplazamiento del gesto puntual
    struct { int8_t st = -1; uint8_t cell; int x, y; bool off; } drawn;
#ifdef PACK_AVATAR
    SpriteState once = S_COUNT;                 // la puntual en curso (S_COUNT = ninguna)
    uint32_t onceUntil = 0;
    uint32_t t = 0, tStart = 0;                 // reloj propio: el cuadro sale del tiempo transcurrido
    SpriteState shownPrev = S_COUNT;
    Rect last{};
    SpriteState shown() const;
#endif
    uint8_t cellNow() const;
    int offX() const { return (int)gx * SPRITE_ZOOM; }
    int offY() const { return (int)gy * SPRITE_ZOOM; }
    SpriteState fromEmotion(const char *e) const;
};
