#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include "soc/soc_caps.h"
#include <stdint.h>

#ifndef DEVICE_NAME
#define DEVICE_NAME "Bruce Remote Master"
#endif

// =============================================================================
// Bruce split-display MASTER  --  generic ESP32-S3 (N16R8 recommended)
//
// This board runs FULL Bruce headless: no local panel. The global `tft` is a
// full-screen PSRAM canvas (USE_REMOTE_CANVAS) whose pixels are streamed over
// SPI to a ZX2D80CE02S slave running remote_display/slave_zx2d80ce02s. All
// radios/modules wire to THIS board's GPIO (the whole point of the split: the
// slave's parallel panel leaves it no free IO for radios).
//
// Pin choices avoid the octal flash/PSRAM range GPIO26..37 used on N16R8, the
// native-USB pins (19/20), and UART0 (43/44). Adjust to your wiring.
// =============================================================================

// ---- USB (native CDC) ----
#define USB_VID 0x303a
#define USB_PID 0x1001

// =============================================================================
// SPI link to the ZX2D80CE02S slave display (master = controller)
// =============================================================================
#define REMOTE_LINK_SCLK 12
#define REMOTE_LINK_MOSI 11
#define REMOTE_LINK_MISO 13
#define REMOTE_LINK_CS 10
#define REMOTE_LINK_IRQ 14 // slave -> master, active low (touch pending)
#define REMOTE_LINK_SPI_BUS HSPI
#define REMOTE_LINK_HZ 20000000    // 20 MHz to start; raise once proven
#define REMOTE_LINK_FLUSH_MS 20    // ~50 Hz dirty-band diff cadence

// =============================================================================
// Radios / modules  --  wired to the master's own GPIO
// =============================================================================
// Shared radio SPI bus (CC1101 + nRF24), independent of the display link bus.
#define SPI_SCK_PIN 4
#define SPI_MOSI_PIN 5
#define SPI_MISO_PIN 6
#define SPI_SS_PIN 7
static const uint8_t SS = SPI_SS_PIN;
static const uint8_t MOSI = SPI_MOSI_PIN;
static const uint8_t SCK = SPI_SCK_PIN;
static const uint8_t MISO = SPI_MISO_PIN;

// microSD on the radio SPI bus (optional; set CS to your wiring or -1)
#define SDCARD_CS -1
#define SDCARD_SCK SPI_SCK_PIN
#define SDCARD_MISO SPI_MISO_PIN
#define SDCARD_MOSI SPI_MOSI_PIN

#define USE_CC1101_VIA_SPI
#define CC1101_SS_PIN 7
#define CC1101_GDO0_PIN 15
#define CC1101_MOSI_PIN SPI_MOSI_PIN
#define CC1101_SCK_PIN SPI_SCK_PIN
#define CC1101_MISO_PIN SPI_MISO_PIN

#define USE_NRF24_VIA_SPI
#define NRF24_SS_PIN 16
#define NRF24_CE_PIN 17
#define NRF24_MOSI_PIN SPI_MOSI_PIN
#define NRF24_SCK_PIN SPI_SCK_PIN
#define NRF24_MISO_PIN SPI_MISO_PIN

// =============================================================================
// I2C (PN532 NFC/RFID @0x24 and other add-ons)
// =============================================================================
#define GROVE_SDA 8
#define GROVE_SCL 9
#define SYS_I2C_SDA GROVE_SDA
#define SYS_I2C_SCL GROVE_SCL
static const uint8_t SDA = GROVE_SDA;
static const uint8_t SCL = GROVE_SCL;

// =============================================================================
// Serial / GPS
// =============================================================================
#define SERIAL_TX 18
#define SERIAL_RX 21
#define GPS_SERIAL_TX SERIAL_TX
#define GPS_SERIAL_RX SERIAL_RX

// =============================================================================
// Display: headless PSRAM canvas streamed over SPI (no local panel)
// =============================================================================
#define USE_REMOTE_CANVAS // selects lib/HAL/display/remote_canvas.h
#define HAS_SCREEN
// Pinned LANDSCAPE. The PSRAM canvas IS a landscape 320x240 sprite: Bruce reads
// tftWidth/tftHeight from tft.width()/height() (the sprite dims), so these macros
// alone orient the UI. The slave's ST7789 is 240x320 native and rotates to
// landscape to match (see remote_display/slave_zx2d80ce02s). LANDSCAPE_LOCK hides
// the portrait entries in the Orientation menu, which would give Bruce a 240x300
// layout that overflows the 320x240 canvas; the two landscape orientations
// (0 / 180) stay selectable and the slave honors the 180 flip.
#define TFT_WIDTH 320
#define TFT_HEIGHT 240
#define ROTATION 0 // landscape (even rotation); slave maps 0->normal, 2->180
#define LANDSCAPE_LOCK // canvas aspect is fixed landscape: forbid portrait rotations
#define MINBRIGHT (uint8_t)1
#define SMOOTH_FONT 1

// TFT_eSPI still compiles (we use its TFT_eSprite as the RAM canvas), so it
// needs a driver + pins defined. These pins are NEVER driven: tft.init() is
// overridden to allocate the sprite instead of touching a panel. Dummy values
// in the high, otherwise-unused GPIO range.
#define USER_SETUP_LOADED
#define ST7789_DRIVER
#define TFT_MISO -1
#define TFT_MOSI 47
#define TFT_SCLK 48
#define TFT_CS 38
#define TFT_DC 39
#define TFT_RST 40

// Font sizes
#define FP 1
#define FM 2
#define FG 3

// =============================================================================
// Touch: delivered over the SPI link (no local touch controller)
// =============================================================================
#define HAS_TOUCH 1
#define HAS_CAPACITIVE_TOUCH 1

// =============================================================================
// Buttons
// =============================================================================
#define HAS_BTN 1
#define BTN_ALIAS "\"Boot\""
#define BTN_PIN 0 // BOOT button
#define BTN_ACT LOW
#define SEL_BTN 0

// =============================================================================
// Infrared / RF (external modules)
// =============================================================================
#define TXLED 1 // IR TX default
#define RXLED 2 // IR RX default
#define LED_ON HIGH
#define LED_OFF LOW

#define IR_TX_PINS '{{"GPIO1", 1}, {"GPIO2", 2}, {"GPIO15", 15}, {"GPIO16", 16}, {"GPIO17", 17}}'
#define IR_RX_PINS '{{"GPIO1", 1}, {"GPIO2", 2}, {"GPIO15", 15}, {"GPIO16", 16}, {"GPIO17", 17}}'
#define RF_TX_PINS '{{"GPIO1", 1}, {"GPIO2", 2}, {"GPIO15", 15}, {"GPIO16", 16}, {"GPIO17", 17}}'
#define RF_RX_PINS '{{"GPIO1", 1}, {"GPIO2", 2}, {"GPIO15", 15}, {"GPIO16", 16}, {"GPIO17", 17}}'

// =============================================================================
// BadUSB (USB HID)
// =============================================================================
#define USB_as_HID 1
#define BAD_TX GROVE_SDA
#define BAD_RX GROVE_SCL

// =============================================================================
// Deep sleep
// =============================================================================
#define DEEPSLEEP_WAKEUP_PIN 0
#define DEEPSLEEP_PIN_ACT LOW

#endif /* Pins_Arduino_h */
