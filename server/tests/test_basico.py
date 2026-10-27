"""Tests sin red: protocolo, ánimo, permisos, compuerta y parseo de salidas del LLM.

    cd server && ../.venv/bin/python -m pytest -q
"""
import asyncio
import json

import pytest

from brain import heartbeat, llm
from brain.gate import Compuerta
from brain.memory import Memoria
from brain.mood import Mood
from protocol import Card, Confirm, Hello, Touch, parse_entrante
from tools.base import Herramienta, PermisoDenegado, Registro
from tools.negocio_mock import NegocioMock


# --- protocolo -----------------------------------------------------------------
def test_entrantes_validos():
    assert isinstance(parse_entrante('{"type":"hello","device_id":"x","has_mic":true}'), Hello)
    t = parse_entrante('{"type":"touch","target":"card:lead1:responder","x":3,"y":4,"gesture":"tap"}')
    assert isinstance(t, Touch) and t.target.startswith("card:")


@pytest.mark.parametrize("raw", ['{"type":"nada"}', '{"type":"touch","target":"face","gesture":"pellizco"}', "no json"])
def test_entrantes_invalidos(raw):
    with pytest.raises(ValueError):
        parse_entrante(raw)


def test_confirm_por_defecto():
    c = Confirm(id="a", question="¿?", preview="hola")
    assert c.options == ["Enviar", "Editar", "Cancelar"] and json.loads(c.model_dump_json())["type"] == "confirm"


def test_card_rechaza_kind_raro():
    with pytest.raises(ValueError):
        Card(id="x", kind="spam", title="t")


# --- ánimo -----------------------------------------------------------------------
def test_animo_en_rango_y_deriva():
    m = Mood(energia=1, curiosidad=1, aburrimiento=0, afecto=1)
    for _ in range(50):
        m.aplicar("lead_nuevo")
    assert all(0 <= v <= 1 for v in m.dict().values())
    m.avanzar(horas=48)
    assert abs(m.curiosidad - 0.3) < 0.05      # vuelve al reposo


def test_animo_eventos_y_caras():
    m = Mood()
    m.aplicar("lead_nuevo"); m.aplicar("lead_nuevo")
    assert m.emocion() == "excited"
    m = Mood(aburrimiento=0.9, curiosidad=0.2)
    assert m.emocion() == "bored"
    m.aplicar("toque"); m.aplicar("voz")
    assert m.aburrimiento < 0.5
    assert Mood().emocion(dormido=True) == "sleeping"


def test_de_noche_baja_la_energia():
    m = Mood(energia=0.8)
    m.avanzar(horas=6, de_noche=True)
    assert m.energia < 0.6


# --- permisos ---------------------------------------------------------------------
def test_act_sin_confirmar_no_se_ejecuta():
    neg = NegocioMock()
    h = next(h for h in neg.herramientas() if h.nombre == "enviar_mensaje")
    with pytest.raises(PermisoDenegado):
        asyncio.run(h(destinatario_id=neg.leads[0]["id"], texto="hola"))
    assert neg.enviados == []


def test_read_y_draft_son_libres():
    async def f():
        return {"ok": True}
    for nivel in ("read", "draft"):
        assert asyncio.run(Herramienta("x", nivel, "", f)()) == {"ok": True}


class HubFalso:
    def __init__(self):
        self.enviados = []

    async def emitir(self, m):
        self.enviados.append(m)


def _compuerta(tmp_path, timeout=0.3):
    return Compuerta(HubFalso(), Memoria(str(tmp_path / "t.db")), timeout)


def test_compuerta_envia_solo_con_enviar(tmp_path):
    async def go():
        neg = NegocioMock()
        h = next(h for h in neg.herramientas() if h.nombre == "enviar_mensaje")
        comp = _compuerta(tmp_path)
        lead = neg.leads[0]
        tarea = asyncio.create_task(comp.proponer(h, {"destinatario_id": lead["id"], "texto": "hola"}, "¿?", "hola"))
        await asyncio.sleep(0.05)
        confirm = comp.hub.enviados[0]
        assert comp.responder(confirm.id, "Enviar", "test")
        res = await tarea
        assert res["ejecutado"] and neg.enviados and comp.memoria.auditoria()[0]["confirmado_por"] == "test"
    asyncio.run(go())


