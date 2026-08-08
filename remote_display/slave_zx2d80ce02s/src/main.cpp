/*
 * Bruce split-display SLAVE firmware  --  PanelLan ZX2D80CE02S (SC05_X)
 * ---------------------------------------------------------------------------
 * Role: dumb pixel blitter + touch return. Receives RGB565 tiles from the
 * master over SPI (this board = SPI peripheral) and paints them on the
 * parallel ST7789; reads the FT6336 cap-touch and hands the master the latest
 * touch state on MISO (full-duplex, on every control frame) plus a TOUCH_IRQ
 * line so the master knows when to look.
 *
 * Panel + touch pin map copied verbatim from the Bruce ZX2D80CE02S board port
 * (boards/ZX2D80CE02S/pins_arduino.h + interface.cpp).
 *
 * The SPI link uses the board's only free general-IO (EXT-IO header pins
 * 10..14, 21); the parallel panel and the I2C touch consume everything else.
 * ---------------------------------------------------------------------------
 */
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <driver/spi_slave.h>

#include "bruce_remote_link.h" // shared with the master (see -I.. in platformio.ini)

/* ================= Panel (parallel ST7789, dual-bank 8080) ================= */
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_Parallel8 _bus;
    lgfx::Light_PWM _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 40000000;
            cfg.pin_wr = 17; // TFT_WR
            cfg.pin_rd = -1; // TFT_RD (not wired)
            cfg.pin_rs = 18; // TFT_DC / RS
            cfg.pin_d0 = 16;
            cfg.pin_d1 = 40;
            cfg.pin_d2 = 15;
            cfg.pin_d3 = 7;
            cfg.pin_d4 = 41;
            cfg.pin_d5 = 42;
            cfg.pin_d6 = 2;
            cfg.pin_d7 = 1;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = -1; // tied on PCB
            cfg.pin_rst = 3; // shared board/touch reset
            cfg.pin_busy = -1;
            cfg.panel_width = BRL_PANEL_W;
            cfg.panel_height = BRL_PANEL_H;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.readable = false;
            cfg.invert = true;      // TFT_INVERSION_ON
            cfg.rgb_order = true;   // TFT_RGB_ORDER = TFT_RGB (flip on bring-up if colors look wrong)
            cfg.dlen_16bit = false; // 8-bit bus
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 47; // TFT_BL, active high
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};
static LGFX lcd;

/* ============================ FT6336 touch (I2C) ========================== */
#define TOUCH_SDA 8
#define TOUCH_SCL 9
#define TOUCH_INT 48
#define TOUCH_ADDR 0x38
#define FT6336_REG_NUM 0x02
#define FT6336_REG_DATA 0x03

static uint8_t ft_read(uint8_t reg) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(TOUCH_ADDR, 1);
    return Wire.available() ? Wire.read() : 0;
}
static bool ft_read_touch(int16_t &x, int16_t &y) {
    uint8_t n = ft_read(FT6336_REG_NUM);
    if (n == 0 || n > 2) return false;
    uint8_t d[4];
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(FT6336_REG_DATA);
    Wire.endTransmission(false);
    Wire.requestFrom(TOUCH_ADDR, 4);
    for (int i = 0; i < 4; i++) d[i] = Wire.read();
    x = ((d[0] & 0x0F) << 8) | d[1];
    y = ((d[2] & 0x0F) << 8) | d[3];
    return true;
}

/* ============================== SPI slave ================================= */
// EXT-IO header pins (the only free general IO on the board).
#define PIN_SPI_CS 10
#define PIN_SPI_SCLK 12
#define PIN_SPI_MOSI 11
#define PIN_SPI_MISO 13
#define PIN_TOUCH_IRQ 14 // slave -> master, active low

#define SPI_SLAVE_HOST SPI2_HOST

// DMA-capable buffers must be word-aligned. RX must hold a full band payload.
static WORD_ALIGNED_ATTR uint8_t rx_ctrl[16];
static WORD_ALIGNED_ATTR uint8_t tx_ctrl[16];
static WORD_ALIGNED_ATTR uint8_t rx_payload[BRL_MAX_PAYLOAD];

// Latest touch status, refreshed by the touch task, snapshotted into tx_ctrl
// before each control transaction is armed.
// Not volatile: every access is inside the portMUX critical section below,
// which provides the memory barrier and mutual exclusion between the touch
// task (core 0) and the SPI loop.
static brl_status_t g_status;
static portMUX_TYPE g_status_mux = portMUX_INITIALIZER_UNLOCKED;

static void spi_slave_init() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_SPI_MOSI;
    buscfg.miso_io_num = PIN_SPI_MISO;
    buscfg.sclk_io_num = PIN_SPI_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = BRL_MAX_PAYLOAD;

    spi_slave_interface_config_t slvcfg = {};
    slvcfg.mode = 0;
    slvcfg.spics_io_num = PIN_SPI_CS;
    slvcfg.queue_size = 3;
    slvcfg.flags = 0;

    ESP_ERROR_CHECK(spi_slave_initialize(SPI_SLAVE_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));
}

