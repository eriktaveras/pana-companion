"""Genera fuentes VLW antialiasadas (las que carga LovyanGFX con loadFont) desde un TTF.

    work/venv/bin/python tools/vlw.py FUENTE.ttf TAMAÑO PESO CARACTERES NOMBRE

Escribe include/fonts/NOMBRE.h con `static const uint8_t NOMBRE[]`. Formato VLW
(big endian): cabecera de 6 int32 (glifos, versión 11, tamaño, 0, ascent, descent),
7 int32 por glifo (unicode, alto, ancho, avance, arriba, izquierda, 0) y luego
los bitmaps de alfa de 8 bits en el mismo orden.
"""
import struct
import sys
from pathlib import Path

import freetype

LATIN = "".join(chr(c) for c in range(0x20, 0x7F)) + "".join(chr(c) for c in range(0xA1, 0x100))


def main():
    ttf, size, weight, chars, name = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    if chars.startswith("latin"):          # «latin…–» = latin más esos caracteres
        chars = LATIN + chars[5:]
    face = freetype.Face(ttf)
    try:
        axes = {a.tag: a for a in face.get_variation_info().axes}
        coords = []
        for tag, a in axes.items():
            coords.append(weight if tag == "wght" else (min(max(size, a.minimum), a.maximum) if tag == "opsz" else a.default))
        face.set_var_design_coords(coords)
    except Exception:
        pass
    face.set_pixel_sizes(0, size)

    glyphs, bitmaps = [], []
    for ch in sorted(set(chars), key=ord):
        if face.get_char_index(ch) == 0 and ch != " ":
            continue
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        g = face.glyph
        bm = g.bitmap
        rows, width, pitch = bm.rows, bm.width, bm.pitch
        data = bytes(bm.buffer)
        alpha = b"".join(data[r * pitch:r * pitch + width] for r in range(rows))
        glyphs.append((ord(ch), rows, width, g.advance.x >> 6, g.bitmap_top, g.bitmap_left))
        bitmaps.append(alpha)

    ascent = face.size.ascender >> 6
    descent = -(face.size.descender >> 6)
    out = struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent)
    for u, h, w, adv, top, left in glyphs:
        out += struct.pack(">7i", u, h, w, adv, top, left, 0)
    out += b"".join(bitmaps)

    hex_rows = ",\n".join(", ".join(f"0x{b:02x}" for b in out[i:i + 24]) for i in range(0, len(out), 24))
    dest = Path(__file__).resolve().parent.parent / "firmware" / "include" / "fonts" / f"{name}.h"
    dest.write_text(f"#pragma once\n// {Path(ttf).name} {size}px peso {weight}, {len(glyphs)} glifos (tools/vlw.py)\n"
                    f"#include <stdint.h>\nstatic const uint8_t {name}[] = {{\n{hex_rows}\n}};\n")
    print(f"{dest.name}: {len(glyphs)} glifos, {len(out)} bytes")


if __name__ == "__main__":
    main()
