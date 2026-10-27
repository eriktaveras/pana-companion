"""Dibuja el avatar de ejemplo («Pana», un robot) en pixel art: 16 estados en PNG de 120x160, listos
para tools/make_avatar.py. Sirve de plantilla: cambia colores o formas aquí, o sustituye la carpeta
por tus propios dibujos con los mismos nombres (<estado>-NNN.png).

Uso: python tools/demo_avatar.py avatar/pana && python tools/make_avatar.py avatar/pana
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 120, 160
OUT = (36, 46, 60)          # contorno
BODY = (200, 214, 228)      # carcasa
SHADE = (150, 168, 188)     # sombra de la carcasa
VISOR = (14, 26, 36)        # pantalla de la cara
EYE = (80, 228, 210)        # ojos (turquesa)
ACCENT = {"idle": EYE, "listening": (255, 90, 120), "thinking": (168, 139, 250), "talking": EYE,
          "working": (255, 176, 32), "success": (92, 240, 138), "wave": EYE, "confused": (255, 215, 128),
          "reading": (122, 168, 255), "typing": (122, 168, 255), "searching": (255, 176, 32),
          "executing": (92, 240, 138), "approval": (255, 215, 128), "error": (255, 106, 106),
          "reconnecting": (255, 176, 32), "sleeping": (120, 130, 150)}
FRAMES = {"idle": 24, "listening": 16, "thinking": 20, "talking": 16, "working": 16, "success": 16,
          "wave": 18, "confused": 16, "reading": 20, "typing": 12, "searching": 20, "executing": 16,
          "approval": 16, "error": 12, "reconnecting": 16, "sleeping": 24}


F = 0.82                    # escala del dibujo alrededor de los pies (cabe en ~98x120, lo que usa la UI)


class Escalado:
    """ImageDraw con las coordenadas escaladas F alrededor de (60, 150): se dibuja en «unidades» de
    120x160 y el robot sale más pequeño, con los pies en el mismo sitio."""
    def __init__(self, d):
        self.d = d

    def _p(self, v):
        if isinstance(v, (list, tuple)) and v and isinstance(v[0], (list, tuple)):
            return [self._p(x) for x in v]
        if isinstance(v, (list, tuple)) and len(v) == 4 and not isinstance(v[0], (list, tuple)):
            return [round(60 + (v[0] - 60) * F), round(150 + (v[1] - 150) * F),
                    round(60 + (v[2] - 60) * F), round(150 + (v[3] - 150) * F)]
        return (round(60 + (v[0] - 60) * F), round(150 + (v[1] - 150) * F))

    def __getattr__(self, name):
        f = getattr(self.d, name)

        def g(xy, *a, **kw):
            if "width" in kw and kw["width"] > 1:
                kw["width"] = max(1, round(kw["width"] * F))
            if name == "rounded_rectangle" and a:
                a = (max(1, round(a[0] * F)),) + a[1:]
            return f(self._p(xy), *a, **kw)
        return g


def rr(d, box, r, fill, outline=OUT):
    d.rounded_rectangle(box, r, fill=fill, outline=outline, width=2)


def eyes(d, st, k, n, dy):
    lx, rx, ey = 47, 67, 49 + dy
    col = ACCENT["error"] if st == "error" else EYE
    blink = st in ("idle", "listening", "talking", "wave") and k % 12 == 11
    if st == "sleeping":
        for x in (lx, rx):
            d.line([(x - 1, ey + 5), (x + 7, ey + 5)], fill=(90, 110, 130), width=2)
    elif st == "error":
        for x in (lx, rx):
            d.line([(x, ey), (x + 6, ey + 8)], fill=col, width=2)
            d.line([(x + 6, ey), (x, ey + 8)], fill=col, width=2)
    elif st in ("success", "wave"):
        for x in (lx, rx):                                  # ojos felices ^ ^
            d.line([(x - 1, ey + 6), (x + 3, ey + 1), (x + 7, ey + 6)], fill=col, width=2)
    elif blink:
        for x in (lx, rx):
            d.rectangle([x, ey + 4, x + 6, ey + 5], fill=col)
    else:
        ox = {"thinking": 2, "searching": round(3 * math.sin(k / n * 2 * math.pi)), "reading": 0}.get(st, 0)
        oy = {"thinking": -3, "reading": 3, "typing": 3}.get(st, 0)
        h = 6 if st == "confused" and k % 8 < 4 else 9
        for i, x in enumerate((lx, rx)):
            hh = h if not (st == "confused" and i == 1) else 6
            d.rounded_rectangle([x + ox, ey + oy + (9 - hh), x + 6 + ox, ey + oy + 9], 2, fill=col)
            d.point((x + 1 + ox, ey + oy + (9 - hh) + 1), fill=(220, 255, 250))


def mouth(d, st, k, dy):
    y = 63 + dy
    if st == "talking":
        h = [1, 4, 2, 5, 3, 1, 4, 2][k % 8]
        d.rounded_rectangle([55, y - h, 65, y + h], 2, fill=EYE)
    elif st in ("success", "wave"):
        d.arc([53, y - 6, 67, y + 3], 20, 160, fill=EYE, width=2)
    elif st == "error" or st == "confused":
        d.line([(55, y), (58, y - 1), (62, y + 1), (65, y)], fill=ACCENT[st], width=2)
    elif st != "sleeping":
        d.line([(56, y), (64, y)], fill=(60, 140, 135), width=2)


def arm(d, x0, y0, x1, y1):
    d.line([(x0, y0), (x1, y1)], fill=OUT, width=9)
    d.line([(x0, y0), (x1, y1)], fill=BODY, width=5)
    d.ellipse([x1 - 5, y1 - 5, x1 + 5, y1 + 5], fill=BODY, outline=OUT, width=2)


def frame(st, k, n):
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = Escalado(ImageDraw.Draw(im))
    acc = ACCENT[st]
    t = k / n
    bob = 0 if st in ("error", "sleeping") else round(1.2 * math.sin(t * 2 * math.pi))
    if st == "success":
        bob = -abs(round(3 * math.sin(t * 4 * math.pi)))
    dy = bob
    # piernas y pies (no se mueven: así no baila)
    for x in (44, 66):
        rr(d, [x, 126, x + 10, 146], 3, SHADE)
        rr(d, [x - 4, 142, x + 14, 150], 3, BODY)
    # brazos según el estado (detrás del cuerpo los que bajan)
    ls, rs = (36, 94 + dy), (84, 94 + dy)
    wav = math.sin(t * 2 * math.pi * 2)
    if st == "success":
        arm(d, *ls, 24, 66 + dy); arm(d, *rs, 96, 66 + dy)
    elif st == "wave":
        arm(d, *ls, 30, 122 + dy); arm(d, *rs, 100 + round(6 * wav), 66 + dy)
    elif st == "approval":
        arm(d, *ls, 30, 122 + dy); arm(d, *rs, 98, 70 + dy)
    elif st in ("reading", "typing", "searching", "executing"):
        arm(d, *ls, 44, 116 + dy); arm(d, *rs, 76, 116 + dy)
    elif st == "thinking":
        arm(d, *ls, 30, 122 + dy); arm(d, *rs, 70, 76 + dy)
    else:
        sw = round(2 * math.sin(t * 2 * math.pi)) if st == "idle" else 0
        arm(d, *ls, 29 - sw, 122 + dy); arm(d, *rs, 91 + sw, 122 + dy)
    # cuerpo y pecho con su luz
    rr(d, [36, 84 + dy, 84, 128 + dy], 10, BODY)
    d.rectangle([76, 90 + dy, 82, 124 + dy], fill=SHADE)
    rr(d, [49, 96 + dy, 71, 114 + dy], 4, VISOR)
    lit = 0.5 + 0.5 * math.sin(t * 2 * math.pi * (2 if st in ("working", "executing") else 1))
    d.rounded_rectangle([54, 102 + dy, 66, 108 + dy], 2, fill=tuple(int(c * (0.45 + 0.55 * lit)) for c in acc))
    # cuello, cabeza, orejas y antena
    rr(d, [54, 76 + dy, 66, 86 + dy], 2, SHADE)
    for x in (23, 91):
        glow = st == "listening" and (k // 2) % 2 == 0
        rr(d, [x, 44 + dy, x + 6, 62 + dy], 2, acc if glow else SHADE)
    rr(d, [28, 26 + dy, 92, 80 + dy], 15, BODY)
    d.rectangle([82, 34 + dy, 88, 72 + dy], fill=SHADE)
    rr(d, [36, 37 + dy, 84, 71 + dy], 10, VISOR)
    d.line([(60, 26 + dy), (60, 14 + dy)], fill=OUT, width=3)
    pulse = 3 + (1 if (st in ("listening", "working", "reconnecting") and (k // 3) % 2) else 0)
    d.ellipse([60 - pulse, 10 + dy - pulse, 60 + pulse, 10 + dy + pulse], fill=acc, outline=OUT, width=1)
    eyes(d, st, k, n, dy)
    mouth(d, st, k, dy)

    # accesorios
    if st == "thinking":
        for i in range(3):
            if (k // 4) % 4 > i:
                d.ellipse([92 + i * 7, 20 - i * 4, 96 + i * 7, 24 - i * 4], fill=acc)
    elif st == "reading":
        rr(d, [38, 108 + dy, 82, 124 + dy], 2, (240, 236, 220))
        d.line([(60, 108 + dy), (60, 124 + dy)], fill=OUT, width=1)
        for i in range(3):
            d.line([(43, 112 + i * 4 + dy), (56, 112 + i * 4 + dy)], fill=(150, 160, 180))
            d.line([(64, 112 + i * 4 + dy), (77, 112 + i * 4 + dy)], fill=(150, 160, 180))
    elif st == "typing":
        rr(d, [34, 114 + dy, 86, 126 + dy], 2, (70, 84, 100))
        for i in range(6):
            on = (k + i * 3) % 6 == 0
            d.rectangle([39 + i * 8, 118 + dy, 44 + i * 8, 121 + dy], fill=acc if on else (130, 145, 160))
    elif st == "searching":
        cx, cy = 74 + round(4 * math.sin(t * 2 * math.pi)), 104 + dy
        d.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], outline=OUT, width=3, fill=(180, 220, 255, 140))
        d.line([(cx + 6, cy + 6), (cx + 13, cy + 13)], fill=OUT, width=4)
    elif st == "executing":
        rr(d, [36, 106 + dy, 84, 126 + dy], 3, (10, 20, 16))
        d.text((40, 108 + dy), ">_" if (k // 4) % 2 else ">", fill=acc)
    elif st == "working":
        cx, cy, a0 = 101, 30, t * 360
        for i in range(8):
            a = math.radians(a0 + i * 45)
            d.line([(cx, cy), (cx + 9 * math.cos(a), cy + 9 * math.sin(a))], fill=acc, width=3)
        d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=acc, outline=OUT, width=1)
        d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=VISOR)
    elif st in ("confused", "approval"):
        x, y = 98, 22 + (1 if (k // 4) % 2 else 0)
        d.arc([x - 6, y - 8, x + 6, y + 4], 180, 90, fill=acc, width=3)
        d.line([(x, y + 4), (x, y + 8)], fill=acc, width=3)
        d.rectangle([x - 1, y + 12, x + 1, y + 14], fill=acc)
    elif st == "error":
        x = 100
        d.rectangle([x - 2, 14, x + 2, 28], fill=acc)
        d.rectangle([x - 2, 32, x + 2, 36], fill=acc)
    elif st == "reconnecting":
        for i in range(3):
            if (k // 3) % 4 > i:
                r = 5 + i * 5
                d.arc([100 - r, 34 - r, 100 + r, 34 + r], 225, 315, fill=acc, width=2)
        d.ellipse([98, 32, 102, 36], fill=acc)
    elif st == "sleeping":
        for i in range(3):
            p = (t + i / 3) % 1
            x, y = 92 + round(10 * p), 34 - round(26 * p)
            if p < 0.85:
                s = 3 + i
                d.line([(x, y), (x + s, y), (x, y + s), (x + s, y + s)], fill=(170, 180, 200), width=1)
    elif st == "success":
        for i in range(4):
            if (k + i * 2) % 8 < 4:
                x, y = [(16, 40), (104, 46), (12, 84), (108, 90)][i]
                d.line([(x - 3, y), (x + 3, y)], fill=acc, width=1)
                d.line([(x, y - 3), (x, y + 3)], fill=acc, width=1)
    elif st == "listening":
        for i in range(2):
            r = 6 + i * 5 + (k % 4)
            d.arc([14 - r, 53 - r, 14 + r, 53 + r], 120, 240, fill=acc, width=2)
            d.arc([106 - r, 53 - r, 106 + r, 53 + r], -60, 60, fill=acc, width=2)
    return im


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "avatar/pana")
    out.mkdir(parents=True, exist_ok=True)
    for st, n in FRAMES.items():
        for k in range(n):
            frame(st, k, n).save(out / f"{st}-{k:03d}.png")
    print(f"{sum(FRAMES.values())} cuadros en {out}")


if __name__ == "__main__":
    main()
