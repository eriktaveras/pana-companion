"""Motor de ánimo: cuatro variables de 0 a 1 que derivan con el tiempo y con lo que pasa.

- energia: alta de día, baja de noche; el sueño la recarga.
- curiosidad: sube con eventos nuevos, baja sola.
- aburrimiento: sube con el tiempo sin nada que hacer y si le ignoran; baja al interactuar.
- afecto: sube cuando le tocan o le habla; baja muy despacio.
"""
from dataclasses import asdict, dataclass

# Hacia dónde tiende cada variable sola y a qué velocidad (fracción por hora)
_REPOSO = {"energia": 0.6, "curiosidad": 0.3, "aburrimiento": 0.7, "afecto": 0.5}
_VELOCIDAD = {"energia": 0.10, "curiosidad": 0.25, "aburrimiento": 0.12, "afecto": 0.02}

# Efecto inmediato de cada evento
EFECTOS = {
    "lead_nuevo": {"curiosidad": +0.35, "energia": +0.15, "aburrimiento": -0.4},
    "noshow": {"curiosidad": +0.2, "aburrimiento": -0.2, "energia": -0.05},
    "toque": {"afecto": +0.06, "aburrimiento": -0.25, "energia": +0.05},
    "voz": {"afecto": +0.08, "aburrimiento": -0.35, "curiosidad": +0.1},
    "accion_confirmada": {"afecto": +0.05, "energia": +0.05},
    "accion_cancelada": {"afecto": -0.02},
    "ignorado": {"aburrimiento": +0.15, "afecto": -0.03},
    "sistema_mal": {"energia": -0.1, "curiosidad": +0.2},
    "sueño": {"energia": +0.6, "aburrimiento": -0.5, "curiosidad": +0.1},
}


def _clamp(x):
    return max(0.0, min(1.0, x))


@dataclass
class Mood:
    energia: float = 0.6
    curiosidad: float = 0.4
    aburrimiento: float = 0.3
    afecto: float = 0.5

    def avanzar(self, horas: float, de_noche: bool = False):
        """Deriva hacia el reposo. De noche la energía baja en vez de recuperarse."""
        for k, reposo in _REPOSO.items():
            if k == "energia" and de_noche:
                reposo = 0.15
            v = getattr(self, k)
            f = min(1.0, _VELOCIDAD[k] * horas)
            setattr(self, k, _clamp(v + (reposo - v) * f))

    def aplicar(self, evento: str):
        for k, d in EFECTOS.get(evento, {}).items():
            setattr(self, k, _clamp(getattr(self, k) + d))

    def emocion(self, dormido: bool = False) -> str:
        """Cara en reposo según el ánimo (las reacciones puntuales las decide el heartbeat)."""
        if dormido:
            return "sleeping"
        if self.energia < 0.2:
            return "sleeping"
        if self.curiosidad > 0.75 and self.energia > 0.5:
            return "excited"
        if self.aburrimiento > 0.75:
            return "bored"
        if self.afecto < 0.2:
            return "sad"
        if self.afecto > 0.7 and self.energia > 0.5:
            return "happy"
        return "idle"

    def ganas_de_hablar(self) -> float:
        """0..1: cuánto le apetece intervenir por su cuenta. Lo usa el heartbeat como filtro."""
        return _clamp(0.15 + 0.4 * self.curiosidad + 0.3 * self.aburrimiento + 0.2 * self.energia - 0.25)

    def tono(self) -> str:
        """Pista corta para el prompt."""
        partes = []
        partes.append("con energía" if self.energia > 0.6 else "cansado" if self.energia < 0.3 else "tranquilo")
        if self.curiosidad > 0.6:
            partes.append("curioso")
        if self.aburrimiento > 0.6:
            partes.append("aburrido")
        if self.afecto > 0.7:
            partes.append("cariñoso")
        elif self.afecto < 0.3:
            partes.append("algo distante")
        return ", ".join(partes)

    def dict(self):
        return {k: round(v, 3) for k, v in asdict(self).items()}
