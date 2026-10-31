# AGENTS.md · pana-companion

Pana es un companion de escritorio que pone cara y voz a un agente de IA (Hermes Agent o un LLM compatible con OpenAI) en placas Waveshare con pantalla redonda. El firmware ('el cuerpo') solo pinta, anima, escucha y reproduce. El servidor FastAPI ('el cerebro') hace STT con whisper.cpp, TTS con Piper, habla con el agente y controla la placa por WebSocket según protocol/PROTOCOL.md.

## Stack

Firmware en C++ (Arduino-ESP32 3.x con PlatformIO/pioarduino, LovyanGFX, ArduinoJson, WebSockets, WiFiManager) para ESP32-S3 y ESP32-P4. Servidor en Python 3.11+ (FastAPI, uvicorn, pydantic 2, httpx, 

## Comandos

- Instalar: `uv venv .venv && uv pip install --python .venv/bin/python -r server/requirements.txt`
- Tests: `cd server && ../.venv/bin/python -m pytest -q`
- Build: `uv tool install platformio && cd firmware && pio run -e amoled175 && pio run -e p4-4c`

## Notas para agentes

Lee primero CLAUDE.md y .claude/skills/personalizar-pana/SKILL.md. Todo va en español: código, comentarios e interfaz. No hay CI (ni .github/ ni .forgejo/), ni linter, ni pyproject. La única verificación automática son los tests de server/tests/test_basico.py, que no usan red; pytest.ini pone pythonpath=. y hay que lanzarlo desde server/. Placas: amoled175 usa src/ui/ui175.cpp y p4-4c usa src/ui/ui720.cpp; los pines están en include/board.h y el contrato cuerpo-cerebro en protocol/PROTOCOL.md (con protocol/face.json). Si cambias ese contrato, actualiza a la vez server/protocol.py, el firmware y server/sim. firmware/src/character/sprites43.h y sprites_data43.cpp los genera tools/make_avatar.py: no se editan a mano. Lo mismo vale para las fuentes de include/fonts/, que salen de tools/vlw.py. El firmware compila en Linux sin secrets.h (se incluye con __has_include). Grabar la placa y sacar capturas con tools/shot.py exige hardware por USB, así que en esta máquina solo se puede compilar.

---
Propuesto por el Taller de hermes-web a partir de su reconocimiento del repo; revísalo antes de fusionar.