// Run one fixed-size slave transaction (blocks until the master clocks it).
static esp_err_t spi_slave_xfer(void *tx, void *rx, size_t len) {
    spi_slave_transaction_t t = {};
    t.length = len * 8; // bits
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_slave_transmit(SPI_SLAVE_HOST, &t, portMAX_DELAY);
}

/* ============================== Touch task =============================== */
static void touch_task(void *) {
    int16_t last_x = -1, last_y = -1;
    bool was_down = false;
    for (;;) {
        int16_t x, y;
        bool down = ft_read_touch(x, y);
        bool changed = false;
        brl_status_t s;
        portENTER_CRITICAL(&g_status_mux);
        s = g_status;
        portEXIT_CRITICAL(&g_status_mux);

        if (down) {
            uint8_t state = was_down ? BRL_TOUCH_MOVE : BRL_TOUCH_DOWN;
            if (!was_down || x != last_x || y != last_y) {
                s.touch_state = state;
                s.touch_x = x;
                s.touch_y = y;
                s.seq++;
                s.flags |= 0x01;
                changed = true;
            }
            was_down = true;
            last_x = x;
            last_y = y;
        } else if (was_down) {
            s.touch_state = BRL_TOUCH_UP;
            s.touch_x = last_x;
            s.touch_y = last_y;
            s.seq++;
            s.flags &= ~0x01;
            changed = true;
            was_down = false;
        }

        if (changed) {
            brl_status_finalize(&s);
            portENTER_CRITICAL(&g_status_mux);
            g_status = s;
            portEXIT_CRITICAL(&g_status_mux);
            // Assert IRQ (active low) so the master knows to poll.
            digitalWrite(PIN_TOUCH_IRQ, LOW);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* ============================ Apply a command ============================ */
static void apply_header(const brl_header_t *h) {
    switch (h->opcode) {
    case BRL_OP_TILE: {
        uint32_t len = brl_payload_len(h);
        if (len == 0 || len > BRL_MAX_PAYLOAD) return;
        // Second transaction: the pixel payload.
        if (spi_slave_xfer(nullptr, rx_payload, len) != ESP_OK) return;
        lcd.pushImage(h->x, h->y, h->w, h->h, (uint16_t *)rx_payload);
        break;
    }
    case BRL_OP_FILLRECT:
        lcd.fillRect(h->x, h->y, h->w, h->h, h->arg0);
        break;
    case BRL_OP_BACKLIGHT:
        lcd.setBrightness(h->arg0 & 0xFF);
        break;
    case BRL_OP_SLEEP:
        lcd.sleep(); // arg0==1; wake handled via BRL_OP_BACKLIGHT/next tile
        if (h->arg0 == 0) lcd.wakeup();
        break;
    case BRL_OP_ROTATION:
        lcd.setRotation(h->arg0 & 0x03);
        break;
    case BRL_OP_POLLTOUCH:
        // Master collected the status we just sent full-duplex; drop IRQ.
        digitalWrite(PIN_TOUCH_IRQ, HIGH);
        break;
    case BRL_OP_SYNC:
    case BRL_OP_PING:
    default:
        break;
    }
}

/* ================================ setup ================================= */
void setup() {
    Serial.begin(115200);

    pinMode(PIN_TOUCH_IRQ, OUTPUT);
    digitalWrite(PIN_TOUCH_IRQ, HIGH); // idle high (no pending touch)

    lcd.init();
    lcd.setRotation(0);
    lcd.setSwapBytes(true); // master (TFT_eSPI sprite) ships byte-swapped RGB565
    lcd.fillScreen(TFT_BLACK);
    lcd.setBrightness(255);

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    pinMode(TOUCH_INT, INPUT);

    // Seed the status block the master will read.
    brl_status_t s = {};
    s.panel_w = BRL_PANEL_W;
    s.panel_h = BRL_PANEL_H;
    s.touch_state = BRL_TOUCH_UP;
    brl_status_finalize(&s);
    g_status = s;

    spi_slave_init();
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, nullptr, 2, nullptr, 0);
}

/* ================================= loop ================================= */
void loop() {
    // Prime this control transaction's MISO with the freshest status.
    portENTER_CRITICAL(&g_status_mux);
    memcpy(tx_ctrl, (const void *)&g_status, 16);
    portEXIT_CRITICAL(&g_status_mux);

    // Block until the master sends a 16-byte control frame (full-duplex: it
    // simultaneously clocks our status block out on MISO).
    if (spi_slave_xfer(tx_ctrl, rx_ctrl, 16) != ESP_OK) return;

    brl_header_t h;
    memcpy(&h, rx_ctrl, sizeof h);
    if (!brl_header_valid(&h)) return; // resync on next transaction
    apply_header(&h);
}
