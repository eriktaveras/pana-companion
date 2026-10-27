"""Carpeta de PNG → avatar del firmware (firmware/src/character/sprites43.h + sprites_data43.cpp).

Entrada: una carpeta con un PNG por cuadro, de 120x160 con transparencia, llamados
<estado>-NNN.png (por ejemplo idle-000.png, idle-001.png…). Los 16 estados que entiende el
servidor están en ESTADOS; si falta alguno, se usa el de idle. Opcional: avatar.json con, por
estado, {"fps": 12, "loop": true, "label": "Listo"} para cambiar ritmo o rótulo.

Todos los cuadros comparten una paleta de 255 colores (el índice 0 es el fondo, lo transparente)
y van comprimidos con RLE. La placa amplía el rectángulo que ocupa el personaje (el mismo en todos
los cuadros, así no baila) ×2,5 con vecino más cercano: es pixel art.

Uso: python tools/make_avatar.py <carpeta>          (necesita numpy y pillow)
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

RAIZ = Path(__file__).resolve().parent.parent
W, H = 120, 160
BG = (0x05, 0x08, 0x0C)                    # el fondo de la interfaz
ESTADOS = ["idle", "listening", "thinking", "talking", "working", "success", "wave", "confused",
           "reading", "typing", "searching", "executing", "approval", "error", "reconnecting", "sleeping"]
ROTULO = {"idle": "Listo", "listening": "Te escucho", "thinking": "Pensando", "talking": "Hablando",
          "working": "Trabajando", "success": "Hecho", "wave": "Hola", "confused": "Tengo una duda",
          "reading": "Leyendo", "typing": "Escribiendo", "searching": "Buscando", "executing": "Ejecutando",
          "approval": "¿Me autorizas?", "error": "Revisar error", "reconnecting": "Conectando",
          "sleeping": "Descansando"}
# iconos de 8x8 del cartel de estado (una fila por byte, el bit alto a la izquierda)
ICONOS = {"idle": [16, 84, 56, 254, 56, 84, 16, 0], "thinking": [60, 66, 129, 129, 66, 60, 8, 16],
          "talking": [16, 50, 245, 245, 245, 50, 16, 0], "success": [1, 3, 6, 140, 216, 112, 32, 0],
          "wave": [40, 170, 170, 170, 254, 124, 56, 56], "listening": [60, 66, 153, 133, 137, 17, 34, 28],
          "working": [36, 126, 219, 102, 102, 219, 126, 36], "confused": [60, 66, 2, 12, 16, 16, 0, 16],
          "reading": [231, 153, 153, 187, 153, 187, 153, 255], "typing": [0, 255, 129, 213, 171, 213, 153, 255],
          "searching": [56, 68, 130, 130, 68, 60, 6, 3], "executing": [64, 96, 112, 120, 112, 96, 64, 0],
          "approval": [60, 66, 2, 12, 16, 16, 0, 16], "error": [24, 60, 36, 102, 66, 219, 129, 255],
          "reconnecting": [126, 129, 60, 66, 24, 36, 0, 24], "sleeping": [254, 4, 8, 16, 32, 64, 254, 0]}


def encode(f: np.ndarray) -> bytes:
    """RLE: c&128 → (c&127)+1 copias del byte siguiente; si no, (c&127)+1 bytes literales."""
    b, out, i, n = f.tobytes(), bytearray(), 0, f.size
    while i < n:
        j = i
        while j < n and b[j] == b[i] and j - i < 128:
            j += 1
        if j - i >= 2:
            out += bytes([0x80 | (j - i - 1), b[i]])
            i = j
            continue
        k = i
        while k < n and k - i < 128 and not (k + 1 < n and b[k] == b[k + 1]):
            k += 1
        k = max(k, i + 1)
        out += bytes([k - i - 1]) + b[i:k]
        i = k
    return bytes(out)


def rgb565(c) -> int:
    r, g, b = (int(v) for v in c[:3])       # int: en uint8 de numpy los desplazamientos se desbordan
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    src = Path(sys.argv[1]).expanduser()
    meta = json.loads((src / "avatar.json").read_text()) if (src / "avatar.json").exists() else {}
    por_estado = {}
    for e in ESTADOS:
        fs = sorted(src.glob(f"{e}-*.png"))
        if fs:
            por_estado[e] = fs
    if "idle" not in por_estado:
        sys.exit(f"faltan los cuadros de idle en {src} (idle-000.png…)")
    rgba = {e: [np.asarray(Image.open(f).convert("RGBA")) for f in fs] for e, fs in por_estado.items()}
    for e, fr in rgba.items():
        for a in fr:
            assert a.shape == (H, W, 4), f"{e}: los cuadros deben ser de {W}x{H}"

    # una paleta común de 255 colores para todo lo opaco
    opacos = np.concatenate([a[a[..., 3] >= 128][:, :3] for fr in rgba.values() for a in fr])
    tira = Image.fromarray(opacos.reshape(1, -1, 3).astype(np.uint8))
    q = tira.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:255 * 3], np.uint8).reshape(-1, 3)
    pal = np.vstack([np.array([BG], np.uint8), pal])          # 0 = fondo

    def indices(a: np.ndarray) -> np.ndarray:
        rgb = a[..., :3].reshape(-1, 1, 3).astype(np.int32)
        d = ((rgb - pal[None, 1:, :].astype(np.int32)) ** 2).sum(-1)
        idx = (d.argmin(1) + 1).astype(np.uint8).reshape(H, W)
        idx[a[..., 3] < 128] = 0
        return idx

    cuadros, anims = [], []
    for e in ESTADOS:
        fr = rgba.get(e, rgba["idle"])
        m = meta.get(e, {})
        anims.append({"name": e, "label": m.get("label", ROTULO[e]), "first": len(cuadros), "count": len(fr),
                      "fps": int(m.get("fps", 12)), "loop": bool(m.get("loop", e != "wave")),
                      "icon": m.get("icon", ICONOS[e])})
        cuadros += [indices(a) for a in fr]
        if e not in rgba:
            print(f"{e}: sin cuadros, usa los de idle")

    datos, offs = bytearray(), [0]
    for f in cuadros:
        datos += encode(f)
        offs.append(len(datos))
    x0 = y0 = 10 ** 9
    x1 = y1 = -1
    for f in cuadros:
        m = np.argwhere(f != 0)
        if len(m):
            y0, y1 = min(y0, m[:, 0].min()), max(y1, m[:, 0].max())
            x0, x1 = min(x0, m[:, 1].min()), max(x1, m[:, 1].max())
    cw, ch = x1 - x0 + 1, y1 - y0 + 1

    out = RAIZ / "firmware" / "src" / "character"
    with open(out / "sprites_data43.cpp", "w") as f:
        f.write("// Generado por tools/make_avatar.py — no editar a mano\n#ifdef PACK_AVATAR\n#include <stdint.h>\n")
        f.write(f"extern const uint8_t PACK_RLE[{len(datos)}] = {{\n")
        for i in range(0, len(datos), 32):
            f.write(",".join(str(v) for v in datos[i:i + 32]) + ",\n")
        f.write("};\n")
        f.write(f"extern const uint32_t PACK_OFFS[{len(offs)}] = {{\n")
        for i in range(0, len(offs), 12):
            f.write(",".join(str(v) for v in offs[i:i + 12]) + ",\n")
        f.write("};\n")
        f.write("extern const uint16_t PACK_PAL[256] = {\n")
        f.write(",".join(f"0x{rgb565(c):04x}" for c in pal) + ",\n};\n#endif\n")
    with open(out / "sprites43.h", "w") as f:
        f.write(f"""// Generado por tools/make_avatar.py — no editar a mano
