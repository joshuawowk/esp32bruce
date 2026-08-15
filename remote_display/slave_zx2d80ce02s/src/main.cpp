/*
 * Bruce split-display SLAVE firmware  --  PanelLan ZX2D80CE02S (SC05_X)
 * ---------------------------------------------------------------------------
 * Role: dumb pixel blitter + touch return. Receives RGB565 tiles from the
 * master over SPI (this board = SPI peripheral, RX only) and paints them on the
 * parallel ST7789; reads the FT6336 cap-touch and TXes touch packets to the
 * master over a one-way UART on pin 13 (the old MISO wire).
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
#include <driver/gpio.h> // gpio_set_drive_capability (weaken pin-13 UART edges -> less SPI crosstalk)

#include "bruce_remote_link.h" // shared with the master (see -I.. in platformio.ini)

// Touch return runs over a UART on pin 13 (see UART section below), NOT over SPI MISO:
// the ESP32-S3 spi_slave DMA cannot clock varying multi-byte data back to the Arduino
// master (confirmed on hw). The SPI link is now master->slave only (tiles + commands).

// The ST7789 is native PORTRAIT; we scan it in landscape (setRotation 1/3) so the
// master's landscape 320x240 stream (BRL_PANEL_W x BRL_PANEL_H) maps 1:1. Native
// size is a slave-only detail, kept out of the wire protocol.
#define SLAVE_NATIVE_W 240
#define SLAVE_NATIVE_H 320
#define SLAVE_ROT_NORMAL 1 // landscape; flip to 3 on bring-up if the image is upside down
#define SLAVE_ROT_FLIP 3   // landscape rotated 180 (Bruce "Landscape (180)")

// Boot-time R/G/B/W panel self-test (see setup()). Runs BEFORE any SPI activity so
// it proves the parallel bus + panel init independent of the master. Set to 0 once
// the panel is confirmed good to skip the ~2.5s sweep.
#define SLAVE_SELFTEST 1

/* ================= Panel (parallel ST7789, dual-bank 8080) ================= */

// The manufacturer's own LovyanGFX driver for this exact board (smartpanle/
// PanelLan_esp32_arduino, board sc05_x) subclasses Panel_ST7789 to ship a
// panel-specific init sequence (power/VCOM/gamma). The stock Panel_ST7789 init uses
// generic values; on this glass that can leave the controller in power-on GRAM noise
// (snow). Use the vendor bytes verbatim so our bring-up matches the proven driver.
class Panel_SC05X : public lgfx::Panel_ST7789 {
protected:
    const uint8_t *getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0x11, 0 + CMD_INIT_DELAY, 120, // SLPOUT + 120ms
            0x36, 1, 0x00,                 // MADCTL (setRotation overrides this)
            0x3A, 1, 0x05,                 // COLMOD = 16bpp
            0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33, // PORCTRL
            0xB7, 1, 0x46,                 // GCTRL
            0xBB, 1, 0x1B,                 // VCOMS
            0xC0, 1, 0x2C,                 // LCMCTRL
            0xC2, 1, 0x01,                 // VDVVRHEN
            0xC3, 1, 0x0F,                 // VRHS
            0xC4, 1, 0x20,                 // VDVS
            0xC6, 1, 0x0F,                 // FRCTRL2
            0xD0, 2, 0xA4, 0xA1,           // PWCTRL1
            0xD6, 1, 0xA1,
            0xE0, 14, 0xF0, 0x00, 0x06, 0x04, 0x05, 0x05, 0x31, 0x44, 0x48, 0x36, 0x12, 0x12, 0x2B, 0x34, // PVGAM
            0xE1, 14, 0xF0, 0x0B, 0x0F, 0x0F, 0x0D, 0x26, 0x31, 0x43, 0x47, 0x38, 0x14, 0x14, 0x2C, 0x32, // NVGAM
            0x21, 0,                       // INVON (matches cfg.invert = true)
            0x29, 0,                       // DISPON
            0x2C, 0,                       // RAMWR
            0xFF, 0xFF,                    // end
        };
        return listno == 0 ? list0 : nullptr;
    }
};

