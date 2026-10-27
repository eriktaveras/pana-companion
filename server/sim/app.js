// Simulador de la pantalla de 240x320. Se conecta al mismo /ws que la placa y pinta lo
// mismo que firmware/src/ui/ (misma composición, mismos tamaños, mismas fuentes).
import { Cara, FW, FH, rrect } from "./face.js";

const W = 240, H = 320, BAR = 16, PANEL_Y = BAR + FH, PANEL_H = H - PANEL_Y;
const COL_KIND = { lead: "#5CF08A", noshow: "#FFB020", brief: "#7AA8FF", system: "#FF6A6A" };
const F13 = "600 13px 'Hanken Grotesk'", F17 = "600 17px 'Hanken Grotesk'";

const lienzo = document.getElementById("pantalla");
const g = lienzo.getContext("2d");
const log = (...a) => { const el = document.getElementById("log"); el.textContent = a.join(" ") + "\n" + el.textContent.slice(0, 4000); };

let cara, ws, conectado = false;
const ui = { bubble: null, card: null, confirm: null, scroll: 0, grabando: false,
             revelado: 0, tCard: 0, tConfirm: 0 };

// Animaciones (mismas curvas que firmware/src/ui/ui.cpp)
const C1 = 1.70158;
const easeOutBack = (x) => { x = Math.min(1, Math.max(0, x)); return 1 + (C1 + 1) * (x - 1) ** 3 + C1 * (x - 1) ** 2; };
const LETRAS_SEG = 32;      // máquina de escribir de la burbuja

// --- WebSocket --------------------------------------------------------------------
function conectar() {
  ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    conectado = true; cara.desconectado = false;
    ui.bubble = ui.card = ui.confirm = null;          // el servidor repinta lo actual
    ws.send(JSON.stringify({ type: "hello", device_id: "sim-" + Math.random().toString(36).slice(2, 6), fw_version: "sim",
      screen: [W, H], has_mic: !!navigator.mediaDevices, has_speaker: true, kind: "sim" }));
    log("conectado");
  };
  ws.onclose = () => { conectado = false; cara.desconectado = true; setTimeout(conectar, 1500); };
  ws.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) return audio.frame(ev.data);
    const m = JSON.parse(ev.data);
    if (m.type !== "ping" && m.type !== "mood") log("←", ev.data.slice(0, 160));
    switch (m.type) {
      case "state": cara.set(m.emotion, m.animation); if (m.look) cara.lookAt(m.look[0], m.look[1]); break;
      case "say":
        if (!ui.bubble || ui.bubble.id !== m.id) { ui.scroll = 0; ui.revelado = 0; if (m.audio) audio.abrir(m.id); }
        ui.bubble = m; break;
      case "audio_end": audio.cerrar(m.id); break;
      case "card": if (!ui.card || ui.card.id !== m.id) ui.tCard = performance.now(); ui.card = m; break;
      case "clear":
        if (m.what !== "bubble") ui.card = null;
        if (m.what !== "card") ui.bubble = null; break;
      case "confirm": ui.confirm = { ...m, hasta: performance.now() + m.expires_in * 1000 }; ui.scroll = 0; ui.tConfirm = performance.now(); break;
      case "confirm_close": if (ui.confirm && ui.confirm.id === m.id) ui.confirm = null; break;
      case "mood": document.getElementById("mood").textContent =
        Object.entries(m).filter(([k]) => k !== "type").map(([k, v]) => `${k} ${v.toFixed(2)}`).join(" · "); break;
      case "ping": ws.send(JSON.stringify({ type: "pong", t: m.t })); break;
      case "ota": log("OTA pedido (la placa se actualizaría):", m.url); break;
    }
  };
}
const enviar = (o) => conectado && ws.send(JSON.stringify(o));

