// Pantalla y táctil de la Waveshare AMOLED 1.75 (ver board.h).
#ifdef BOARD_175
#include <Arduino.h>
#include "board.h"

static volatile bool touchIrq = false;
volatile uint32_t touchIrqs = 0, touchReads = 0;
static void IRAM_ATTR onTouchIrq() { touchIrq = true; touchIrqs++; }

// AXP2101 (0x34): BLDO1 a 3,3 V, «OLED VDD» según TamaPoke (si el PMU se resetea queda apagado).
// El resto de rieles como vengan: el firmware de fábrica deja DCDC1 y ALDO1, que es lo que tiene conexión.
static void pmuPanelOn() {
    auto v = lgfx::i2c::readRegister8(I2C_PORT, 0x34, 0x90, 400000);
    if (!v.has_value()) { Serial.println("pmu: AXP2101 no responde"); return; }
    lgfx::i2c::writeRegister8(I2C_PORT, 0x34, 0x96, 0x1C, 0, 400000);   // 500 + 28·100 mV
    lgfx::i2c::writeRegister8(I2C_PORT, 0x34, 0x90, (v.value() & 0x01) | 0x10, 0, 400000);   // ALDO1 + BLDO1
    delay(20);
}

bool LGFX::init() {
    lgfx::i2c::init(I2C_PORT, PIN_I2C_SDA, PIN_I2C_SCL);
    pmuPanelOn();
    bool ok = LGFX_Device::init();
    // CST9217: reset y luego avisa por INT (bajada) cuando tiene datos. Leerlo dormido cuelga el
    // bus ~1 s (lo vio TamaPoke), así que solo se lee tras una interrupción o con el dedo puesto.
    pinMode(PIN_TP_RST, OUTPUT);
    digitalWrite(PIN_TP_RST, LOW);
    delay(10);
    digitalWrite(PIN_TP_RST, HIGH);
    delay(100);
    pinMode(PIN_TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TP_INT), onTouchIrq, FALLING);
    _touchOk = true;
    imuBegin();
    // si no contesta, suele estar colgado: solo vuelve quitándole la corriente del todo (USB fuera y PWR 6 s)
    Serial.printf("táctil: %s\n", lgfx::i2c::transactionWrite(I2C_PORT, 0x5A, nullptr, 0, 100000).has_value() ? "0x5A responde" : "0x5A no responde");
    return ok;
}

bool LGFX::getTouch(lgfx::touch_point_t *tp) {
    if (!_touchOk) return false;
    static uint32_t lastPoll = 0;
    // por si la interrupción no llega: un sondeo cada 50 ms con el INT abajo
    bool intLow = digitalRead(PIN_TP_INT) == LOW;
    if (!touchIrq && !_down && !(intLow && millis() - lastPoll > 50)) return false;
    lastPoll = millis();
    touchIrq = false;
    touchReads++;
    static const uint8_t cmd[2] = {0xD0, 0x00};
    uint8_t d[10] = {};
    if (!lgfx::i2c::transactionWriteRead(I2C_PORT, 0x5A, cmd, 2, d, sizeof(d), 400000).has_value()) {
        _down = false;
        return false;
    }
    static const uint8_t ack[3] = {0xD0, 0x00, 0xAB};
    lgfx::i2c::transactionWrite(I2C_PORT, 0x5A, ack, 3, 400000);
    int n = d[5] & 0x7F;
    if (d[6] != 0xAB || n == 0 || n > 2 || (d[0] & 0x0F) != 0x06) {
        _down = false;
        return false;
    }
    int x = (d[1] << 4) | (d[3] >> 4), y = (d[2] << 4) | (d[3] & 0x0F);
    // el panel va girado 180° respecto al táctil (mirror_x y mirror_y en el BSP)
    tp->x = constrain(WIDTH - 1 - x, 0, WIDTH - 1);
    tp->y = constrain(HEIGHT - 1 - y, 0, HEIGHT - 1);
    _down = true;
    return true;
}