class LGFX : public lgfx::LGFX_Device {
    Panel_SC05X _panel;
    lgfx::Bus_Parallel8 _bus;
    lgfx::Light_PWM _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 20000000; // vendor sc05_x known-good; 40MHz overclocked the ST7789 8080 write cycle -> snow
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
            cfg.panel_width = SLAVE_NATIVE_W;  // native portrait; rotated to landscape at runtime
            cfg.panel_height = SLAVE_NATIVE_H;
            cfg.memory_width = SLAVE_NATIVE_W;  // GRAM addressing extents (vendor sc05_x sets these)
            cfg.memory_height = SLAVE_NATIVE_H;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            // NOTE: the vendor driver uses offset_rotation=2, but we drive orientation
            // explicitly via setRotation(SLAVE_ROT_NORMAL/FLIP) + map_touch, which assume
            // the offset_rotation=0 default. Leave it at 0 so those transforms stay valid;
            // orientation is tuned after a coherent image (full-screen fills don't care).
            cfg.dummy_read_pixel = 8; // vendor sc05_x (harmless here: readable=false)
            cfg.dummy_read_bits = 1;
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
static void ft_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
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

// The FT6336 reports in the panel's NATIVE portrait frame (rx in [0,SLAVE_NATIVE_W),
// ry in [0,SLAVE_NATIVE_H)). Rotate it into the master's landscape 320x240 canvas
// coordinates so touch lines up with what's drawn. Mirrors the standard ST7789
// rotation-1/3 transform. NOTE: exact axis flips depend on how the FT6336 is
// mounted -- if touch is mirrored on bring-up, negate the affected axis here.
static void map_touch(int16_t rx, int16_t ry, uint8_t rot, uint16_t &lx, uint16_t &ly) {
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= SLAVE_NATIVE_W) rx = SLAVE_NATIVE_W - 1;
    if (ry >= SLAVE_NATIVE_H) ry = SLAVE_NATIVE_H - 1;
    if (rot == SLAVE_ROT_FLIP) { // landscape 180
        lx = (uint16_t)((SLAVE_NATIVE_H - 1) - ry);
        ly = (uint16_t)rx;
    } else { // SLAVE_ROT_NORMAL, landscape
        lx = (uint16_t)ry;
        ly = (uint16_t)((SLAVE_NATIVE_W - 1) - rx);
    }
}

/* ================= SPI slave (RX-only) + UART touch return ================ */
// EXT-IO header pins (the only free general IO on the board).
#define PIN_SPI_CS 10
#define PIN_SPI_SCLK 12
#define PIN_SPI_MOSI 11
#define PIN_LINK_UART_TX 13 // was SPI MISO; now a one-way UART TX to the master (touch packets)
#define PIN_TOUCH_IRQ 14    // legacy slave->master IRQ; unused now (touch is pushed over UART)

#define SPI_SLAVE_HOST SPI2_HOST

// UART to the master for touch return on pin 13. This standalone board runs no Bruce modules,
// so its hardware UART1 is free; the master listens on its UART2 (see remote_canvas.cpp).
static HardwareSerial LinkTx(1);

// DMA-capable RX buffer (word-aligned). One self-framing transaction per CS frame; the master
// only sends (tiles + commands), so there is no MISO/TX buffer anymore.
static WORD_ALIGNED_ATTR uint8_t rx_frame[BRL_MAX_FRAME];

// Current landscape scan rotation (SLAVE_ROT_NORMAL / SLAVE_ROT_FLIP). Written by the SPI loop
// on BRL_OP_ROTATION, read by the touch task; a single aligned byte so accesses are atomic.
static uint8_t g_rotation = SLAVE_ROT_NORMAL;
static uint8_t g_touch_seq = 0; // ++ per distinct touch event; only touched by the touch task
// micros() of the last received tile. The touch task defers UART TX while tiles are actively
// streaming so the pin-13 UART edges don't crosstalk onto the concurrent MOSI/SCLK tile data.
static volatile uint32_t g_last_tile_us = 0;

// Send one framed touch packet to the master over the UART. Sent twice for redundancy (touch is
// low-rate and the master dedups by seq, so a duplicate is free insurance against a dropped byte).
static void link_send_touch(uint8_t state, uint16_t x, uint16_t y, uint8_t seq) {
    brl_touch_pkt_t p = {};
    p.state = state;
    p.x = x;
    p.y = y;
    p.seq = seq;
    brl_touch_finalize(&p);
    LinkTx.write((const uint8_t *)&p, sizeof p);
    LinkTx.write((const uint8_t *)&p, sizeof p);
}

