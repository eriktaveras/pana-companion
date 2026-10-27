# Protocolo Companion v1

Fuente de verdad del contrato entre el cerebro (servidor en tu máquina) y los cuerpos
(las placas y el simulador web). Si algo cambia aquí, cambia en
`server/protocol.py`, `firmware/src/net/` y `server/sim/app.js`.

- Transporte: WebSocket en `ws://<servidor>:8770/ws`.
- Mensajes de texto: JSON con campo `type`.
- Audio: frames **binarios** PCM 16-bit little-endian, mono, 16 kHz, de 1024 muestras
  (2048 bytes) como máximo. Para cambiar a Opus más adelante se añade `codec` a
  `say`/`audio_start` y nada más.
- Varios cuerpos pueden estar conectados a la vez (placa + simulador): el servidor
  emite a todos y acepta toques y respuestas de cualquiera.

## Servidor → cuerpo

| type | campos | qué hace |
|---|---|---|
| `state` | `emotion`, `animation?`, `loop?`, `look?` | Cambia la cara. Emociones: `idle happy thinking talking alert sad bored sleeping excited love confused` (love = ojos de corazón, confused = espiral; sin conexión el cuerpo pone ojos en X por su cuenta). `animation` admite `nod`, `shake`, `bounce`, `look_left`, `look_right` (un gesto puntual). `look: [x, y]` en −1..1 hace que mire ahí 4 s (x: izquierda→derecha, y: arriba→abajo); los ojos van con resortes y rebotan un poco. |
| `say` | `text`, `emotion`, `audio` (bool), `id` | Burbuja de texto. Si `audio` es true, a continuación llegan frames binarios y luego `audio_end` con el mismo `id`. La boca se mueve con el RMS de lo que suena. |
| `audio_end` | `id` | Fin del audio de un `say`. |
| `card` | `id`, `kind` (`lead noshow brief system`), `title`, `lines[]`, `actions[]`, `icon?` (`whatsapp instagram facebook`) | Tarjeta en la mitad de abajo. Cada acción es `{id, label}`; al tocarla el cuerpo manda `touch` con `target = "card:<card_id>:<action_id>"`. |
| `clear` | `what` (`card bubble all`) | Quita la tarjeta, la burbuja o ambas. |
| `confirm` | `id`, `question`, `preview`, `options[]`, `expires_in` | Diálogo de confirmación a pantalla completa. **Obligatorio antes de toda acción externa.** Por defecto `options = ["Enviar","Editar","Cancelar"]`. Pasado `expires_in` segundos el cuerpo lo cierra solo (el servidor ya lo dio por cancelado). |
| `confirm_close` | `id`, `result` | El servidor cierra un confirm (lo respondió otro cuerpo, caducó o ya se ejecutó). |
| `mood` | `energia`, `curiosidad`, `aburrimiento`, `afecto` | Solo informativo (el simulador lo pinta; la placa lo ignora). |
| `dash` | `sys{cpu, gpu, ram, ram_gb, ram_total, temp, up, host}`, `svc[[nombre, 0/1]…]`, `agenda{ok, why, items[{h, fin, t, now}], left, total}`, `redes{yt{subs, views, d, last{t, views}}, ig{followers, posts, d}}` | Tablero de la P4 cada 5 s (`server/brain/tablero.py`): homelab, servicios, agenda de hoy y redes. `d` = variación desde la primera lectura del día. Lo que no está configurado va con `ok: false` o no viene. |
| `ota` | `url`, `version` | La placa baja el firmware de `url` por HTTP y se reinicia. |
| `ping` | `t` | El cuerpo responde `pong` con el mismo `t`. |

## Cuerpo → servidor

| type | campos | qué hace |
|---|---|---|
| `hello` | `device_id`, `fw_version`, `screen: [240,320]`, `has_mic`, `has_speaker`, `kind` (`board`/`sim`) | Primer mensaje tras conectar. El servidor contesta con el `state` actual. |
| `touch` | `target`, `x`, `y`, `gesture` (`tap long_press swipe_left swipe_right swipe_up swipe_down`) | `target`: `face`, `bubble`, `card:<id>:<action>`, `screen`. |
| `confirm_reply` | `id`, `choice` | Una de las `options` del confirm. |
| `audio_start` | `id`, `sample_rate` (16000) | Empieza el micro (push-to-talk: mantener pulsada la cara). Después, frames binarios. |
| `audio_end` | `id` | Se soltó el botón. |
| `pong` | `t` | Respuesta a `ping`. |
| `status` | `rssi`, `heap`, `psram`, `uptime`, `fw_version` | Cada 30 s. |

## Reglas

1. El cuerpo no decide nada: pinta, anima, captura toques y audio.
2. Un `confirm` nunca se ejecuta por defecto: caducar = cancelar.
3. Un frame binario que llega sin un `say` con `audio: true` abierto se descarta.
4. El servidor ignora frames de micro fuera de un `audio_start`/`audio_end`.
