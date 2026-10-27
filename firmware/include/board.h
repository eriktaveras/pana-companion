#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#if defined(BOARD_175)
// Waveshare ESP32-S3-Touch-AMOLED-1.75: redonda 466x466, CO5300 por QSPI, táctil CST9217,
// ES8311 (altavoz) + ES7210 (dos micros), AXP2101. Pines y secuencia de arranque del BSP oficial
// waveshare/esp32_s3_touch_amoled_1_75 3.0.1, probado en la placa.
// Los rieles del AXP2101 se quedan como vienen: DCDC1 y ALDO1 (códecs) ya salen encendidos.
#define HAS_AUDIO 1
#define HAS_ES7210 1

#define PIN_I2C_SDA 15      // compartido: CST9217 (0x5A), ES8311 (0x18), ES7210 (0x40), AXP2101, RTC
#define PIN_I2C_SCL 14
#define I2C_PORT 0
#define PIN_TP_INT 11
#define PIN_TP_RST 40

#define PIN_I2S_MCLK 42
#define PIN_I2S_MCLK2 16     // copia del MCLK para el ES7210 (ver audio.cpp)
#define PIN_I2S_BCLK 9
#define PIN_I2S_WS 45
#define PIN_I2S_DOUT 8      // al ES8311
#define PIN_I2S_DIN 10      // del ES7210
#define PIN_PA_SHDN 46      // aquí es enable: en ALTO suena (PA_ON)
#define PA_ON HIGH

// El CO5300 de esta placa: 480 columnas con 6 de margen y la secuencia de arranque del BSP
struct Panel_CO5300_175 : public lgfx::Panel_CO5300 {
    const uint8_t *getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0xFE, 1, 0x20,
            0x19, 1, 0x10,
            0x1C, 1, 0xA0,
            0xFE, 1, 0x00,
            0xC4, 1, 0x80,
            0x3A, 1, 0x55,
            0x35, 1, 0x00,
            0x53, 1, 0x20,
            0x51, 1, 0xFF,
            0x63, 1, 0xFF,
            0x2A, 4, 0x00, 0x06, 0x01, 0xD7,
            0x2B, 4, 0x00, 0x00, 0x01, 0xD1,
            0x11, 0x80, 0,
            0x29, 0x80, 0,
            0xff, 0xff,
        };
        return listno == 0 ? list0 : nullptr;
    }
};

class LGFX : public lgfx::LGFX_Device {
    Panel_CO5300_175 _panel;
    lgfx::Bus_SPI _bus;
    bool _touchOk = false;
    volatile bool _down = false;

public:
    static const int WIDTH = 466, HEIGHT = 466;
    LGFX() {
        auto b = _bus.config();
        b.spi_host = SPI2_HOST;
        b.spi_mode = 0;
        b.freq_write = 80000000;
        b.freq_read = 20000000;
        b.spi_3wire = true;
        b.use_lock = true;
        b.dma_channel = SPI_DMA_CH_AUTO;
        b.pin_sclk = 38;
        b.pin_io0 = 4;
        b.pin_io1 = 5;
        b.pin_io2 = 6;
        b.pin_io3 = 7;
        _bus.config(b);
        _panel.setBus(&_bus);

        auto p = _panel.config();
        p.pin_cs = 12;
        p.pin_rst = 39;
        p.pin_busy = -1;
        p.panel_width = WIDTH;
        p.panel_height = HEIGHT;
        p.memory_width = 480;
        p.memory_height = HEIGHT;
        p.offset_x = 6;
        p.offset_y = 0;
        p.readable = false;
        p.bus_shared = false;
        _panel.config(p);
        setPanel(&_panel);
    }
    bool init();                                   // pantalla + I2C + táctil (board175.cpp)
    bool getTouch(lgfx::touch_point_t *tp);        // CST9217 leído a mano: LovyanGFX no lo trae
    // copia el trozo (x, y, w, h) del sprite de pantalla completa, alineado a pares como pide el CO5300
    void flush(LGFX_Sprite &s, int x, int y, int w, int h);
};
bool imuBegin();        // QMI8658 (0x6B): solo acelerómetro, ±4 g
bool imuMoved();        // ¿se movió desde la última lectura? (levantarla, girarla)

