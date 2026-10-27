"""El cerebro de Pana: une ánimo, memoria, herramientas, compuerta y voz, y reacciona a
lo que llega de los cuerpos (toques, confirmaciones, voz) y del mundo (leads, sistema)."""
import asyncio
import json
import logging
import random
import re
import time
import uuid
from datetime import datetime

from pydantic import BaseModel, Field

from brain import heartbeat, hermes, llm, persona
from brain.avisos_hermes import PanelHerramientas
from brain.gate import Compuerta
from brain.memory import Memoria
from brain.mood import Mood
from config import Config
from protocol import Card, CardAction, Clear, Mood as MoodMsg, Say, AudioEnd, State
from voice.voz import CHUNK
from tools import sistema
from tools.base import Herramienta, Registro

log = logging.getLogger("cerebro")

# Reacciones rápidas a un toque en la cara, sin LLM (tienen que ser instantáneas)
_TOQUES = ["¿Dime?", "Aquí estoy.", "Jeje, eso hace cosquillas.", "¿Qué hay?", "Dale, te escucho."]


def _sin_ids(d):
    """Copia sin campos id/t para la conversación: así el modelo no los recita."""
    if isinstance(d, dict):
        return {k: _sin_ids(v) for k, v in d.items() if k not in ("id", "t", "respondido")}
    if isinstance(d, list):
        return [_sin_ids(x) for x in d]
    return d


