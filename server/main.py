"""Servidor del companion: WebSocket para los cuerpos, simulador web y API de consola.

    cd server && ../.venv/bin/python main.py
"""
import asyncio
import logging
import time
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from brain import obsidian, tablero
from brain.avisos_hermes import AvisosHermes
from brain.core import Cerebro
from config import RAIZ, cfg
from hub import Cuerpo, Hub
from protocol import (EMOTIONS, AudioEndIn, AudioStart, ConfirmReply, Hello, Ota, Ping, Pong, Say, State,
                      Status, Touch, parse_entrante)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)-9s %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger("main")
for ruidoso in ("httpx", "uvicorn.access"):
    logging.getLogger(ruidoso).setLevel(logging.WARNING)

hub = Hub()


def _negocio():
    if cfg.negocio_adapter == "mock":
        from tools.negocio_mock import NegocioMock
        return NegocioMock()
    from tools.negocio_real import NegocioReal
    return NegocioReal()


def _voz():
    try:
        from voice.voz import Voz
        return Voz()
    except Exception as e:
        log.warning("voz desactivada: %s", e)
        return None


cerebro = Cerebro(cfg, hub, negocio=_negocio(), voz=_voz())
avisos = AvisosHermes(cerebro)


@asynccontextmanager
async def vida(app):
    tareas = [asyncio.create_task(cerebro.vivir()), asyncio.create_task(_latido()),
              asyncio.create_task(obsidian.vigilar(hub)),
              asyncio.create_task(tablero.vigilar(hub))]
    log.info("Pana despierto en :%d (negocio=%s, voz=%s)", cfg.port, cfg.negocio_adapter,
             "sí" if cerebro.voz and cerebro.voz.tts_listo() else "no")
    yield
    for t in tareas:
        t.cancel()


app = FastAPI(title="Companion", lifespan=vida)


async def _latido():
    """ping cada 20 s: así la placa detecta una conexión muerta."""
    while True:
        await asyncio.sleep(20)
        await hub.emitir(Ping(t=int(time.time())))


# --- WebSocket ---------------------------------------------------------------------
@app.websocket("/ws")
async def ws(websocket: WebSocket):
    await websocket.accept()
    c = Cuerpo(websocket)
    hub.cuerpos.append(c)
    mic: bytearray | None = None
    try:
        while True:
            m = await websocket.receive()
            if m["type"] == "websocket.disconnect":
                break
            if m.get("bytes") is not None:
                if mic is not None:
                    mic.extend(m["bytes"])
                continue
            try:
                msg = parse_entrante(m["text"])
            except ValueError as e:
                log.warning("mensaje fuera de protocolo de %s: %.120s (%s)", c.nombre, m.get("text"), e)
                continue
            if isinstance(msg, Hello):
                c.info = msg
                log.info("hola de %s (%s, fw %s, mic=%s, altavoz=%s)", msg.device_id, msg.kind, msg.fw_version,
                         msg.has_mic, msg.has_speaker)
                await hub.repintar(c)
                if "state" not in hub.pantalla:
                    await cerebro.pintar_animo()
            elif isinstance(msg, Touch):
                log.info("toque %s %s (%d,%d) de %s", msg.gesture, msg.target, msg.x, msg.y, c.nombre)
                asyncio.create_task(cerebro.toque(msg, c.nombre))
            elif isinstance(msg, ConfirmReply):
                log.info("confirm %s → %s de %s", msg.id, msg.choice, c.nombre)
                cerebro.responder_confirm(msg.id, msg.choice, c.nombre)
            elif isinstance(msg, AudioStart):
                mic = bytearray()
                await hub.emitir(State(emotion="idle", look=(0.0, -0.8)))     # escucha mirando arriba, a quien le habla
            elif isinstance(msg, AudioEndIn):
                if mic is not None:
                    asyncio.create_task(cerebro.voz_entrante(bytes(mic)))
                mic = None
            elif isinstance(msg, Status):
                c.status = msg
            elif isinstance(msg, Pong):
                pass
    except WebSocketDisconnect:
        pass
    finally:
        hub.cuerpos.remove(c)
        log.info("se fue %s", c.nombre)


# --- simulador ----------------------------------------------------------------------
SIM = Path(__file__).parent / "sim"
app.mount("/sim", StaticFiles(directory=SIM, html=True), name="sim")


@app.get("/")
async def raiz():
    return RedirectResponse("/sim/")


@app.get("/face.json")
async def face():
    return FileResponse(RAIZ / "protocol" / "face.json")


