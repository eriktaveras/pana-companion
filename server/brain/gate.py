"""Compuerta de acciones externas. Ninguna herramienta `act` se ejecuta sin pasar por aquí:
se manda un `confirm` a los cuerpos, se espera el toque y se audita el resultado.
Si nadie responde en el plazo, se cancela. Nunca se ejecuta por defecto."""
import asyncio
import logging
import time
import uuid

from protocol import Confirm, ConfirmClose
from tools.base import Herramienta

log = logging.getLogger("gate")


class Compuerta:
    def __init__(self, hub, memoria, timeout_s: float):
        self.hub = hub
        self.memoria = memoria
        self.timeout_s = timeout_s
        self._esperando: dict[str, asyncio.Future] = {}

    def responder(self, id: str, eleccion: str, quien: str) -> bool:
        """Lo llama el WebSocket al llegar un confirm_reply. Solo vale la primera respuesta."""
        fut = self._esperando.get(id)
        if not fut or fut.done():
            return False
        fut.set_result((eleccion, quien))
        return True

    async def preguntar(self, pregunta: str, preview: str, opciones=("Enviar", "Editar", "Cancelar")) -> tuple[str, str]:
        """Muestra el diálogo y devuelve (elección, quién). 'caducado' si no hubo respuesta."""
        id = uuid.uuid4().hex[:8]
        fut = asyncio.get_running_loop().create_future()
        self._esperando[id] = fut
        await self.hub.emitir(Confirm(id=id, question=pregunta, preview=preview, options=list(opciones),
                                      expires_in=int(self.timeout_s)))
        try:
            eleccion, quien = await asyncio.wait_for(fut, self.timeout_s)
        except asyncio.TimeoutError:
            eleccion, quien = "caducado", "-"
        finally:
            self._esperando.pop(id, None)
        await self.hub.emitir(ConfirmClose(id=id, result=eleccion))
        log.info("confirm %s → %s (%s)", id, eleccion, quien)
        return eleccion, quien

    async def ejecutar(self, h: Herramienta, args: dict, quien: str) -> dict:
        """Ejecuta una herramienta act YA confirmada y deja rastro en la auditoría."""
        t0 = time.perf_counter()
        try:
            res = await h(_confirmado=True, **args)
        except Exception as e:
            res = {"ok": False, "error": str(e)}
        self.memoria.auditar(h.nombre, args, quien, str(res))
        log.info("act %s %s → %s (%.2fs)", h.nombre, args, res, time.perf_counter() - t0)
        return res

    async def proponer(self, h: Herramienta, args: dict, pregunta: str, preview: str,
                       reescribir=None, max_ediciones=3) -> dict:
        """Ciclo completo: preguntar → (Editar → reescribir → preguntar)* → ejecutar o no.
        `reescribir(preview) -> (args, preview)` genera otra versión cuando el usuario toca Editar."""
        for _ in range(max_ediciones + 1):
            eleccion, quien = await self.preguntar(pregunta, preview)
            if eleccion == "Enviar":
                return {"ejecutado": True, "resultado": await self.ejecutar(h, args, quien)}
            if eleccion == "Editar" and reescribir:
                args, preview = await reescribir(preview)
                continue
            self.memoria.auditar(h.nombre, args, quien, f"no ejecutado: {eleccion}")
            return {"ejecutado": False, "motivo": eleccion}
        return {"ejecutado": False, "motivo": "demasiadas ediciones"}