class Cerebro:
    def __init__(self, cfg: Config, hub, negocio=None, voz=None):
        self.cfg = cfg
        self.hub = hub
        self.memoria = Memoria(cfg.db, embedder=llm.embed)
        self.mood = Mood(**self.memoria.get("mood", {}))
        self.voz = voz
        self.registro = Registro()
        for h in sistema.herramientas():
            self.registro.add(h)
        self.negocio = negocio
        if negocio:
            for h in negocio.herramientas():
                self.registro.add(h)
        self.registro.add(Herramienta("redactar_mensaje", "draft",
                                      "redacta (sin enviar) un mensaje para un lead o cliente",
                                      self.redactar_mensaje, {"destinatario_id": "id", "intencion": "qué se quiere decir"}))
        self.compuerta = Compuerta(hub, self.memoria, cfg.confirm_timeout_s)
        self.tarjetas: dict[str, dict] = {}       # card_id → contexto (lead, borrador...)
        self._t_animo = time.time()
        self._t_interaccion = self.memoria.get("t_interaccion", time.time())
        self._intervenciones: list[float] = []
        self._dicho: list[tuple[float, str]] = []
        self._cache_negocio = (0.0, {})
        self._despertar = asyncio.Event()        # para que el heartbeat reaccione ya
        self._hablando = asyncio.Lock()

    # --- tiempo y reglas ---------------------------------------------------------
    def ahora(self) -> datetime:
        return datetime.now(self.cfg.tz)

    def en_silencio(self) -> bool:
        a = self.ahora()
        return self.cfg.en_silencio(a.hour * 60 + a.minute)

    def minutos_desde_interaccion(self) -> int:
        return int((time.time() - self._t_interaccion) / 60)

    def intervenciones_ultima_hora(self) -> int:
        corte = time.time() - 3600
        self._intervenciones = [t for t in self._intervenciones if t > corte]
        return len(self._intervenciones)

    def dicho_reciente(self) -> list[str]:
        corte = time.time() - 3600
        self._dicho = [(t, s) for t, s in self._dicho if t > corte]
        return [s for _, s in self._dicho][-6:]

    def _interaccion(self):
        self._t_interaccion = time.time()
        self.memoria.set("t_interaccion", self._t_interaccion)

    # --- ánimo y cara --------------------------------------------------------------
    def avanzar_animo(self):
        horas = (time.time() - self._t_animo) / 3600
        self._t_animo = time.time()
        h = self.ahora().hour
        self.mood.avanzar(horas, de_noche=h >= 23 or h < 6)
        if self.minutos_desde_interaccion() > 90:
            self.mood.aburrimiento = min(1.0, self.mood.aburrimiento + 0.02 * horas * 10)
        self.memoria.set("mood", self.mood.dict())

    def evento_animo(self, ev: str):
        self.mood.aplicar(ev)
        self.memoria.set("mood", self.mood.dict())

    async def pintar_animo(self, animacion: str | None = None):
        await self.hub.emitir(State(emotion=self.mood.emocion(dormido=self.en_silencio()), animation=animacion))
        await self.hub.emitir(MoodMsg(**self.mood.dict()))

    # --- hablar ----------------------------------------------------------------
    async def decir(self, texto: str, emocion="talking", espontaneo=False):
        """Burbuja + voz (si hay TTS y algún cuerpo con altavoz). Vuelve a la cara del ánimo al acabar."""
        async def una_vez():
            yield texto
        await self.decir_stream(una_vez(), emocion, espontaneo)

    async def decir_stream(self, trozos, emocion="talking", espontaneo=False) -> str:
        """Habla según llegan los trozos de texto: la voz empieza con la primera frase."""
        async with self._hablando:
            id = uuid.uuid4().hex[:8]
            con_audio = bool(self.voz and self.voz.tts_listo() and self.hub.hay_altavoz())
            completo, pendiente, abierto = "", "", False
            await self.hub.emitir(State(emotion="talking" if con_audio else emocion))

            # Cada frase se encarga a la voz en cuanto llega y suena en orden: con una voz más
            # lenta que el tiempo real (una voz propia lenta), la siguiente se genera mientras suena esta.
            cola: asyncio.Queue = asyncio.Queue()

            async def reproductor():
                while (tarea := await cola.get()) is not None:
                    pcm = await tarea
                    for i in range(0, len(pcm), CHUNK):
                        await self.hub.emitir_audio(pcm[i:i + CHUNK])
                        await asyncio.sleep(CHUNK / 32000 / 1.5)     # ~1,5x el tiempo real: no inundar la placa

            sonando = asyncio.create_task(reproductor())

            async def soltar(frase: str):
                nonlocal abierto
                await self.hub.emitir(Say(id=id, text=completo.strip(), emotion=emocion, audio=con_audio))
                if con_audio and frase.strip():
                    abierto = True
                    cola.put_nowait(asyncio.create_task(self.voz.audio(frase)))

            try:
                async for t in trozos:
                    completo += t
                    pendiente += t
                    # se corta por frase para que la voz arranque cuanto antes
                    while (m := re.search(r"[.!?¿¡…:;]\s|\n", pendiente)):
                        frase, pendiente = pendiente[:m.end()], pendiente[m.end():]
                        await soltar(frase)
                if pendiente.strip() or not completo.strip():
                    await soltar(pendiente)
            finally:
                cola.put_nowait(None)
                await sonando
            if abierto:
                await self.hub.emitir(AudioEnd(id=id))
            texto = completo.strip()
            self.memoria.turno("pana", texto)
            self._dicho.append((time.time(), texto))
            if espontaneo:
                self._intervenciones.append(time.time())
            asyncio.create_task(self._borrar_burbuja(id, len(texto)))
            await self.hub.emitir(State(emotion=emocion if emocion != "talking" else self.mood.emocion()))
            return texto

    async def _borrar_burbuja(self, id: str, n: int):
        # con una tarjeta esperando debajo, la burbuja se aparta enseguida para que se vea
        await asyncio.sleep(2 if "card" in self.hub.pantalla else min(25, 5 + n * 0.07))
        if self.hub.pantalla.get("bubble", {}).get("id") == id:
            await self.hub.emitir(Clear(what="bubble"))

    # --- contexto de negocio (compacto, cacheado 30 s) ----------------------------
    async def contexto_negocio(self) -> dict:
        t, datos = self._cache_negocio
        if time.time() - t < 30:
            return datos
        out = {}
        for nombre in ("listar_leads", "listar_noshows", "listar_briefs", "estado_sistema"):
            h = self.registro.get(nombre)
            if not h:
                continue
            try:
                out[nombre] = await h()
            except Exception as e:
                out[nombre] = {"error": str(e)}
        # recorte: solo lo que cabe en un vistazo
        if "listar_leads" in out and "leads" in out["listar_leads"]:
            out["leads"] = [{k: l[k] for k in ("id", "nombre", "canal", "temperatura", "hace_horas", "resumen", "ultimo_mensaje")}
                            for l in out.pop("listar_leads")["leads"][:5]]
        if "listar_noshows" in out:
            out["noshows"] = out.pop("listar_noshows").get("noshows", [])
        if "listar_briefs" in out:
            out["briefs"] = out.pop("listar_briefs").get("briefs", [])
        if "estado_sistema" in out:
            s = out.pop("estado_sistema")
            out["sistema"] = {"temp_max": s.get("temp_max"), "carga_1m": s.get("carga_1m"),
                              "memoria": s.get("memoria"), "modelos": s.get("modelos")}
        self._cache_negocio = (time.time(), out)
        return out

    def invalidar_negocio(self):
        self._cache_negocio = (0.0, {})

    # --- herramientas draft ----------------------------------------------------
    async def redactar_mensaje(self, destinatario_id: int, intencion: str = "dar seguimiento", previo: str = "") -> dict:
        lead = await self.registro.get("ver_lead")(id=destinatario_id) if self.registro.get("ver_lead") else {}
        extra = f"\nUn borrador anterior que el usuario quiso editar (haz otra versión distinta, más corta): {previo}" if previo else ""
        msgs = [
            {"role": "system", "content": f"Redactas mensajes de {persona.DUENO} "
             "para sus clientes por WhatsApp/Instagram. Tono cercano y profesional, tuteo, español neutro, "
             "2-4 frases, sin emojis exagerados, sin inventar precios ni fechas que no estén en los datos. "
             "Nunca dejes huecos ni marcadores como $X, [nombre] o XX: si no tienes el dato, propón una llamada corta. "
             "Termina con una pregunta que mueva la conversación. Devuelve SOLO el mensaje."},
            {"role": "user", "content": f"Contacto: {json.dumps(lead, ensure_ascii=False)}\nIntención: {intencion}{extra}"},
        ]
        texto = await llm.chat(msgs, nivel="chat", max_tokens=220, temperature=0.8)
        return {"destinatario_id": destinatario_id, "texto": texto.strip().strip('"')}

    # --- eventos del mundo -----------------------------------------------------
    async def evento(self, tipo: str, texto: str, datos: dict | None = None):
        self.memoria.evento(tipo, texto, datos)
        self.evento_animo(tipo)
        self.invalidar_negocio()
        if tipo == "lead_nuevo" and datos:
            asyncio.create_task(self.reaccionar_lead(datos))
        else:
            self._despertar.set()

    async def reaccionar_lead(self, lead: dict):
        """Entra un lead: Pana se emociona, avisa, redacta y enseña la tarjeta."""
        await self.hub.emitir(State(emotion="excited", animation="bounce"))
        borrador = await self.redactar_mensaje(lead["id"], "responder su último mensaje y proponer una llamada corta")
        id = f"lead{lead['id']}"
        self.tarjetas[id] = {"lead": lead, "borrador": borrador["texto"]}
        await self.hub.emitir(State(emotion="excited", look=(0.0, 1.0)))       # mira a la tarjeta
        await self.hub.emitir(Card(id=id, kind="lead", title=f"{lead['nombre']} · {lead['canal']}",
                                   lines=[lead["resumen"], f"«{lead['ultimo_mensaje']}»", f"Temperatura: {lead['temperatura']}"],
                                   actions=[CardAction(id="responder", label="Responder"), CardAction(id="luego", label="Luego")],
                                   icon=lead["canal"].lower()))
        if not self.en_silencio():
            await self.decir(f"Entró un lead {lead['temperatura']}: {lead['nombre'].split()[0]}, por {lead['canal']}. Ya te tengo un borrador.",
                             emocion="excited")

    # --- lo que llega de los cuerpos -------------------------------------------
    async def toque(self, t, quien: str):
        self._interaccion()
        if t.target.startswith("card:"):
            _, card_id, accion = t.target.split(":", 2)
            await self.accion_tarjeta(card_id, accion, quien)
            return
        if t.target == "face":
            self.evento_animo("toque")
            if t.gesture == "tap":
                cara = "love" if self.mood.afecto > 0.75 else "happy"      # mucho cariño acumulado: corazones
                await self.hub.emitir(State(emotion=cara, animation="bounce"))
                if self.ahora().hour * 60 + self.ahora().minute >= self.cfg.brief_at and not self.brief_entregado_hoy():
                    await self.brief_matutino()
                elif random.random() < 0.5:
                    await self.decir(random.choice(_TOQUES), emocion="happy")
                else:
                    await asyncio.sleep(1.2)
                    await self.pintar_animo()
        elif t.target == "bubble":
            await self.hub.emitir(Clear(what="bubble"))

    async def accion_tarjeta(self, card_id: str, accion: str, quien: str):
        ctx = self.tarjetas.get(card_id)
        if not ctx:
            await self.hub.emitir(Clear(what="card"))
            return
        if accion == "responder" and "lead" in ctx:
            await self.hub.emitir(Clear(what="card"))
            lead = ctx["lead"]
            h = self.registro.get("enviar_mensaje")

            async def reescribir(previo):
                await self.hub.emitir(State(emotion="thinking"))
                b = await self.redactar_mensaje(lead["id"], "responder su último mensaje", previo=previo)
                return {"destinatario_id": lead["id"], "texto": b["texto"]}, b["texto"]

            res = await self.compuerta.proponer(h, {"destinatario_id": lead["id"], "texto": ctx["borrador"]},
                                                f"¿Le envío esto a {lead['nombre'].split()[0]}?", ctx["borrador"],
                                                reescribir=reescribir)
            self.tarjetas.pop(card_id, None)
            self.invalidar_negocio()
            if res["ejecutado"] and res["resultado"].get("ok"):
                self.evento_animo("accion_confirmada")
                await self.decir(f"Listo, enviado a {lead['nombre'].split()[0]}.", emocion="happy")
            elif res["ejecutado"]:
                await self.decir(f"No se pudo enviar: {res['resultado'].get('error', 'error')}.", emocion="sad")
            else:
                self.evento_animo("accion_cancelada")
                await self.decir("Ok, no envío nada." if res["motivo"] != "caducado" else "Lo dejé sin enviar, no me contestaste.",
                                 emocion="idle")
        elif accion in ("luego", "cerrar", "ok"):
            self.tarjetas.pop(card_id, None)
            await self.hub.emitir(Clear(what="card"))
            await self.pintar_animo()

    def responder_confirm(self, id: str, eleccion: str, quien: str):
        self._interaccion()
        self.compuerta.responder(id, eleccion, quien)

    # --- conversación (texto o voz) --------------------------------------------
    async def conversar(self, texto: str, por_voz=False) -> str:
        self._interaccion()
        self.evento_animo("voz")
        self.memoria.turno("usuario", texto)
        await self.hub.emitir(State(emotion="thinking"))
        t0 = time.perf_counter()
        if self.cfg.brain == "hermes":
            return await self._conversar_hermes(texto, t0)
        negocio, recuerdos = await asyncio.gather(self.contexto_negocio(), self.memoria.buscar(texto, k=4))
        # la persona va sola y primero (prefijo fijo → llama.cpp lo reutiliza de la caché);
        # lo que cambia en cada turno va después
        msgs = [{"role": "system", "content": persona.PERSONA + persona.REGLAS_CONVERSACION},
                {"role": "system", "content": f"Te sientes: {self.mood.tono()}. Hora: {self.ahora().strftime('%A %H:%M')}.\n"
                 f"Datos del negocio ahora mismo (los leads van del más caliente al más frío):\n{json.dumps(_sin_ids(negocio), ensure_ascii=False)}\n"
                 f"Recuerdos relevantes: {json.dumps([r['texto'] for r in recuerdos], ensure_ascii=False)}"}]
        for t in self.memoria.ultimos_turnos(8)[:-1]:
            msgs.append({"role": "user" if t["rol"] == "usuario" else "assistant", "content": t["texto"]})
        msgs.append({"role": "user", "content": texto})
        intencion = asyncio.create_task(self._intencion(texto, negocio))
        try:
            respuesta = await self.decir_stream(llm.chat_stream(msgs, nivel="chat", max_tokens=160, temperature=0.4))
        except Exception as e:
            log.warning("conversación falló (%s): plan B", e)
            respuesta = "Se me trabó el cerebro, ¿me lo repites?"
            await self.decir(respuesta, emocion="confused")
        log.info("conversación %.2fs: «%s» → «%s»", time.perf_counter() - t0, texto, respuesta)
        await intencion
        return respuesta

    async def _conversar_hermes(self, texto: str, t0: float) -> str:
        """El turno lo resuelve Hermes (herramientas, memoria, skills); aquí solo se pinta y se dice."""
        usó_herramientas = con_laptop = False
        panel = PanelHerramientas(self.hub, "hermes-placa", "Hermes trabajando")

        async def herramienta(nombre: str, etiqueta: str, estado: str, call_id: str):
            nonlocal usó_herramientas, con_laptop
            if estado != "running":
                await panel.acaba(call_id, ok=estado not in ("failed", "error"))
                return
            usó_herramientas = con_laptop = True
            await panel.empieza(call_id, nombre, etiqueta)

        async def aprobacion(ev: dict) -> str:
            # el diálogo de confirmación de siempre; caducar o cancelar = denegar
            preview = ev.get("command") or ev.get("description") or json.dumps(ev, ensure_ascii=False)[:400]
            eleccion, _ = await self.compuerta.preguntar("Hermes pide permiso", preview,
                                                         ("Permitir", "Esta sesión", "Denegar"))
            await self.hub.emitir(State(emotion="executing" if eleccion in ("Permitir", "Esta sesión") else "thinking"))
            return {"Permitir": "once", "Esta sesión": "session"}.get(eleccion, "deny")

        # Hermes puede tardar 20 s con herramientas: en cuanto empieza a usarlas (o si a los 4 s no ha
        # dicho nada) Pana suelta una frase corta. Va por la misma cola que la respuesta, así que
        # suena antes y no se pisa con ella.
        cola: asyncio.Queue = asyncio.Queue()
        relleno_dicho = False

        def relleno(nombre: str = "") -> str:
            n = nombre.lower()
            if "search" in n or "web" in n or "browser" in n:
                return "Lo busco. "
            if "skill" in n or "read" in n or "memory" in n or "session" in n:
                return "Déjame revisarlo. "
            if "terminal" in n or "process" in n or "execute" in n:
                return "Voy a mirarlo. "
            return "Dame un momento. "

        herramienta_original = herramienta

        async def herramienta(nombre: str, etiqueta: str, estado: str, call_id: str):
            nonlocal relleno_dicho
            if estado == "running" and not relleno_dicho:
                relleno_dicho = True
                await cola.put(("t", relleno(nombre)))
            await herramienta_original(nombre, etiqueta, estado, call_id)

        async def productor():
            try:
                async for t in hermes.responder(texto, herramienta, aprobacion):
                    await cola.put(("t", t))
                await cola.put(("fin", None))
            except Exception as e:
                await cola.put(("error", e))

        async def trozos():
            # tras cada herramienta, el primer texto vuelve a poner cara de hablar (si no, sigue con la laptop)
            nonlocal con_laptop, relleno_dicho
            tarea = asyncio.create_task(productor())
            try:
                while True:
                    try:
                        tipo, v = await asyncio.wait_for(cola.get(), timeout=None if relleno_dicho else 4.0)
                    except asyncio.TimeoutError:
                        relleno_dicho = True
                        yield "Mmm, a ver. "
                        continue
                    if tipo == "fin":
                        return
                    if tipo == "error":
                        raise v
                    relleno_dicho = True           # ya habló (Hermes o el relleno): no hace falta otro
                    if con_laptop:
                        con_laptop = False
                        await self.hub.emitir(State(emotion="talking"))
                    yield v
            finally:
                tarea.cancel()

        try:
            respuesta = await self.decir_stream(trozos())
            if usó_herramientas:       # trabajo hecho: pulgar arriba un momento y fuera el panel
                panel.cerrar_pendientes()
                await self.hub.emitir(State(emotion="success"))
                await asyncio.sleep(2.5)
                await self.hub.emitir(Clear(what="card"))
                await self.pintar_animo()
        except Exception as e:
            log.warning("hermes falló (%s): plan B", e)
            respuesta = "No pude hablar con Hermes, ¿lo intentamos otra vez?"
            await self.decir(respuesta, emocion="confused")
        log.info("conversación hermes %.2fs: «%s» → «%s»", time.perf_counter() - t0, texto, respuesta)
        return respuesta

    async def _intencion(self, texto: str, negocio: dict):
        """En paralelo a la respuesta: ¿pidió una acción o contó algo que conviene recordar?"""
        class Intencion(BaseModel):
            enviar: bool = False
            destinatario_id: int | None = None
            mensaje: str | None = None
            recordar: str | None = Field(None, description="hecho duradero sobre el usuario o su trabajo que dijo, en tercera persona")

        msgs = [{"role": "system", "content": "Analiza la frase del usuario. enviar=true solo si pide explícitamente mandar un mensaje a "
                 "alguien de la lista (usa su id). recordar: un hecho duradero que convenga guardar (preferencias, planes, personas); "
                 "null si no hay."},
                {"role": "user", "content": f"Contactos: {json.dumps(negocio.get('leads', []) + negocio.get('noshows', []), ensure_ascii=False)}\n"
                 f"Frase: {texto}"}]
        it = await llm.estructurado(msgs, Intencion, nivel="fast", max_tokens=200, temperature=0.2)
        if not it:
            return
        if it.recordar:
            await self.memoria.recordar("hecho", it.recordar)
            log.info("recuerdo nuevo: %s", it.recordar)
        if it.enviar and it.destinatario_id and self.registro.get("enviar_mensaje"):
            b = await self.redactar_mensaje(it.destinatario_id, it.mensaje or texto)
            res = await self.compuerta.proponer(self.registro.get("enviar_mensaje"), b, "¿Lo envío?", b["texto"])
            if res["ejecutado"]:
                await self.decir("Enviado.", emocion="happy")

    async def voz_entrante(self, pcm: bytes):
        if not self.voz:
            return
        await self.hub.emitir(State(emotion="thinking"))
        t0 = time.perf_counter()
        try:        # el último audio del micro, para depurar lo que oye
            from voice.voz import pcm_a_wav
            open("data/ultimo_mic.wav", "wb").write(pcm_a_wav(pcm))
        except OSError:
            pass
        texto = await self.voz.transcribir(pcm)
        log.info("stt %.2fs (%.1f s de audio): %s", time.perf_counter() - t0, len(pcm) / 32000, texto)
        if not texto.strip():
            if getattr(self.voz, "ultima_voz", 0.0) < 0.35:      # un toque sin hablar: nada que contestar
                await self.pintar_animo()
                return
            await self.decir("No te escuché bien, ¿me lo repites?", emocion="confused")
            return
        # lo que entendió, en la consola: así se ve al momento si se equivocó
        await self.hub.emitir(Card(id="oido", kind="brief", title="Te entendí", lines=[texto]))
        await self.conversar(texto, por_voz=True)
        if self.hub.pantalla.get("card", {}).get("id") == "oido":
            await self.hub.emitir(Clear(what="card"))

    # --- decisiones del heartbeat ----------------------------------------------
    async def aplicar_decision(self, d):
        if d.action == "none":
            await self.pintar_animo()
        elif d.action == "speak":
            await self.decir(d.text, emocion=d.emotion or "talking", espontaneo=True)
        elif d.action == "show_card":
            id = "llm" + uuid.uuid4().hex[:6]
            self.tarjetas[id] = {"llm": True}
            await self.hub.emitir(Card(id=id, kind=d.card.kind, title=d.card.title, lines=d.card.lines,
                                       actions=[CardAction(id="ok", label="Ok")]))
            self._intervenciones.append(time.time())
            if d.emotion:
                await self.hub.emitir(State(emotion=d.emotion))
        elif d.action == "propose_action":
            self._intervenciones.append(time.time())
            asyncio.create_task(self._proponer_desde_ciclo(d))
        elif d.action == "change_mood":
            await self.hub.emitir(State(emotion=d.emotion))

    async def _proponer_desde_ciclo(self, d):
        """El ciclo solo decide a quién y para qué; el texto lo escribe el redactor (modelo de chat)."""
        h = self.registro.get(d.tool)
        args = dict(d.args or {})
        if d.tool == "enviar_mensaje":
            dest = args.get("destinatario_id")
            ver = self.registro.get("ver_lead")
            info = await ver(id=dest) if ver and dest is not None else {"error": "sin destinatario"}
            if "error" in info:
                log.info("propuesta descartada: %s", info["error"])
                return
            nombre = info["nombre"].split()[0]

            async def reescribir(previo):
                b = await self.redactar_mensaje(dest, d.reason, previo=previo)
                return {"destinatario_id": dest, "texto": b["texto"]}, b["texto"]

            args, preview = await reescribir("")
            res = await self.compuerta.proponer(h, args, f"¿Le escribo a {nombre}?", preview, reescribir=reescribir)
            if res["ejecutado"]:
                await self.decir(f"Listo, le escribí a {nombre}.", emocion="happy")
        else:
            await self.compuerta.proponer(h, args, d.question or "¿Lo hago?", d.preview)

    # --- brief de la mañana (lo prepara el sueño) --------------------------------
    def brief_entregado_hoy(self) -> bool:
        return self.memoria.get("brief_entregado") == self.ahora().date().isoformat()

    async def brief_matutino(self):
        from brain import dream
        await dream.dar_brief(self)

    # --- bucle autónomo ----------------------------------------------------------
    async def vivir(self):
        """Heartbeat + reloj del sueño y del brief. Corre mientras viva el servidor."""
        from brain import dream
        await asyncio.sleep(3)
        while True:
            try:
                await asyncio.wait_for(self._despertar.wait(), self.cfg.heartbeat_min * 60)
                motivo = "evento"
                await asyncio.sleep(2)        # junta eventos que llegan seguidos
            except asyncio.TimeoutError:
                motivo = "timer"
            self._despertar.clear()
            try:
                await dream.reloj(self)
                await heartbeat.ciclo(self, motivo)
            except Exception:
                log.exception("ciclo falló")
