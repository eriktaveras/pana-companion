"""Mensajes del protocolo (ver protocol/PROTOCOL.md). Se validan con Pydantic en ambos sentidos."""
from typing import Annotated, Literal, Union

from pydantic import BaseModel, Field, TypeAdapter

Emotion = Literal["idle", "happy", "thinking", "talking", "alert", "sad", "bored", "sleeping", "excited", "love", "confused", "working",
                  # animaciones del paquete hermes-agent-pack (4.3"); la 2.8" las cae en la más parecida
                  "reading", "typing", "searching", "executing", "approval", "error", "success", "wave"]
EMOTIONS = list(Emotion.__args__)

# --- servidor → cuerpo -------------------------------------------------------


class State(BaseModel):
    type: Literal["state"] = "state"
    emotion: Emotion
    animation: str | None = None
    loop: bool = True
    look: tuple[float, float] | None = None     # mirar a (x, y) en -1..1


class Say(BaseModel):
    type: Literal["say"] = "say"
    id: str
    text: str
    emotion: Emotion = "talking"
    audio: bool = False


class AudioEnd(BaseModel):
    type: Literal["audio_end"] = "audio_end"
    id: str


class CardAction(BaseModel):
    id: str
    label: str


class GraphNode(BaseModel):
    label: str
    x: int                 # 0..1000
    y: int
    deg: int               # enlaces
    group: int             # carpeta de primer nivel (Graph.groups); -1 = enlace a una nota que no existe


class Graph(BaseModel):
    """El grafo de Obsidian (brain/obsidian.py) para la pestaña «grafo» de la 4.3"."""
    type: Literal["graph"] = "graph"
    title: str
    nodes: list[GraphNode]
    edges: list[tuple[int, int]]
    groups: list[str]
    notes: int
    links: int


class ToolRow(BaseModel):
    st: Literal["run", "ok", "err"]
    name: str
    detail: str = ""
    t0: float = 0                    # epoch de inicio (la placa toma la hora del ping: cuenta en vivo)
    secs: float | None = None        # duración, al acabar


class Card(BaseModel):
    type: Literal["card"] = "card"
    id: str
    kind: Literal["lead", "noshow", "brief", "system", "tools"]   # tools: líneas "estado\tnombre\tdetalle"
    title: str
    lines: list[str] = []
    actions: list[CardAction] = []
    icon: str | None = None          # whatsapp | instagram | facebook (logo pequeño en la cabecera)
    # solo kind tools: la consola de las pantallas grandes (las 4 líneas de arriba siguen para la 2.8" y el sim)
    tools: list[ToolRow] = []
    tools_total: int = 0


class Clear(BaseModel):
    type: Literal["clear"] = "clear"
    what: Literal["card", "bubble", "all"] = "all"


class Confirm(BaseModel):
    type: Literal["confirm"] = "confirm"
    id: str
    question: str
    preview: str
    options: list[str] = ["Enviar", "Editar", "Cancelar"]
    expires_in: int = 300


class ConfirmClose(BaseModel):
    type: Literal["confirm_close"] = "confirm_close"
    id: str
    result: str


class Mood(BaseModel):
    type: Literal["mood"] = "mood"
    energia: float
    curiosidad: float
    aburrimiento: float
    afecto: float


class Ota(BaseModel):
    type: Literal["ota"] = "ota"
    url: str
    version: str


class Ping(BaseModel):
    type: Literal["ping"] = "ping"
    t: int


# --- cuerpo → servidor -------------------------------------------------------


class Hello(BaseModel):
    type: Literal["hello"]
    device_id: str
    fw_version: str = "?"
    screen: list[int] = [240, 320]
    has_mic: bool = False
    has_speaker: bool = False
    kind: Literal["board", "sim"] = "board"


class Touch(BaseModel):
    type: Literal["touch"]
    target: str
    x: int = 0
    y: int = 0
    gesture: Literal["tap", "long_press", "swipe_left", "swipe_right", "swipe_up", "swipe_down"] = "tap"


class ConfirmReply(BaseModel):
    type: Literal["confirm_reply"]
    id: str
    choice: str


class AudioStart(BaseModel):
    type: Literal["audio_start"]
    id: str = "mic"
    sample_rate: int = 16000


class AudioEndIn(BaseModel):
    type: Literal["audio_end"]
    id: str = "mic"


class Pong(BaseModel):
    type: Literal["pong"]
    t: int = 0


class Status(BaseModel):
    type: Literal["status"]
    rssi: int = 0
    heap: int = 0
    psram: int = 0
    uptime: int = 0
    fw_version: str = "?"


Entrante = Annotated[Union[Hello, Touch, ConfirmReply, AudioStart, AudioEndIn, Pong, Status], Field(discriminator="type")]
_entrante = TypeAdapter(Entrante)


def parse_entrante(raw: str | bytes):
    """Valida un mensaje JSON del cuerpo. Lanza ValueError si no es del protocolo."""
    return _entrante.validate_json(raw)