// --- audio: reproducción con lip-sync y micrófono -------------------------------------
const audio = {
  ctx: null, fin: 0, niveles: [], id: null,
  abrir(id) { this.ctx ??= new AudioContext(); this.id = id; this.fin = Math.max(this.fin, this.ctx.currentTime + 0.15); },
  frame(buf) {
    if (!this.id || !this.ctx) return;
    const pcm = new Int16Array(buf), n = pcm.length;
    const b = this.ctx.createBuffer(1, n, 16000), d = b.getChannelData(0);
    let acc = 0;
    for (let i = 0; i < n; i++) { d[i] = pcm[i] / 32768; acc += d[i] * d[i]; }
    const s = this.ctx.createBufferSource(); s.buffer = b; s.connect(this.ctx.destination);
    s.start(this.fin);
    this.niveles.push([this.fin, this.fin + n / 16000, nivelBoca(Math.sqrt(acc / n))]);
    this.fin += n / 16000;
  },
  cerrar(id) { if (this.id === id) this.id = null; },
  nivel() {
    if (!this.ctx) return 0;
    const t = this.ctx.currentTime;
    this.niveles = this.niveles.filter(([, b]) => b > t);
    const a = this.niveles.find(([x, y]) => x <= t && t < y);
    return a ? a[2] : 0;
  },
};

// Mismo mapeo que la placa: RMS → 3 posiciones de boca (cerrada, media, abierta)
function nivelBoca(rms) { const db = 20 * Math.log10(Math.max(rms, 1e-5)); return db < -38 ? 0 : db < -24 ? 0.5 : 1; }

const mic = {
  stream: null, ctx: null, nodo: null, resto: [],
  async empezar() {
    try {
      this.stream ??= await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true } });
    } catch (e) { log("sin micro:", e.message, "(abre el simulador por localhost)"); return false; }
    this.ctx = new AudioContext();
    const src = this.ctx.createMediaStreamSource(this.stream);
    this.nodo = this.ctx.createScriptProcessor(4096, 1, 1);
    const ratio = this.ctx.sampleRate / 16000;
    this.nodo.onaudioprocess = (e) => {
      const x = e.inputBuffer.getChannelData(0);
      for (let i = 0; i < x.length / ratio; i++) this.resto.push(x[Math.floor(i * ratio)]);
      while (this.resto.length >= 1024) {
        const out = new Int16Array(1024);
        for (let i = 0; i < 1024; i++) out[i] = Math.max(-1, Math.min(1, this.resto[i])) * 32767;
        this.resto = this.resto.slice(1024);
        if (conectado) ws.send(out.buffer);
      }
    };
    src.connect(this.nodo); this.nodo.connect(this.ctx.destination);
    enviar({ type: "audio_start", id: "mic", sample_rate: 16000 });
    return true;
  },
  parar() {
    if (!this.ctx) return;
    this.nodo.disconnect(); this.ctx.close(); this.ctx = null; this.resto = [];
    enviar({ type: "audio_end", id: "mic" });
  },
};

// --- texto ------------------------------------------------------------------------
function envolver(texto, ancho, fuente) {
  g.font = fuente;
  const lineas = [];
  for (const parrafo of String(texto).split("\n")) {
    let l = "";
    for (const p of parrafo.split(" ")) {
      const prueba = l ? l + " " + p : p;
      if (g.measureText(prueba).width > ancho && l) { lineas.push(l); l = p; } else l = prueba;
    }
    lineas.push(l);
  }
  return lineas;
}

function recortar(texto, ancho, fuente) {
  g.font = fuente;
  if (g.measureText(texto).width <= ancho) return texto;
  while (texto.length && g.measureText(texto + "…").width > ancho) texto = texto.slice(0, -1);
  return texto + "…";
}

