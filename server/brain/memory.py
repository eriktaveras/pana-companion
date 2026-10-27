"""Memoria en SQLite. Los embeddings van como BLOB float32 y la búsqueda es un
producto punto con numpy: para unos miles de recuerdos sobra y no hay extensiones
que compilar. Si crece mucho, se cambia por sqlite-vec sin tocar la interfaz.
"""
import json
import logging
import sqlite3
import time
from pathlib import Path

import numpy as np

log = logging.getLogger("memoria")

ESQUEMA = """
CREATE TABLE IF NOT EXISTS eventos (
    id INTEGER PRIMARY KEY, t REAL, tipo TEXT, texto TEXT, datos TEXT, visto INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS recuerdos (            -- hechos sobre el usuario, personas, conversaciones resumidas
    id INTEGER PRIMARY KEY, t REAL, tipo TEXT, texto TEXT, vec BLOB, peso REAL DEFAULT 1, activo INTEGER DEFAULT 1);
CREATE TABLE IF NOT EXISTS personas (
    id INTEGER PRIMARY KEY, nombre TEXT UNIQUE, notas TEXT, t REAL);
CREATE TABLE IF NOT EXISTS conversacion (          -- turnos crudos del día; el sueño los resume
    id INTEGER PRIMARY KEY, t REAL, rol TEXT, texto TEXT);
CREATE TABLE IF NOT EXISTS diario (
    fecha TEXT PRIMARY KEY, texto TEXT, brief TEXT, t REAL);
CREATE TABLE IF NOT EXISTS auditoria (
    id INTEGER PRIMARY KEY, t REAL, herramienta TEXT, args TEXT, confirmado_por TEXT, resultado TEXT);
CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT);
"""


