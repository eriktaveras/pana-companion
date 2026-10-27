// Cara procedural de Pana. ESTE ARCHIVO Y firmware/src/character/face.cpp HACEN LO MISMO:
// si cambias uno, cambia el otro. Los parámetros por emoción salen de protocol/face.json.
// Lienzo de 240x170. Todo se dibuja con rectángulos redondeados, elipses y columnas
// verticales (la boca), que es lo que la placa pinta rápido.

export const FW = 240, FH = 170;
const CLAVES = ["ew", "eh", "gap", "ey", "round", "lid", "tilt", "smile", "mw", "mcurve", "mopen", "blush"];
export const FONDO = [5, 8, 12];
const ROSA = [255, 106, 154];

// c sobre el fondo con opacidad a (la placa no tiene alfa: se mezcla a mano, igual que mix() en face.cpp)
export function mix(c, a, base = FONDO) { return c.map((v, i) => Math.round(v * a + base[i] * (1 - a))); }
const css = (c) => `rgb(${c.join(",")})`;

// Resortes amortiguados (k, c iguales que spring() en firmware/src/character/face.cpp):
// forma 140/15, color 90/19 (sin rebote), mirada 260/20.

function hex(c) { return [parseInt(c.slice(0, 2), 16), parseInt(c.slice(2, 4), 16), parseInt(c.slice(4, 6), 16)]; }

export class Cara {
  constructor(params) {
    this.P = params;                  // face.json → emotions
    this.emocion = "idle";
    this.p = this._objetivo("idle");  // valores actuales (se interpolan)
    this.color = hex(this.P.idle.color);
    this.lookX = 0; this.lookY = 0; this.tLookX = 0; this.tLookY = 0;
    this.parpadeo = 0;                // 0 abierto … 1 cerrado
    this.proxParpadeo = 2000;
    this.proxMirada = 1500;
    this.gesto = null; this.tGesto = 0;
    this.boca = 0;                    // apertura por lip-sync (0..1)
    this.desconectado = false;
    this.v = {}; this.vc = [0, 0, 0]; this.vl = { x: 0, y: 0 }; this.holdLook = 0;
  }

  lookAt(x, y) {
    this.tLookX = Math.max(-1, Math.min(1, x)) * 16;
    this.tLookY = Math.max(-1, Math.min(1, y)) * 10;
    this.holdLook = (this.ahora || 0) + 4000;
  }

  _objetivo(e) { const o = {}; for (const k of CLAVES) o[k] = this.P[e][k]; return o; }

  set(emocion, animacion) {
    if (this.P[emocion]) {
      if (this.P[emocion].shape !== this.P[this.emocion].shape) this.parpadeo = 1;   // el parpadeo tapa el cambio de forma
      this.emocion = emocion;
    }
    if (animacion) { this.gesto = animacion; this.tGesto = 0; }
  }

  // dt en ms
  paso(dt, ahora) {
    const e = this.desconectado ? "bored" : this.emocion;
    const obj = this._objetivo(e);
    const sdt = Math.min(dt, 50) / 1000;
    for (const c of CLAVES) {
      this.v[c] ??= 0;
      this.v[c] += ((obj[c] - this.p[c]) * 140 - this.v[c] * 15) * sdt;
      this.p[c] += this.v[c] * sdt;
    }
    const col = this.desconectado ? [90, 100, 110] : hex(this.P[e].color);
    for (let i = 0; i < 3; i++) {       // el color sin rebote
      this.vc[i] += ((col[i] - this.color[i]) * 90 - this.vc[i] * 19) * sdt;
      this.color[i] = Math.max(0, Math.min(255, this.color[i] + this.vc[i] * sdt));
    }

    // parpadeo aleatorio (no dormido)
    this.proxParpadeo -= dt;
    if (this.proxParpadeo <= 0 && e !== "sleeping") { this.parpadeo = 1; this.proxParpadeo = 2200 + Math.random() * 4000; }
    this.parpadeo = Math.max(0, this.parpadeo - dt / 140);

    // miradas: en idle/bored/thinking mira a los lados de vez en cuando
    this.proxMirada -= dt;
    if (this.proxMirada <= 0 && ahora > this.holdLook) {
      const inquieto = e === "idle" || e === "bored" || e === "thinking";
      this.tLookX = inquieto ? (Math.random() * 2 - 1) * (e === "thinking" ? 6 : 12) : 0;
      this.tLookY = e === "thinking" ? -6 : inquieto ? (Math.random() * 2 - 1) * 4 : 0;
      this.proxMirada = 1200 + Math.random() * 3000;
    }
    let gx = 0, gy = 0;
    if (this.gesto) {
      this.tGesto += dt;
      const t = this.tGesto / 1000;
      if (this.gesto === "bounce") gy = -Math.abs(Math.sin(t * 10)) * 10 * Math.exp(-t * 3);
      else if (this.gesto === "nod") gy = Math.sin(t * 14) * 6 * Math.exp(-t * 2.5);
      else if (this.gesto === "shake") gx = Math.sin(t * 16) * 9 * Math.exp(-t * 2.5);
      else if (this.gesto === "look_left") gx = -14;
      else if (this.gesto === "look_right") gx = 14;
      if (this.tGesto > 1300) this.gesto = null;
    }
    // sacadas rápidas con un pelín de rebote
    this.vl.x += ((this.tLookX + gx - this.lookX) * 260 - this.vl.x * 20) * sdt; this.lookX += this.vl.x * sdt;
    this.vl.y += ((this.tLookY + gy - this.lookY) * 260 - this.vl.y * 20) * sdt; this.lookY += this.vl.y * sdt;
    this.ahora = ahora;
  }

