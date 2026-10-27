---
name: personalizar-pana
description: Personaliza Pana (el companion de escritorio en placas Waveshare ESP32-S3 AMOLED 1.75 y ESP32-P4 4C) a gusto del usuario — cambiar el personaje o avatar, los colores y el diseño de la pantalla, la persona y la voz del agente, añadir datos o widgets al tablero (homelab, agenda, redes, lo que sea), portarlo a otra placa de Waveshare, y compilar, grabar y comprobarlo con capturas de la propia placa. Úsala cuando el usuario pida «cambia el personaje», «ponle mi cara», «otros colores», «más grande la hora», «añade un widget», «que hable de otra forma», «que se llame…», «adáptalo a mi placa», o cualquier cambio en este repo.
---

# Personalizar Pana

Pana tiene un **cuerpo** (`firmware/`, la placa) y un **cerebro** (`server/`, Python). Hablan por WebSocket con el protocolo de `protocol/PROTOCOL.md`. La placa no decide nada: pinta lo que le manda el servidor y le devuelve toques y audio.

Antes de tocar nada, pregunta lo justo si el pedido es ambiguo (qué placa tiene, si quiere el cambio en las dos interfaces). Luego hazlo, compílalo y enséñalo con una captura.

## Mapa rápido

| Quiero cambiar… | Dónde |
|---|---|
| El personaje | `tools/demo_avatar.py` (el robot) o PNG propios → `tools/make_avatar.py` → `firmware/src/character/sprites43.h` + `sprites_data43.cpp` |
| Colores, tipografías, disposición (P4 720×720) | `firmware/src/ui/ui720.cpp` (constantes arriba; una función `draw…` por zona o página) |
| Lo mismo en la AMOLED 1.75 (466×466) | `firmware/src/ui/ui175.cpp` |
| Fuentes (tamaños, glifos) | `tools/vlw.py` → `firmware/include/fonts/*.h` |
| Pines, pantalla, táctil de una placa | `firmware/include/board.h` + `firmware/src/board*.cpp` |
| Nombre, carácter, forma de hablar | `server/brain/persona.py`, o sin código: `PERSONA_FILE`, `COMPANION_NAME`, `COMPANION_OWNER` en `.env` |
| Voz (TTS) y reconocimiento (STT) | `.env`: `PIPER_MODEL`, `TTS_ENGINE`, `STT_URL`, `STT_VOCABULARIO`; código en `server/voice/voz.py` |
| Datos del tablero (homelab, agenda, redes) | `server/brain/tablero.py` → mensaje `dash` → `firmware/src/ui/model.h` (`Dash`) → `firmware/src/net/net.cpp` → `ui720.cpp` |
| Qué hace con lo que dices | `server/brain/core.py` (conversación), `server/brain/hermes.py` (Hermes Agent) |
| Lo que Hermes hace en otros canales | `server/brain/avisos_hermes.py` + `hermes-plugin/companion/` |

## Recetas

### Cambiar el personaje

- **Retocar el robot:** edita `tools/demo_avatar.py` (colores `BODY`, `SHADE`, `VISOR`, `EYE`, `ACCENT` por estado; formas en `frame()`).
- **Uno nuevo:** cuadros de **120×160 con transparencia**, con el personaje apoyado abajo (pies hacia y≈150), nombrados `<estado>-NNN.png`.
  - Son 16 estados: idle, listening, thinking, talking, working, success, wave, confused, reading, typing, searching, executing, approval, error, reconnecting, sleeping.
  - Los que falten usan idle.
  - Con `avatar.json` se ajustan `fps`, `loop`, `label` e `icon` por estado.

```bash
python tools/demo_avatar.py avatar/pana      # o tu carpeta de PNG
python tools/make_avatar.py avatar/pana
```

- Cabe en flash de sobra (unos 500 KB por 280 cuadros). Es pixel art: la placa lo amplía ×2,5 con vecino más cercano.
- Para otra escala cambia las seis macros `AV_*` de `firmware/src/character/avatar.h`.
- Si lo haces más grande, comprueba que no pisa el panel de abajo (`CARD` en `ui720.cpp`).
- Si el usuario da una foto o un dibujo, genera los cuadros en pixel art a 120×160: mantén la silueta fija entre cuadros y anima ojos, boca y brazos. Así no baila.

