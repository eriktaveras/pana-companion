"""Adaptador FALSO del servidor de negocio (CRM): leads del chat, no-shows, briefs y
envío de mensajes. Datos inventados pero realistas para desarrollar sin tocar nada real.
Se sustituye por negocio_real.py cuando tengamos los endpoints.
"""
import itertools
import random
import time

from tools.base import Herramienta

_ids = itertools.count(100)

_NOMBRES = ["María Fernández", "José Peña", "Carolina Ruiz", "Luis Almonte", "Andrea Mejía",
            "Ramón Castillo", "Paola Jiménez", "Kevin Rosario", "Yaritza Báez", "Miguel Tavárez"]
_CANALES = ["WhatsApp", "Instagram", "Facebook"]
_INTERESES = [
    ("Quiere el chatbot de WhatsApp para su clínica dental", "¿Cuánto cuesta y en cuánto tiempo lo tienen?", "caliente"),
    ("Pregunta por la consultoría de automatización", "Tengo una agencia de viajes y pierdo mucho tiempo respondiendo", "tibio"),
    ("Comentó AGENTES en el reel y pidió la guía", "Me interesa montar agentes para mi tienda", "frío"),
    ("Quiere entrar al taller del sábado", "¿Todavía quedan cupos para el taller?", "caliente"),
    ("Dueño de restaurante, quiere reservas automáticas", "Vi tu video, ¿me ayudas a montarlo?", "tibio"),
    ("Pide precio del curso en línea", "¿El curso incluye las grabaciones?", "tibio"),
]


class NegocioMock:
    def __init__(self):
        ahora = time.time()
        self.leads = [self._lead(ahora - random.randint(2, 30) * 3600) for _ in range(4)]
        self.noshows = [
            {"id": next(_ids), "nombre": "Héctor Guzmán", "cita": "Consultoría 1:1", "cuando": "ayer 4:00 PM",
             "telefono": "+1 809 555 0142"},
        ]
        self.briefs = [
            {"id": next(_ids), "marca": "Marca A", "pieza": "Vídeo de producto", "vence": "lunes", "estado": "guion pendiente"},
            {"id": next(_ids), "marca": "Marca B", "pieza": "Tutorial", "vence": "en 9 días", "estado": "grabado"},
        ]
        self.enviados: list[dict] = []

    def _lead(self, t=None, nombre=None, mensaje=None):
        resumen, msg, temp = random.choice(_INTERESES)
        return {"id": next(_ids), "nombre": nombre or random.choice(_NOMBRES), "canal": random.choice(_CANALES),
                "resumen": resumen, "ultimo_mensaje": mensaje or msg, "temperatura": temp,
                "t": t or time.time(), "respondido": False}

    def nuevo_lead(self, nombre=None, mensaje=None) -> dict:
        """Para simular que entra un lead (lo usa la consola y los tests)."""
        lead = self._lead(nombre=nombre, mensaje=mensaje)
        if mensaje:     # con mensaje propio, el resumen es el mensaje y la temperatura sale de él
            lead["resumen"] = "Escribió por primera vez"
            caliente = any(w in mensaje.lower() for w in ("cuánto", "cuanto", "precio", "cuesta", "cupo", "contratar"))
            lead["temperatura"] = "caliente" if caliente else "tibio"
        else:
            lead["temperatura"] = "caliente"
        self.leads.append(lead)
        return lead

    # --- herramientas ---
    async def listar_leads(self) -> dict:
        pend = [l for l in self.leads if not l["respondido"]]
        orden = {"caliente": 0, "tibio": 1, "frío": 2}
        pend.sort(key=lambda l: (orden[l["temperatura"]], -l["t"]))
        return {"leads": [{**l, "hace_horas": round((time.time() - l["t"]) / 3600, 1)} for l in pend]}

    async def ver_lead(self, id: int) -> dict:
        for l in self.leads:
            if l["id"] == int(id):
                return l
        return {"error": f"no hay lead {id}"}

    async def listar_noshows(self) -> dict:
        return {"noshows": self.noshows}

    async def listar_briefs(self) -> dict:
        return {"briefs": self.briefs}

    async def enviar_mensaje(self, destinatario_id: int, texto: str) -> dict:
        destino = next((l for l in self.leads + self.noshows if l["id"] == int(destinatario_id)), None)
        if not destino:
            return {"ok": False, "error": f"no existe el destinatario {destinatario_id}"}
        destino["respondido"] = True
        self.enviados.append({"a": destino["nombre"], "texto": texto, "t": time.time()})
        return {"ok": True, "a": destino["nombre"], "canal": destino.get("canal", "WhatsApp"), "simulado": True}

    def herramientas(self) -> list[Herramienta]:
        return [
            Herramienta("listar_leads", "read", "leads del chat sin responder, el más caliente primero", self.listar_leads),
            Herramienta("ver_lead", "read", "detalle de un lead", self.ver_lead, {"id": "id del lead"}),
            Herramienta("listar_noshows", "read", "citas a las que no se presentó el cliente", self.listar_noshows),
            Herramienta("listar_briefs", "read", "briefs de marcas pendientes y cuándo vencen", self.listar_briefs),
            Herramienta("enviar_mensaje", "act", "envía un mensaje por el canal del lead o cliente",
                        self.enviar_mensaje, {"destinatario_id": "id del lead o no-show", "texto": "mensaje final"}),
        ]
