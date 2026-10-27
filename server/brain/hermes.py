"""Cliente del API server de Hermes (HERMES_URL, por defecto 127.0.0.1:8642).

Con COMPANION_BRAIN=hermes la conversación no va al llama-server directo sino a Hermes:
sus herramientas, su memoria y sus skills. Una sesión fija (X-Hermes-Session-Id) da
continuidad entre turnos. Del stream SSE salen tres cosas:
  - texto (delta.content) → se va diciendo por frases
  - event: hermes.tool.progress → el cuerpo se pone a «trabajar» (sprite con laptop)
  - event: approval.request → confirm en la pantalla; la respuesta vuelve a /v1/runs/{id}/approval
"""
import json
import logging
import re
import time
from pathlib import Path
from typing import AsyncIterator, Awaitable, Callable

import httpx

from config import cfg

log = logging.getLogger("hermes")

# Va delante del mensaje del usuario en cada turno: lo que sale se lee en 240 px y se oye con Piper
_INSTRUCCION = ("[Respuesta para el companion de escritorio: se lee en una pantalla pequeña y se dice en voz alta. "
                "Contesta en español, en 1 o 2 frases cortas (unas 25 palabras en total), sin markdown, sin listas, "
                "sin emojis ni URLs; las cifras, redondeadas.]\n\n")

_cliente: httpx.AsyncClient | None = None


def _clave() -> str:
    p = Path(cfg.hermes_key_file).expanduser()
    return p.read_text().strip() if p.exists() else ""


def _http() -> httpx.AsyncClient:
    global _cliente
    if _cliente is None:
        # Hermes manda «: keepalive» cada 10 s: el plazo de lectura cubre herramientas largas
        _cliente = httpx.AsyncClient(timeout=httpx.Timeout(cfg.hermes_timeout_s, connect=5),
                                     headers={"Authorization": f"Bearer {_clave()}"})
    return _cliente


def para_voz(texto: str) -> str:
    """Quita lo que Piper leería mal o que no cabe en la burbuja (markdown, URLs)."""
    t = re.sub(r"```.*?```", " ", texto, flags=re.S)
    t = re.sub(r"`([^`]*)`", r"\1", t)
    t = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", t)
    t = re.sub(r"https?://\S+", "", t)
    t = re.sub(r"[*_#>]+", "", t)
    return t


async def responder(texto: str,
                    al_usar_herramienta: Callable[[str, str, str, str], Awaitable[None]],
                    al_pedir_aprobacion: Callable[[dict], Awaitable[str]]) -> AsyncIterator[str]:
    """Trozos de la respuesta de Hermes según llegan.
    `al_usar_herramienta(nombre, etiqueta, estado, id)` se llama al empezar (running) y al acabar cada herramienta.
    `al_pedir_aprobacion(evento)` devuelve once | session | deny y se reenvía a Hermes."""
    body = {"model": "hermes-agent", "stream": True,
            "messages": [{"role": "user", "content": _INSTRUCCION + texto}]}
    headers = {"X-Hermes-Session-Id": cfg.hermes_session}
    t0 = time.perf_counter()
    primero = True
    evento = None
    async with _http().stream("POST", f"{cfg.hermes_url}/chat/completions", json=body, headers=headers) as r:
        r.raise_for_status()
        async for linea in r.aiter_lines():
            if not linea or linea.startswith(":"):
                if not linea:
                    evento = None
                continue
            if linea.startswith("event: "):
                evento = linea[7:].strip()
                continue
            if not linea.startswith("data: ") or linea == "data: [DONE]":
                continue
            datos = json.loads(linea[6:])
            if evento == "hermes.tool.progress":
                estado = datos.get("status", "")
                if estado == "running":
                    log.info("herramienta %s: %s", datos.get("tool"), datos.get("label", "")[:80])
                await al_usar_herramienta(datos.get("tool", ""), datos.get("label", ""), estado,
                                          datos.get("toolCallId", ""))
                continue
            if evento == "approval.request":
                eleccion = await al_pedir_aprobacion(datos)
                run_id = datos.get("run_id")
                pedido = {"choice": eleccion}
                if datos.get("request_id"):
                    pedido["request_id"] = datos["request_id"]
                a = await _http().post(f"{cfg.hermes_url}/runs/{run_id}/approval", json=pedido)
                log.info("aprobación %s → %s (%s)", run_id, eleccion, a.status_code)
                continue
            delta = (datos.get("choices") or [{}])[0].get("delta", {}).get("content")
            if delta:
                if primero:
                    log.info("hermes primer token %.2fs", time.perf_counter() - t0)
                    primero = False
                yield para_voz(delta)
    log.info("hermes turno %.2fs", time.perf_counter() - t0)