#elif defined(BOARD_P4)
#include <lgfx/v1/platforms/esp32p4/Bus_DSI.hpp>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
// Waveshare ESP32-P4-WIFI6-Touch-LCD-4C: redonda 720x720 por MIPI-DSI (JD9365, 2 carriles),
// táctil GT911, ES8311 + ES7210 y Wi-Fi por el ESP32-C6 (esp_hosted, pines por defecto del core).
// Pines, tiempos y secuencia del BSP oficial de Waveshare (esp32_p4_wifi6_touch_lcd_xc 3.0.1), probado en la placa.
//
// UI propia a 720 nativos (ui720.cpp): pinta en un sprite de pantalla completa y flush() copia con el
// PPA (que de paso da la vuelta a los bytes) solo lo que cambió al framebuffer que escanea el DPI.
// Copiarlo entero en cada cuadro le quitaba al DPI su ancho de PSRAM: rayas y fondo que palpitaba.
#define HAS_AUDIO 1
#define HAS_ES7210 1

#define PIN_I2C_SDA 7       // compartido: GT911 (0x5D), ES8311 (0x18), ES7210 (0x40)
#define PIN_I2C_SCL 8
#define I2C_PORT 0
#define PIN_LCD_RST 27
#define PIN_LCD_BL 26       // PWM, invertido

#define PIN_I2S_MCLK 13     // los dos códecs
#define PIN_I2S_BCLK 12
#define PIN_I2S_WS 10
#define PIN_I2S_DOUT 9      // al ES8311
#define PIN_I2S_DIN 11      // del ES7210
#define PIN_PA_SHDN 53      // enable del amplificador: en ALTO suena
#define PA_ON HIGH

struct Panel_JD9365_4C : public lgfx::Panel_DSI {
    esp_lcd_panel_handle_t dpi() const { return _disp_panel_handle; }
protected:
    const uint8_t *getInitParams(size_t listno) const override;   // boardp4.cpp
    size_t getInitDelay(size_t listno) const override { return listno == 0 ? 120 : listno == 1 ? 20 : 0; }
};

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_DSI _bus;
    Panel_JD9365_4C _panel;
    lgfx::Light_PWM _light;
    lgfx::Touch_GT911 _touch;

public:
    static const int PHYS = 720;
    static const int WIDTH = PHYS, HEIGHT = PHYS;
    LGFX() {
        auto b = _bus.config();
        b.bus_id = 0;
        b.lane_num = 2;
        b.lane_mbps = 1500;
        b.ldo_chan_id = 3;
        b.ldo_voltage_mv = 2500;
        _bus.config(b);

        auto d = _panel.config_detail();
        d.dpi_freq_mhz = 80;
        d.hsync_pulse_width = 20;
        d.hsync_back_porch = 20;
        d.hsync_front_porch = 40;
        d.vsync_pulse_width = 4;
        d.vsync_back_porch = 12;
        d.vsync_front_porch = 24;
        _panel.config_detail(d);
        auto p = _panel.config();
        p.panel_width = p.memory_width = PHYS;
        p.panel_height = p.memory_height = PHYS;
        p.offset_x = p.offset_y = 0;
        p.readable = true;
        p.rgb_order = true;
        p.bus_shared = false;
        p.pin_cs = -1;
        p.pin_rst = -1;              // el reset lo da init() a mano
        _panel.config(p);
        _panel.setBus(&_bus);

        auto l = _light.config();
        l.pin_bl = PIN_LCD_BL;
        l.invert = true;
        l.freq = 5000;
        l.pwm_channel = 0;
        _light.config(l);
        _panel.setLight(&_light);

        auto t = _touch.config();
        t.x_min = 0; t.x_max = PHYS - 1;
        t.y_min = 0; t.y_max = PHYS - 1;
        t.pin_int = -1;
        t.pin_rst = -1;
        t.bus_shared = true;
        t.i2c_port = I2C_PORT;
        t.i2c_addr = 0x5D;
        t.pin_sda = PIN_I2C_SDA;
        t.pin_scl = PIN_I2C_SCL;
        t.freq = 400000;
        _touch.config(t);
        _panel.setTouch(&_touch);
        setPanel(&_panel);
    }
    bool init();                                   // reset del panel, DSI, PPA y táctil (boardp4.cpp)
    bool getTouch(lgfx::touch_point_t *tp);
    void present(LGFX_Sprite &s);                  // el sprite de 720x720 entero a la pantalla
    bool dcs(uint8_t cmd, uint8_t v) { return _bus.writeParams(cmd, &v, 1); }   // un registro del JD9365
    void flush(LGFX_Sprite &s, int x, int y, int w, int h);   // solo ese trozo
};
inline bool imuBegin() { return false; }   // la 4C no trae acelerómetro
inline bool imuMoved() { return false; }

#else
#error "Elige la placa en platformio.ini: -DBOARD_175 (AMOLED 1.75) o -DBOARD_P4 (P4 4C)"
#endif
