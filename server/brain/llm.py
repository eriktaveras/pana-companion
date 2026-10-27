"""Cliente OpenAI-compatible para un llama-server (o cualquier API compatible con OpenAI).

Tres niveles: fast (ciclo autónomo), chat (conversación y voz) y big (sueño).
Las salidas estructuradas se piden con json_schema (llama.cpp lo convierte en
gramática) y además se validan con Pydantic: reintento y, si sigue mal, None.
"""
import json
import logging
import time
from typing import AsyncIterator, TypeVar

import httpx
import numpy as np
from pydantic import BaseModel, ValidationError

from config import cfg

log = logging.getLogger("llm")
T = TypeVar("T", bound=BaseModel)

_NIVELES = {
    "fast": lambda: (cfg.fast_url, cfg.fast_model),
    "chat": lambda: (cfg.chat_url, cfg.chat_model),
    "big": lambda: (cfg.big_url, cfg.big_model),
}

_cliente: httpx.AsyncClient | None = None


def _http() -> httpx.AsyncClient:
    global _cliente
    if _cliente is None:
        _cliente = httpx.AsyncClient(timeout=httpx.Timeout(120, connect=5),
                                     headers={"Authorization": f"Bearer {cfg.llm_key()}"})
    return _cliente


def _cuerpo(nivel, mensajes, max_tokens, temperature, schema=None, stream=False):
    url, model = _NIVELES[nivel]()
    body = {
        "model": model,
        "messages": mensajes,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": stream,
        # sin razonamiento visible: aquí importa la latencia
        "chat_template_kwargs": {"enable_thinking": False},
    }
    if schema:
        body["response_format"] = {"type": "json_schema", "json_schema": {"name": "salida", "schema": schema}}
    return url, body


async def chat(mensajes: list[dict], nivel="chat", max_tokens=400, temperature=0.7) -> str:
    url, body = _cuerpo(nivel, mensajes, max_tokens, temperature)
    t0 = time.perf_counter()
    r = await _http().post(f"{url}/chat/completions", json=body)
    r.raise_for_status()
    texto = r.json()["choices"][0]["message"]["content"] or ""
    log.info("llm %s %.2fs %d car.", nivel, time.perf_counter() - t0, len(texto))
    return texto.strip()


async def chat_stream(mensajes: list[dict], nivel="chat", max_tokens=300, temperature=0.7) -> AsyncIterator[str]:
    """Trozos de texto según llegan (para empezar a hablar antes de que termine)."""
    url, body = _cuerpo(nivel, mensajes, max_tokens, temperature, stream=True)
    t0 = time.perf_counter()
    primero = True
    # timeout de lectura = plazo para el primer token y entre trozos: si la GPU está
    # saturada se corta y quien llama usa su plan B, en vez de quedarse colgado
    async with _http().stream("POST", f"{url}/chat/completions", json=body,
                              timeout=httpx.Timeout(20, connect=5)) as r:
        r.raise_for_status()
        async for linea in r.aiter_lines():
            if not linea.startswith("data: ") or linea == "data: [DONE]":
                continue
            delta = json.loads(linea[6:])["choices"][0].get("delta", {}).get("content")
            if delta:
                if primero:
                    log.info("llm %s primer token %.2fs", nivel, time.perf_counter() - t0)
                    primero = False
                yield delta


def parse_modelo(texto: str, modelo: type[T]) -> T | None:
    """Saca el JSON del texto (tolera ```json y basura alrededor) y lo valida."""
    t = texto.strip()
    if "```" in t:
        t = t.split("```")[1].removeprefix("json").strip()
    a, b = t.find("{"), t.rfind("}")
    if a < 0 or b < a:
        return None
    try:
        return modelo.model_validate_json(t[a:b + 1])
    except (ValidationError, ValueError):
        return None


async def estructurado(mensajes: list[dict], modelo: type[T], nivel="fast", max_tokens=500,
                       temperature=0.6, intentos=2) -> T | None:
    """Pide una salida que cumpla `modelo`. Devuelve None si no lo consigue."""
    schema = modelo.model_json_schema()
    msgs = list(mensajes)
    for i in range(intentos):
        url, body = _cuerpo(nivel, msgs, max_tokens, temperature, schema=schema)
        t0 = time.perf_counter()
        try:
            r = await _http().post(f"{url}/chat/completions", json=body)
            r.raise_for_status()
            texto = r.json()["choices"][0]["message"]["content"] or ""
        except (httpx.HTTPError, KeyError) as e:
            log.warning("llm %s error: %s", nivel, e)
            return None
        dt = time.perf_counter() - t0
        obj = parse_modelo(texto, modelo)
        if obj is not None:
            log.info("llm %s json %.2fs (intento %d)", nivel, dt, i + 1)
            return obj
        log.warning("llm %s json inválido (intento %d, %.2fs): %.200s", nivel, i + 1, dt, texto)
        msgs = msgs + [{"role": "assistant", "content": texto},
                       {"role": "user", "content": "Eso no es JSON válido para el esquema. Responde SOLO el JSON."}]
    return None


async def embed(textos: list[str]) -> np.ndarray:
    """Vectores normalizados (n, d) en float32."""
    r = await _http().post(f"{cfg.embed_url}/embeddings", json={"model": cfg.embed_model, "input": textos})
    r.raise_for_status()
    v = np.array([d["embedding"] for d in sorted(r.json()["data"], key=lambda d: d["index"])], dtype=np.float32)
    return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-9)