static void spi_slave_init() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_SPI_MOSI;
    buscfg.miso_io_num = -1; // RX-only: pin 13 is now the touch UART, not SPI MISO
    buscfg.sclk_io_num = PIN_SPI_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = BRL_MAX_FRAME;

    spi_slave_interface_config_t slvcfg = {};
    // MUST be mode 1 (or 3) whenever DMA is used: the ESP32-S3 spi_slave DMA RX path cannot
    // sample MOSI on the first clock edge in modes 0/2 -> it drops the MSB and reads every
    // frame shifted left 1 bit (magic 0xB2D5 arrives as 0x65AA -> all frames rejected -> black
    // screen). Espressif: "DMA requires SPI modes 1 and 3." Must match the master's
    // REMOTE_LINK_SPI_MODE (SPI_MODE1) in lib/HAL/display/remote_canvas.cpp.
    slvcfg.mode = 1;
    slvcfg.spics_io_num = PIN_SPI_CS;
    slvcfg.queue_size = 3;
    slvcfg.flags = 0;

    ESP_ERROR_CHECK(spi_slave_initialize(SPI_SLAVE_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));
}

// Arm ONE self-framing RX transaction: up to BRL_MAX_FRAME bytes, CS-delimited. Returns bytes
// received (trans_len). No TX buffer -- the master never reads MISO.
static size_t spi_slave_frame() {
    spi_slave_transaction_t t = {};
    t.length = BRL_MAX_FRAME * 8; // max bits; actual set in trans_len on CS deassert
    t.tx_buffer = nullptr;
    t.rx_buffer = rx_frame;
    if (spi_slave_transmit(SPI_SLAVE_HOST, &t, portMAX_DELAY) != ESP_OK) return 0;
    return t.trans_len / 8;
}

