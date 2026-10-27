"""Lo que Hermes hace por otros canales (Telegram, Mattermost…) se ve en la placa.

El plugin companion de Hermes (todos los caminos: dashboard, terminal, gateway)
manda aquí agent:start, tool:start, tool:end y agent:end.
  start → cara de pensar + tarjeta «Hermes trabajando · canal» con lo que le pidieron
  tool  → panel de herramientas (icono, nombre, detalle y estado de cada llamada) y la animación
          de lo que hace: lee, busca, escribe, ejecuta (ver animacion_herramienta); si falla, error
  end   → tarjeta con la respuesta, pulgar arriba y un resumen de una frase dicho en voz
          (lo escribe el modelo rápido, Qwen 4B; si falla, se dice una frase fija)
Lo que se pregunta desde la propia placa (api_server) y las tareas cron no llegan: el plugin las filtra.
Colores: se reutilizan los tipos de tarjeta que ya pinta la placa (noshow = ámbar, lead = verde).
"""
import asyncio
import logging
import re
import time

from brain import hermes, llm
from protocol import Card, CardAction, Clear, State, ToolRow

log = logging.getLogger("avisos")

CANALES = {"telegram": "Telegram", "mattermost": "Mattermost", "discord": "Discord", "whatsapp": "WhatsApp",
           "slack": "Slack", "matrix": "Matrix", "signal": "Signal", "email": "correo",
           "cli": "Terminal", "tui": "Dashboard", "web": "Dashboard", "dashboard": "Dashboard", "desktop": "App"}


def _lineas(texto: str, ancho=30, n=3) -> list[str]:
    """Parte en líneas cortas para la tarjeta (la placa además recorta con «...»)."""
    out, linea = [], ""
    for palabra in " ".join(texto.split()).split(" "):
        if len(linea) + len(palabra) + 1 > ancho and linea:
            out.append(linea)
            linea = palabra
            if len(out) == n:
                break
        else:
            linea = f"{linea} {palabra}".strip()
    if linea and len(out) < n:
        out.append(linea)
    return out


def detalle(nombre: str, args) -> str:
    """Lo que hace la llamada en pocas palabras: el comando, la búsqueda, la ruta…"""
    if isinstance(args, dict):
        for k in ("command", "cmd", "query", "url", "path", "file_path", "pattern", "name", "skill", "action"):
            if isinstance(args.get(k), str) and args[k].strip():
                return args[k]
        for v in args.values():
            if isinstance(v, str) and v.strip():
                return v
        return ""
    return str(args or "")


def comando_corto(c: str) -> str:
    """Quita el relleno del principio de un comando de terminal para que se vea lo que importa."""
    c = " ".join(c.split())
    for _ in range(4):
        c0 = c
        c = re.sub(r"^(sudo|exec|nohup|timeout \S+)\s+", "", c)
        c = re.sub(r"^env(\s+-i)?(\s+[A-Za-z_][A-Za-z0-9_]*=\S*)*\s+", "", c)    # env -i VAR=... cmd
        c = re.sub(r"^([A-Za-z_][A-Za-z0-9_]*=\S*\s+)+", "", c)                      # VAR=... cmd
        c = re.sub(r"^cd\s+\S+\s*(&&|;)\s*", "", c)                                 # cd dir && cmd
        if c == c0:
            break
    return c


def animacion_herramienta(nombre: str) -> str:
    """Qué animación pone el avatar mientras corre la herramienta de Hermes."""
    n = nombre.lower()
    if n == "clarify":
        return "confused"
    if any(k in n for k in ("search", "skills_list", "grep", "find")):
        return "searching"
    if any(k in n for k in ("terminal", "process", "execute", "exec", "delegate", "cron", "click")):
        return "executing"
    if any(k in n for k in ("write", "patch", "edit", "memory", "todo", "send", "skill_manage", "type")):
        return "typing"
    if any(k in n for k in ("read", "view", "extract", "fetch", "vision", "browser", "snapshot", "navigate")):
        return "reading"
    return "working"


class PanelHerramientas:
    """Las llamadas a herramientas de un turno, pintadas en la placa (tarjeta kind «tools»)."""

    def __init__(self, hub, card_id: str, titulo: str):
        self.hub, self.id, self.titulo = hub, card_id, titulo
        self.filas: dict[str, list] = {}       # call_id → [estado, nombre, detalle, t0, t1]

    async def empieza(self, call_id: str, nombre: str, det: str):
        if "terminal" in nombre or "process" in nombre:
            det = comando_corto(det)
        self.filas[call_id or f"sin-id-{len(self.filas)}"] = ["run", nombre, det, time.time(), None]
        await self.hub.emitir(State(emotion=animacion_herramienta(nombre)))
        await self._pintar()

    async def acaba(self, call_id: str, ok: bool = True):
        fila = self.filas.get(call_id) or next((f for f in reversed(self.filas.values()) if f[0] == "run"), None)
        if fila:
            fila[0] = "ok" if ok else "err"
            fila[4] = time.time()
            if not ok:                  # se queda con cara de error hasta la siguiente herramienta
                await self.hub.emitir(State(emotion="error"))
            await self._pintar()

    def cerrar_pendientes(self):
        for f in self.filas.values():
            if f[0] == "run":
                f[0] = "ok"
                f[4] = time.time()

    async def _pintar(self):
        limpio = lambda t, n: " ".join(str(t).split())[:n]
        filas = list(self.filas.values())
        lineas = [f"{e}\t{limpio(n, 90)}\t{limpio(d, 90)}" for e, n, d, _, _ in filas[-4:]]
        consola = [ToolRow(st=e, name=limpio(n, 60), detail=limpio(d, 240), t0=t0,
                           secs=None if t1 is None else round(t1 - t0, 1)) for e, n, d, t0, t1 in filas[-10:]]
        n = len(filas)
        titulo = f"{self.titulo} · {n}" if n > 4 else self.titulo      # en la 2.8" solo caben las 4 últimas
        await self.hub.emitir(Card(id=self.id, kind="tools", title=titulo, lines=lineas,
                                   tools=consola, tools_total=n))