### Colores y diseño

- La paleta está al principio de `ui720.cpp` y `ui175.cpp`: `BG`, `INK`, `MUTED`, `AMBER`, `GREEN`, `TEAL`, `VIOLET`…
- Las medidas son constantes con nombre: `AV_TOP`, `CARD`, `GA_OUT`…, el radio del anillo, etc.
- **Pantalla redonda:** usa `halfAt(y0, y1)` para saber cuánto ancho hay a una altura. Nada debe pisar el anillo del borde (`RING_IN`).
- **Suavizado:** arcos con `aaArc`/`arc`, nunca `fillArc` (sale en escalera); círculos con `drawSpot`; rectángulos con `fillSmoothRoundRect`.
- **Rendimiento en la P4:** el bucle `uiFrame` pinta por capas. Si algo se mueve, repinta y copia solo su rectángulo (`lcd->flush(scr, x, y, w, h)`). Un repintado completo en cada cuadro satura la PSRAM y la pantalla parpadea con rayas (ver `docs/hardware.md`).
- **Fuentes:** se generan con `tools/vlw.py FUENTE.ttf TAMAÑO PESO CARACTERES NOMBRE`. Usa «latin…» para el alfabeto más extras.

### Persona y voz

- Lo más simple: `PERSONA_FILE` en `.env` con un texto propio (`{nombre}` y `{dueno}` se rellenan).
- La regla de no hacer nada externo sin confirmación debe quedarse siempre.
- Voces de Piper: <https://github.com/rhasspy/piper/blob/master/VOICES.md>. Si quieren su propia voz clonada, `TTS_ENGINE=http` con un servicio que reciba `{"texto"}` y devuelva PCM 16 kHz mono.
- `STT_VOCABULARIO`: nombres propios que Whisper debe reconocer.

### Un dato nuevo en el tablero

Ejemplo: precio de una cripto o el tiempo.

1. **Servidor:** en `tablero.py`, una función `async` que lo obtenga, con su propio ritmo (como `_youtube`, que se pide cada 15 min) y sin romper si falla. Añádelo al dict del mensaje `dash`.
2. **Modelo:** campo nuevo en `struct Dash` (`model.h`) y parseo en el bloque `"dash"` de `net.cpp`.
3. **Interfaz:** dibújalo en `ui720.cpp`, en la página que toque (`drawDay`, `drawLab` o el panel de la esfera, `drawPanel`). Si cambia a menudo, mételo en la clave de repintado o repinta solo su zona.
4. **Documentación:** el campo nuevo en `protocol/PROTOCOL.md`.
5. **Test:** uno en `server/tests/` si hay lógica (fechas, cálculos).

### Otra placa de Waveshare

Sigue [`docs/hardware.md`](../../../docs/hardware.md#añadir-una-placa). Los pines y la secuencia de la pantalla salen del BSP oficial de Waveshare para esa placa (registro de componentes de Espressif), no de memoria. Ve por pasos: primero pantalla, luego táctil y luego audio, comprobando cada uno con el monitor serie.

## Compilar, grabar y comprobar

```bash
cd firmware
pio run -e amoled175 -t upload                 # AMOLED 1.75
pio run -e p4-4c && ../tools/flash_p4.sh       # P4 4C (esptool de ESP-IDF)
python ../tools/shot.py <puerto> /tmp/captura 12   # captura de lo que pinta la placa (PNG)
```

- Mira siempre la captura antes de dar algo por bueno. Para ver otras páginas, `shot.py` acepta atajos de serie al final (`g` página siguiente, `e` consola).
- Abrir el puerto serie reinicia la placa: espera ~12 s a que conecte antes de capturar.
- Servidor: `cd server && python -m pytest -q` y `python main.py`. El simulador web está en `http://localhost:8770`.

## Reglas

- El protocolo es el contrato: si cambias un mensaje, cambia a la vez `server/protocol.py`, `firmware/src/net/net.cpp`, `server/sim/app.js` y `protocol/PROTOCOL.md`.
- Nada de secretos en el repo: claves en archivos aparte o en `.env`; la Wi-Fi en `firmware/include/secrets.h`, que está ignorado.
- Comentarios y textos de la interfaz en español, con el mismo estilo que el código que los rodea.