/* ============================== Touch task =============================== */
// Polls the FT6336; on each distinct DOWN/MOVE/UP event, maps to canvas coords and latches it, then
// TXes it to the master over the UART *only when the SPI tile bus is idle* (so the pin-13 UART edges
// don't crosstalk onto concurrent tile data -> menu corruption during touch-driven redraws). A slow
// heartbeat confirms link liveness; the master dedups by seq, so a heartbeat never registers as a touch.
static void touch_task(void *) {
    int16_t last_x = -1, last_y = -1;
    bool was_down = false;
    uint32_t last_send = 0;
    bool pend = false; // a distinct event is latched, waiting for an idle SPI window
    uint8_t p_state = BRL_TOUCH_UP, p_seq = 0;
    uint16_t p_x = 0, p_y = 0;
    for (;;) {
        int16_t x, y;
        bool down = ft_read_touch(x, y);
        uint8_t rot = g_rotation;
        uint16_t lx = 0, ly = 0;

        if (down) {
            uint8_t st = was_down ? BRL_TOUCH_MOVE : BRL_TOUCH_DOWN;
            if (!was_down || x != last_x || y != last_y) {
                map_touch(x, y, rot, lx, ly); // -> landscape canvas coords
                p_state = st;
                p_x = lx;
                p_y = ly;
                p_seq = ++g_touch_seq;
                pend = true;
            }
            was_down = true;
            last_x = x;
            last_y = y;
        } else if (was_down) {
            map_touch(last_x, last_y, rot, lx, ly);
            p_state = BRL_TOUCH_UP;
            p_x = lx;
            p_y = ly;
            p_seq = ++g_touch_seq;
            pend = true;
            was_down = false;
        }

        // Hold off the UART while tiles are actively streaming (idle for >12ms == burst over).
        bool tiles_busy = (uint32_t)(micros() - g_last_tile_us) < 12000u;
        if (!tiles_busy) {
            if (pend) {
                link_send_touch(p_state, p_x, p_y, p_seq);
                pend = false;
                last_send = millis();
            } else if (millis() - last_send >= 2000) {
                last_send = millis();
                link_send_touch(BRL_TOUCH_UP, 0, 0, g_touch_seq); // slow heartbeat (same seq -> ignored)
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* ============================ Apply a command ============================ */
// Self-framing: the pixel payload (if any) already arrived in the SAME transaction, right
// after the 16-byte header; `payload`/`payload_len` point at it.
static void apply_header(const brl_header_t *h, const uint8_t *payload, size_t payload_len) {
    switch (h->opcode) {
    case BRL_OP_TILE: {
        g_last_tile_us = micros(); // mark SPI-bus activity so the touch task holds off UART TX
        uint32_t len = brl_payload_len(h);
        if (len == 0 || len > BRL_MAX_PAYLOAD || payload_len < len) return;
        lcd.pushImage(h->x, h->y, h->w, h->h, (const uint16_t *)payload);
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
    case BRL_OP_ROTATION: {
        // The canvas is always landscape; the master only ever means "normal" or
        // "180". bit1 of its rotation selects the flip (0/1 -> normal, 2/3 -> 180);
        // the portrait bit is ignored (portrait is locked out on the master).
        uint8_t sr = (h->arg0 & 0x02) ? SLAVE_ROT_FLIP : SLAVE_ROT_NORMAL;
        lcd.setRotation(sr);
        g_rotation = sr;
        break;
    }
    case BRL_OP_POLLTOUCH: // legacy no-op: touch is now pushed over the UART, not polled on SPI
    case BRL_OP_SYNC:
    case BRL_OP_PING:
    default:
        break;
    }
}

/* ================================ setup ================================= */
void setup() {
    Serial.begin(115200);
    delay(2000); // let the native USB-CDC (ARDUINO_USB_CDC_ON_BOOT) enumerate so the banner survives
    Serial.println("\n[slave] boot: ZX2D80CE02S split-display blitter");

    // Touch-return UART on pin 13 (TX only; the master never sends to us here).
    LinkTx.begin(BRL_UART_BAUD, SERIAL_8N1, /*rx=*/-1, /*tx=*/PIN_LINK_UART_TX);
    // Weakest drive strength: gentler edges on the pin-13 wire -> less capacitive crosstalk onto the
    // adjacent MOSI/SCLK tile lines. Fine for reception over the short jumper.
    gpio_set_drive_capability((gpio_num_t)PIN_LINK_UART_TX, GPIO_DRIVE_CAP_0);
    Serial.printf("[slave] touch UART TX on pin %d @ %d baud\n", PIN_LINK_UART_TX, BRL_UART_BAUD);

    bool ok = lcd.init();
    Serial.printf("[slave] lcd.init() = %d\n", ok);
    lcd.setRotation(SLAVE_ROT_NORMAL); // landscape 320x240 to match the master's canvas
    g_rotation = SLAVE_ROT_NORMAL;
    lcd.setSwapBytes(true); // master (TFT_eSPI sprite) ships byte-swapped RGB565
    lcd.setBrightness(255);

#if SLAVE_SELFTEST
    // Panel self-test, BEFORE any SPI activity: prove the parallel bus + panel init
    // work, decoupled from the master. Full-screen fills are orientation-independent,
    // so this is valid regardless of rotation. Solid colors here => the panel is good
    // and any remaining fault is in the SPI/tile path; screen stays snowy => the panel
    // bring-up itself is still failing (see remote_display/README.md bring-up section).
    Serial.println("[slave] panel self-test: R/G/B/W sweep");
    lcd.fillScreen(TFT_RED);   delay(300);
    lcd.fillScreen(TFT_GREEN); delay(300);
    lcd.fillScreen(TFT_BLUE);  delay(300);
    lcd.fillScreen(TFT_WHITE); delay(300);
    Serial.println("[slave] panel self-test done");
#endif
    lcd.fillScreen(TFT_BLACK);

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    pinMode(TOUCH_INT, INPUT);
    // Put the FT6336 into normal active polling mode (matches the vendor Touch_FT5x06 init):
    // DEVICE_MODE=working, PWR=active (out of monitor/standby), G_MODE=polling (INT asserted while
    // touched), and disable auto-switch to monitor mode.
    delay(50);
    ft_write(0x00, 0x00); // DEVICE_MODE = normal working
    ft_write(0xA5, 0x00); // PWR_MODE = active
    ft_write(0x86, 0x00); // CTRL = no auto monitor
    ft_write(0xA4, 0x00); // G_MODE = polling

    spi_slave_init();
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, nullptr, 2, nullptr, 0);
    Serial.println("[slave] setup complete; RX tiles over SPI, TX touch over UART");
}

/* ================================= loop ================================= */
void loop() {
    // Block until the master clocks one CS-framed message (header + optional payload).
    size_t nbytes = spi_slave_frame();
    if (nbytes < 16) return; // runt / no frame

    brl_header_t h;
    memcpy(&h, rx_frame, sizeof h);
    if (!brl_header_valid(&h)) return; // realigns automatically on the next CS frame
    apply_header(&h, rx_frame + 16, nbytes - 16);
}