class AvisosHermes:
    def __init__(self, cerebro):
        self.c = cerebro
        self.activas: dict[str, dict] = {}       # session_id → {canal, mensaje, t0, panel}

    def _id(self, sesion: str) -> str:
        return "hermes-" + (sesion or "x")[-8:]

    async def recibir(self, ev: dict):
        tipo, sesion = ev.get("event", ""), ev.get("session_id", "")
        canal = CANALES.get(ev.get("platform", ""), ev.get("platform", "") or "Hermes")
        if tipo == "agent:start":
            self.activas[sesion] = {"canal": canal, "mensaje": ev.get("message", ""), "t0": time.time(),
                                    "panel": PanelHerramientas(self.c.hub, self._id(sesion), f"Hermes trabajando · {canal}")}
            log.info("hermes empieza en %s: %s", canal, ev.get("message", "")[:80])
            await self.c.hub.emitir(State(emotion="thinking"))
            await self.c.hub.emitir(Card(id=self._id(sesion), kind="noshow", title=f"Hermes trabajando · {canal}",
                                         lines=_lineas(ev.get("message", ""), n=4)))
        elif tipo in ("tool:start", "tool:end"):
            info = self.activas.get(sesion)
            if not info:                          # llegó a mitad de turno (p. ej. tras reiniciar el companion)
                info = self.activas[sesion] = {"canal": canal, "mensaje": "", "t0": time.time(),
                                               "panel": PanelHerramientas(self.c.hub, self._id(sesion),
                                                                          f"Hermes trabajando · {canal}")}
            if tipo == "tool:start":
                await info["panel"].empieza(ev.get("tool_call_id", ""), ev.get("tool_name", ""), ev.get("detail", ""))
            else:
                await info["panel"].acaba(ev.get("tool_call_id", ""), ev.get("ok", True))
        elif tipo == "agent:end":
            asyncio.create_task(self._terminar(sesion, canal, ev))

    async def _terminar(self, sesion: str, canal: str, ev: dict):
        info = self.activas.pop(sesion, {"mensaje": ev.get("message", ""), "t0": time.time()})
        respuesta = hermes.para_voz(ev.get("response", "")).strip()     # sin **negritas** ni `código` en la tarjeta
        log.info("hermes termina en %s (%.0fs): %s", canal, time.time() - info["t0"], respuesta[:80])
        if not respuesta:
            await self.c.hub.emitir(Clear(what="card"))
            await self.c.pintar_animo()
            return
        n = len(info.get("panel").filas) if info.get("panel") else 0
        hechas = f" · {n} herramienta{'s' if n != 1 else ''}" if n else ""
        await self.c.hub.emitir(Card(id=self._id(sesion), kind="lead", title=f"Hermes · {canal} · listo{hechas}",
                                     lines=_lineas(respuesta, n=4), actions=[CardAction(id="ok", label="Vale")]))
        await self.c.hub.emitir(State(emotion="success", animation="bounce"))
        if self.c.en_silencio():                  # de noche, solo la tarjeta
            return
        resumen = await self._resumir(canal, info["mensaje"], respuesta)
        await self.c.decir(resumen, emocion="happy")

    async def _resumir(self, canal: str, pedido: str, respuesta: str) -> str:
        msgs = [{"role": "system", "content":
                 "Eres la voz de un muñequito de escritorio que avisa a su dueño de lo que su agente Hermes acaba de hacer "
                 "por otro canal. Escribe UNA frase en español de máximo 18 palabras, para decirla en voz alta: qué "
                 "terminó y el dato clave. Sin markdown, sin emojis, sin comillas, sin URLs."},
                {"role": "user", "content": f"Canal: {canal}\nLe pidieron: {pedido[:400]}\nRespondió: {respuesta[:500]}"}]
        try:
            t = await asyncio.wait_for(llm.chat(msgs, nivel="fast", max_tokens=60, temperature=0.3), 20)
            t = t.strip().strip('"«»').split("\n")[0]
            if t:
                return t
        except Exception as e:
            log.warning("resumen falló: %s", e)
        return f"Hermes terminó lo que le pediste por {canal}."
