# Bruce split-display ("remote canvas")

Run full Bruce as **two boards**: a **master** ESP32-S3 that holds all the
radios/modules and renders the UI into a PSRAM framebuffer, and a **slave**
PanelLan **ZX2D80CE02S** (aka SC05_X) that is a dumb pixel blitter + touch
return. They talk over **SPI**.

```
┌─ MASTER ESP32-S3 (N16R8) ───────────────┐          ┌─ SLAVE ZX2D80CE02S ───────────┐
│ full Bruce, USE_REMOTE_CANVAS            │  SPI     │ standalone LovyanGFX blitter   │
│  tft = full-screen TFT_eSprite in PSRAM  │ ───────▶ │  SPI-peripheral DMA receive    │
│  flush task: CRC per 16-row band,        │  tiles   │   → pushImage() to parallel    │
│              send only changed bands     │          │      ST7789 (240x320)          │
│  radios (CC1101/nRF24/PN532/IR) on GPIO  │ ◀─────── │  FT6336 touch → status + IRQ   │
│  touch events → touchPoint global        │ touch    │                                │
└──────────────────────────────────────────┘          └────────────────────────────────┘
```

Why: the ZX2D80CE02S drives its ST7789 over an **8-bit parallel** bus that eats
~13 GPIO, leaving nothing for radios. Put the radios on a master with GPIO
headroom and use the nice panel purely as a screen.

## Why pixel-perfect works

Every one of Bruce's ~2,500 `tft.*` calls goes through one object,
`tft_logger tft`, whose base class is selectable. `USE_REMOTE_CANVAS` makes that
base a full-screen **`TFT_eSprite` in PSRAM** — so all drawing (menus, text,
JPEG/GIF/PNG, QR, icons) renders **real pixels into RAM**, then we ship the
bitmap. The slave never parses a Bruce primitive; it just blits. No vector
protocol, so no font/sprite/image translation gaps.

**Known v1 limitation:** `sprite.pushSprite()` composites onto TFT_eSPI's
(headless) parent panel, not the canvas, so it renders nowhere. The only core
user is the `tururururu.cpp` shark easter-egg; the entire real UI draws directly
to `tft` and is unaffected. (Landscape rotation is likewise deferred — v1 is
pinned portrait 240×320 so the canvas dims equal the slave's native panel.)

## Files

| Path | What |
|---|---|
| `bruce_remote_link.h` | Shared SPI wire protocol (both firmwares). Host-tested. |
| `slave_zx2d80ce02s/` | Standalone slave firmware (PlatformIO, LovyanGFX). |
| `../lib/HAL/display/remote_canvas.{h,cpp}` | Master headless canvas + SPI link + flush task. |
| `../boards/bruce-remote-master/` | Master board target (pins, ini, interface, partitions). |
| `../boards/_boards_json/bruce-remote-master.json` | Master board definition. |
| `../lib/HAL/display/tft.h` | +1 guarded branch selecting `remote_canvas.h`. |

## Wire protocol (SPI, master = controller)

Fixed **16-byte** little-endian frames (`brl_header_t`). Master→slave; on
`BRL_OP_TILE` a `w*h*2`-byte RGB565 payload follows as a second transaction.
Every 16-byte control frame is full-duplex: the slave clocks its current
**16-byte `brl_status_t`** (touch + panel info) back on MISO. A `TOUCH_IRQ`
line (slave→master, active low) signals a pending touch so the master knows to
poll. Both structs carry a checksum; both are compile-time asserted to 16 bytes.

Opcodes: `SYNC, TILE, FILLRECT, BACKLIGHT, SLEEP, ROTATION, POLLTOUCH, PING`.

Dirty-band streaming: the 240×320 framebuffer is diffed in 20 bands of 16 rows
(`esp_rom_crc32_le` per band); only changed bands are sent, so an idle screen
generates **zero** SPI traffic. Full frame = 150 KB; at 20 MHz SPI a full
redraw is ~75 ms, individual band updates are sub-ms.

## Wiring

| Signal | Master (default) | Slave ZX2D80CE02S (EXT-IO) |
|---|---|---|
| SCLK | GPIO12 | GPIO12 |
| MOSI | GPIO11 | GPIO11 |
| MISO | GPIO13 | GPIO13 |
| CS   | GPIO10 | GPIO10 |
| TOUCH_IRQ | GPIO14 (in, pull-up) | GPIO14 (out) |
| GND/3V3 | shared | shared |

Keep SPI traces short; start at 20 MHz. Master pins are configurable in
`boards/bruce-remote-master/pins_arduino.h`; slave pins in
`slave_zx2d80ce02s/src/main.cpp`. Radios wire to the master's other GPIO (see
its `pins_arduino.h`: CC1101/nRF24 on SPI 4/5/6, PN532 on I2C 8/9, …).

## Build & flash

Slave (display board):
```
cd remote_display/slave_zx2d80ce02s
pio run -t upload
```

Master (radios board):
```
pio run -e bruce-remote-master -t upload
```

## Bring-up milestones (on hardware)

1. **SPI pixel path** — verify the slave receives a header + payload and blits
   one band. This exercises the ESP32-S3 SPI-slave DMA re-arm timing (the
   header→payload gap `REMOTE_LINK_GAP_US`); tune it here. Confirm color order
   (`rgb_order`/`setSwapBytes`) with a known test tile.
2. **Boot Bruce on the canvas** — full-frame flush; confirm the menu appears.
3. **Touch** — IRQ + POLLTOUCH → navigate menus; check the one-event `seq`
   handshake and coordinate mapping.
4. **Dirty-band tuning** — cadence vs. CPU; raise `REMOTE_LINK_HZ`.
5. **Radios** — wire and validate an app end-to-end (the payoff).
6. **Backlight / sleep** control frames.

## Status

- Protocol header: **host-compiled + self-tested** (`sizeof`, checksums,
  corruption detection).
- Slave firmware: complete, self-contained (panel/touch config lifted verbatim
  from the working Bruce ZX2D80CE02S port); **not yet flashed**.
- Master integration: implemented; **compile/runtime bring-up is on-hardware**
  (SPI-slave DMA timing and TFT_eSprite-as-`tft` runtime behavior can't be
  verified without the boards).