#pragma once
#include <stdint.h>

#define PACK_W {W}
#define PACK_H {H}
#define PACK_X0 {x0}           // rectángulo que ocupa el personaje en el cuadro
#define PACK_Y0 {y0}
#define PACK_CW {cw}
#define PACK_CH {ch}

extern const uint8_t PACK_RLE[];      // RLE: c&128 → (c&127)+1 copias; si no, (c&127)+1 literales
extern const uint32_t PACK_OFFS[];    // cuadro → posición en PACK_RLE (+1 al final)
#define PACK_COLORS 256
extern const uint16_t PACK_PAL[];     // paleta de 256 en RGB565 nativo; el 0 es el fondo

struct PackAnim {{ const char *name, *label; uint16_t first, count; uint8_t fps; bool loop; uint8_t pal; uint8_t icon[8]; }};
enum SpriteState : uint8_t {{ {", ".join("S_" + e.upper() for e in ESTADOS)}, S_COUNT }};
static const PackAnim PACK_ANIMS[] = {{
""")
        for a in anims:
            f.write(f'    {{"{a["name"]}", "{a["label"]}", {a["first"]}, {a["count"]}, {a["fps"]}, '
                    f'{"true" if a["loop"] else "false"}, 0, {{{", ".join(str(v) for v in a["icon"])}}}}},\n')
        f.write("};\n")
    print(f"{len(cuadros)} cuadros, {len(datos) // 1024} KB · personaje en x {x0}..{x1}, y {y0}..{y1} "
          f"({cw}x{ch} → {cw * 5 // 2}x{ch * 5 // 2} en pantalla)")


if __name__ == "__main__":
    main()
