/*
 * remote_canvas.h  --  Headless "remote canvas" TFT backend for the Bruce
 * split-display MASTER.
 *
 * The master has no local panel. Instead the global `tft` object is a
 * full-screen TFT_eSprite living in PSRAM: every one of Bruce's ~2500 tft.*
 * draw calls renders real pixels into that RAM buffer. A background task diffs
 * the buffer in horizontal bands and streams only the changed bands over SPI to
 * the ZX2D80CE02S slave, which blits them to its parallel ST7789. Touch comes
 * back over the same link.
 *
 * Selected by -DUSE_REMOTE_CANVAS (see lib/HAL/display/tft.h). Requires
 * -DHAS_SCREEN so tft_logger uses tft_display (this class) as its base, and
 * -I<repo>/remote_display so bruce_remote_link.h is found.
 *
 * KNOWN LIMITATION (v1): sub-sprite compositing via sprite.pushSprite() targets
 * TFT_eSPI's (headless) parent panel, not this canvas, so it renders nowhere.
 * The only core user is the tururururu.cpp shark easter-egg; the entire real UI
 * draws directly to tft and is unaffected.
 */
#ifndef LIB_HAL_REMOTE_CANVAS_H
#define LIB_HAL_REMOTE_CANVAS_H
#include <pins_arduino.h>

#ifdef USE_REMOTE_CANVAS
#include <TFT_eSPI.h>

#include "bruce_remote_link.h"
#include "tft_defines.h"

// The global canvas. Public inheritance from TFT_eSprite gives tft_logger the
// full TFT_eSPI/TFT_eSprite drawing API, all rendering into the RAM buffer.
class tft_display : public TFT_eSprite {
public:
    explicit tft_display(int16_t _W = TFT_WIDTH, int16_t _H = TFT_HEIGHT);
    friend class tft_logger;
    friend class tft_sprite;

    // Panel shims: no local panel, so init()/begin() allocate the canvas and
    // bring up the SPI link; writecommand() maps the few commands Bruce issues
    // (sleep in/out) to link messages.
    void init(uint8_t tc = 0);
    void begin(uint8_t tc = 0);
    void writecommand(uint8_t c);
    void invertDisplay(bool i);
    void setRotation(uint8_t r);
    uint8_t getRotation();

    // Extras tft_logger expects (not present in stock TFT_eSPI).
    uint32_t getTextColor() const;
    uint32_t getTextBgColor() const;
    uint8_t getTextSize() const;
    TFT_eSPI *native();

    // Raw RGB565 framebuffer (valid after init()).
    uint16_t *frameBuffer();

    // Keep TFT_eSprite's uint16_t* overloads visible, and add the 8bpp+palette
    // forms Bruce's JS interpreter calls. TFT_eSprite lacks them (only TFT_eSPI
    // has them, and that targets the dead parent panel), so render pixel-by-
    // pixel into the canvas buffer -- exactly as the TFT_eSPI backend's
    // tft_sprite does.
    using TFT_eSprite::pushImage;
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t *data, bool bpp8, uint16_t *cmap) {
        if (!data || !bpp8 || !cmap) return;
        for (int32_t row = 0; row < h; ++row)
            for (int32_t col = 0; col < w; ++col) drawPixel(x + col, y + row, cmap[data[row * w + col]]);
    }
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, bool bpp8, uint16_t *cmap) {
        pushImage(x, y, w, h, const_cast<uint8_t *>(data), bpp8, cmap);
    }

private:
    int16_t _cw, _ch;
    uint8_t _rot = 0;
    bool _ready = false;
    static TFT_eSPI &parent();
    void ensureInit();
};

// Nested sprites (the `sprite`/`draw` globals + app scratch sprites). Parent is
// the canvas; direct drawing works, pushSprite() has the limitation noted above.
class tft_sprite : public TFT_eSprite {
public:
    explicit tft_sprite(tft_display *parent) : TFT_eSprite(parent) {}

    using TFT_eSprite::pushImage;
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t *data, bool bpp8, uint16_t *cmap) {
        if (!data || !bpp8 || !cmap) return;
        for (int32_t row = 0; row < h; ++row)
            for (int32_t col = 0; col < w; ++col) drawPixel(x + col, y + row, cmap[data[row * w + col]]);
    }
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, bool bpp8, uint16_t *cmap) {
        pushImage(x, y, w, h, const_cast<uint8_t *>(data), bpp8, cmap);
    }
};

// ---- SPI link control surface (implemented in remote_canvas.cpp) ----
// Called by the board interface.cpp.
void remote_canvas_send_backlight(uint8_t level);
void remote_canvas_send_sleep(bool on);
void remote_canvas_send_rotation(uint8_t r);
// Sends BRL_OP_POLLTOUCH and returns the slave's status block. Returns true iff
// a checksum-valid status was read.
bool remote_canvas_poll_touch(brl_status_t *out);

#endif // USE_REMOTE_CANVAS
#endif // LIB_HAL_REMOTE_CANVAS_H
