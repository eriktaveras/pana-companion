"""Cuerpos conectados (placa y simuladores). Emite a todos y recuerda lo que está en
pantalla para repintarlo cuando alguien se conecta tarde."""
import asyncio
import logging

from fastapi import WebSocket
from pydantic import BaseModel

log = logging.getLogger("hub")


class Cuerpo:
    def __init__(self, ws: WebSocket):
        self.ws = ws
        self.info = None          # Hello
        self.status = None        # último Status
        self.lock = asyncio.Lock()

    @property
    def nombre(self):
        return self.info.device_id if self.info else "?"


class Hub:
    def __init__(self):
        self.cuerpos: list[Cuerpo] = []
        self.pantalla: dict[str, dict] = {}     # state / bubble / card / confirm actuales

    def hay_altavoz(self) -> bool:
        return any(c.info and c.info.has_speaker for c in self.cuerpos)

    async def _enviar(self, c: Cuerpo, texto: str | None = None, datos: bytes | None = None):
        try:
            async with c.lock:
                if texto is not None:
                    await c.ws.send_text(texto)
                else:
                    await c.ws.send_bytes(datos)
        except Exception as e:
            log.info("no se pudo enviar a %s: %s", c.nombre, e)

    async def emitir(self, msg: BaseModel | dict):
        d = msg.model_dump() if isinstance(msg, BaseModel) else msg
        t = d["type"]
        # lo que está en pantalla, para repintar a quien llegue después
        if t == "state":
            self.pantalla["state"] = d
        elif t == "say":
            self.pantalla["bubble"] = {**d, "audio": False}
        elif t == "card":
            self.pantalla["card"] = d
        elif t == "confirm":
            self.pantalla["confirm"] = d
        elif t == "graph":
            self.pantalla["graph"] = d
        elif t == "dash":
            self.pantalla["dash"] = d
        elif t == "confirm_close":
            if self.pantalla.get("confirm", {}).get("id") == d["id"]:
                self.pantalla.pop("confirm", None)
        elif t == "clear":
            if d["what"] in ("card", "all"):
                self.pantalla.pop("card", None)
            if d["what"] in ("bubble", "all"):
                self.pantalla.pop("bubble", None)
        texto = _json(d)
        await asyncio.gather(*(self._enviar(c, texto=texto) for c in list(self.cuerpos)))

    async def emitir_audio(self, pcm: bytes):
        destinos = [c for c in self.cuerpos if c.info and c.info.has_speaker]
        await asyncio.gather(*(self._enviar(c, datos=pcm) for c in destinos))

    async def repintar(self, c: Cuerpo):
        for k in ("graph", "dash", "state", "bubble", "card", "confirm"):
            if k in self.pantalla:
                await self._enviar(c, texto=_json(self.pantalla[k]))


def _json(d: dict) -> str:
    import json
    return json.dumps(d, ensure_ascii=False)