// --- acelerómetro QMI8658: para despertar la pantalla al levantarla -------------------------
static bool imuOk = false;
bool imuBegin() {
    auto id = lgfx::i2c::readRegister8(I2C_PORT, 0x6B, 0x00, 400000);
    if (!id.has_value() || id.value() != 0x05) { Serial.println("imu: QMI8658 no responde"); return false; }
    lgfx::i2c::writeRegister8(I2C_PORT, 0x6B, 0x02, 0x40, 0, 400000);   // autoincremento, little endian
    lgfx::i2c::writeRegister8(I2C_PORT, 0x6B, 0x03, 0x16, 0, 400000);   // ±4 g, ~125 Hz
    lgfx::i2c::writeRegister8(I2C_PORT, 0x6B, 0x08, 0x01, 0, 400000);   // solo acelerómetro
    imuOk = true;
    return true;
}

bool imuMoved() {
    static int16_t last[3];
    static bool have = false;
    if (!imuOk) return false;
    uint8_t reg = 0x35, d[6];
    if (!lgfx::i2c::transactionWriteRead(I2C_PORT, 0x6B, &reg, 1, d, 6, 400000).has_value()) return false;
    int16_t a[3];
    for (int i = 0; i < 3; i++) a[i] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8);
    int diff = 0;
    for (int i = 0; i < 3; i++) diff = max(diff, abs(a[i] - last[i]));
    bool moved = have && diff > 1500;                // ~0,18 g (8192 por g)
    memcpy(last, a, sizeof(a));
    have = true;
    return moved;
}

void touchProbe() {          // diagnóstico por serie (t): reset y prueba a distintos tiempos
    pinMode(PIN_TP_RST, OUTPUT);
    digitalWrite(PIN_TP_RST, LOW);
    delay(20);
    digitalWrite(PIN_TP_RST, HIGH);
    uint32_t t0 = millis();
    static const uint8_t cmd[2] = {0xD0, 0x00};
    for (int k = 0; k < 12; k++) {
        delay(k < 6 ? 10 : 100);
        bool ack = lgfx::i2c::transactionWrite(I2C_PORT, 0x5A, nullptr, 0, 100000).has_value();
        uint8_t d[8] = {};
        bool rd = lgfx::i2c::transactionWriteRead(I2C_PORT, 0x5A, cmd, 2, d, 8, 100000).has_value();
        Serial.printf("t+%lu ms: INT %d · ack %d · lee %d · %02x %02x %02x %02x %02x %02x %02x\n", millis() - t0,
                      digitalRead(PIN_TP_INT), ack, rd, d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
    }
    Serial.printf("RST=%d INT=%d\n", digitalRead(PIN_TP_RST), digitalRead(PIN_TP_INT));
}

void LGFX::flush(LGFX_Sprite &s, int x, int y, int w, int h) {
    int x1 = min(WIDTH, x + w), y1 = min(HEIGHT, y + h);
    x = max(0, x) & ~1;
    y = max(0, y) & ~1;
    x1 = min(WIDTH, (x1 + 1) & ~1);
    y1 = min(HEIGHT, (y1 + 1) & ~1);
    if (x1 <= x || y1 <= y) return;
    // por franjas de 16 filas copiadas a SRAM: cada ventana empieza y acaba en par
    static uint16_t *band = nullptr;
    if (!band) band = (uint16_t *)heap_caps_malloc(WIDTH * 16 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const uint16_t *src = (const uint16_t *)s.getBuffer();
    const int bw = x1 - x;
    startWrite();
    for (int yy = y; yy < y1; yy += 16) {
        int bh = min(16, y1 - yy);
        waitDMA();                                    // la franja anterior puede seguir saliendo por DMA
        for (int r = 0; r < bh; r++) memcpy(band + r * bw, src + (yy + r) * WIDTH + x, bw * 2);
        pushImage(x, yy, bw, bh, (const lgfx::swap565_t *)band);
    }
    waitDMA();
    endWrite();
}
#endif