def test_compuerta_caduca_y_no_envia(tmp_path):
    async def go():
        neg = NegocioMock()
        h = next(h for h in neg.herramientas() if h.nombre == "enviar_mensaje")
        comp = _compuerta(tmp_path, timeout=0.1)
        res = await comp.proponer(h, {"destinatario_id": neg.leads[0]["id"], "texto": "hola"}, "¿?", "hola")
        assert res == {"ejecutado": False, "motivo": "caducado"} and neg.enviados == []
        assert "no ejecutado" in comp.memoria.auditoria()[0]["resultado"]
    asyncio.run(go())


def test_compuerta_editar_reescribe(tmp_path):
    async def go():
        neg = NegocioMock()
        h = next(h for h in neg.herramientas() if h.nombre == "enviar_mensaje")
        comp = _compuerta(tmp_path)
        lid = neg.leads[0]["id"]

        async def reescribir(prev):
            return {"destinatario_id": lid, "texto": "v2"}, "v2"
        tarea = asyncio.create_task(comp.proponer(h, {"destinatario_id": lid, "texto": "v1"}, "¿?", "v1", reescribir))
        await asyncio.sleep(0.05)
        comp.responder(comp.hub.enviados[0].id, "Editar", "t")
        await asyncio.sleep(0.05)
        segundo = [m for m in comp.hub.enviados if m.type == "confirm"][-1]
        assert segundo.preview == "v2"
        comp.responder(segundo.id, "Enviar", "t")
        await tarea
        assert neg.enviados[-1]["texto"] == "v2"
    asyncio.run(go())


def test_solo_vale_la_primera_respuesta(tmp_path):
    async def go():
        comp = _compuerta(tmp_path)
        t = asyncio.create_task(comp.preguntar("¿?", "x"))
        await asyncio.sleep(0.02)
        id = comp.hub.enviados[0].id
        assert comp.responder(id, "Cancelar", "placa")
        assert not comp.responder(id, "Enviar", "sim")
        assert (await t)[0] == "Cancelar"
    asyncio.run(go())


# --- salidas del LLM -------------------------------------------------------------
@pytest.mark.parametrize("texto,accion", [
    ('{"reason":"nada","action":"none"}', "none"),
    ('```json\n{"reason":"x","action":"speak","text":"Hola"}\n```', "speak"),
    ('Claro: {"reason":"x","action":"change_mood","emotion":"happy"} listo', "change_mood"),
])
def test_parse_decision(texto, accion):
    assert llm.parse_modelo(texto, heartbeat.Decision).action == accion


@pytest.mark.parametrize("texto", ['{"action":"bailar"}', "nada", '{"reason":"x","action":"speak","emotion":"furioso"}'])
def test_parse_decision_mala(texto):
    assert llm.parse_modelo(texto, heartbeat.Decision) is None


class CerebroFalso:
    def __init__(self, silencio=False, intervenciones=0):
        self._s, self._i = silencio, intervenciones
        self.registro = Registro()
        for h in NegocioMock().herramientas():
            self.registro.add(h)

        class C:
            max_spontaneous_hour = 3
        self.cfg = C()

    def en_silencio(self):
        return self._s

    def intervenciones_ultima_hora(self):
        return self._i


def test_filtro_respeta_silencio_y_cupo():
    hablar = heartbeat.Decision(reason="x", action="speak", text="hola")
    assert heartbeat.filtrar(CerebroFalso(silencio=True), hablar).action == "none"
    assert heartbeat.filtrar(CerebroFalso(intervenciones=3), hablar).action == "none"
    assert heartbeat.filtrar(CerebroFalso(), hablar).action == "speak"