  // g: CanvasRenderingContext2D ya trasladado al origen del área de la cara
  pintar(g) {
    const p = this.p, t = this.ahora / 1000;
    const fondo = css(FONDO);
    g.fillStyle = fondo; g.fillRect(0, 0, FW, FH);
    const c = this.color.map(Math.round);
    const col = css(c);
    const forma = this.desconectado ? "x" : this.P[this.emocion].shape;
    const respira = 1 + 0.025 * Math.sin(t * 2.2);
    const eh = Math.max(3, p.eh * respira * (1 - this.parpadeo * 0.92));
    const ew = Math.max(4, p.ew);
    for (const lado of [-1, 1]) {
      const cx = FW / 2 + lado * p.gap / 2 + this.lookX;
      const cy = p.ey + this.lookY;
      if (forma === "rect") this._ojo(g, c, cx, cy, ew, eh, lado);
      else this._glifo(g, c, forma, cx, cy, Math.min(ew, p.eh) * (1 - this.parpadeo * 0.85), lado, t);
    }
    // mejillas: dos óvalos rosas bajo los ojos (mezclados con el fondo según blush)
    if (p.blush > 0.3 && !this.desconectado) {
      g.fillStyle = css(mix(ROSA, Math.min(1, p.blush) * 0.75));
      for (const lado of [-1, 1]) {
        g.beginPath();
        g.ellipse(FW / 2 + lado * (p.gap / 2 + 10) + this.lookX, p.ey + p.eh / 2 + 10 + this.lookY, 12, 5, 0, 0, Math.PI * 2);
        g.fill();
      }
    }
    // boca por columnas: curva + apertura (lip-sync)
    const my = p.ey + p.eh / 2 + 30 + this.lookY * 0.6;
    const abre = p.mopen + this.boca * 18;
    const media = p.mw / 2;
    g.fillStyle = col;
    for (let x = -media; x <= media; x++) {
      const u = x / media, cv = p.mcurve * (1 - u * u);
      const top = my + cv - 1.5, bot = my + cv + 1.5 + abre * (1 - u * u);
      g.fillRect(FW / 2 + x + this.lookX * 0.6, top, 1, bot - top);
    }
    this._extra(g, col, t);
  }

  // Ojo de pantalla (estilo EMO): halo en 3 capas, cuerpo, brillo y máscaras de párpado/sonrisa
  _ojo(g, c, cx, cy, ew, eh, lado) {
    const p = this.p, fondo = css(FONDO);
    const x0 = cx - ew / 2, y0 = cy - eh / 2;
    const r = Math.max(0, Math.min(p.round, eh / 2, ew / 2));
    for (const [d, a] of [[9, 0.07], [6, 0.13], [3, 0.24]]) {
      g.fillStyle = css(mix(c, a));
      rrect(g, x0 - d, y0 - d, ew + 2 * d, eh + 2 * d, r + d); g.fill();
    }
    g.fillStyle = css(c);
    rrect(g, x0, y0, ew, eh, r); g.fill();
    // brillo: sigue un poco a la mirada, como un reflejo en el cristal
    if (eh > 14) {
      const hx = x0 + ew * 0.3 + this.lookX * 0.25, hy = y0 + eh * 0.27 + this.lookY * 0.2;
      g.fillStyle = css(mix([255, 255, 255], 0.85, c));
      g.beginPath(); g.arc(hx, hy, Math.max(2, ew * 0.12), 0, Math.PI * 2); g.fill();
      g.beginPath(); g.arc(hx + ew * 0.16, hy + ew * 0.13, Math.max(1, ew * 0.05), 0, Math.PI * 2); g.fill();
    }
    g.fillStyle = fondo;
    // párpado: tilt > 0 baja el lado de dentro (enfado/alerta), < 0 el de fuera (tristeza)
    const dentro = Math.max(0, p.tilt) * 0.45, fuera = Math.max(0, -p.tilt) * 0.45;
    const izq = lado < 0 ? fuera : dentro, der = lado < 0 ? dentro : fuera;
    if (p.lid + izq + der > 0.01) {
      g.beginPath();
      g.moveTo(x0 - 10, y0 - 10); g.lineTo(x0 + ew + 10, y0 - 10);
      g.lineTo(x0 + ew + 10, y0 + eh * (p.lid + der)); g.lineTo(x0 - 10, y0 + eh * (p.lid + izq));
      g.closePath(); g.fill();
    }
    // ojos felices: una elipse de fondo muerde la parte de abajo
    if (p.smile > 0.02) {
      g.beginPath();
      g.ellipse(cx, y0 + eh + ew * 0.12, ew * 0.68, eh * p.smile * 0.85 + 4, 0, 0, Math.PI * 2);
      g.fill();
    }
  }

