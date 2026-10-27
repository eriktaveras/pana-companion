"""El grafo de un vault de Obsidian (OBSIDIAN_VAULT), para la página «grafo» de la placa.

Lee las notas (.md fuera de .obsidian), saca los [[enlaces]] (con alias, #encabezado o ruta) y
los resuelve por nombre de archivo como Obsidian; los que no existen quedan como nodos fantasma
(el vault tiene hideUnresolved=false). La disposición es de fuerzas (Fruchterman-Reingold) con
semilla fija, así no baila entre envíos; las coordenadas van en 0..1000.
Se vuelve a mandar cuando cambia algo del vault (se mira cada 30 s): Hermes escribe en él.
"""
import asyncio
import logging
import re
from pathlib import Path

import numpy as np

from config import cfg
from protocol import Graph, GraphNode

log = logging.getLogger("obsidian")

ENLACE = re.compile(r"\[\[([^\]|#^]+)(?:[#^][^\]|]*)?(?:\|[^\]]*)?\]\]")
MAX_NODOS = 150


def _notas(vault: Path) -> list[Path]:
    return sorted(p for p in vault.rglob("*.md") if ".obsidian" not in p.parts and ".trash" not in p.parts)


def huella(vault: Path) -> tuple:
    """Cambia si se crea, borra o edita una nota."""
    return tuple((str(p), p.stat().st_mtime_ns) for p in _notas(vault))


def _disponer(n: int, aristas: list[tuple[int, int]], semilla=7, pasos=400) -> np.ndarray:
    rng = np.random.default_rng(semilla)
    pos = rng.uniform(-1, 1, (n, 2))
    if n == 1:
        return np.array([[500.0, 500.0]])
    k = np.sqrt(4.0 / n)
    t = 0.2
    a = np.array(aristas, int).reshape(-1, 2)
    for _ in range(pasos):
        d = pos[:, None, :] - pos[None, :, :]
        dist = np.maximum(np.linalg.norm(d, axis=-1), 1e-3)
        mov = (d / dist[..., None] * (k * k / dist)[..., None]).sum(1)        # repulsión
        if len(a):
            da = pos[a[:, 0]] - pos[a[:, 1]]
            la = np.maximum(np.linalg.norm(da, axis=-1), 1e-3)
            f = da / la[:, None] * (la * la / k)[:, None]                       # atracción
            np.add.at(mov, a[:, 0], -f)
            np.add.at(mov, a[:, 1], f)
        mov -= pos * 0.6 * k                                                    # al centro (los sueltos)
        lm = np.maximum(np.linalg.norm(mov, axis=-1), 1e-6)
        pos += mov / lm[:, None] * np.minimum(lm, t)[:, None]
        t *= 0.985
    pos -= pos.min(0)
    pos /= np.maximum(pos.max(0), 1e-6)
    return pos * 1000             # 0..1000


def construir(vault: Path) -> Graph:
    notas = _notas(vault)
    por_nombre = {}
    for p in notas:
        por_nombre.setdefault(p.stem.lower(), p)
    ids, nodos, aristas = {}, [], set()

    def nodo(clave, etiqueta, grupo):
        if clave not in ids:
            ids[clave] = len(nodos)
            nodos.append({"label": etiqueta, "group": grupo})
        return ids[clave]

    grupos: list[str] = []

    def grupo_de(p: Path) -> int:
        rel = p.relative_to(vault).parts
        g = rel[0] if len(rel) > 1 else "/"
        if g not in grupos:
            grupos.append(g)
        return grupos.index(g)

    for p in notas:
        nodo(str(p), p.stem, grupo_de(p))
    for p in notas:
        try:
            texto = p.read_text(errors="ignore")
        except OSError:
            continue
        a = ids[str(p)]
        for m in ENLACE.finditer(texto):
            destino = m.group(1).strip()
            nombre = Path(destino).name.removesuffix(".md").lower()
            q = por_nombre.get(nombre)
            b = ids[str(q)] if q else nodo("?" + nombre, Path(destino).name, -1)   # fantasma
            if a != b:
                aristas.add((min(a, b), max(a, b)))
    grado = [0] * len(nodos)
    for a, b in aristas:
        grado[a] += 1
        grado[b] += 1
    # si es enorme, se quedan los más conectados
    orden = sorted(range(len(nodos)), key=lambda i: -grado[i])[:MAX_NODOS]
    nuevo = {v: i for i, v in enumerate(orden)}
    aristas = [(nuevo[a], nuevo[b]) for a, b in aristas if a in nuevo and b in nuevo]
    # las conectadas llenan el recuadro; las sueltas van alrededor, como en Obsidian (si no, una
    # sola nota sin enlaces empuja todo el resto a una esquina)
    pos = np.zeros((len(orden), 2))
    con = [i for i, v in enumerate(orden) if grado[v]]
    suel = [i for i, v in enumerate(orden) if not grado[v]]
    if con:
        idx = {v: k for k, v in enumerate(con)}
        sub = _disponer(len(con), [(idx[a], idx[b]) for a, b in aristas])
        pos[con] = 80 + sub * (840 if suel else 1000) / 1000 - (0 if suel else 80)
    for k, i in enumerate(suel):
        ang = -np.pi * 0.75 + 2 * np.pi * k / max(len(suel), 1)
        pos[i] = 500 + 470 * np.cos(ang), 500 + 470 * np.sin(ang)
    pos = np.clip(pos, 0, 1000)
    salida = [GraphNode(label=nodos[v]["label"][:40], x=int(pos[i, 0]), y=int(pos[i, 1]), deg=grado[v],
                        group=nodos[v]["group"]) for i, v in enumerate(orden)]
    return Graph(title=vault.name, nodes=salida, edges=aristas, groups=grupos,
                 notes=len(notas), links=len(aristas))


async def vigilar(hub, cada_s: float = 30):
    vault = Path(cfg.obsidian_vault).expanduser()
    if not vault.is_dir():
        log.info("sin vault de Obsidian en %s: la pestaña grafo queda vacía", vault)
        return
    ultima = None
    while True:
        try:
            h = await asyncio.to_thread(huella, vault)
            if h != ultima:
                g = await asyncio.to_thread(construir, vault)
                await hub.emitir(g)
                log.info("grafo de %s: %d notas, %d enlaces", vault.name, g.notes, g.links)
                ultima = h
        except Exception as e:
            log.warning("grafo de Obsidian falló: %s", e)
        await asyncio.sleep(cada_s)