# --- OTA: la placa baja de aquí su firmware ---------------------------------------
FW = RAIZ / "server" / "data" / "fw"


@app.get("/fw/firmware.bin")
async def firmware():
    return FileResponse(FW / "firmware.bin", media_type="application/octet-stream")


# --- API de consola --------------------------------------------------------------------
class TextoIn(BaseModel):
    text: str


class LeadIn(BaseModel):
    nombre: str | None = None
    mensaje: str | None = None


@app.post("/api/state/{emotion}")
async def api_state(emotion: str, animation: str | None = None, x: float | None = None, y: float | None = None):
    if emotion not in EMOTIONS:
        return JSONResponse({"error": f"emociones: {EMOTIONS}"}, 400)
    await hub.emitir(State(emotion=emotion, animation=animation, look=(x, y) if x is not None and y is not None else None))
    return {"ok": True, "cuerpos": len(hub.cuerpos)}


@app.post("/api/say")
async def api_say(t: TextoIn):
    asyncio.create_task(cerebro.decir(t.text))
    return {"ok": True}


@app.post("/api/chat")
async def api_chat(t: TextoIn):
    """Hablarle por texto (como si fuera voz ya transcrita)."""
    return {"respuesta": await cerebro.conversar(t.text)}


@app.post("/api/lead")
async def api_lead(l: LeadIn):
    if not hasattr(cerebro.negocio, "nuevo_lead"):
        return JSONResponse({"error": "solo con el adaptador mock"}, 400)
    lead = cerebro.negocio.nuevo_lead(l.nombre, l.mensaje)
    await cerebro.evento("lead_nuevo", f"lead nuevo de {lead['nombre']} por {lead['canal']}: {lead['ultimo_mensaje']}", lead)
    return lead


@app.post("/api/evento/{tipo}")
async def api_evento(tipo: str, t: TextoIn):
    await cerebro.evento(tipo, t.text)
    return {"ok": True}


@app.post("/api/hermes-evento")
async def api_hermes_evento(ev: dict):
    """Lo manda el plugin companion de Hermes (agent:start/step/end de otros canales)."""
    asyncio.create_task(avisos.recibir(ev))
    return {"ok": True}


@app.post("/api/ciclo")
async def api_ciclo():
    from brain import heartbeat
    d = await heartbeat.ciclo(cerebro, "manual")
    return d.model_dump()


@app.post("/api/sueno")
async def api_sueno():
    from brain import dream
    s = await dream.soñar(cerebro)
    return s.model_dump() if s else {"error": "sin sueño"}


@app.post("/api/brief")
async def api_brief():
    await cerebro.brief_matutino()
    return {"ok": True}


@app.post("/api/ota")
async def api_ota(version: str = "nueva"):
    if not (FW / "firmware.bin").exists():
        return JSONResponse({"error": "no hay data/fw/firmware.bin"}, 400)
    await hub.emitir(Ota(url=f"{cfg.public_url}/fw/firmware.bin", version=version))
    return {"ok": True}


@app.get("/api/debug/tareas")
async def api_tareas():
    """Pila de cada tarea asyncio viva: para ver dónde se queda colgado algo."""
    import io
    out = []
    for t in asyncio.all_tasks():
        b = io.StringIO()
        t.print_stack(limit=12, file=b)
        out.append(b.getvalue())
    return {"tareas": out}


@app.post("/api/debug/confirm")
async def api_debug_confirm(pregunta: str = "Hermes pide permiso", preview: str = "rm -rf ~/tmp/cache"):
    """Para probar el diálogo en la placa: no ejecuta nada, solo registra la elección."""
    asyncio.create_task(cerebro.compuerta.preguntar(pregunta, preview, ("Permitir", "Esta sesión", "Denegar")))
    return {"ok": True}


@app.get("/api/estado")
async def api_estado():
    return {
        "animo": cerebro.mood.dict(),
        "emocion": cerebro.mood.emocion(cerebro.en_silencio()),
        "silencio": cerebro.en_silencio(),
        "cuerpos": [{"id": c.nombre, "kind": c.info.kind if c.info else "?",
                     "status": c.status.model_dump() if c.status else None} for c in hub.cuerpos],
        "pantalla": hub.pantalla,
        "intervenciones_ultima_hora": cerebro.intervenciones_ultima_hora(),
        "auditoria": cerebro.memoria.auditoria(10),
        "recuerdos": cerebro.memoria.todos_recuerdos()[-20:],
    }


if __name__ == "__main__":
    uvicorn.run(app, host=cfg.host, port=cfg.port, log_level="warning", ws_max_size=2**20)
