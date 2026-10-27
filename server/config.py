"""Configuración: todo sale de .env (raíz del repo) o del entorno. Sin secretos en el código."""
import os
from dataclasses import dataclass
from pathlib import Path
from zoneinfo import ZoneInfo

RAIZ = Path(__file__).resolve().parent.parent


def _cargar_env():
    f = RAIZ / ".env"
    if not f.exists():
        return
    for linea in f.read_text().splitlines():
        linea = linea.strip()
        if not linea or linea.startswith("#") or "=" not in linea:
            continue
        k, v = linea.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip())


_cargar_env()


def _e(k, defecto):
    return os.environ.get(k, defecto)


def _hora(s):
    h, m = s.split(":")
    return int(h) * 60 + int(m)


@dataclass
class Config:
    host: str = _e("COMPANION_HOST", "0.0.0.0")
    port: int = int(_e("COMPANION_PORT", "8770"))
    db: str = _e("COMPANION_DB", "data/companion.db")
    tz: ZoneInfo = ZoneInfo(_e("COMPANION_TZ", "UTC"))
    public_url: str = _e("COMPANION_PUBLIC_URL", "http://127.0.0.1:8770")

    llm_key_file: str = _e("LLM_API_KEY_FILE", "")
    fast_url: str = _e("LLM_FAST_URL", "http://127.0.0.1:8081/v1")
    fast_model: str = _e("LLM_FAST_MODEL", "local")
    chat_url: str = _e("LLM_CHAT_URL", "http://127.0.0.1:8081/v1")
    chat_model: str = _e("LLM_CHAT_MODEL", "local")
    big_url: str = _e("LLM_BIG_URL", "http://127.0.0.1:8081/v1")
    big_model: str = _e("LLM_BIG_MODEL", "local")
    embed_url: str = _e("EMBED_URL", "http://127.0.0.1:8084/v1")
    embed_model: str = _e("EMBED_MODEL", "qwen3-embedding-0.6b")

    obsidian_vault: str = _e("OBSIDIAN_VAULT", "")

    heartbeat_min: float = float(_e("HEARTBEAT_MINUTES", "3"))
    max_spontaneous_hour: int = int(_e("MAX_SPONTANEOUS_PER_HOUR", "3"))
    quiet_start: int = _hora(_e("QUIET_START", "22:30"))
    quiet_end: int = _hora(_e("QUIET_END", "07:30"))
    confirm_timeout_s: float = float(_e("CONFIRM_TIMEOUT_MINUTES", "5")) * 60
    dream_at: int = _hora(_e("DREAM_AT", "03:00"))
    brief_at: int = _hora(_e("BRIEF_AT", "07:45"))

    # Cerebro de la conversación: "hermes" (API server de Hermes, con herramientas y memoria)
    # o "local" (llama-server directo con la persona de Pana)
    brain: str = _e("COMPANION_BRAIN", "hermes")
    hermes_url: str = _e("HERMES_URL", "http://127.0.0.1:8642/v1")
    hermes_key_file: str = _e("HERMES_API_KEY_FILE", "~/.config/pana/hermes-api-key")
    hermes_session: str = _e("HERMES_SESSION", "pana")
    hermes_timeout_s: float = float(_e("HERMES_TIMEOUT_S", "180"))

    negocio_adapter: str = _e("NEGOCIO_ADAPTER", "mock")

    stt_url: str = _e("STT_URL", "http://127.0.0.1:8771/inference")
    tts_engine: str = _e("TTS_ENGINE", "piper")
    piper_model: str = _e("PIPER_MODEL", "voices/es_MX-claude-high.onnx")
    # TTS_ENGINE=http: un servicio propio de voz (POST {"texto": ...} → PCM 16 kHz mono); Piper de respaldo
    tts_http_url: str = _e("TTS_HTTP_URL", "http://127.0.0.1:5002/tts")

    # Quién es quién: el nombre de la persona a la que acompaña (lo usa la persona del LLM) y, si
    # quieres otro carácter, un archivo de texto con la persona entera ({nombre} y {dueno} se rellenan)
    name: str = _e("COMPANION_NAME", "Pana")
    owner: str = _e("COMPANION_OWNER", "")
    persona_file: str = _e("PERSONA_FILE", "")

    def llm_key(self) -> str:
        p = Path(os.path.expanduser(self.llm_key_file))
        return p.read_text().strip() if p.exists() else ""

    def en_silencio(self, minuto_del_dia: int) -> bool:
        """¿Está dentro del horario de no molestar? (el tramo puede cruzar medianoche)"""
        a, b = self.quiet_start, self.quiet_end
        return a <= minuto_del_dia < b if a < b else (minuto_del_dia >= a or minuto_del_dia < b)


cfg = Config()