// --- pintar (la misma composición que firmware/src/ui/ui.cpp) ---------------------------
const botonesCard = () => {
  const acc = ui.card?.actions || [], n = acc.length || 1, w = (224 - (n - 1) * 6) / n;
  return acc.map((a, i) => ({ ...a, x: 8 + i * (w + 6), y: 280, w, h: 32 }));
};
const botonesConfirm = () => {
  const op = ui.confirm.options, n = op.length, w = (224 - (n - 1) * 6) / n;
  return op.map((o, i) => ({ label: o, x: 8 + i * (w + 6), y: 262, w, h: 50 }));
};
const COL_BOTON = { Enviar: "#2E9E5B", Editar: "#3A6FD8", Cancelar: "#5A6470" };

function pintar() {
  g.fillStyle = "#05080c"; g.fillRect(0, 0, W, H);
  if (ui.confirm) return pintarConfirm();
  // barra de estado
  g.font = F13; g.fillStyle = "#8090a0";
  const h = new Date(); g.fillText(`${String(h.getHours()).padStart(2, "0")}:${String(h.getMinutes()).padStart(2, "0")}`, 196, 12);
  g.fillStyle = conectado ? "#40E0C0" : "#c04040"; g.beginPath(); g.arc(10, 8, 4, 0, 7); g.fill();
  if (ui.grabando) { g.fillStyle = "#ff4060"; g.fillText("● escuchando", 22, 12); }
  // cara
  g.save(); g.translate(0, BAR); cara.pintar(g); g.restore();
  // panel de abajo: burbuja > tarjeta > pista
  if (ui.bubble) pintarBurbuja();
  else if (ui.card) pintarTarjeta();
  else { g.font = F13; g.fillStyle = "#3a4854"; g.textAlign = "center"; g.fillText("mantén pulsada la cara para hablar", W / 2, 258); g.textAlign = "left"; }
}

function pintarBurbuja() {
  // piquito hacia la boca + texto que aparece letra a letra
  g.fillStyle = "#121a22";
  g.beginPath(); g.moveTo(W / 2 - 9, PANEL_Y + 6); g.lineTo(W / 2 + 9, PANEL_Y + 6); g.lineTo(W / 2, PANEL_Y - 5); g.closePath(); g.fill();
  rrect(g, 8, PANEL_Y + 4, 224, PANEL_H - 10, 12); g.fill();
  const lineas = envolver(ui.bubble.text, 204, F17);
  const max = 5;
  // mientras escribe, el scroll sigue a la última línea visible
  let quedan = Math.floor(ui.revelado), ultima = 0;
  lineas.forEach((l, i) => { if (quedan > 0) ultima = i; quedan -= l.length + 1; });
  const desde = Math.min(Math.max(ui.scroll, ultima - max + 1, 0), Math.max(0, lineas.length - max));
  ui.scroll = desde;
  g.font = F17; g.fillStyle = "#e8eef2";
  let resto = Math.floor(ui.revelado);
  for (let i = 0; i < lineas.length; i++) {
    const vis = lineas[i].slice(0, Math.max(0, resto));
    resto -= lineas[i].length + 1;
    if (i >= desde && i < desde + max) g.fillText(vis, 18, PANEL_Y + 28 + (i - desde) * 22);
  }
  if (lineas.length > max) { g.fillStyle = "#40505c"; g.fillRect(228, PANEL_Y + 10 + (desde / lineas.length) * 110, 2, 110 * max / lineas.length); }
}

