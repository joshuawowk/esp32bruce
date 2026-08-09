/*
 * bruce_remote_link.h  --  Shared SPI wire protocol for the Bruce split-display
 * ("remote canvas") architecture.
 *
 *   MASTER (radios, runs full Bruce, renders into a PSRAM framebuffer)
 *      --- SPI (master = controller) --->  SLAVE (ZX2D80CE02S, dumb blitter)
 *      <-- TOUCH_IRQ + MISO status block --
 *
 * This header is intentionally dependency-free (only <stdint.h>) so the exact
 * same definitions compile on both firmwares AND host-compile for tests.
 *
 * All multi-byte fields are LITTLE-ENDIAN (both ends are ESP32-S3 / LE, so the
 * packed structs map 1:1 to the wire with no marshalling).
 *
 * Wire model
 * ----------
 * Every master->slave message begins with a fixed 16-byte brl_header_t.
 * For BRL_OP_TILE, exactly (w*h*2) bytes of RGB565 pixel payload follow the
 * header (as a second SPI transaction). No other opcode carries a payload.
 *
 * Slave->master data (touch + panel info) travels as a fixed 16-byte
 * brl_status_t clocked out on MISO in response to BRL_OP_POLLTOUCH / BRL_OP_SYNC.
 * The slave keeps this block primed so a read always returns current state; it
 * raises TOUCH_IRQ (active low) when `seq` has advanced (new event to collect).
 */
#ifndef BRUCE_REMOTE_LINK_H
#define BRUCE_REMOTE_LINK_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Versioning + panel geometry                                         */
/* ------------------------------------------------------------------ */
#define BRL_PROTO_VERSION 2 /* v2: streamed canvas is landscape 320x240 */

/* Streamed canvas geometry -- the coordinate system BOTH ends agree on. The
 * master renders full Bruce into a landscape 320x240 PSRAM sprite and ships it;
 * touch is reported back in these same coordinates. The slave's physical ST7789
 * is 240x320 *native portrait* -- it rotates to landscape (setRotation 1/3) so
 * this 320x240 stream maps 1:1 onto its logical framebuffer. That native size is
 * a slave-only detail (SLAVE_NATIVE_W/H in slave_zx2d80ce02s/src/main.cpp), not
 * part of the wire protocol. */
#define BRL_PANEL_W 320 /* streamed canvas width  (landscape) */
#define BRL_PANEL_H 240 /* streamed canvas height (landscape) */

/* Dirty-rectangle streaming: the framebuffer is diffed in horizontal
 * bands of BRL_BAND_ROWS rows; only changed bands are transmitted. */
#define BRL_BAND_ROWS 16
#define BRL_NUM_BANDS (BRL_PANEL_H / BRL_BAND_ROWS)          /* 15 bands    */
#define BRL_BAND_BYTES (BRL_PANEL_W * BRL_BAND_ROWS * 2)     /* 10240 bytes */
#define BRL_FB_BYTES (BRL_PANEL_W * BRL_PANEL_H * 2)         /* 153600      */

/* Largest pixel payload the slave must be able to receive in one
 * transaction. A full band is the natural unit; keep the slave DMA RX
 * buffer at least this big. */
#define BRL_MAX_PAYLOAD BRL_BAND_BYTES

/* ------------------------------------------------------------------ */
/* Framing                                                             */
/* ------------------------------------------------------------------ */
#define BRL_MAGIC0 0xB2 /* 'Bruce'  */
#define BRL_MAGIC1 0xD5 /* 'Display'*/

/* Opcodes (master -> slave) */
enum {
    BRL_OP_SYNC = 0x01,      /* handshake; slave returns brl_status_t (panel_w/h, proto) */
    BRL_OP_TILE = 0x02,      /* blit RGB565 tile at (x,y,w,h); w*h*2 payload bytes follow */
    BRL_OP_FILLRECT = 0x03,  /* fill (x,y,w,h) with RGB565 color in arg0; no payload       */
    BRL_OP_BACKLIGHT = 0x04, /* arg0 = 0..255 backlight level                              */
    BRL_OP_SLEEP = 0x05,     /* arg0 = 1 -> panel sleep, 0 -> wake                         */
    BRL_OP_ROTATION = 0x06,  /* arg0 = 0..3 panel rotation                                 */
    BRL_OP_POLLTOUCH = 0x07, /* slave returns current brl_status_t on MISO                 */
    BRL_OP_PING = 0x08,      /* liveness check                                             */
};

