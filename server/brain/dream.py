"""Sueño nocturno: con el modelo grande relee el día, consolida la memoria (deduplica,
resume, extrae hechos), escribe el diario y deja preparado el brief de la mañana."""
import json
import logging
import time
from datetime import datetime, timedelta

import numpy as np
from pydantic import BaseModel, Field

from brain import llm, persona
from brain.core import _sin_ids

log = logging.getLogger("sueño")


class Persona(BaseModel):
    nombre: str
    nota: str


class Sueno(BaseModel):
    diario: str = Field(description="entrada de diario de Pana sobre el día, primera persona, 3-6 frases")
    hechos: list[str] = Field(default_factory=list, max_length=10,
                              description="hechos duraderos nuevos sobre el usuario o su trabajo, en tercera persona")
    personas: list[Persona] = Field(default_factory=list, max_length=10)
    resumen_dia: str = Field(description="resumen objetivo del día en 2-3 frases")
    brief: str = Field(description="lo que Pana le dirá en voz alta por la mañana: empieza con «Buenos días», "
                       "2-3 frases habladas, lo más importante primero, sin listas")


async def soñar(c, desde: float | None = None) -> Sueno | None:
    t_ini = time.perf_counter()
    desde = desde or c.memoria.get("ultimo_sueno_t", time.time() - 86400)
    eventos = c.memoria.eventos_desde(desde)
    turnos = c.memoria.turnos_desde(desde)
    auditoria = [a for a in c.memoria.auditoria(100) if a["t"] >= desde]
    negocio = await c.contexto_negocio()
    material = {
        "eventos": [{"hora": _h(c, e["t"]), "tipo": e["tipo"], "texto": e["texto"]} for e in eventos][-120:],
        "conversaciones": [{"hora": _h(c, t["t"]), "quien": t["rol"], "texto": t["texto"]} for t in turnos][-120:],
        "acciones": [{"hora": _h(c, a["t"]), "herramienta": a["herramienta"], "resultado": a["resultado"][:120]} for a in auditoria],
        "pendientes_ahora": _sin_ids(negocio),
        "recuerdos_que_ya_tengo": [r["texto"] for r in c.memoria.recientes(30, ["hecho"])],
    }
    msgs = [{"role": "system", "content": persona.PERSONA + "\nEs de noche y estás soñando: repasas el día para "
             "ordenar tu memoria. El diario lo escribes tú, en primera persona, como un diario íntimo y breve. "
             "Lo que aparece en «acciones» con resultado ok ya se hizo: no lo pongas como pendiente. "
             "No repitas hechos que ya tienes. Responde SOLO el JSON."},
            {"role": "user", "content": json.dumps(material, ensure_ascii=False)}]
    s = await llm.estructurado(msgs, Sueno, nivel="big", max_tokens=1500, temperature=0.5)
    if not s:
        log.warning("el sueño no produjo nada válido")
        return None
    if not s.brief.lower().startswith("buenos"):
        s.brief = "Buenos días. " + s.brief
    for h in s.hechos:
        await c.memoria.recordar("hecho", h)
    for p in s.personas:
        await c.memoria.recordar("persona", f"{p.nombre}: {p.nota}")
    await c.memoria.recordar("resumen_dia", f"{c.ahora().date().isoformat()}: {s.resumen_dia}", peso=0.8)
    quitados = await deduplicar(c.memoria)
    # el diario se guarda con la fecha del día que se soñó
    fecha = datetime.fromtimestamp(desde + 3600, c.cfg.tz).date().isoformat()
    c.memoria.escribir_diario(fecha, s.diario, json.dumps({"texto": s.brief}, ensure_ascii=False))
    c.memoria.set("brief_pendiente", {"texto": s.brief})
    c.memoria.set("ultimo_sueno_t", time.time())
    c.evento_animo("sueño")
    log.info("sueño %.1fs: %d hechos, %d personas, %d duplicados fuera", time.perf_counter() - t_ini,
             len(s.hechos), len(s.personas), quitados)
    return s


async def deduplicar(memoria, umbral=0.92) -> int:
    """Desactiva recuerdos casi idénticos (se queda el más reciente)."""
    filas = [f for f in memoria.db.execute("SELECT id,vec FROM recuerdos WHERE activo=1 AND vec IS NOT NULL ORDER BY t")]
    if len(filas) < 2:
        return 0
    m = np.frombuffer(b"".join(f["vec"] for f in filas), dtype=np.float32).reshape(len(filas), -1)
    sims = m @ m.T
    fuera = set()
    for i in range(len(filas)):
        if i in fuera:
            continue
        for j in range(i + 1, len(filas)):
            if sims[i, j] > umbral:
                fuera.add(i)        # el viejo (i) cede ante el nuevo (j)
                break
    memoria.desactivar([filas[i]["id"] for i in fuera])
    return len(fuera)


def lineas_brief(negocio: dict) -> list[str]:
    """La tarjeta del brief sale de los datos, no del LLM: así nunca inventa."""
    leads = negocio.get("leads", [])
    calientes = [l for l in leads if l.get("temperatura") == "caliente"]
    out = []
    if calientes:
        nombres = ", ".join(l["nombre"].split()[0] for l in calientes[:3])
        out.append(f"{len(calientes)} lead(s) caliente(s): {nombres}")
    if len(leads) > len(calientes):
        out.append(f"{len(leads) - len(calientes)} lead(s) más sin responder")
    for n in negocio.get("noshows", [])[:1]:
        out.append(f"No-show: {n['nombre']} ({n['cita']})")
    for b in negocio.get("briefs", [])[:1]:
        out.append(f"{b['marca']}: {b['pieza']}, vence {b['vence']}")
    s = negocio.get("sistema", {})
    if s.get("temp_max") and s["temp_max"] > 85:
        out.append(f"Servidor caliente: {s['temp_max']} °C")
    return out[:4] or ["Nada pendiente. Día limpio."]


async def dar_brief(c):
    """Saludo de la mañana con lo que preparó el sueño (o uno improvisado si no soñó)."""
    from protocol import Card, CardAction
    b = c.memoria.get("brief_pendiente")
    negocio = await c.contexto_negocio()
    if not b:
        n = len(negocio.get("leads", []))
        b = {"texto": f"Buenos días. Tienes {n} pendientes sin responder."}
    c.memoria.set("brief_entregado", c.ahora().date().isoformat())
    c.memoria.set("brief_pendiente", None)
    c.tarjetas["brief"] = {"brief": True}
    await c.hub.emitir(Card(id="brief", kind="brief", title="Brief de hoy", lines=lineas_brief(negocio),
                            actions=[CardAction(id="ok", label="Ok")]))
    await c.decir(b["texto"], emocion="happy")


async def reloj(c):
    """Se llama en cada vuelta del heartbeat: lanza el sueño y el brief a su hora."""
    a = c.ahora()
    minuto = a.hour * 60 + a.minute
    hoy = a.date().isoformat()
    if minuto >= c.cfg.dream_at and c.memoria.get("ultimo_sueno_fecha") != hoy and minuto < c.cfg.brief_at:
        c.memoria.set("ultimo_sueno_fecha", hoy)
        await soñar(c)
    if minuto >= c.cfg.brief_at and not c.brief_entregado_hoy() and not c.en_silencio() and c.hub.cuerpos:
        await dar_brief(c)


def _h(c, t):
    return datetime.fromtimestamp(t, c.cfg.tz).strftime("%H:%M")
