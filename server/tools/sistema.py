"""Estado REAL de la máquina donde corre el servidor: temperaturas, carga,
memoria y si los modelos responden. Solo lectura."""
import os
import time
from pathlib import Path

import httpx

from config import cfg
from tools.base import Herramienta


def _temperaturas() -> dict:
    out = {}
    for hw in Path("/sys/class/hwmon").glob("hwmon*"):
        try:
            nombre = (hw / "name").read_text().strip()
        except OSError:
            continue
        for f in hw.glob("temp*_input"):
            try:
                v = int(f.read_text()) / 1000
            except (OSError, ValueError):
                continue
            etiqueta = f.with_name(f.name.replace("_input", "_label"))
            et = etiqueta.read_text().strip() if etiqueta.exists() else f.stem
            out[f"{nombre}/{et}"] = round(v, 1)
    return out


def _memoria() -> dict:
    try:
        info = dict(l.split(":", 1) for l in Path("/proc/meminfo").read_text().splitlines())
        kb = lambda k: int(info[k].split()[0])
        return {"total_gb": round(kb("MemTotal") / 2**20, 1), "disponible_gb": round(kb("MemAvailable") / 2**20, 1)}
    except (OSError, KeyError):
        return {}


async def _modelos() -> dict:
    out = {}
    for nombre, url in (("rapido", cfg.fast_url), ("chat", cfg.chat_url), ("embeddings", cfg.embed_url)):
        base = url.removesuffix("/v1")
        t0 = time.perf_counter()
        try:
            async with httpx.AsyncClient(timeout=3) as c:
                r = await c.get(f"{base}/health")
            out[nombre] = "ok" if r.status_code == 200 else f"http {r.status_code}"
        except httpx.HTTPError:
            out[nombre] = "caído"
        out[nombre + "_ms"] = round((time.perf_counter() - t0) * 1000)
    return out


async def estado_sistema() -> dict:
    carga = os.getloadavg() if hasattr(os, "getloadavg") else (0, 0, 0)
    temps = _temperaturas()
    return {
        "carga_1m": round(carga[0], 2), "nucleos": os.cpu_count(),
        "memoria": _memoria(),
        "temp_max": max(temps.values()) if temps else None,
        "temperaturas": temps,
        "modelos": await _modelos(),
    }


async def hora() -> dict:
    from datetime import datetime
    ahora = datetime.now(cfg.tz)
    return {"hora": ahora.strftime("%H:%M"), "fecha": ahora.strftime("%A %d de %B de %Y")}


def herramientas() -> list[Herramienta]:
    return [
        Herramienta("estado_sistema", "read", "temperaturas, carga, memoria y estado de los modelos del servidor", estado_sistema),
        Herramienta("hora", "read", "fecha y hora local", hora),
    ]