// Logos de canal dibujados (16 px): círculo verde de WhatsApp, cuadrado de Instagram, círculo azul de Facebook
function icono(nombre, x, y) {
  const n = (nombre || "").toLowerCase();
  if (n.includes("whatsapp")) {
    g.fillStyle = "#25D366"; g.beginPath(); g.arc(x + 8, y + 8, 8, 0, 7); g.fill();
    g.strokeStyle = "#fff"; g.lineWidth = 1.6; g.beginPath(); g.arc(x + 8, y + 8, 4.6, 0, 7); g.stroke();
    g.fillStyle = "#fff"; g.beginPath(); g.moveTo(x + 3, y + 14); g.lineTo(x + 4.5, y + 10); g.lineTo(x + 7, y + 12.5); g.closePath(); g.fill();
  } else if (n.includes("instagram")) {
    g.fillStyle = "#E1306C"; rrect(g, x, y, 16, 16, 4.5); g.fill();
    g.strokeStyle = "#fff"; g.lineWidth = 1.6; rrect(g, x + 3, y + 3, 10, 10, 3); g.stroke();
    g.beginPath(); g.arc(x + 8, y + 8, 2.4, 0, 7); g.stroke();
    g.fillStyle = "#fff"; g.fillRect(x + 11, y + 4, 1.6, 1.6);
  } else if (n.includes("facebook")) {
    g.fillStyle = "#1877F2"; g.beginPath(); g.arc(x + 8, y + 8, 8, 0, 7); g.fill();
    g.fillStyle = "#fff"; g.font = "700 13px 'Hanken Grotesk'"; g.fillText("f", x + 5.5, y + 13);
  } else return false;
  return true;
}

function pintarTarjeta() {
  const c = ui.card, col = COL_KIND[c.kind] || "#888";
  const oy = Math.round(PANEL_H * (1 - easeOutBack((performance.now() - ui.tCard) / 340)));
  g.save(); g.beginPath(); g.rect(0, PANEL_Y, W, PANEL_H); g.clip(); g.translate(0, oy);
  g.fillStyle = "#121a22"; rrect(g, 8, PANEL_Y + 4, 224, PANEL_H - 10, 10); g.fill();
  g.fillStyle = col; rrect(g, 8, PANEL_Y + 4, 224, 22, 10); g.fill(); g.fillRect(8, PANEL_Y + 16, 224, 10);
  const conIcono = icono(c.icon, 13, PANEL_Y + 7);
  const tx = conIcono ? 34 : 16;
  g.font = F13; g.fillStyle = "#05080c"; g.fillText(recortar(c.title, 224 - tx, F13), tx, PANEL_Y + 20);
  g.fillStyle = "#d8e0e6";
  c.lines.slice(0, 4).forEach((l, i) => g.fillText(recortar(l, 208, F13), 16, PANEL_Y + 44 + i * 17));
  for (const b of botonesCard()) {
    g.fillStyle = b.id === c.actions[0]?.id ? col : "#2a3640"; rrect(g, b.x, b.y, b.w, b.h, 8); g.fill();
    g.fillStyle = b.id === c.actions[0]?.id ? "#05080c" : "#e8eef2"; g.font = F13; g.textAlign = "center";
    g.fillText(b.label, b.x + b.w / 2, b.y + 21); g.textAlign = "left";
  }
  g.restore();
}

function pintarConfirm() {
  const c = ui.confirm;
  const oy = Math.round(46 * (1 - easeOutBack((performance.now() - ui.tConfirm) / 360)));
  g.save(); g.translate(0, oy);
  g.font = F17; g.fillStyle = "#FFB020";
  envolver(c.question, 224, F17).slice(0, 2).forEach((l, i) => g.fillText(l, 8, 22 + i * 21));
  g.fillStyle = "#121a22"; rrect(g, 8, 56, 224, 186, 10); g.fill();
  const lineas = envolver(c.preview, 206, F13), max = 10;
  ui.scroll = Math.min(ui.scroll, Math.max(0, lineas.length - max));
  g.font = F13; g.fillStyle = "#e8eef2";
  lineas.slice(ui.scroll, ui.scroll + max).forEach((l, i) => g.fillText(l, 17, 76 + i * 17));
  const resta = Math.max(0, (c.hasta - performance.now()) / (c.expires_in * 1000));
  g.fillStyle = "#FFB020"; g.fillRect(8, 250, 224 * resta, 3);
  for (const b of botonesConfirm()) {
    g.fillStyle = COL_BOTON[b.label] || "#5A6470"; rrect(g, b.x, b.y, b.w, b.h, 10); g.fill();
    g.fillStyle = "#fff"; g.font = F13; g.textAlign = "center"; g.fillText(b.label, b.x + b.w / 2, b.y + 30); g.textAlign = "left";
  }
  g.restore();
}

