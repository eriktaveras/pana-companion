"""Cómo se dicen las siglas y marcas. Se aplica IGUAL al texto de entrenamiento de la voz de
la voz y al texto que Pana va a decir: si cambia aquí, hay que reentrenar (o al menos se nota).
Ajústalo a tus siglas."""
import re

_REGLAS = [
    (r"\bn8n\b", "ene ocho ene"),
    (r"\bUSB\b", "u ese be"),
    (r"\bPCs?\b", "pe ce"),
    (r"\bIPs?\b", "i pe"),
    (r"\bAPIs?\b", "a pe i"),
    (r"\bVPS\b", "ve pe ese"),
    (r"\bIA\b", "i a"),
    (r"\bGPU\b", "ge pe u"),
    (r"\bCRM\b", "ce erre eme"),
    (r"\bGitHub\b", "Guít-jáb"),
    (r"\bTypeSafe\b", "Táip-séif"),
]
_COMPILADAS = [(re.compile(p, re.IGNORECASE if p.startswith(r"\bn8n") else 0), r) for p, r in _REGLAS]


def a_voz(texto: str) -> str:
    for p, r in _COMPILADAS:
        texto = p.sub(r, texto)
    return texto
