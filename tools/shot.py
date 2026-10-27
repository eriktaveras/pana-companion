# Lee el log serie unos segundos, pide la captura ("s") y la guarda como PNG.
import sys, time, serial
import subprocess
port, out, wait = sys.argv[1], sys.argv[2], float(sys.argv[3])
cmds = sys.argv[4] if len(sys.argv) > 4 else ""
s = serial.Serial(); s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False                 # en la 4.3" (CH340) DTR/RTS dejan la placa en reset
s.open()
t0 = time.time(); buf = b""
# los comandos van 3 s antes de capturar: abrir el puerto reinicia la 4.3" y se perderían
enviados = False
while time.time() - t0 < wait:
    buf += s.read(4096)
    if cmds and not enviados and time.time() - t0 > wait - 3:
        for c in cmds: s.write(c.encode()); time.sleep(0.3)
        enviados = True
print(buf.decode("utf-8", "replace"))
s.reset_input_buffer(); s.write(b"s")
data = b""; t0 = time.time()
while b"SHOT" not in data and time.time() - t0 < 5: data += s.read(4096)
hdr_end = data.index(b"\n", data.index(b"SHOT"))
w, h = map(int, data[data.index(b"SHOT"):hdr_end].split()[1:3])
raw = data[hdr_end + 1:]
while len(raw) < w * h * 2 and time.time() - t0 < 20 + w * h * 2 / 10000: raw += s.read(65536)   # 115200 baudios ≈ 11 KB/s
raw = raw[: w * h * 2]
rgb = bytearray()
for i in range(w * h):
    v = (raw[2 * i] << 8) | raw[2 * i + 1]
    rgb += bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
ppm = out + ".ppm"
open(ppm, "wb").write(b"P6 %d %d 255\n" % (w, h) + rgb)
subprocess.run(["sips", "-s", "format", "png", ppm, "--out", out], capture_output=True)
print("captura:", out)
