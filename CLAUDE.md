# Pana · notas para Claude

Companion de escritorio para un agente de IA (Hermes Agent) en placas Waveshare: `firmware/` es el cuerpo (Arduino-ESP32 3.x + LovyanGFX, PlatformIO) y `server/` el cerebro (FastAPI). Hablan por WebSocket según `protocol/PROTOCOL.md`.

- Para cualquier personalización (personaje, colores, persona, voz, tablero, otra placa) usa la skill **personalizar-pana** (`.claude/skills/personalizar-pana/SKILL.md`).
- Placas: `amoled175` (ESP32-S3 AMOLED 1.75, UI `ui175.cpp`) y `p4-4c` (ESP32-P4 4C, UI `ui720.cpp`). Detalles en `docs/hardware.md`.
- La P4 se graba con `tools/flash_p4.sh`, no con `pio -t upload`.
- Antes de dar un cambio de interfaz por bueno, compílalo y saca una captura de la placa con `tools/shot.py`.
- Tests del servidor: `cd server && python -m pytest -q`.
- Idioma del código, comentarios e interfaz: español.
