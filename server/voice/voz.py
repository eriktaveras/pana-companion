"""Voz: STT con whisper-server (whisper.cpp) y TTS con
Piper en CPU (más rápido que el tiempo real). Todo sale y entra como PCM 16 kHz mono int16.

Con TTS_ENGINE=http habla con un servicio de voz propio (TTS_HTTP_URL,
Chatterbox en CPU, ~2x más lento que el tiempo real): core.decir_stream genera la frase
siguiente mientras suena la actual. Si el servicio falla, cada frase sale con Piper.
"""
import os
import asyncio
import io
import logging
import time
import wave
from pathlib import Path
from typing import AsyncIterator

import httpx
import numpy as np

from config import RAIZ, cfg
from voice.fonetica import a_voz

log = logging.getLogger("voz")
CHUNK = 2048            # bytes por frame binario (1024 muestras)


def pcm_a_wav(pcm: bytes, rate=16000) -> bytes:
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return b.getvalue()


def normalizar(pcm: bytes, objetivo=0.5) -> bytes:
    """El micro de la placa entra bajito: se sube la ganancia hasta un pico de `objetivo`."""
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    if not len(x):
        return pcm
    x -= x.mean()
    pico = np.percentile(np.abs(x), 99.9) or 1
    g = min(40.0, objetivo * 32767 / pico)
    return np.clip(x * g, -32767, 32767).astype(np.int16).tobytes()


# Nombres que Whisper no conoce y oye mal («CRE», «Metavera»): se le pasan como contexto
VOCABULARIO = os.environ.get("STT_VOCABULARIO", "Hermes, Pana, Obsidian, homelab, Instagram, YouTube, Claude, ESP32, Waveshare.")


def tramos_con_voz(pcm: bytes, rate=16000) -> tuple[float, float]:
    """(segundos con voz, pico en dBFS). Ventanas de 20 ms por encima del suelo de ruido.
    Sin esto, whisper «oye» frases enteras en el silencio."""
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    if len(x) < rate // 10:
        return 0.0, -120.0
    x -= x.mean()
    n = rate // 50
    v = x[: len(x) // n * n].reshape(-1, n)
    db = 20 * np.log10(np.maximum(np.sqrt((v ** 2).mean(axis=1)), 1) / 32768)
    suelo = np.percentile(db, 15)
    umbral = max(suelo + 12, -62)
    return float((db > umbral).sum() * n / rate), float(db.max())


def remuestrear(x: np.ndarray, de: int, a: int) -> np.ndarray:
    if de == a:
        return x
    from scipy.signal import resample_poly
    from math import gcd
    g = gcd(de, a)
    return resample_poly(x, a // g, de // g)


class Voz:
    def __init__(self):
        self._piper = None
        self._http = httpx.AsyncClient(timeout=60)
        self._cargar_piper()

    def _cargar_piper(self):
        ruta = Path(cfg.piper_model)
        if not ruta.is_absolute():
            ruta = RAIZ / "server" / ruta
        if not ruta.exists():
            log.warning("sin voz: no existe %s", ruta)
            return
        try:
            from piper import PiperVoice
            self._piper = PiperVoice.load(str(ruta))
            log.info("piper cargado: %s (%d Hz)", ruta.name, self._piper.config.sample_rate)
        except Exception as e:
            log.warning("piper no carga: %s", e)

    def tts_listo(self) -> bool:
        return self._piper is not None or cfg.tts_engine == "http"

    async def transcribir(self, pcm: bytes) -> str:
        self.ultima_voz = 0.0
        if len(pcm) < 16000:            # menos de medio segundo: fue un toque, no una frase
            return ""
        voz, pico = tramos_con_voz(pcm)
        self.ultima_voz = voz
        log.info("micro: %.2f s con voz, pico %.0f dBFS", voz, pico)
        if voz < 0.35 or pico < -50:
            return ""
        wav = pcm_a_wav(normalizar(pcm))
        try:
            r = await self._http.post(cfg.stt_url, files={"file": ("voz.wav", wav, "audio/wav")},
                                      data={"language": "es", "response_format": "json", "temperature": "0",
                                            "prompt": VOCABULARIO})
            r.raise_for_status()
            return r.json().get("text", "").strip()
        except httpx.HTTPError as e:
            log.warning("stt falló: %s", e)
            return ""

    def _sintetizar_bloque(self, texto: str) -> bytes:
        """Piper es síncrono: se corre en un hilo aparte."""
        partes = []
        for trozo in self._piper.synthesize(texto):
            partes.append(trozo.audio_int16_array)
        if not partes:
            return b""
        x = np.concatenate(partes).astype(np.float32)
        x = remuestrear(x, self._piper.config.sample_rate, 16000)
        return np.clip(x, -32767, 32767).astype(np.int16).tobytes()

    async def audio(self, texto: str) -> bytes:
        """La frase entera en PCM 16 kHz. Con un servicio de voz propio, si falla, Piper."""
        if not texto.strip():
            return b""
        t0 = time.perf_counter()
        pcm, motor = b"", "piper"
        if cfg.tts_engine == "http":
            try:
                # sin a_voz(): el servicio aplica su propia fonética (la que se probó con Chatterbox)
                r = await self._http.post(cfg.tts_http_url, json={"texto": texto}, timeout=120)
                r.raise_for_status()
                pcm, motor = r.content, "http"
            except httpx.HTTPError as e:
                log.warning("la voz propia falló (%s): Piper", e)
        if not pcm and self._piper:
            pcm = await asyncio.to_thread(self._sintetizar_bloque, a_voz(texto))
        log.info("tts %s %.2fs → %.1f s de audio: %s", motor, time.perf_counter() - t0, len(pcm) / 32000, texto[:60])
        return pcm

    async def sintetizar(self, texto: str) -> AsyncIterator[bytes]:
        if not self.tts_listo() or not texto.strip():
            return
        pcm = await self.audio(texto)
        for i in range(0, len(pcm), CHUNK):
            yield pcm[i:i + CHUNK]
            # no inundar la placa: ~1,5x el tiempo real
            await asyncio.sleep(CHUNK / 32000 / 1.5)
