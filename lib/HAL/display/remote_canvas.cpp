/*
 * remote_canvas.cpp  --  implementation of the headless PSRAM canvas + SPI link
 * for the Bruce split-display master. See remote_canvas.h.
 */
#include "remote_canvas.h"

#ifdef USE_REMOTE_CANVAS
#include <Arduino.h>
#include <SPI.h>
#include <esp_rom_crc.h>
#include <esp_rom_sys.h> // esp_rom_printf -> UART0/CH343/ttyACM0 bring-up logs (master native USB absent)

// Log each received UART touch packet to UART0 (CH343/ttyACM0) for bring-up verification. Set 0 to silence.
#ifndef BRL_TOUCH_LOG
#define BRL_TOUCH_LOG 1
#endif

/* ----------------------- Link pin / timing config ------------------------ */
/* Override any of these in the board's pins_arduino.h. Defaults target the
 * ESP32-S3 pins that avoid the octal-PSRAM range (26..37) on an N16R8 master. */
#ifndef REMOTE_LINK_SCLK
#define REMOTE_LINK_SCLK 12
#endif
#ifndef REMOTE_LINK_MOSI
#define REMOTE_LINK_MOSI 11
#endif
#ifndef REMOTE_LINK_MISO
#define REMOTE_LINK_MISO 13
#endif
#ifndef REMOTE_LINK_CS
#define REMOTE_LINK_CS 10
#endif
#ifndef REMOTE_LINK_IRQ
#define REMOTE_LINK_IRQ 14
#endif
#ifndef REMOTE_LINK_HZ
#define REMOTE_LINK_HZ 10000000 /* 10 MHz bring-up (was 20); raise once the link is proven */
#endif
#ifndef REMOTE_LINK_CTRL_HZ
/* Control frames (send_control: backlight/sleep/rotation) are write-only now -- touch comes back
 * over the UART, not MISO -- so this clock is not timing-critical. Kept modest (2 MHz). Tiles
 * (send_tile) are also write-only and run at REMOTE_LINK_HZ. */
#define REMOTE_LINK_CTRL_HZ 2000000
#endif
#ifndef REMOTE_LINK_SPI_MODE
/* MUST be mode 1 or 3: the ESP32-S3 spi_slave DMA RX path cannot sample MOSI on the first
 * clock edge in modes 0/2 -> it drops the MSB and reads every frame shifted left 1 bit
 * (magic 0xB2D5 arrives as 0x65AA). Espressif: "DMA requires SPI modes 1 and 3." The slave
 * (slvcfg.mode) MUST match this exactly. */
#define REMOTE_LINK_SPI_MODE SPI_MODE1
#endif
#ifndef REMOTE_LINK_GAP_US
/* Header->payload gap: the slave must return from the header transaction, parse it, and
 * re-arm its RX DMA before the master clocks the payload. 40us was too tight (garbled
 * tiles); 500us gives generous margin for bring-up. Reduce once the link is proven. */
#define REMOTE_LINK_GAP_US 500
#endif
#ifndef REMOTE_LINK_FLUSH_MS
#define REMOTE_LINK_FLUSH_MS 20 /* ~50 Hz band-diff cadence */
#endif
#ifndef REMOTE_LINK_TILE_GAP_MS
/* Inter-tile pacing: after a band's payload the slave spends ~0.5-1ms in pushImage()
 * blitting it to the panel, during which it is NOT armed for the next SPI transaction.
 * The master must wait before clocking the next tile or the slave misses the header and
 * desyncs permanently (black screen). Generous for bring-up; reduce once proven. */
#define REMOTE_LINK_TILE_GAP_MS 3
#endif
#ifndef REMOTE_LINK_SPI_BUS
#define REMOTE_LINK_SPI_BUS HSPI
#endif

/* Touch-return link: the slave TXes framed touch packets on the pin-13 wire (formerly SPI MISO),
 * which the master receives on hardware UART2. UART2 is dedicated to touch on the split-display
 * master: Bruce's GPS/NRF24 modules (the other UART2 users) are moved to UART1 on this board (see
 * the USE_REMOTE_CANVAS guards in gps_tracker.h / wardriving.h / nrf_common.cpp), so nothing else
 * ever reprograms UART2 -> touch never conflicts. */