  // Ojos con forma: corazón (late), espiral (gira) y X (sin conexión). Halo = el mismo glifo más grande.
  _glifo(g, c, forma, cx, cy, s, lado, t) {
    if (s < 6) {                       // parpadeando: una raya
      g.fillStyle = css(c); g.fillRect(cx - 18, cy - 2, 36, 4); return;
    }
    if (forma === "heart") {
      const late = 1 + 0.07 * Math.max(0, Math.sin(t * 7));
      corazon(g, cx, cy, s * 1.22 * late, css(mix(c, 0.14)));
      corazon(g, cx, cy, s * 1.08 * late, css(mix(c, 0.3)));
      corazon(g, cx, cy, s * late, css(c));
      g.fillStyle = css(mix([255, 255, 255], 0.8, c));
      g.beginPath(); g.arc(cx - s * 0.22, cy - s * 0.2, s * 0.09, 0, Math.PI * 2); g.fill();
    } else if (forma === "spiral") {
      const giro = t * 3 * lado;
      for (const [w, a] of [[9, 0.15], [5, 1]]) {
        g.fillStyle = css(mix(c, a));
        for (let i = 0; i <= 60; i++) {
          const u = i / 60, ang = giro + u * Math.PI * 4.4, rad = 2 + u * s * 0.46;
          g.beginPath(); g.arc(cx + Math.cos(ang) * rad, cy + Math.sin(ang) * rad, w / 2, 0, Math.PI * 2); g.fill();
        }
      }
    } else if (forma === "x") {
      const d = s * 0.36;
      for (const [w, a] of [[13, 0.15], [7, 1]]) {
        g.strokeStyle = css(mix(c, a)); g.lineWidth = w; g.lineCap = "round";
        g.beginPath(); g.moveTo(cx - d, cy - d); g.lineTo(cx + d, cy + d);
        g.moveTo(cx + d, cy - d); g.lineTo(cx - d, cy + d); g.stroke();
      }
    }
  }

  _extra(g, col, t) {
    const ex = this.desconectado ? "" : this.P[this.emocion].extra;
    g.fillStyle = col;
    if (ex === "dots") for (let i = 0; i < 3; i++) {
      const r = 3 + 2 * Math.max(0, Math.sin(t * 5 - i * 0.9));
      g.beginPath(); g.arc(186 + i * 14, 26, r, 0, Math.PI * 2); g.fill();
    } else if (ex === "bang") {
      const y = 14 + Math.sin(t * 8) * 2;
      g.fillRect(204, y, 7, 22); g.fillRect(204, y + 27, 7, 7);
    } else if (ex === "zzz") {
      g.font = "700 14px 'Hanken Grotesk'";
      for (let i = 0; i < 3; i++) {
        const f = (t * 0.5 + i / 3) % 1;
        g.globalAlpha = 1 - f;
        g.fillText("z", 168 + f * 30 + i * 4, 60 - f * 44);
      }
      g.globalAlpha = 1;
    } else if (ex === "sparkle") for (let i = 0; i < 3; i++) {
      const a = Math.max(0, Math.sin(t * 4 + i * 2.1));
      const sx = [30, 206, 196][i], sy = [30, 22, 120][i], s = 3 + a * 5;
      g.globalAlpha = a;
      g.fillRect(sx - s, sy - 1, s * 2, 2); g.fillRect(sx - 1, sy - s, 2, s * 2);
      g.globalAlpha = 1;
    }
    if (this.desconectado) {          // ojos en X + aviso: sin servidor
      g.fillStyle = "#c04040";
      g.font = "700 13px 'Hanken Grotesk'";
      g.fillText("sin conexión", 82, 160);
    }
  }
}

// Corazón con dos círculos y un triángulo (igual que heart() en face.cpp)
function corazon(g, cx, cy, s, fill) {
  const r = s * 0.29;
  g.fillStyle = fill;
  g.beginPath(); g.arc(cx - s * 0.24, cy - s * 0.12, r, 0, Math.PI * 2); g.fill();
  g.beginPath(); g.arc(cx + s * 0.24, cy - s * 0.12, r, 0, Math.PI * 2); g.fill();
  g.beginPath(); g.moveTo(cx - s * 0.51, cy - s * 0.04); g.lineTo(cx + s * 0.51, cy - s * 0.04); g.lineTo(cx, cy + s * 0.52); g.closePath(); g.fill();
}

export function rrect(g, x, y, w, h, r) {
  g.beginPath();
  g.moveTo(x + r, y); g.arcTo(x + w, y, x + w, y + h, r); g.arcTo(x + w, y + h, x, y + h, r);
  g.arcTo(x, y + h, x, y, r); g.arcTo(x, y, x + w, y, r); g.closePath();
}
