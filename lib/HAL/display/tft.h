#ifndef LIB_HAL_DISPLAY_TFT_H
#define LIB_HAL_DISPLAY_TFT_H
#include <pins_arduino.h>

#if !defined(USE_ARDUINO_GFX) && !defined(USE_LOVYANGFX) && !defined(USE_TFT_ESPI) && !defined(USE_M5GFX) &&  \
    !defined(USE_REMOTE_CANVAS)
#define USE_TFT_ESPI
#endif

class TFT_eSPI;
class tft_sprite;
class tft_logger;
#include <SPI.h>
#if defined(USE_TFT_ESPI)
#include "tftespi.h"

#elif defined(USE_ARDUINO_GFX)
#include "ardgfx.h"

#elif defined(USE_LOVYANGFX)
#include "lovyan.h"

#elif defined(USE_M5GFX)
#include "m5gfx.h"

#elif defined(USE_REMOTE_CANVAS)
#include "remote_canvas.h" // headless PSRAM canvas + SPI link (split-display master)

#endif
#endif // LIB_HAL_DISPLAY_TFT_H
