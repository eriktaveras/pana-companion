"""Interfaz común de herramientas y niveles de permiso.

read  → libre: consulta datos.
draft → libre: redacta, pero el resultado se queda en el servidor.
act   → solo se ejecuta tras un `confirm` táctil (ver brain/gate.py). Llamarla
        directamente lanza PermisoDenegado: así un fallo del LLM no puede saltárselo.
"""
from dataclasses import dataclass, field
from typing import Awaitable, Callable, Literal

Nivel = Literal["read", "draft", "act"]


class PermisoDenegado(Exception):
    pass


@dataclass
class Herramienta:
    nombre: str
    nivel: Nivel
    descripcion: str
    fn: Callable[..., Awaitable[dict]]
    parametros: dict = field(default_factory=dict)   # {nombre: descripción}

    async def __call__(self, _confirmado: bool = False, **kw) -> dict:
        if self.nivel == "act" and not _confirmado:
            raise PermisoDenegado(f"{self.nombre} es de nivel act: necesita confirmación táctil")
        return await self.fn(**kw)


class Registro:
    def __init__(self):
        self._h: dict[str, Herramienta] = {}

    def add(self, h: Herramienta):
        self._h[h.nombre] = h

    def get(self, nombre: str) -> Herramienta | None:
        return self._h.get(nombre)

    def por_nivel(self, *niveles: str) -> list[Herramienta]:
        return [h for h in self._h.values() if h.nivel in niveles]

    def catalogo(self) -> str:
        """Lista para el prompt."""
        lineas = []
        for h in self._h.values():
            p = ", ".join(f"{k}: {v}" for k, v in h.parametros.items()) or "sin parámetros"
            lineas.append(f"- {h.nombre} [{h.nivel}] {h.descripcion} ({p})")
        return "\n".join(lineas)
