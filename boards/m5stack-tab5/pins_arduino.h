#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include "soc/soc_caps.h"
#include <stdint.h>

#define USB_VID 0x303a
#define USB_PID 0x1001

// Tab5 exposes no simple Arduino user LED; point off-chip (no-op)
static const uint8_t LED_BUILTIN = SOC_GPIO_PIN_COUNT;
#define BUILTIN_LED LED_BUILTIN
#define RGB_BUILTIN LED_BUILTIN
#define RGB_BRIGHTNESS 64

// UART0 broken out on the M5-Bus (runtime console is USB-CDC)
static const uint8_t TX = 37;
static const uint8_t RX = 38;

// System I2C (shared with M5Unified internal bus): SDA=G31, SCL=G32
static const uint8_t SDA = 31;
static const uint8_t SCL = 32;

// Default Arduino SPI == aux SPI on the M5-Bus (external CC1101 / nRF24 / LoRa)
static const uint8_t SS   = 16;
static const uint8_t MOSI = 18;
static const uint8_t MISO = 19;
static const uint8_t SCK  = 5;

// GPIO aliases
static const uint8_t G0=0,  G1=1,  G2=2,  G3=3,  G4=4,  G5=5,  G6=6,  G7=7;
static const uint8_t G8=8,  G9=9,  G10=10, G11=11, G12=12, G13=13, G14=14, G15=15;
static const uint8_t G16=16, G17=17, G18=18, G19=19, G20=20, G21=21, G22=22, G23=23;
static const uint8_t G26=26, G27=27, G28=28, G29=29, G30=30, G31=31, G32=32, G34=34;
static const uint8_t G35=35, G36=36, G37=37, G38=38, G39=39, G40=40, G41=41, G42=42;
static const uint8_t G43=43, G44=44, G45=45, G47=47, G48=48, G50=50, G51=51, G52=52, G53=53, G54=54;

#endif /* Pins_Arduino_h */
