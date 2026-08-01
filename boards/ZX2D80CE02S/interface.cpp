/*
 * PanelLan ZX2D80CE02S (SC05_X / WT32S3-28S PRO)
 * Board interface implementation for Bruce firmware.
 *
 * Hardware:
 *   - WT32-S3-WROVER: ESP32-S3 N8R2 (8MB QIO flash, 2MB QSPI PSRAM)
 *   - ST7789 240x320 IPS, 8-bit 8080 parallel (dual GPIO bank; see pins_arduino.h)
 *   - FocalTech FT6x36 / FT5x06 capacitive touch (I2C @ 0x38): SDA=8, SCL=9, INT=48
 *   - Backlight PWM on GPIO47, BOOT button on GPIO0
 *   - microSD present but not wired in the vendor config -> Bruce uses LittleFS
 */

#include "core/bus_HAL.h"
#include "core/powerSave.h"
#include "core/utils.h"
#include <Arduino.h>
#include <Wire.h>
#include <globals.h>
#include <interface.h>

// =============================================
// Touch screen (FT6x36 / FT5x06 via I2C)
// =============================================
#define ZX_TOUCH_SDA  8
#define ZX_TOUCH_SCL  9
#define ZX_TOUCH_INT  48
#define ZX_TOUCH_ADDR 0x38

#define ZX_BTN_PIN 0 // BOOT button
#define ZX_BTN_ACT LOW

// The FT6x36/FT5x06 share the FT6236 register protocol:
//   reg 0x02: number of touch points; regs 0x03..0x06: point 1 X/Y (12-bit)
#define FT6336_REG_NUM_TOUCHES 0x02
#define FT6336_REG_TOUCH_DATA  0x03

static bool touchInitialized = false;

static uint8_t ft6336_read_reg(uint8_t reg) {
    Wire.beginTransmission(ZX_TOUCH_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(ZX_TOUCH_ADDR, 1);
    if (Wire.available()) return Wire.read();
    return 0;
}

static bool ft6336_read_touch(int16_t &x, int16_t &y) {
    uint8_t touches = ft6336_read_reg(FT6336_REG_NUM_TOUCHES);
    if (touches == 0 || touches > 2) return false;

    uint8_t data[4];
    Wire.beginTransmission(ZX_TOUCH_ADDR);
    Wire.write(FT6336_REG_TOUCH_DATA);
    Wire.endTransmission(false);
    Wire.requestFrom(ZX_TOUCH_ADDR, 4);
    for (int i = 0; i < 4; i++) { data[i] = Wire.read(); }

    x = ((data[0] & 0x0F) << 8) | data[1];
    y = ((data[2] & 0x0F) << 8) | data[3];
    return true;
}

/***************************************************************************************
** Function name: _setup_gpio()
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    // ---- Touch (FT6x36) ----
    // Touch reset is tied to the board/LCD reset (GPIO3), toggled by tft.init(),
    // so no separate reset is driven here.
    pinMode(ZX_TOUCH_INT, INPUT);
    setSysI2CBus(&Wire);
    Wire.begin(ZX_TOUCH_SDA, ZX_TOUCH_SCL);
    touchInitialized = true;

    // ---- Default external module wiring (EXT-IO header) ----
    bruceConfigPins.rfModule = CC1101_SPI_MODULE;
    bruceConfigPins.rfidModule = PN532_I2C_MODULE;
    bruceConfigPins.irRx = RXLED;
    bruceConfigPins.irTx = TXLED;

    Serial.begin(115200);
}

/***************************************************************************************
** Function name: _post_setup_gpio()
** Description:   second stage gpio setup after the TFT is initialized
***************************************************************************************/
void _post_setup_gpio() {
    // Backlight (GPIO47, active high) via PWM. Done after tft.init().
    pinMode(TFT_BL, OUTPUT);
    analogWrite(TFT_BL, 255);
}

/*********************************************************************
** Function: getBattery
** No battery ADC divider is wired on this board.
**********************************************************************/
int getBattery() { return 100; }

/*********************************************************************
** Function: _setBrightness
** set brightness value (0-100)
**********************************************************************/
void _setBrightness(uint8_t brightval) {
    if (brightval == 0) {
        analogWrite(TFT_BL, 0);
    } else {
        int bl = MINBRIGHT + round(((255 - MINBRIGHT) * brightval / 100));
        analogWrite(TFT_BL, bl);
    }
}

/*********************************************************************
** Function: InputHandler
** Handles touch and the BOOT button, mapping to Bruce's press globals.
**********************************************************************/
void InputHandler(void) {
    static long tm = 0;

    if (millis() - tm > 200 || LongPress) {
        // ---- Touch ----
        if (touchInitialized) {
            int16_t raw_x, raw_y;
            if (ft6336_read_touch(raw_x, raw_y)) {
                tm = millis();

                // Map the panel-native (portrait 240x320) coordinates to the
                // active rotation.
                int16_t t_x = raw_x;
                int16_t t_y = raw_y;

                if (bruceConfigPins.rotation == 1) {
                    // Landscape
                    t_x = raw_y;
                    t_y = (TFT_WIDTH - 1) - raw_x;
                } else if (bruceConfigPins.rotation == 2) {
                    // Portrait inverted
                    t_x = (TFT_WIDTH - 1) - raw_x;
                    t_y = (TFT_HEIGHT - 1) - raw_y;
                } else if (bruceConfigPins.rotation == 3) {
                    // Landscape inverted
                    t_x = (TFT_HEIGHT - 1) - raw_y;
                    t_y = raw_x;
                }
                // rotation == 0: portrait default, no transform

                if (!wakeUpScreen()) AnyKeyPress = true;
                else return;

                touchPoint.x = t_x;
                touchPoint.y = t_y;
                touchPoint.pressed = true;
                touchHeatMap(touchPoint);
            }
        }

        // ---- BOOT button (Select) ----
        if (digitalRead(ZX_BTN_PIN) == ZX_BTN_ACT) {
            if (!wakeUpScreen()) {
                AnyKeyPress = true;
                SelPress = true;
            }
            while (digitalRead(ZX_BTN_PIN) == ZX_BTN_ACT) delay(10); // debounce
        }
    }
}

/*********************************************************************
** Function: powerOff
** Deep sleep, wake on the BOOT button.
**********************************************************************/
void powerOff() {
    analogWrite(TFT_BL, 0);
    tft.writecommand(0x10); // SLPIN
    esp_sleep_enable_ext0_wakeup((gpio_num_t)ZX_BTN_PIN, ZX_BTN_ACT);
    esp_deep_sleep_start();
}

/*********************************************************************
** Function: goToDeepSleep
**********************************************************************/
void goToDeepSleep() { powerOff(); }

/*********************************************************************
** Function: checkReboot
** Long-press BOOT to power off.
**********************************************************************/
void checkReboot() {
    int c = 0;
    while (digitalRead(ZX_BTN_PIN) == ZX_BTN_ACT) {
        delay(100);
        if (++c > 20) powerOff(); // ~2s
    }
}

/***************************************************************************************
** Function name: isCharging()
***************************************************************************************/
bool isCharging() { return false; }