#ifndef REMOTE_LINK_UART_RX
#define REMOTE_LINK_UART_RX REMOTE_LINK_MISO /* pin 13 */
#endif
#ifndef REMOTE_LINK_UART_NUM
#define REMOTE_LINK_UART_NUM 2
#endif

/* ------------------------------ Link state ------------------------------- */
static SPIClass linkSPI(REMOTE_LINK_SPI_BUS);
static SemaphoreHandle_t linkMux = nullptr;
static uint16_t *g_fb = nullptr;
static int g_w = 0, g_h = 0;
static uint32_t bandCrc[BRL_NUM_BANDS];
static uint8_t g_seq = 0;

// Touch UART receiver + framing state (only touched by the InputHandler task via poll_touch).
static HardwareSerial LinkRx(REMOTE_LINK_UART_NUM);
static uint8_t g_touch_last_seq = 0;
static bool g_touch_seq_init = false;

/* Forward decl (called by tft_display::ensureInit below). */
static void remote_canvas_begin(uint16_t *fb, int w, int h);

/* --------------------------- Low-level frames ---------------------------- */
// Control frames are now WRITE-ONLY (backlight/sleep/rotation). The master no longer reads the
// slave over SPI -- touch comes back over the UART (see remote_canvas_poll_touch). The old
// full-duplex MISO readback was removed: the ESP32-S3 spi_slave cannot clock varying data back.
static void send_control(brl_header_t *h) {
    if (!linkMux) return;
    h->seq = g_seq++;
    brl_header_finalize(h);
    xSemaphoreTake(linkMux, portMAX_DELAY);
    linkSPI.beginTransaction(SPISettings(REMOTE_LINK_CTRL_HZ, MSBFIRST, REMOTE_LINK_SPI_MODE));
    digitalWrite(REMOTE_LINK_CS, LOW);
    linkSPI.transferBytes((const uint8_t *)h, nullptr, 16);
    digitalWrite(REMOTE_LINK_CS, HIGH);
    linkSPI.endTransaction();
    xSemaphoreGive(linkMux);
}

static void send_tile(int x, int y, int w, int h, const uint16_t *px) {
    if (!linkMux) return;
    brl_header_t hd = {};
    hd.opcode = BRL_OP_TILE;
    hd.x = (uint16_t)x;
    hd.y = (uint16_t)y;
    hd.w = (uint16_t)w;
    hd.h = (uint16_t)h;
    hd.seq = g_seq++;
    brl_header_finalize(&hd);
    const size_t plen = (size_t)w * (size_t)h * 2u;

    xSemaphoreTake(linkMux, portMAX_DELAY);
    linkSPI.beginTransaction(SPISettings(REMOTE_LINK_HZ, MSBFIRST, REMOTE_LINK_SPI_MODE));
    // Self-framing: header + payload in ONE CS assertion (no gap, no inner CS toggle). The slave
    // receives them as one CS-delimited frame and parses the header from the front. This means
    // the slave is only ever armed for ONE kind of transaction (the max-size self-framing xfer,
    // with its status block on MISO), so a concurrent status read (poll_touch) always reads valid
    // status -- there is no "armed for a payload / MISO undriven" window to race.
    digitalWrite(REMOTE_LINK_CS, LOW);
    // Use full-duplex transferBytes (MISO discarded) rather than write-only writeBytes so the SPI
    // peripheral stays in one consistent mode -- a write-only tile followed by a full-duplex status
    // read otherwise leaves the readback returning zeros.
    linkSPI.transferBytes((const uint8_t *)&hd, nullptr, 16); /* 16-byte header ... */
    linkSPI.transferBytes((const uint8_t *)px, nullptr, plen); /* ... immediately followed by the pixels */
    digitalWrite(REMOTE_LINK_CS, HIGH);
    linkSPI.endTransaction();
    xSemaphoreGive(linkMux);
    // Pace between tiles so the slave can blit + re-arm before the next frame.
    vTaskDelay(pdMS_TO_TICKS(REMOTE_LINK_TILE_GAP_MS));
}