def test_filtro_no_deja_proponer_herramientas_que_no_son_act():
    d = heartbeat.Decision(reason="x", action="propose_action", tool="listar_leads", preview="x")
    assert heartbeat.filtrar(CerebroFalso(), d).action == "none"
    d = heartbeat.Decision(reason="x", action="propose_action", tool="borrar_todo", preview="x")
    assert heartbeat.filtrar(CerebroFalso(), d).action == "none"
    d = heartbeat.Decision(reason="x", action="propose_action", tool="enviar_mensaje", preview="hola", args={"destinatario_id": 1})
    assert heartbeat.filtrar(CerebroFalso(), d).action == "propose_action"


# --- memoria ---------------------------------------------------------------------
def test_memoria_busca_por_similitud(tmp_path):
    import numpy as np
    vocab = ["lead", "servidor", "taller", "cafe"]

    async def emb(textos):
        v = np.array([[t.count(w) + 0.01 for w in vocab] for t in textos], dtype=np.float32)
        return v / np.linalg.norm(v, axis=1, keepdims=True)

    async def go():
        m = Memoria(str(tmp_path / "m.db"), embedder=emb)
        await m.recordar("hecho", "al usuario le gusta el cafe cafe")
        await m.recordar("hecho", "el taller es el sabado taller")
        r = await m.buscar("cuando es el taller", k=1)
        assert "taller" in r[0]["texto"]
    asyncio.run(go())


def test_horario_de_silencio_cruza_medianoche():
    from config import Config
    c = Config()
    c.quiet_start, c.quiet_end = 22 * 60 + 30, 7 * 60 + 30
    assert c.en_silencio(23 * 60) and c.en_silencio(3 * 60) and not c.en_silencio(12 * 60)


# --- voz ---------------------------------------------------------------------------
def test_puerta_de_voz_descarta_silencio_y_acepta_voz():
    import numpy as np
    from voice.voz import tramos_con_voz
    rng = np.random.default_rng(0)
    silencio = (rng.normal(0, 30, 16000 * 3)).astype(np.int16).tobytes()
    assert tramos_con_voz(silencio)[0] < 0.35
    t = np.arange(16000 * 3) / 16000
    voz = rng.normal(0, 30, len(t)) + 3000 * np.sin(2 * np.pi * 220 * t) * ((t > 1) & (t < 2))
    s, pico = tramos_con_voz(voz.astype(np.int16).tobytes())
    assert 0.8 < s < 1.2 and pico > -30


def test_fonetica_siglas():
    from voice.fonetica import a_voz
    assert a_voz("Monta n8n en tu VPS con IA") == "Monta ene ocho ene en tu ve pe ese con i a"
    assert a_voz("La API de GitHub") == "La a pe i de Guít-jáb"
    assert a_voz("IAN no cambia") == "IAN no cambia"


def test_agenda_ical_de_hoy():
    from datetime import date
    from zoneinfo import ZoneInfo
    from brain.tablero import eventos_de_hoy
    tz = ZoneInfo("America/Santo_Domingo")
    ics = "\r\n".join([
        "BEGIN:VCALENDAR",
        "BEGIN:VEVENT", "UID:a", "DTSTART:20261027T150000Z", "DTEND:20261027T160000Z", "SUMMARY:Llamada con\\, Ana", "END:VEVENT",
        "BEGIN:VEVENT", "UID:b", "DTSTART;TZID=America/Santo_Domingo:20261001T090000",
        "DTEND;TZID=America/Santo_Domingo:20261001T093000", "RRULE:FREQ=WEEKLY;BYDAY=TU,TH", "SUMMARY:Stand-up", "END:VEVENT",
        "BEGIN:VEVENT", "UID:c", "DTSTART;VALUE=DATE:20261027", "DTEND;VALUE=DATE:20261028", "SUMMARY:Cumple", "END:VEVENT",
        "BEGIN:VEVENT", "UID:d", "DTSTART:20261028T150000Z", "SUMMARY:Mañana", "END:VEVENT",
        "END:VCALENDAR"])
    ev = eventos_de_hoy(ics, date(2026, 10, 27), tz)      # martes
    assert [e["t"] for e in ev] == ["Cumple", "Stand-up", "Llamada con, Ana"]
    assert ev[1]["ini"].hour == 9 and ev[2]["ini"].hour == 11
