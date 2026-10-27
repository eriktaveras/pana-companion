"""Ciclo autónomo. Cada N minutos (o al instante si pasa algo) Pana mira un resumen
compacto de su mundo y decide UNA cosa, casi siempre «none».

El LLM solo propone: las reglas de aquí (horario de silencio, cupo de intervenciones
por hora, nivel de permiso de la herramienta) tienen la última palabra.
"""
import json
import logging
import time
from typing import Literal

from pydantic import BaseModel, Field

from brain import llm, persona
from protocol import Emotion

log = logging.getLogger("heartbeat")


class TarjetaLLM(BaseModel):
    kind: Literal["lead", "noshow", "brief", "system"]
    title: str = Field(max_length=40)
    lines: list[str] = Field(default_factory=list, max_length=4)


class Decision(BaseModel):
    reason: str = Field(description="por qué, en pocas palabras (solo para el log)", max_length=160)
    action: Literal["none", "speak", "show_card", "propose_action", "change_mood"]
    emotion: Emotion | None = None
    text: str | None = Field(None, description="lo que dice, 1-2 frases cortas (speak)", max_length=220)
    card: TarjetaLLM | None = None
    tool: str | None = Field(None, description="herramienta act a proponer (propose_action)")
    args: dict[str, str | int] | None = None
    question: str | None = Field(None, max_length=80)
    preview: str | None = Field(None, max_length=400)


INSTRUCCIONES = """Este es tu ciclo de pensamiento en segundo plano. Mira el resumen y decide UNA acción.

Acciones:
- "none": no hacer nada. Es lo normal: la mayoría de los ciclos deben terminar así.
- "speak": decir algo corto al usuario. Solo si aporta (un pendiente que se está enfriando, algo raro en el
  servidor, un dato útil). Nada de saludos vacíos ni de repetir lo que ya dijiste.
- "show_card": enseñar una tarjeta (kind lead/noshow/brief/system, título corto, hasta 4 líneas).
- "propose_action": proponer una acción externa (tool de nivel act + args + question + preview con el
  texto exacto que se enviaría). El usuario tendrá que confirmarla tocando la pantalla.
- "change_mood": solo cambiar la cara (emotion).

Reglas: no repitas algo que ya dijiste en la última hora (mira "ya_dicho"). Si el usuario está en silencio
de no molestar, responde "none". Responde SOLO el JSON."""


def snapshot(c, eventos: list[dict], negocio: dict, recuerdos: list[dict]) -> dict:
    """Resumen compacto del mundo para un ciclo. Nunca el historial completo."""
    ahora = c.ahora()
    return {
        "hora": ahora.strftime("%A %H:%M"),
        "no_molestar": c.en_silencio(),
        "animo": c.mood.dict(),
        "minutos_sin_hablar_con_el_usuario": c.minutos_desde_interaccion(),
        "intervenciones_ultima_hora": c.intervenciones_ultima_hora(),
        "eventos_nuevos": [{"tipo": e["tipo"], "texto": e["texto"]} for e in eventos[-8:]],
        "negocio": negocio,
        "recuerdos": [r["texto"] for r in recuerdos],
        "ya_dicho": c.dicho_reciente(),
        "herramientas_act": [h.nombre for h in c.registro.por_nivel("act")],
    }


async def decidir(c, snap: dict) -> Decision:
    msgs = [
        {"role": "system", "content": persona.sistema(c.mood.tono()) + "\n" + INSTRUCCIONES},
        {"role": "user", "content": json.dumps(snap, ensure_ascii=False)},
    ]
    d = await llm.estructurado(msgs, Decision, nivel="fast", max_tokens=400)
    return d or Decision(reason="salida inválida del LLM → none", action="none")


def filtrar(c, d: Decision) -> Decision:
    """Las reglas mandan sobre el LLM."""
    if d.action in ("speak", "show_card", "propose_action"):
        if c.en_silencio():
            return Decision(reason=f"silencio (quería {d.action})", action="none")
        if c.intervenciones_ultima_hora() >= c.cfg.max_spontaneous_hour:
            return Decision(reason=f"cupo por hora agotado (quería {d.action})", action="none")
    if d.action == "speak" and not d.text:
        return Decision(reason="speak sin texto", action="none")
    if d.action == "show_card" and not d.card:
        return Decision(reason="show_card sin tarjeta", action="none")
    if d.action == "propose_action":
        h = c.registro.get(d.tool or "")
        if not h or h.nivel != "act" or not d.preview:
            return Decision(reason=f"propuesta inválida ({d.tool})", action="none")
    if d.action == "change_mood" and not d.emotion:
        return Decision(reason="change_mood sin emoción", action="none")
    return d


async def ciclo(c, motivo: str = "timer"):
    t0 = time.perf_counter()
    c.avanzar_animo()
    eventos = c.memoria.eventos_nuevos()
    if c.en_silencio() and not any(e["tipo"] == "sistema_mal" for e in eventos):
        log.info("ciclo (%s) silencio → none · ánimo %s", motivo, c.mood.dict())
        await c.pintar_animo()
        return Decision(reason="silencio", action="none")
    negocio = await c.contexto_negocio()
    consulta = " ".join(e["texto"] for e in eventos) or "pendientes de hoy"
    recuerdos = await c.memoria.buscar(consulta, k=3)
    d = filtrar(c, await decidir(c, snapshot(c, eventos, negocio, recuerdos)))
    log.info("ciclo (%s) %s · %s · %.2fs · ánimo %s", motivo, d.action, d.reason,
             time.perf_counter() - t0, c.mood.dict())
    await c.aplicar_decision(d)
    return d
