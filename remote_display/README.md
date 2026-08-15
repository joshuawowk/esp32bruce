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

**Orientation:** pinned **landscape 320×240**. The PSRAM canvas is a 320×240
sprite, so Bruce (which reads `tftWidth`/`tftHeight` from `tft.width()`/`height()`)
lays out landscape. The slave's ST7789 is 240×320 native and scans in landscape
(`setRotation` 1/3) to match. Both landscape orientations work — the master's
"Landscape (180)" flip is honored by the slave and touch — while the portrait
orientations are locked out (`LANDSCAPE_LOCK`), since the fixed-aspect canvas
can't relayout to portrait without reallocating it.

**Known v1 limitation:** `sprite.pushSprite()` composites onto TFT_eSPI's
(headless) parent panel, not the canvas, so it renders nowhere. The only core
user is the `tururururu.cpp` shark easter-egg; the entire real UI draws directly
to `tft` and is unaffected.

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

0. **First light / "snowy" screen** — the slave now runs a **panel self-test** at boot
   (`SLAVE_SELFTEST` in `slave_zx2d80ce02s/src/main.cpp`): it prints a serial banner and
   sweeps the screen **red → green → blue → white → black** *before* any SPI activity, so
   you can validate the panel independently of the master. Flash the slave **alone** (EXT-IO
   SPI disconnected) and watch the **native USB-CDC** port (`pio device monitor -b 115200` —
   the board re-enumerates as a new `/dev/ttyACM*` on reset; a CH340/UART adapter shows
   nothing because `ARDUINO_USB_CDC_ON_BOOT=1`). Interpret:
   - **Color bars appear** → parallel bus + ST7789 init are good; move to step 1 (fault, if
     any, is in the SPI/tile path).
   - **Screen stays snowy** → panel bring-up is still failing. Drop `cfg.freq_write` further
     (16 → 10 MHz); confirm the init override is compiled in.
   - **Wrong colors** (R/B swapped) → flip `cfg.rgb_order`; **inverted/negative** → flip `cfg.invert`.
   - **Nothing on serial after ~2 s** → wrong port, or a panic in `lcd.init()` (build once with
     `-DARDUINO_USB_CDC_ON_BOOT=0` to force logs onto UART0 and read the backtrace).

   The panel config mirrors the vendor's own LovyanGFX driver (smartpanle `sc05_x`) for this
   exact board: **`freq_write = 20 MHz`** (40 MHz overclocked the 8080 write cycle → snow) and
   a **panel-specific ST7789 init sequence** (`Panel_SC05X::getInitCommands`, power/VCOM/gamma).
   The GPIO-bank-straddling data pins are *not* an issue for LovyanGFX (its S3 `Bus_Parallel8`
   drives LCD_CAM through the GPIO matrix, bank-agnostic — unlike TFT_eSPI's `DUAL_BANK` hack).

1. **SPI pixel path** ✅ (2026-08-15) — CRITICAL gotcha found here: the ESP32-S3
   `spi_slave` DMA path **cannot sample MOSI in SPI mode 0/2** (Espressif: "DMA requires
   SPI modes 1 and 3"). In mode 0 every frame arrived shifted left one bit (magic `B2 D5`
   read as `65 AA`) → all rejected → black screen. **Both ends must use SPI mode 1**:
   master `REMOTE_LINK_SPI_MODE = SPI_MODE1` (`remote_canvas.cpp`), slave `slvcfg.mode = 1`
   (`main.cpp`). Also lowered `REMOTE_LINK_HZ` to 10 MHz and added inter-tile pacing
   (`REMOTE_LINK_TILE_GAP_MS`) + a 500 µs header→payload `REMOTE_LINK_GAP_US` for margin.
2. **Boot Bruce on the canvas** ✅ (2026-08-15) — menu renders on the slave, centered.
   Two master-side fixes were needed: (a) the headless canvas never runs `TFT_eSPI::init()`,
   so the global `tftMutex` was NULL and the first `tft.drawRect` (a base shape helper not
   overridden by `TFT_eSprite`) aborted with `xQueueGiveMutexRecursive` → boot loop; fixed by
   creating `tftMutex` in `remote_canvas.cpp ensureInit()`. (b) Bruce's main menu was shifted
   40 px left because `MenuItemInterface` caches `iconCenterX = tftWidth/2` at C++ static-init
   (before `begin_tft()` sets the real 320), and its recompute never fired; fixed by the
   `rotation = 0xFF` sentinel in `include/MenuItemInterface.h`.
3. **Touch** ⏳ NOT WORKING YET (everything upstream works). Verified with a real finger:
   the slave's FT6336 detects touches, `map_touch` converts to canvas coords, `g_status`
   updates, and the slave asserts `TOUCH_IRQ`. The master sees the IRQ and calls
   `remote_canvas_poll_touch`. The remaining problem is the **slave→master status readback
   (MISO)**: it's unreliable (~30% valid even at a static menu). Ruled out: mode-1 MISO timing
   (fixed — boot SYNC reads `valid=1`), CPU core, `writeBytes`/`transferBytes`, the self-framing
   rewrite. `send_control` now retries the readback 10× (0→~30%), not enough. Lead: the valid
   reads show `panel_w=0` (a truncated status), suggesting the IDF `spi_slave` stops driving MISO
   before the master finishes clocking a short (16-byte) control frame out of its max-size armed
   transaction. Likely real fix: a hardware ready/handshake line, or IDF `spi_master` with flow
   control, rather than Arduino `SPIClass` polling.
4. **Dirty-band tuning** — cadence vs. CPU; raise `REMOTE_LINK_HZ` back toward 20 MHz.
5. **Radios** — wire and validate an app end-to-end (the payoff).
6. **Backlight / sleep** control frames.

## Status

- Protocol header: **host-compiled + self-tested** (`sizeof`, checksums,
  corruption detection).
- Slave firmware: complete, self-contained; panel config now matches the vendor
  `sc05_x` LovyanGFX driver (20 MHz + panel-specific init) and boots a serial +
  color-bar **self-test**. First on-hardware flash showed a snowy panel; root cause
  was the 40 MHz parallel-bus overclock (+ stock init). **First light CONFIRMED**
  (2026-08-14): the R/G/B/W self-test renders as clean solid colors → black, no snow.
- **END-TO-END DISPLAY WORKING** (2026-08-15): the master boots full Bruce, renders into the
  PSRAM canvas, streams over SPI, and the slave blits Bruce's **menu centered and aligned** on
  the panel — verified on hardware. Root causes fixed along the way: snow (40 MHz + stock init),
  master boot-loop (NULL `tftMutex`), black screen (SPI mode 0 vs `spi_slave` DMA → mode 1), and
  the 40 px menu shift (static-init `iconCenterX` cache). See the bring-up milestones above.
- **Remaining: touch** (slave→master MISO readback returns garbage in mode 1) and re-tuning
  `REMOTE_LINK_HZ` back up from the 10 MHz bring-up value.