/* ------------------------------ Public API ------------------------------- */
void remote_canvas_send_backlight(uint8_t level) {
    brl_header_t h = {};
    h.opcode = BRL_OP_BACKLIGHT;
    h.arg0 = level;
    send_control(&h);
}

void remote_canvas_send_sleep(bool on) {
    brl_header_t h = {};
    h.opcode = BRL_OP_SLEEP;
    h.arg0 = on ? 1 : 0;
    send_control(&h);
}

void remote_canvas_send_rotation(uint8_t r) {
    brl_header_t h = {};
    h.opcode = BRL_OP_ROTATION;
    h.arg0 = r & 0x03;
    send_control(&h);
}

// Drain the touch UART and return the most recent COMPLETE, checksum-valid packet seen in this
// call (there may be several buffered -- heartbeats + events; we keep the last). A tiny sliding
// state machine resyncs on the 2-byte magic. Returns false if no full valid packet was available.
static bool link_read_touch(brl_touch_pkt_t *out) {
    static uint8_t buf[sizeof(brl_touch_pkt_t)];
    static uint8_t n = 0;
    bool got = false;
    while (LinkRx.available() > 0) {
        uint8_t b = (uint8_t)LinkRx.read();
        if (n == 0) {
            if (b == BRL_TOUCH_MAGIC0) buf[n++] = b;
        } else if (n == 1) {
            if (b == BRL_TOUCH_MAGIC1) buf[n++] = b;
            else { n = 0; if (b == BRL_TOUCH_MAGIC0) buf[n++] = b; } // resync (b could be a fresh magic0)
        } else {
            buf[n++] = b;
            if (n == sizeof(brl_touch_pkt_t)) {
                n = 0;
                brl_touch_pkt_t p;
                memcpy(&p, buf, sizeof p);
                if (brl_touch_valid(&p)) {
                    *out = p;
                    got = true; // keep scanning; last valid packet wins
                }
            }
        }
    }
    return got;
}

// Return the latest NEW touch event (seq advanced) over the UART, mapped into `out`. Heartbeats
// (same seq) are ignored. Called from the InputHandler task at ~50-100 Hz; the UART driver buffers
// bytes between calls so nothing is lost. Signature unchanged so board interface.cpp is unaffected.
bool remote_canvas_poll_touch(brl_status_t *out) {
    brl_touch_pkt_t p;
    if (!link_read_touch(&p)) return false;
#if BRL_TOUCH_LOG
    esp_rom_printf("URX state=%d x=%d y=%d seq=%d\n", (int)p.state, (int)p.x, (int)p.y, (int)p.seq);
#endif
    if (g_touch_seq_init && p.seq == g_touch_last_seq) return false; // heartbeat / duplicate
    g_touch_seq_init = true;
    g_touch_last_seq = p.seq;
    if (out) {
        out->touch_state = p.state;
        out->touch_x = p.x;
        out->touch_y = p.y;
        out->seq = p.seq;
    }
    return true;
}