/* Touch states carried in brl_status_t.touch_state */
enum {
    BRL_TOUCH_UP = 0,
    BRL_TOUCH_DOWN = 1,
    BRL_TOUCH_MOVE = 2,
};

/* Fixed 16-byte command header: master -> slave. */
typedef struct __attribute__((packed)) {
    uint8_t magic0;   /* BRL_MAGIC0                          [0] */
    uint8_t magic1;   /* BRL_MAGIC1                          [1] */
    uint8_t opcode;   /* BRL_OP_*                            [2] */
    uint8_t flags;    /* reserved / opcode-specific          [3] */
    uint16_t x;       /*                                     [4] */
    uint16_t y;       /*                                     [6] */
    uint16_t w;       /*                                     [8] */
    uint16_t h;       /*                                    [10] */
    uint16_t arg0;    /* color / level / rotation / sleep   [12] */
    uint8_t seq;      /* frame/message sequence             [14] */
    uint8_t checksum; /* brl_checksum(hdr, 15)              [15] */
} brl_header_t;

/* Fixed 16-byte status block: slave -> master. */
typedef struct __attribute__((packed)) {
    uint8_t magic0;      /* BRL_MAGIC0                       [0]  */
    uint8_t magic1;      /* BRL_MAGIC1                       [1]  */
    uint8_t proto;       /* BRL_PROTO_VERSION                [2]  */
    uint8_t touch_state; /* BRL_TOUCH_*                      [3]  */
    uint16_t touch_x;    /* panel-native coordinate          [4]  */
    uint16_t touch_y;    /*                                  [6]  */
    uint16_t panel_w;    /* reported by slave on SYNC        [8]  */
    uint16_t panel_h;    /*                                 [10]  */
    uint8_t seq;         /* ++ on each new distinct event   [12]  */
    uint8_t flags;       /* bit0: touch queue non-empty     [13]  */
    uint8_t reserved;    /*                                 [14]  */
    uint8_t checksum;    /* brl_checksum(status, 15)        [15]  */
} brl_status_t;

/* Both blocks are exactly 16 bytes. Guard it at compile time. */
typedef char brl_header_size_check[(sizeof(brl_header_t) == 16) ? 1 : -1];
typedef char brl_status_size_check[(sizeof(brl_status_t) == 16) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Checksum helpers (sum of bytes, folded with XOR 0xFF)               */
/* ------------------------------------------------------------------ */
static inline uint8_t brl_checksum(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += b[i];
    return (uint8_t)(s ^ 0xFF);
}

static inline void brl_header_finalize(brl_header_t *h) {
    h->magic0 = BRL_MAGIC0;
    h->magic1 = BRL_MAGIC1;
    h->checksum = brl_checksum(h, 15);
}

static inline int brl_header_valid(const brl_header_t *h) {
    return h->magic0 == BRL_MAGIC0 && h->magic1 == BRL_MAGIC1 && h->checksum == brl_checksum(h, 15);
}

static inline void brl_status_finalize(brl_status_t *s) {
    s->magic0 = BRL_MAGIC0;
    s->magic1 = BRL_MAGIC1;
    s->proto = BRL_PROTO_VERSION;
    s->checksum = brl_checksum(s, 15);
}

static inline int brl_status_valid(const brl_status_t *s) {
    return s->magic0 == BRL_MAGIC0 && s->magic1 == BRL_MAGIC1 && s->checksum == brl_checksum(s, 15);
}

/* Pixel payload length implied by a header (0 unless BRL_OP_TILE). */
static inline uint32_t brl_payload_len(const brl_header_t *h) {
    return (h->opcode == BRL_OP_TILE) ? ((uint32_t)h->w * (uint32_t)h->h * 2u) : 0u;
}

#endif /* BRUCE_REMOTE_LINK_H */
