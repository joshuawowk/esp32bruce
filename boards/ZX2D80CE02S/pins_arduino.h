#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include "soc/soc_caps.h"
#include <stdint.h>

#ifndef DEVICE_NAME
#define DEVICE_NAME "ZX2D80CE02S"
#endif

// =============================================
// USB
// =============================================
#define USB_VID 0x303a
#define USB_PID 0x1001

// =============================================
// PanelLan ZX2D80CE02S  (vendor code SC05_X / "WT32S3-28S PRO")
//   - WT32-S3-WROVER module: ESP32-S3 N8R2 (8MB QIO flash, 2MB QSPI PSRAM)
//   - ST7789 240x320 IPS over an 8-bit Intel-8080 PARALLEL bus. The data pins
//     straddle BOTH GPIO output banks, so TFT_PARALLEL_8_BIT_DUAL_BANK (a Bruce
//     TFT_eSPI extension, see Processors/TFT_eSPI_ESP32_S3.h) is required.
//   - FocalTech FT6x36 / FT5x06 capacitive touch on I2C @ 0x38
//   - Backlight PWM, RS485, microSD (not wired in the vendor config), EXT-IO hdr
// Pin map from the official vendor library (smartpanle/PanelLan_esp32_arduino,
// board sc05_x) and QMSD-ESP32-BSP; verified on hardware.
// =============================================

// ---- External SPI bus for optional user radios on the EXT-IO header ----
// EXT-IO exposes 5V/GND + GPIO10,11,12,13,14,21 (the only free general IO).
#define SPI_SCK_PIN  12
#define SPI_MOSI_PIN 13
#define SPI_MISO_PIN 14
#define SPI_SS_PIN   21
static const uint8_t SS = SPI_SS_PIN;
static const uint8_t MOSI = SPI_MOSI_PIN;
static const uint8_t SCK = SPI_SCK_PIN;
static const uint8_t MISO = SPI_MISO_PIN;

// microSD: physically present but NOT wired to a usable SPI in the vendor
// config -> Bruce logs/exports to LittleFS instead.
#define SDCARD_CS   -1
#define SDCARD_SCK  -1
#define SDCARD_MISO -1
#define SDCARD_MOSI -1

// ---- Sub-GHz / 2.4GHz radios: CC1101 + nRF24 share one SPI bus ----
// Shared bus SCK/MOSI/MISO = IO12/13/14 (EXT-IO). Each radio has its own CS.
// The last two control lines land on the RS485 header (GPIO5=DE, GPIO6=DI are
// ESP-driven, safe to repurpose; GPIO4=RO is transceiver-driven, left alone).
// Requires the RS485 port to be unused.
#define USE_CC1101_VIA_SPI
#define CC1101_SS_PIN   21   // EXT-IO
#define CC1101_GDO0_PIN 11   // EXT-IO (also the unused reserved link-TX pin)
#define CC1101_MOSI_PIN SPI_MOSI_PIN
#define CC1101_SCK_PIN  SPI_SCK_PIN
#define CC1101_MISO_PIN SPI_MISO_PIN

#define USE_NRF24_VIA_SPI
#define NRF24_SS_PIN   5   // RS485 header DE
#define NRF24_CE_PIN   6   // RS485 header DI
#define NRF24_MOSI_PIN SPI_MOSI_PIN
#define NRF24_SCK_PIN  SPI_SCK_PIN
#define NRF24_MISO_PIN SPI_MISO_PIN

// =============================================
// I2C (capacitive touch FT6x36 @ 0x38; shared board I2C bus)
// =============================================
#define GROVE_SDA 8
#define GROVE_SCL 9
#define SYS_I2C_SDA GROVE_SDA
#define SYS_I2C_SCL GROVE_SCL
static const uint8_t SDA = GROVE_SDA;
static const uint8_t SCL = GROVE_SCL;
// A PN532 NFC/RFID reader works on this same I2C bus (@0x24) alongside the FT6x36
// touch (@0x38) with NO extra pins: interface.cpp does setSysI2CBus(&Wire) +
// Wire.begin(8,9) and sets rfidModule = PN532_I2C_MODULE, so Bruce's RFID menu
// drives it out of the box. Do NOT define PN532_IRQ/PN532_RF_REST (embedded-only).
// Other I2C add-ons (DS3231 RTC, MAX17048 gauge, PCF8574/AW9523 GPIO expander)
// share this bus the same way.

