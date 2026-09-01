/*
 * Bruce split-display MASTER -- board interface.
 *
 * No local panel or touch: the display is a PSRAM canvas streamed over SPI
 * (see lib/HAL/display/remote_canvas.*), and touch arrives back over the same
 * link. This file wires the radios/modules to the master's GPIO and routes
 * remote touch + backlight through the link control surface.
 */
#include "core/bus_HAL.h"
#include "core/powerSave.h"
#include "core/utils.h"
#include <Arduino.h>
#include <Wire.h>
#include <globals.h> // pulls in tftLogger -> display/tft.h -> remote_canvas.h
#include <interface.h>

// Track the last touch event we forwarded, so a repeated poll of the same
// event doesn't spam Bruce's input globals.
static uint8_t lastTouchSeq = 0;

/***************************************************************************************
** Function name: _setup_gpio()
***************************************************************************************/
void _setup_gpio() {
    // Default external module wiring.
    bruceConfigPins.rfModule = CC1101_SPI_MODULE;
    bruceConfigPins.rfidModule = PN532_I2C_MODULE;
    bruceConfigPins.irRx = RXLED;
    bruceConfigPins.irTx = TXLED;

    // I2C bus for PN532 / add-ons.
    setSysI2CBus(&Wire);
    Wire.begin(SYS_I2C_SDA, SYS_I2C_SCL);

    pinMode(BTN_PIN, INPUT_PULLUP);
    Serial.begin(115200);
}

/***************************************************************************************
** Function name: _post_setup_gpio()
** The SPI link + backlight are brought up by tft.init() (remote_canvas), so
** there is nothing panel-related to do here.
***************************************************************************************/
void _post_setup_gpio() {}

/*********************************************************************
** Function: getBattery  --  master is typically USB/externally powered.
**********************************************************************/
int getBattery() { return 100; }

/*********************************************************************
** Function: _setBrightness  --  forwarded to the slave panel over SPI.
**********************************************************************/
void _setBrightness(uint8_t brightval) {
    uint8_t level = (brightval == 0) ? 0 : (uint8_t)(MINBRIGHT + ((255 - MINBRIGHT) * brightval / 100));
    remote_canvas_send_backlight(level);
}

/*********************************************************************
** Function: InputHandler
** Poll the slave for touch when it flags a pending event (IRQ low), plus the
** local BOOT button used as Select.
**********************************************************************/
void InputHandler(void) {
    static long tm = 0;
    if (millis() - tm < 20 && !LongPress) return;

    // ---- Remote touch (pushed by the slave over the UART; drained here) ----
    {
        brl_status_t st;
        if (remote_canvas_poll_touch(&st) && st.seq != lastTouchSeq) {
            lastTouchSeq = st.seq;
            tm = millis();
            previousMillis = millis(); // ANY touch is activity: reset the dim/screen-off timer so the
                                       // screen never sleeps mid-use (and re-arms the timeout after wake).
            // Any touch (incl. UP) wakes a dimmed/off screen and is swallowed, so the first tap only wakes.
            if (wakeUpScreen()) return;
            if (st.touch_state == BRL_TOUCH_DOWN || st.touch_state == BRL_TOUCH_MOVE) {
                AnyKeyPress = true;
                touchPoint.x = st.touch_x;
                touchPoint.y = st.touch_y;
                touchPoint.pressed = true;
                touchHeatMap(touchPoint);
            }
        }
    }

    // ---- BOOT button (Select) ----
    if (digitalRead(BTN_PIN) == BTN_ACT) {
        previousMillis = millis(); // button activity resets the dim/screen-off timer too
        if (!wakeUpScreen()) {
            AnyKeyPress = true;
            SelPress = true;
        }
        while (digitalRead(BTN_PIN) == BTN_ACT) delay(10); // debounce
    }
}

/*********************************************************************
** Function: powerOff  --  sleep the remote panel and deep-sleep the master.
**********************************************************************/
void powerOff() {
    remote_canvas_send_backlight(0);
    remote_canvas_send_sleep(true);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PIN, BTN_ACT);
    esp_deep_sleep_start();
}

/*********************************************************************
** Function: goToDeepSleep
**********************************************************************/
void goToDeepSleep() { powerOff(); }

/*********************************************************************
** Function: checkReboot  --  long-press BOOT to power off.
**********************************************************************/
void checkReboot() {
    int c = 0;
    while (digitalRead(BTN_PIN) == BTN_ACT) {
        delay(100);
        if (++c > 20) powerOff(); // ~2s
    }
}

/***************************************************************************************
** Function name: isCharging()
***************************************************************************************/
bool isCharging() { return false; }
