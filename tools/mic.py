# Graba 3 s del micrófono de la placa ('m' por serie) mientras el Mac pone un tono,
# y mide cuánto sobresale esa frecuencia. Uso: mic.py PUERTO salida.wav [hz]
import sys, time, serial, subprocess, wave, struct, math
port, out = sys.argv[1], sys.argv[2]
hz = float(sys.argv[3]) if len(sys.argv) > 3 else 1000
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.5); s.reset_input_buffer()
tono = None
if hz > 0:
    tono = subprocess.Popen(["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-f", "lavfi",
                             f"sine=frequency={hz}:duration=5"])
    time.sleep(0.8)
s.write(b"m")
d = b""; t0 = time.time()
while b"MIC " not in d or b"\n" not in d[d.index(b"MIC "):]:
    d += s.read(4096)
    if time.time() - t0 > 8: sys.exit("sin respuesta")
i = d.index(b"MIC "); j = d.index(b"\n", i)
n = int(d[i + 4:j]); raw = d[j + 1:]
while len(raw) < n and time.time() - t0 < 20: raw += s.read(65536)
raw = raw[:n]
if tono: tono.wait()
w = wave.open(out, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(raw); w.close()
x = struct.unpack("<%dh" % (len(raw) // 2), raw)
rms = math.sqrt(sum(v * v for v in x) / len(x))
def potencia(f):   # Goertzel
    k = 2 * math.cos(2 * math.pi * f / 16000); a = b = 0
    for v in x: a, b = v + k * a - b, a
    return math.sqrt(a * a + b * b - k * a * b) / len(x)
ref = sum(potencia(f) for f in (700, 850, 1150, 1300)) / 4
print(f"rms {rms:.0f}  pico {max(map(abs, x))}  {hz:.0f} Hz {potencia(hz) if hz else 0:.1f}  vecinas {ref:.1f}")
