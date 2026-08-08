/*
 * remote_canvas.cpp  --  implementation of the headless PSRAM canvas + SPI link
 * for the Bruce split-display master. See remote_canvas.h.
 */
#include "remote_canvas.h"

#ifdef USE_REMOTE_CANVAS
#include <Arduino.h>
#include <SPI.h>
#include <esp_rom_crc.h>

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
#define REMOTE_LINK_HZ 20000000 /* 20 MHz; raise once the link is proven */
#endif
#ifndef REMOTE_LINK_GAP_US
#define REMOTE_LINK_GAP_US 40 /* header->payload gap so the slave can re-arm RX */
#endif
#ifndef REMOTE_LINK_FLUSH_MS
#define REMOTE_LINK_FLUSH_MS 20 /* ~50 Hz band-diff cadence */
#endif
#ifndef REMOTE_LINK_SPI_BUS
#define REMOTE_LINK_SPI_BUS HSPI
#endif

/* ------------------------------ Link state ------------------------------- */
static SPIClass linkSPI(REMOTE_LINK_SPI_BUS);
static SemaphoreHandle_t linkMux = nullptr;
static uint16_t *g_fb = nullptr;
static int g_w = 0, g_h = 0;
static uint32_t bandCrc[BRL_NUM_BANDS];
static uint8_t g_seq = 0;

/* Forward decl (called by tft_display::ensureInit below). */
static void remote_canvas_begin(uint16_t *fb, int w, int h);

/* --------------------------- Low-level frames ---------------------------- */
static void send_control(brl_header_t *h, brl_status_t *rxstatus) {
    if (!linkMux) return;
    h->seq = g_seq++;
    brl_header_finalize(h);
    uint8_t rx[16];
    xSemaphoreTake(linkMux, portMAX_DELAY);
    linkSPI.beginTransaction(SPISettings(REMOTE_LINK_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(REMOTE_LINK_CS, LOW);
    linkSPI.transferBytes((const uint8_t *)h, rx, 16); /* full-duplex: MISO = status */
    digitalWrite(REMOTE_LINK_CS, HIGH);
    linkSPI.endTransaction();
    xSemaphoreGive(linkMux);
    if (rxstatus) memcpy(rxstatus, rx, 16);
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
    linkSPI.beginTransaction(SPISettings(REMOTE_LINK_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(REMOTE_LINK_CS, LOW);
    linkSPI.writeBytes((const uint8_t *)&hd, 16); /* header transaction */
    digitalWrite(REMOTE_LINK_CS, HIGH);
    delayMicroseconds(REMOTE_LINK_GAP_US); /* let the slave post its payload RX */
    digitalWrite(REMOTE_LINK_CS, LOW);
    linkSPI.writeBytes((const uint8_t *)px, plen); /* pixel transaction */
    digitalWrite(REMOTE_LINK_CS, HIGH);
    linkSPI.endTransaction();
    xSemaphoreGive(linkMux);
}

/* ------------------------------ Public API ------------------------------- */
void remote_canvas_send_backlight(uint8_t level) {
    brl_header_t h = {};
    h.opcode = BRL_OP_BACKLIGHT;
    h.arg0 = level;
    send_control(&h, nullptr);
}

void remote_canvas_send_sleep(bool on) {
    brl_header_t h = {};
    h.opcode = BRL_OP_SLEEP;
    h.arg0 = on ? 1 : 0;
    send_control(&h, nullptr);
}

void remote_canvas_send_rotation(uint8_t r) {
    brl_header_t h = {};
    h.opcode = BRL_OP_ROTATION;
    h.arg0 = r & 0x03;
    send_control(&h, nullptr);
}

bool remote_canvas_poll_touch(brl_status_t *out) {
    brl_header_t h = {};
    h.opcode = BRL_OP_POLLTOUCH;
    brl_status_t st;
    send_control(&h, &st);
    if (!brl_status_valid(&st)) return false;
    if (out) *out = st;
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
    pinMode(REMOTE_LINK_IRQ, INPUT_PULLUP);
    linkSPI.begin(REMOTE_LINK_SCLK, REMOTE_LINK_MISO, REMOTE_LINK_MOSI, -1);

    // Band geometry must match the shared protocol.
    if (w != BRL_PANEL_W || h != BRL_PANEL_H) {
        log_e("remote_canvas: canvas %dx%d != protocol %dx%d", w, h, BRL_PANEL_W, BRL_PANEL_H);
    }

    brl_header_t hs = {};
    hs.opcode = BRL_OP_SYNC;
    brl_status_t st;
    send_control(&hs, &st);
    // The slave reports the canvas geometry it expects; mismatch means a
    // wrong/stale slave firmware is flashed (e.g. a portrait proto-v1 build).
    if (brl_status_valid(&st) && (st.panel_w != BRL_PANEL_W || st.panel_h != BRL_PANEL_H)) {
        log_e("remote_canvas: slave reports %dx%d, expected %dx%d (proto v%d) -- flash mismatch?",
              st.panel_w, st.panel_h, BRL_PANEL_W, BRL_PANEL_H, st.proto);
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

void tft_display::ensureInit() {
    if (_ready) return;
    _ready = true;
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