/* ------------------------------ Flush task ------------------------------- */
static void flushTask(void *) {
    for (int i = 0; i < BRL_NUM_BANDS; i++) bandCrc[i] = 0; /* force first full send */
    const int bandPixels = g_w * BRL_BAND_ROWS;
    for (;;) {
        if (g_fb) {
            for (int b = 0; b < BRL_NUM_BANDS; b++) {
                uint16_t *band = g_fb + (size_t)b * bandPixels;
                uint32_t c = esp_rom_crc32_le(0, (const uint8_t *)band, (uint32_t)bandPixels * 2u);
                if (c != bandCrc[b]) {
                    bandCrc[b] = c;
                    send_tile(0, b * BRL_BAND_ROWS, g_w, BRL_BAND_ROWS, band);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(REMOTE_LINK_FLUSH_MS));
    }
}

static void remote_canvas_begin(uint16_t *fb, int w, int h) {
    g_fb = fb;
    g_w = w;
    g_h = h;
    linkMux = xSemaphoreCreateMutex();

    pinMode(REMOTE_LINK_CS, OUTPUT);
    digitalWrite(REMOTE_LINK_CS, HIGH);
    // SPI link is now master->slave only (tiles + control): MISO (pin 13) is freed for the touch
    // UART RX, so pass -1 for MISO. There is no SPI readback / SYNC handshake anymore; the slave's
    // liveness is confirmed by its UART heartbeat instead.
    linkSPI.begin(REMOTE_LINK_SCLK, -1, REMOTE_LINK_MOSI, -1);
    LinkRx.begin(BRL_UART_BAUD, SERIAL_8N1, REMOTE_LINK_UART_RX, /*tx=*/-1); // touch RX on pin 13 (UART2)

    // Band geometry must match the shared protocol.
    if (w != BRL_PANEL_W || h != BRL_PANEL_H) {
        log_e("remote_canvas: canvas %dx%d != protocol %dx%d", w, h, BRL_PANEL_W, BRL_PANEL_H);
    }

    remote_canvas_send_backlight(255);
    xTaskCreatePinnedToCore(flushTask, "canvasFlush", 4096, nullptr, 1, nullptr, 0);
}

/* --------------------------- tft_display impl ---------------------------- */
TFT_eSPI &tft_display::parent() {
    static TFT_eSPI p; // never init()'d: satisfies TFT_eSprite's parent requirement
    return p;
}

tft_display::tft_display(int16_t _W, int16_t _H) : TFT_eSprite(&parent()), _cw(_W), _ch(_H) {}

// TFT_eSPI creates its global recursive `tftMutex` inside TFT_eSPI::init(), which we
// deliberately bypass (ensureInit allocates the sprite instead of touching a panel).
// But the base-class shape helpers that TFT_eSprite does NOT override (drawRect,
// drawCircle, drawRoundRect, fillRoundRect, drawTriangle, ...) still call
// begin_tft_write/end_tft_write, which take/give tftMutex. Left NULL, the very first
// such call aborts (xQueueGiveMutexRecursive assert on a NULL mutex). Create it here
// so those paths are safe no-ops on the headless canvas (CS is a dummy pin; the
// overridden primitives render into the sprite buffer, so no real panel I/O occurs).
extern SemaphoreHandle_t tftMutex;

void tft_display::ensureInit() {
    if (_ready) return;
    _ready = true;
    if (!tftMutex) tftMutex = xSemaphoreCreateRecursiveMutex();
    setColorDepth(16);
    setAttribute(PSRAM_ENABLE, true);
    void *buf = createSprite(_cw, _ch);
    if (!buf) {
        log_e("remote_canvas: createSprite(%d,%d) failed (need PSRAM)", _cw, _ch);
        return;
    }
    fillSprite(TFT_BLACK);
    remote_canvas_begin(frameBuffer(), _cw, _ch);
}

void tft_display::init(uint8_t) { ensureInit(); }
void tft_display::begin(uint8_t) { ensureInit(); }

void tft_display::writecommand(uint8_t c) {
    if (c == 0x10) remote_canvas_send_sleep(true);       // SLPIN
    else if (c == 0x11) remote_canvas_send_sleep(false); // SLPOUT
}

void tft_display::invertDisplay(bool) { /* fixed on the slave panel in v1 */ }

void tft_display::setRotation(uint8_t r) {
    _rot = r & 0x03;
    remote_canvas_send_rotation(_rot);
    // Pinned landscape: the canvas is a fixed 320x240 sprite and its _cw x _ch
    // dimensions never change (we deliberately do NOT call TFT_eSprite::setRotation,
    // so tft.width()/height() stay 320x240 and Bruce lays out landscape). The value
    // only tells the slave which landscape orientation to scan: bit1 picks the 180
    // flip (0/1 -> normal, 2/3 -> flipped); portrait rotations are locked out on the
    // master (LANDSCAPE_LOCK), so they never reach here.
}

uint8_t tft_display::getRotation() { return _rot; }

uint32_t tft_display::getTextColor() const { return textcolor; }
uint32_t tft_display::getTextBgColor() const { return textbgcolor; }
uint8_t tft_display::getTextSize() const { return textsize; }
TFT_eSPI *tft_display::native() { return &parent(); }

uint16_t *tft_display::frameBuffer() { return (uint16_t *)getPointer(); }

#endif // USE_REMOTE_CANVAS
