"""Tablero de la esfera de 720 (la placa P4): homelab en vivo, agenda de hoy y redes.

Un mensaje `dash` cada 5 s con todo (es pequeño, < 1 KB): así quien se conecta tarde lo tiene en
seguida y la placa no guarda nada. Lo lento va en su propio ritmo y se reutiliza:
  · sistema (CPU, GPU, RAM, temperatura) y servicios: cada envío, de /proc, /sys y /health
  · agenda: cada 10 min, del calendario en iCal (AGENDA_ICS_URL, la «dirección secreta»; varias
    separadas por comas)
  · redes: cada 15 min, YouTube (YT_API_KEY, YT_CHANNEL_ID) e Instagram (INSTAGRAM_ACCESS_TOKEN,
    INSTAGRAM_BUSINESS_ACCOUNT_ID). El «hoy +N» se cuenta desde la primera lectura del día.
Lo que no está configurado va con ok=false y la placa lo dice en vez de inventar.
"""
import asyncio
import logging
import os
import re
import socket
import time
from datetime import date, datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx

from config import cfg

log = logging.getLogger("tablero")

CADA_S = 5
AGENDA_S = 600
REDES_S = 900


# --- sistema ---------------------------------------------------------------------------------
class _Cpu:
    def __init__(self):
        self.prev = None

    def uso(self) -> float:
        try:
            v = [int(x) for x in Path("/proc/stat").read_text().split("\n", 1)[0].split()[1:]]
        except OSError:
            return 0.0
        ocio, total = v[3] + v[4], sum(v)
        if self.prev is None:
            self.prev = (ocio, total)
            return 0.0
        d_ocio, d_total = ocio - self.prev[0], total - self.prev[1]
        self.prev = (ocio, total)
        return round(100 * (1 - d_ocio / d_total), 1) if d_total > 0 else 0.0


def _leer(p: Path) -> str | None:
    try:
        return p.read_text().strip()
    except OSError:
        return None


def _gpu() -> float | None:
    for p in Path("/sys/class/drm").glob("card*/device/gpu_busy_percent"):
        v = _leer(p)
        if v and v.isdigit():
            return float(v)
    return None


def _temp() -> float | None:
    """La del procesador (k10temp Tctl); si no, la más alta que haya."""
    mejor = None
    for hw in Path("/sys/class/hwmon").glob("hwmon*"):
        nombre = _leer(hw / "name") or ""
        for f in hw.glob("temp*_input"):
            v = _leer(f)
            if not v or not v.lstrip("-").isdigit():
                continue
            t = int(v) / 1000
            if nombre == "k10temp" and (_leer(f.with_name(f.name.replace("_input", "_label"))) or "") == "Tctl":
                return round(t, 1)
            mejor = t if mejor is None else max(mejor, t)
    return round(mejor, 1) if mejor is not None else None


def _ram() -> tuple[float, float]:
    try:
        info = dict(l.split(":", 1) for l in Path("/proc/meminfo").read_text().splitlines())
        kb = lambda k: int(info[k].split()[0])
        total, libre = kb("MemTotal") / 2**20, kb("MemAvailable") / 2**20
        return round(total - libre, 1), round(total, 1)
    except (OSError, KeyError, ValueError):
        return 0.0, 0.0


def _uptime() -> str:
    v = _leer(Path("/proc/uptime"))
    if not v:
        return ""
    s = int(float(v.split()[0]))
    d, h = s // 86400, s % 86400 // 3600
    return f"{d} d {h} h" if d else f"{h} h {s % 3600 // 60} min"


def _servicios() -> list[tuple[str, str]]:
    base = lambda u: u.removesuffix("/v1")
    return [("Hermes", base(cfg.hermes_url) + "/health"), ("LLM", base(cfg.chat_url) + "/health"),
            ("Rápido", base(cfg.fast_url) + "/health"), ("Whisper", cfg.stt_url.rsplit("/", 1)[0] + "/"),
            ("Memoria", base(cfg.embed_url) + "/health")]


async def _vivo(c: httpx.AsyncClient, url: str) -> int:
    try:
        r = await c.get(url, timeout=2)
        return 1 if r.status_code < 500 else 0
    except httpx.HTTPError:
        return 0


# --- agenda (iCal) ---------------------------------------------------------------------------
def _desplegar(ics: str) -> list[str]:
    """Une las líneas partidas del iCal (las que siguen empiezan con espacio)."""
    return re.sub(r"\r?\n[ \t]", "", ics).splitlines()