class Memoria:
    def __init__(self, ruta: str, embedder=None):
        Path(ruta).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(ruta, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(ESQUEMA)
        self.embedder = embedder        # async (list[str]) -> np.ndarray normalizado

    # --- clave/valor (ánimo, contadores, último sueño...) ---
    def get(self, k, defecto=None):
        r = self.db.execute("SELECT v FROM kv WHERE k=?", (k,)).fetchone()
        return json.loads(r["v"]) if r else defecto

    def set(self, k, v):
        self.db.execute("INSERT OR REPLACE INTO kv VALUES (?,?)", (k, json.dumps(v)))
        self.db.commit()

    # --- eventos ---
    def evento(self, tipo: str, texto: str, datos: dict | None = None) -> int:
        c = self.db.execute("INSERT INTO eventos (t,tipo,texto,datos) VALUES (?,?,?,?)",
                            (time.time(), tipo, texto, json.dumps(datos or {}, ensure_ascii=False)))
        self.db.commit()
        return c.lastrowid

    def eventos_nuevos(self, marcar=True) -> list[dict]:
        filas = [dict(r) for r in self.db.execute("SELECT * FROM eventos WHERE visto=0 ORDER BY t")]
        if marcar and filas:
            self.db.execute("UPDATE eventos SET visto=1 WHERE visto=0")
            self.db.commit()
        return filas

    def eventos_desde(self, t0: float) -> list[dict]:
        return [dict(r) for r in self.db.execute("SELECT * FROM eventos WHERE t>=? ORDER BY t", (t0,))]

    # --- conversación ---
    def turno(self, rol: str, texto: str):
        self.db.execute("INSERT INTO conversacion (t,rol,texto) VALUES (?,?,?)", (time.time(), rol, texto))
        self.db.commit()

    def ultimos_turnos(self, n=8, minutos=15) -> list[dict]:
        """Solo la conversación en curso: lo viejo vive en los recuerdos, no en el historial."""
        filas = self.db.execute("SELECT rol,texto FROM conversacion WHERE t>=? ORDER BY id DESC LIMIT ?",
                                (time.time() - minutos * 60, n)).fetchall()
        return [dict(r) for r in reversed(filas)]

    def turnos_desde(self, t0: float) -> list[dict]:
        return [dict(r) for r in self.db.execute("SELECT t,rol,texto FROM conversacion WHERE t>=? ORDER BY t", (t0,))]

    # --- recuerdos con embeddings ---
    async def recordar(self, tipo: str, texto: str, peso=1.0) -> int:
        vec = None
        if self.embedder:
            try:
                vec = (await self.embedder([texto]))[0].astype(np.float32).tobytes()
            except Exception as e:      # sin embeddings el recuerdo se guarda igual
                log.warning("embedding falló: %s", e)
        c = self.db.execute("INSERT INTO recuerdos (t,tipo,texto,vec,peso) VALUES (?,?,?,?,?)",
                            (time.time(), tipo, texto, vec, peso))
        self.db.commit()
        return c.lastrowid

    async def buscar(self, consulta: str, k=5, tipos: list[str] | None = None) -> list[dict]:
        """Top-k por similitud coseno (los vectores ya vienen normalizados)."""
        sql = "SELECT id,t,tipo,texto,vec,peso FROM recuerdos WHERE activo=1 AND vec IS NOT NULL"
        args = []
        if tipos:
            sql += f" AND tipo IN ({','.join('?' * len(tipos))})"
            args += tipos
        filas = self.db.execute(sql, args).fetchall()
        if not filas or not self.embedder:
            return self.recientes(k, tipos)
        try:
            q = (await self.embedder([consulta]))[0]
        except Exception as e:
            log.warning("embedding de consulta falló: %s", e)
            return self.recientes(k, tipos)
        m = np.frombuffer(b"".join(f["vec"] for f in filas), dtype=np.float32).reshape(len(filas), -1)
        sims = m @ q * np.array([f["peso"] for f in filas], dtype=np.float32)
        orden = np.argsort(-sims)[:k]
        return [{"id": filas[i]["id"], "tipo": filas[i]["tipo"], "texto": filas[i]["texto"],
                 "sim": float(sims[i])} for i in orden]

    def recientes(self, k=5, tipos=None) -> list[dict]:
        sql = "SELECT id,tipo,texto FROM recuerdos WHERE activo=1"
        args = []
        if tipos:
            sql += f" AND tipo IN ({','.join('?' * len(tipos))})"
            args += tipos
        return [dict(r) for r in self.db.execute(sql + " ORDER BY t DESC LIMIT ?", args + [k])]

    def todos_recuerdos(self) -> list[dict]:
        return [dict(r) for r in self.db.execute("SELECT id,t,tipo,texto FROM recuerdos WHERE activo=1 ORDER BY t")]

    def desactivar(self, ids: list[int]):
        self.db.executemany("UPDATE recuerdos SET activo=0 WHERE id=?", [(i,) for i in ids])
        self.db.commit()

    # --- diario ---
    def escribir_diario(self, fecha: str, texto: str, brief: str):
        self.db.execute("INSERT OR REPLACE INTO diario VALUES (?,?,?,?)", (fecha, texto, brief, time.time()))
        self.db.commit()

    def diario(self, fecha: str) -> dict | None:
        r = self.db.execute("SELECT * FROM diario WHERE fecha=?", (fecha,)).fetchone()
        return dict(r) if r else None

    # --- auditoría de acciones externas ---
    def auditar(self, herramienta: str, args: dict, confirmado_por: str, resultado: str):
        self.db.execute("INSERT INTO auditoria (t,herramienta,args,confirmado_por,resultado) VALUES (?,?,?,?,?)",
                        (time.time(), herramienta, json.dumps(args, ensure_ascii=False), confirmado_por, resultado))
        self.db.commit()

    def auditoria(self, n=50) -> list[dict]:
        return [dict(r) for r in self.db.execute("SELECT * FROM auditoria ORDER BY id DESC LIMIT ?", (n,))]
