"""Hermes → placa del companion. Reenvía el ciclo de cada turno a 127.0.0.1:8770/api/hermes-evento
(agent:start, tool:start, tool:end, agent:end).

Como plugin corre dentro del agente, así que cubre todos los caminos (chat del dashboard, terminal,
gateway). Se filtran el API server (es la propia placa hablando con Hermes) y las tareas cron.
Cada aviso sale en un hilo con 2 s de plazo: si el companion no está, Hermes ni se entera."""
import json
import os
import threading
import urllib.request

# dónde corre el servidor de Pana (en la misma máquina que Hermes, por defecto)
URL = os.environ.get("PANA_EVENT_URL", "http://127.0.0.1:8770/api/hermes-evento")
IGNORAR = ("api", "cron")
_plataforma: dict[str, str] = {}          # session_id → plataforma (pre_tool_call no siempre la trae)


def _enviar(datos: dict):
    def ir():
        try:
            req = urllib.request.Request(URL, data=json.dumps(datos).encode(), headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=2).read()
        except Exception:
            pass
    threading.Thread(target=ir, daemon=True).start()


def _plat(kw: dict) -> str:
    sid = kw.get("session_id") or ""
    p = kw.get("platform") or _plataforma.get(sid, "")
    if p and sid:
        _plataforma[sid] = p
    return p or ""


def _vale(p: str) -> bool:
    return not any(p.startswith(x) for x in IGNORAR)


def on_pre_llm_call(**kw):
    p = _plat(kw)
    if _vale(p):
        _enviar({"event": "agent:start", "platform": p, "session_id": kw.get("session_id", ""),
                 "message": str(kw.get("user_message") or "")[:500]})
    return None                             # no inyecta nada en el turno


def _detalle(args) -> str:
    """Lo que hace la llamada en pocas palabras (el comando, la búsqueda, la ruta…), nunca los args enteros."""
    if isinstance(args, dict):
        for k in ("command", "cmd", "query", "url", "path", "file_path", "pattern", "name", "skill", "action"):
            if isinstance(args.get(k), str) and args[k].strip():
                return args[k][:120]
        for v in args.values():
            if isinstance(v, str) and v.strip():
                return v[:120]
    return ""


def on_pre_tool_call(**kw):
    p = _plat(kw)
    if _vale(p):
        _enviar({"event": "tool:start", "platform": p, "session_id": kw.get("session_id") or kw.get("task_id", ""),
                 "tool_name": kw.get("tool_name", ""), "tool_call_id": kw.get("tool_call_id", ""),
                 "detail": _detalle(kw.get("args"))})
    return None


def on_post_tool_call(**kw):
    p = _plat(kw)
    if _vale(p):
        estado = str(kw.get("status") or "")
        ok = not kw.get("error_type") and estado not in ("error", "failed", "blocked")
        _enviar({"event": "tool:end", "platform": p, "session_id": kw.get("session_id") or kw.get("task_id", ""),
                 "tool_name": kw.get("tool_name", ""), "tool_call_id": kw.get("tool_call_id", ""), "ok": ok})


def on_post_llm_call(**kw):
    p = _plat(kw)
    if _vale(p):
        _enviar({"event": "agent:end", "platform": p, "session_id": kw.get("session_id", ""),
                 "message": str(kw.get("user_message") or "")[:500],
                 "response": str(kw.get("assistant_response") or "")[:500]})


def on_session_end(**kw):
    # un turno que falla o se interrumpe no pasa por post_llm_call: quitar la tarjeta de «trabajando»
    if kw.get("completed") is False or kw.get("failed") or kw.get("interrupted"):
        p = _plat(kw)
        if _vale(p):
            _enviar({"event": "agent:end", "platform": p, "session_id": kw.get("session_id", ""), "response": ""})


def register(ctx) -> None:
    ctx.register_hook("pre_llm_call", on_pre_llm_call)
    ctx.register_hook("pre_tool_call", on_pre_tool_call)
    ctx.register_hook("post_tool_call", on_post_tool_call)
    ctx.register_hook("post_llm_call", on_post_llm_call)
    ctx.register_hook("on_session_end", on_session_end)