def _fecha(valor: str, params: str, tz: ZoneInfo) -> tuple[datetime, bool]:
    """→ (instante en la zona local, ¿todo el día?)"""
    if "VALUE=DATE" in params or re.fullmatch(r"\d{8}", valor):
        return datetime.strptime(valor[:8], "%Y%m%d").replace(tzinfo=tz), True
    if valor.endswith("Z"):
        return datetime.strptime(valor, "%Y%m%dT%H%M%SZ").replace(tzinfo=ZoneInfo("UTC")).astimezone(tz), False
    m = re.search(r"TZID=([^;:]+)", params)
    zona = tz
    if m:
        try:
            zona = ZoneInfo(m.group(1))
        except Exception:
            pass
    return datetime.strptime(valor[:15], "%Y%m%dT%H%M%S").replace(tzinfo=zona).astimezone(tz), False


_DIAS = {"MO": 0, "TU": 1, "WE": 2, "TH": 3, "FR": 4, "SA": 5, "SU": 6}


def _ocurre_hoy(ini: datetime, rrule: str, hoy: date, tz: ZoneInfo) -> bool:
    """RRULE lo justo: DAILY y WEEKLY (con INTERVAL, BYDAY, UNTIL y COUNT aproximado)."""
    r = dict(p.split("=", 1) for p in rrule.split(";") if "=" in p)
    if ini.date() > hoy:
        return False
    if "UNTIL" in r:
        u, _ = _fecha(r["UNTIL"], "", tz)
        if u.date() < hoy:
            return False
    paso = int(r.get("INTERVAL", "1"))
    dias = (hoy - ini.date()).days
    if r.get("FREQ") == "DAILY":
        n = dias // paso
        return dias % paso == 0 and ("COUNT" not in r or n < int(r["COUNT"]))
    if r.get("FREQ") == "WEEKLY":
        byday = [_DIAS[d[-2:]] for d in r.get("BYDAY", "").split(",") if d[-2:] in _DIAS] or [ini.weekday()]
        semanas = (hoy - (ini.date() - timedelta(days=ini.weekday()))).days // 7
        return hoy.weekday() in byday and semanas % paso == 0 and ("COUNT" not in r or semanas * len(byday) < int(r["COUNT"]))
    return False


def eventos_de_hoy(ics: str, hoy: date, tz: ZoneInfo) -> list[dict]:
    out, ev = [], None
    for l in _desplegar(ics):
        if l == "BEGIN:VEVENT":
            ev = {"exdate": []}
        elif l == "END:VEVENT" and ev is not None:
            if "DTSTART" in ev and ev.get("STATUS") != "CANCELLED":
                ini, todo = ev["DTSTART"]
                fin = ev.get("DTEND", (ini + timedelta(hours=1), todo))[0]
                if "RRULE" in ev:
                    if _ocurre_hoy(ini, ev["RRULE"], hoy, tz) and not any(x.date() == hoy for x in ev["exdate"]):
                        dur = fin - ini
                        ini = datetime.combine(hoy, ini.timetz())
                        fin = ini + dur
                    else:
                        ini = None
                if ini is not None and (ini.date() == hoy or (todo and ini.date() <= hoy < fin.date())):
                    out.append({"ini": ini, "fin": fin, "todo": todo, "t": ev.get("SUMMARY", "(sin título)"),
                                "uid": ev.get("UID", "") + ini.isoformat()})
            ev = None
        elif ev is not None and ":" in l:
            k, v = l.split(":", 1)
            nombre, _, params = k.partition(";")
            if nombre in ("DTSTART", "DTEND"):
                try:
                    ev[nombre] = _fecha(v, params, tz)
                except ValueError:
                    pass
            elif nombre == "EXDATE":
                for x in v.split(","):
                    try:
                        ev["exdate"].append(_fecha(x, params, tz)[0])
                    except ValueError:
                        pass
            elif nombre in ("SUMMARY", "RRULE", "STATUS", "UID", "RECURRENCE-ID"):
                ev[nombre] = v.replace("\\,", ",").replace("\\;", ";").replace("\\n", " ")
    vistos, unicos = set(), []
    for e in sorted(out, key=lambda e: (not e["todo"], e["ini"])):
        if e["uid"] not in vistos:
            vistos.add(e["uid"])
            unicos.append(e)
    return unicos


async def _agenda(c: httpx.AsyncClient) -> dict:
    urls = [u.strip() for u in os.environ.get("AGENDA_ICS_URL", "").split(",") if u.strip()]
    if not urls:
        return {"ok": False, "why": "falta AGENDA_ICS_URL", "items": []}
    tz = cfg.tz
    hoy = datetime.now(tz).date()
    eventos = []
    for u in urls:
        r = await c.get(u, timeout=15, follow_redirects=True)
        r.raise_for_status()
        eventos += eventos_de_hoy(r.text, hoy, tz)
    eventos.sort(key=lambda e: (not e["todo"], e["ini"]))
    return {"ok": True, "items": eventos}