// --- toques (mismos gestos que la placa) ----------------------------------------------
let toque = null;
const dentro = (b, x, y) => x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
function pos(e) { const r = lienzo.getBoundingClientRect(); return [Math.round((e.clientX - r.left) * W / r.width), Math.round((e.clientY - r.top) * H / r.height)]; }

lienzo.addEventListener("pointerdown", (e) => {
  const [x, y] = pos(e);
  toque = { x, y, t: performance.now(), ptt: null };
  if (!ui.confirm && y >= BAR && y < PANEL_Y)       // push-to-talk: mantener la cara 300 ms
    toque.ptt = setTimeout(async () => { if (toque && await mic.empezar()) { ui.grabando = true; cara.set(cara.emocion, "look_left"); } }, 300);
});
lienzo.addEventListener("pointerup", (e) => {
  if (!toque) return;
  const [x, y] = pos(e), dx = x - toque.x, dy = y - toque.y, dt = performance.now() - toque.t;
  clearTimeout(toque.ptt);
  const t0 = toque; toque = null;
  if (ui.grabando) { ui.grabando = false; mic.parar(); return; }
  if (Math.abs(dy) > 25 && Math.abs(dy) > Math.abs(dx)) { ui.scroll = Math.max(0, ui.scroll + (dy < 0 ? 2 : -2)); return; }
  if (Math.abs(dx) > 40) return enviar({ type: "touch", target: "screen", x, y, gesture: dx < 0 ? "swipe_left" : "swipe_right" });
  if (ui.confirm) {
    const b = botonesConfirm().find((b) => dentro(b, x, y));
    if (b) enviar({ type: "confirm_reply", id: ui.confirm.id, choice: b.label });
    return;
  }
  const gesto = dt > 600 ? "long_press" : "tap";
  if (t0.y >= BAR && t0.y < PANEL_Y) return enviar({ type: "touch", target: "face", x, y, gesture: gesto });
  if (ui.bubble) { ui.bubble = null; return enviar({ type: "touch", target: "bubble", x, y, gesture: gesto }); }
  if (ui.card) {
    const b = botonesCard().find((b) => dentro(b, x, y));
    if (b) enviar({ type: "touch", target: `card:${ui.card.id}:${b.id}`, x, y, gesture: "tap" });
  }
});

// --- bucle ------------------------------------------------------------------------
let antes = performance.now();
function cuadro(ahora) {
  cara.boca += (audio.nivel() - cara.boca) * 0.5;
  cara.paso(ahora - antes, ahora);
  ui.revelado += (ahora - antes) / 1000 * LETRAS_SEG;
  antes = ahora;
  pintar();
  requestAnimationFrame(cuadro);
}

const params = (await (await fetch("/face.json")).json()).emotions;
await document.fonts.load(F17); await document.fonts.load(F13);
cara = new Cara(params);
cara.desconectado = true;
conectar();
requestAnimationFrame(cuadro);

// consola rápida
document.getElementById("emos").innerHTML = Object.keys(params).map((e) => `<button data-e="${e}">${e}</button>`).join("");
document.getElementById("emos").onclick = (e) => e.target.dataset.e && fetch(`/api/state/${e.target.dataset.e}`, { method: "POST" });
document.getElementById("lead").onclick = () => fetch("/api/lead", { method: "POST", headers: { "content-type": "application/json" }, body: "{}" });
document.getElementById("ciclo").onclick = () => fetch("/api/ciclo", { method: "POST" }).then((r) => r.json()).then((d) => log("ciclo:", JSON.stringify(d)));
document.getElementById("chat").onsubmit = async (e) => {
  e.preventDefault(); const t = e.target.texto.value; e.target.texto.value = "";
  const r = await fetch("/api/chat", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ text: t }) });
  log("pana:", (await r.json()).respuesta);
};