// =============================================
// Serial link to the WiFi co-processor (RX-only) on the EXT-IO header
// co-proc GPIO2 (TX) -> host SERIAL_RX (IO10). The reverse host->co-proc channel
// is unimplemented and there is no free pin for it (IO11 is reused for CC1101
// GDO0), so SERIAL_TX is disabled. GPS lives on the co-processor, not here.
// =============================================
#define SERIAL_TX -1
#define SERIAL_RX 10
#define GPS_SERIAL_TX SERIAL_TX
#define GPS_SERIAL_RX SERIAL_RX

// =============================================
// TFT Display (ST7789 240x320, 8-bit 8080 parallel, dual GPIO bank)
// =============================================
#define USER_SETUP_LOADED
#define ST7789_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 320
#define TFT_INVERSION_ON
#define TFT_RGB_ORDER TFT_RGB
#define TFT_PARALLEL_8_BIT
#define TFT_PARALLEL_8_BIT_DUAL_BANK // D0..D7 span GPIO banks 0 and 1 (see TFT_eSPI_ESP32_S3.h)
#define TFT_CS  -1 // chip-select tied on the PCB (library generates no CS code)
#define TFT_DC  18 // data/command (RS)
#define TFT_RST 3  // shared board reset (also resets the touch controller)
#define TFT_WR  17 // write strobe
#define TFT_RD  -1
#define TFT_D0 16
#define TFT_D1 40
#define TFT_D2 15
#define TFT_D3 7
#define TFT_D4 41
#define TFT_D5 42
#define TFT_D6 2
#define TFT_D7 1
#define TFT_BL 47 // backlight (PWM, active high)
#define TFT_BACKLIGHT_ON HIGH
#define SMOOTH_FONT 1

#define HAS_SCREEN
#define ROTATION 1 // landscape 320x240
#define MINBRIGHT (uint8_t)1
#define BACKLIGHT TFT_BL

// Font sizes
#define FP 1
#define FM 2
#define FG 3

// =============================================
// Capacitive touch (FT6x36 / FT5x06 via I2C @ 0x38)
// =============================================
#define HAS_TOUCH 1
#define HAS_CAPACITIVE_TOUCH 1
#define TOUCH_FT6336_I2C 1
#define FT6336_I2C_ADDR 0x38
#define FT6336_I2C_CONFIG_SDA 8
#define FT6336_I2C_CONFIG_SCL 9
#define FT6336_TOUCH_CONFIG_RST -1 // shares the board/LCD reset on GPIO3
#define FT6336_TOUCH_CONFIG_INT 48

// =============================================
// Buttons
// =============================================
#define HAS_BTN 1
#define BTN_ALIAS "\"Boot\""
#define BTN_PIN 0 // BOOT button
#define BTN_ACT LOW
#define SEL_BTN 0 // BOOT used as Select

// =============================================
// Infrared / RF (external, via EXT-IO header)
// =============================================
#define TXLED 12 // IR TX default (shares the SPI bus; IR is rarely used with radios)
#define RXLED 14 // IR RX default
#define LED_ON HIGH
#define LED_OFF LOW

#define IR_TX_PINS '{{"GPIO10", 10}, {"GPIO11", 11}, {"GPIO12", 12}, {"GPIO13", 13}, {"GPIO14", 14}, {"GPIO21", 21}}'
#define IR_RX_PINS '{{"GPIO10", 10}, {"GPIO11", 11}, {"GPIO12", 12}, {"GPIO13", 13}, {"GPIO14", 14}, {"GPIO21", 21}}'
#define RF_TX_PINS '{{"GPIO10", 10}, {"GPIO11", 11}, {"GPIO12", 12}, {"GPIO13", 13}, {"GPIO14", 14}, {"GPIO21", 21}}'
#define RF_RX_PINS '{{"GPIO10", 10}, {"GPIO11", 11}, {"GPIO12", 12}, {"GPIO13", 13}, {"GPIO14", 14}, {"GPIO21", 21}}'

// =============================================
// Battery — no ADC divider is wired on this board (getBattery() is a stub)
// =============================================

// =============================================
// BadUSB (USB HID)
// =============================================
#define USB_as_HID 1
#define BAD_TX GROVE_SDA
#define BAD_RX GROVE_SCL

// =============================================
// Deep sleep
// =============================================
#define DEEPSLEEP_WAKEUP_PIN 0 // BOOT button
#define DEEPSLEEP_PIN_ACT LOW

#endif /* Pins_Arduino_h */