def _agenda_para_placa(a: dict) -> dict:
    """Lo que manda: hora como texto y si es ahora; quita lo ya terminado salvo que no quede nada."""
    ahora = datetime.now(cfg.tz)
    items = []
    for e in a["items"]:
        items.append({"h": "todo el día" if e["todo"] else e["ini"].strftime("%H:%M"),
                      "fin": "" if e["todo"] else e["fin"].strftime("%H:%M"), "t": e["t"][:60],
                      "now": (e["ini"] <= ahora < e["fin"]) and not e["todo"], "done": e["fin"] <= ahora and not e["todo"]})
    pend = [i for i in items if not i["done"]]
    return {"ok": a["ok"], "why": a.get("why", ""), "items": (pend or items[-2:])[:6], "total": len(items),
            "left": len(pend)}


# --- redes -----------------------------------------------------------------------------------
async def _youtube(c: httpx.AsyncClient) -> dict | None:
    key = os.environ.get("YT_API_KEY")
    canal = os.environ.get("YT_CHANNEL_ID", "")       # UC… de tu canal
    if not key or not canal:
        return None
    api = "https://www.googleapis.com/youtube/v3/"
    r = await c.get(api + "channels", params={"part": "statistics", "id": canal, "key": key}, timeout=10)
    r.raise_for_status()
    st = r.json()["items"][0]["statistics"]
    out = {"subs": int(st.get("subscriberCount", 0)), "views": int(st.get("viewCount", 0)),
           "videos": int(st.get("videoCount", 0))}
    r = await c.get(api + "playlistItems", params={"part": "contentDetails", "playlistId": "UU" + canal[2:],
                                                   "maxResults": 1, "key": key}, timeout=10)
    if r.status_code == 200 and r.json().get("items"):
        vid = r.json()["items"][0]["contentDetails"]["videoId"]
        r = await c.get(api + "videos", params={"part": "snippet,statistics", "id": vid, "key": key}, timeout=10)
        if r.status_code == 200 and r.json().get("items"):
            v = r.json()["items"][0]
            out["last"] = {"t": v["snippet"]["title"][:70], "views": int(v["statistics"].get("viewCount", 0))}
    return out


async def _instagram(c: httpx.AsyncClient) -> dict | None:
    tok, cuenta = os.environ.get("INSTAGRAM_ACCESS_TOKEN"), os.environ.get("INSTAGRAM_BUSINESS_ACCOUNT_ID")
    if not tok or not cuenta:
        return None
    r = await c.get(f"https://graph.facebook.com/v21.0/{cuenta}",
                    params={"fields": "followers_count,media_count", "access_token": tok}, timeout=10)
    r.raise_for_status()
    d = r.json()
    return {"followers": int(d.get("followers_count", 0)), "posts": int(d.get("media_count", 0))}


class _DelDia:
    """Primer valor visto cada día, para el «hoy +N»."""
    def __init__(self):
        self.dia, self.base = None, {}

    def delta(self, k: str, v: int) -> int:
        hoy = datetime.now(cfg.tz).date()
        if hoy != self.dia:
            self.dia, self.base = hoy, {}
        self.base.setdefault(k, v)
        return v - self.base[k]


# --- bucle -----------------------------------------------------------------------------------
async def vigilar(hub, cada_s: float = CADA_S):
    cpu, deldia = _Cpu(), _DelDia()
    cpu.uso()
    agenda, t_agenda = {"ok": False, "why": "leyendo…", "items": []}, 0.0
    redes, t_redes = {}, 0.0
    async with httpx.AsyncClient() as c:
        while True:
            try:
                ahora = time.monotonic()
                if ahora - t_agenda > AGENDA_S or (not agenda["ok"] and ahora - t_agenda > 60):
                    t_agenda = ahora
                    try:
                        agenda = await _agenda(c)
                    except Exception as e:
                        log.warning("agenda: %s", e)
                        agenda = {"ok": False, "why": "no se pudo leer", "items": agenda.get("items", [])}
                if ahora - t_redes > REDES_S or not t_redes:
                    t_redes = ahora
                    for nombre, f in (("yt", _youtube), ("ig", _instagram)):
                        try:
                            v = await f(c)
                        except Exception as e:
                            log.warning("redes %s: %s", nombre, e)
                            continue
                        if v is None:
                            redes.pop(nombre, None)
                            continue
                        k = "subs" if nombre == "yt" else "followers"
                        v["d"] = deldia.delta(nombre, v[k])
                        redes[nombre] = v
                usada, total = _ram()
                svc = await asyncio.gather(*(_vivo(c, u) for _, u in _servicios()))
                await hub.emitir({
                    "type": "dash",
                    "sys": {"cpu": cpu.uso(), "gpu": _gpu(), "ram": round(100 * usada / total, 1) if total else 0,
                            "ram_gb": usada, "ram_total": total, "temp": _temp(), "up": _uptime(), "host": os.environ.get("HOMELAB_NAME", socket.gethostname())},
                    "svc": [[n, ok] for (n, _), ok in zip(_servicios(), svc)],
                    "agenda": _agenda_para_placa(agenda),
                    "redes": redes,
                })
            except Exception as e:
                log.warning("tablero: %s", e)
            await asyncio.sleep(cada_s)
