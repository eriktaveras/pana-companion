// Pana · cuerpo. La placa es tonta a propósito: pinta, anima, captura toques y audio.
// Todo lo demás vive en el servidor (server/). Ver protocol/PROTOCOL.md.
//
// Tareas: UI en el núcleo 1 (este loop, ~30 fps) · red, reproducción y micro en el núcleo 0.
// Serie (115200): s captura de pantalla (tools/shot.py) · b pitido · m prueba de micro · v push-to-talk ·
// g página siguiente · n nota del grafo · e abrir/cerrar la consola · x congelar la pantalla · P carta de ajuste (P4) ·
// host <ip> · port <n> · wifi reset · info · r/w <dir> <reg> [val] (I2C en hex)
#include <Arduino.h>
#include <driver/gpio.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_periph.h>
#include "audio/audio.h"
#include "board.h"
#include "config.h"
#include "net/net.h"
#include "ui/model.h"
#include "ui/ui.h"

static LGFX lcd;
Model model;
static SemaphoreHandle_t mtx;
void modelLock() { xSemaphoreTake(mtx, portMAX_DELAY); }
void modelUnlock() { xSemaphoreGive(mtx); }

static void serialCommands() {
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (line.isEmpty() && c == 's') { uiDump(); continue; }
        if (line.isEmpty() && c == 'b') { audioBeep(); continue; }
        if (line.isEmpty() && c == 'm') { audioMicTest(); continue; }
#ifdef BOARD_P4
        if (line.isEmpty() && c == 'P') { extern void uiTestPattern(); uiTestPattern(); continue; }   // carta de ajuste
#endif
#if defined(ROUND_UI) || defined(BOARD_P4)
        if (line.isEmpty() && c == 'x') {               // congela la pantalla (diagnóstico de parpadeo)
            extern bool uiFrozen;
            uiFrozen = !uiFrozen;
            Serial.println(uiFrozen ? "pantalla congelada" : "pantalla viva");
            continue;
        }
#endif
        if (line.isEmpty() && c == 'q') {               // ¿se mueven los pines del I2S? (cambios en 20000 muestras)
            const int pins[] = {PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_MCLK, PIN_I2S_DOUT, PIN_I2S_DIN};
            for (int p : pins) {
                PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[p]);
                int ones = 0, flips = 0, last = gpio_get_level((gpio_num_t)p);
                for (int i = 0; i < 20000; i++) {
                    int v = gpio_get_level((gpio_num_t)p);
                    ones += v; flips += v != last; last = v;
                }
                Serial.printf("gpio %d: %d a 1, %d cambios\n", p, ones, flips);
            }
            continue;
        }
#ifdef BOARD_175
        if (line.isEmpty() && c == 't') { extern void touchProbe(); touchProbe(); continue; }
#endif
        if (line.isEmpty() && (c == 'g' || c == 'n' || c == 'e')) { uiKey(c); continue; }
        if (line.isEmpty() && c == 'v') {            // push-to-talk por serie (pruebas)
            static bool on = false;
            on = !on;
            modelLock(); model.recording = on; modelUnlock();
            if (on) netMicStart(); else netMicStop();
            Serial.println(on ? "ptt on" : "ptt off");
            continue;
        }
        if (c == '\n' || c == '\r') {
            line.trim();
            if (line.startsWith("host ")) {
                netSetHost(line.substring(5));
            } else if (line.startsWith("port ")) {
                netSetPort(line.substring(5).toInt());
            } else if (line == "wifi reset") {
                netResetWifi();
            } else if (line.startsWith("w ") || line.startsWith("r ")) {   // w <dir> <reg> <val> · r <dir> <reg> (hex)
                int a = strtol(line.c_str() + 2, nullptr, 16);
                int sp = line.indexOf(' ', 2), sp2 = line.indexOf(' ', sp + 1);
                int reg = strtol(line.c_str() + sp + 1, nullptr, 16);
                if (line[0] == 'w' && sp2 > 0)
                    lgfx::i2c::writeRegister8(I2C_PORT, a, reg, strtol(line.c_str() + sp2 + 1, nullptr, 16), 0, 400000);
                auto v = lgfx::i2c::readRegister8(I2C_PORT, a, reg, 400000);
                Serial.printf("i2c %02x[%02x] = %02x\n", a, reg, v.has_value() ? v.value() : 0xEE);
            } else if (line == "info") {
                Serial.printf("fw %s · heap %u · psram libre %u · uptime %lus\n", FW_VERSION, ESP.getFreeHeap(),
                              ESP.getFreePsram(), millis() / 1000);
            }
            line = "";
        } else {
            line += c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    mtx = xSemaphoreCreateMutex();
    lcd.init();                  // abre también el I2C 0 (táctil y códec)
    lcd.setRotation(0);
    lcd.setBrightness(200);
    uiBegin(lcd);
    uiSplash("Pana", "despertando…");
    bool audio = audioBegin();
    Serial.printf("\nPana fw %s · audio %s · PSRAM libre %u\n", FW_VERSION, !HAS_AUDIO ? "no tiene" : audio ? "OK" : "FALLA",
                  ESP.getFreePsram());
    netBegin();
}

void loop() {
    static uint32_t next = 0;
    serialCommands();
    uint32_t now = millis();
    if ((int32_t)(now - next) >= 0) {
        next = now + FRAME_MS;
        uiFrame(now);
    }
    delay(1);
}
