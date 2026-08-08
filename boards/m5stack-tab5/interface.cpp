#include "core/bus_HAL.h"
#include "core/display.h"
#include "core/powerSave.h"
#include "core/utils.h"
#include <M5Unified.h>
#include <globals.h>
#include <interface.h>
#include <string.h>

/***************************************************************************************
** M5Stack Tab5 (ESP32-P4) board interface
**
** Display / touch / IMU / power / RTC are driven by M5Unified + M5GFX (USE_M5GFX).
**
** A164 detachable keyboard (STM32F030, I2C 0x6D) on ExtPort1 (SDA=G0, SCL=G1, INT=G50):
** driven here with a SOFTWARE (bit-banged) I2C master. NOTE: on the ESP32-P4 both HP
** I2C controllers are already owned by M5Unified (In_I2C=I2C_NUM_1 @G31/32,
** Ex_I2C=I2C_NUM_0 @G53/54), so Arduino Wire/Wire1 CANNOT be used for the keyboard
** without colliding with the system bus (which would take down touch/backlight/power).
** A bit-banged master on G0/G1 is independent and clock-stretch aware.
** Ref: PORTING_PLAN.md sections 10, 18.2, 20.
***************************************************************************************/

#ifndef KB_TAB5_SDA
#define KB_TAB5_SDA 0
#endif
#ifndef KB_TAB5_SCL
#define KB_TAB5_SCL 1
#endif
#ifndef KB_TAB5_ADDR
#define KB_TAB5_ADDR 0x6D
#endif

// A164 register map (Character mode)
static const uint8_t A164_REG_INT_STAT   = 0x01;
static const uint8_t A164_REG_MODE       = 0x10;
static const uint8_t A164_REG_CHAR_LEN   = 0x40;
static const uint8_t A164_REG_CHAR_EVENT = 0x50;
static const uint8_t A164_MODE_CHARACTER = 0x02;

static bool tab5KbReady = false;
static unsigned long tab5KbNextProbe = 0;

// ---- bit-banged, open-drain I2C on G0/G1 (~100 kHz, clock-stretch tolerant) ----
static inline void kbHalf() { delayMicroseconds(5); }
static inline void kbSdaRelease() { pinMode(KB_TAB5_SDA, INPUT_PULLUP); }
static inline void kbSdaLow() { pinMode(KB_TAB5_SDA, OUTPUT); digitalWrite(KB_TAB5_SDA, LOW); }
static inline int kbSdaRead() { return digitalRead(KB_TAB5_SDA); }
static inline void kbSclLow() { pinMode(KB_TAB5_SCL, OUTPUT); digitalWrite(KB_TAB5_SCL, LOW); }
// Release SCL and wait for the slave to release it too (clock stretching), bounded ~1 ms.
static inline void kbSclReleaseWait() {
    pinMode(KB_TAB5_SCL, INPUT_PULLUP);
    unsigned long t = micros();
    while (digitalRead(KB_TAB5_SCL) == LOW && (micros() - t) < 1000) { /* stretch */ }
}

static void kbIdle() { kbSdaRelease(); kbSclReleaseWait(); }

static void kbStart() { kbSdaRelease(); kbSclReleaseWait(); kbHalf(); kbSdaLow(); kbHalf(); kbSclLow(); kbHalf(); }
static void kbStop() { kbSdaLow(); kbHalf(); kbSclReleaseWait(); kbHalf(); kbSdaRelease(); kbHalf(); }

// write 8 bits MSB-first, return true if ACKed by slave
static bool kbWrite(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {
        if (b & 0x80) kbSdaRelease(); else kbSdaLow();
        kbHalf();
        kbSclReleaseWait();
        kbHalf();
        kbSclLow();
        b <<= 1;
    }
    // ACK clock
    kbSdaRelease();
    kbHalf();
    kbSclReleaseWait();
    int ack = kbSdaRead(); // 0 = ACK
    kbHalf();
    kbSclLow();
    return ack == 0;
}

static uint8_t kbReadByte(bool ack) {
    uint8_t v = 0;
    kbSdaRelease();
    for (uint8_t i = 0; i < 8; i++) {
        kbHalf();
        kbSclReleaseWait();
        v = (uint8_t)((v << 1) | (kbSdaRead() & 1));
        kbHalf();
        kbSclLow();
    }
    // send ACK/NACK
    if (ack) kbSdaLow(); else kbSdaRelease();
    kbHalf();
    kbSclReleaseWait();
    kbHalf();
    kbSclLow();
    kbSdaRelease();
    return v;
}

// Returns register value, or -1 if the device did not ACK (absent/hot-unplugged).
static int a164ReadReg(uint8_t reg) {
    kbStart();
    if (!kbWrite((KB_TAB5_ADDR << 1) | 0)) { kbStop(); return -1; }
    kbWrite(reg);
    kbStart();
    if (!kbWrite((KB_TAB5_ADDR << 1) | 1)) { kbStop(); return -1; }
    uint8_t v = kbReadByte(false);
    kbStop();
    return v;
}

// Returns bytes read, or -1 if absent.
static int a164ReadBlock(uint8_t reg, uint8_t *buf, uint8_t n) {
    kbStart();
    if (!kbWrite((KB_TAB5_ADDR << 1) | 0)) { kbStop(); return -1; }
    kbWrite(reg);
    kbStart();
    if (!kbWrite((KB_TAB5_ADDR << 1) | 1)) { kbStop(); return -1; }
    for (uint8_t i = 0; i < n; i++) buf[i] = kbReadByte(i < (n - 1));
    kbStop();
    return n;
}

static bool a164WriteReg(uint8_t reg, uint8_t val) {
    kbStart();
    bool ok = kbWrite((KB_TAB5_ADDR << 1) | 0);
    if (ok) ok &= kbWrite(reg);
    if (ok) ok &= kbWrite(val);
    kbStop();
    return ok;
}

static bool a164NameIs(const uint8_t *p, uint8_t len, const char *name) {
    if (len != (uint8_t)strlen(name)) return false;
    for (uint8_t i = 0; i < len; i++)
        if ((char)p[i] != name[i]) return false;
    return true;
}

// Drain the Character-mode FIFO. Fills `k` (word/enter/del/ctrl/alt/exit_key) and the
// nav out-params. Returns 1 if any event, 0 if empty, -1 if the keyboard is absent.
static int tab5KbDrain(keyStroke &k, bool &up, bool &dn, bool &lf, bool &rt, bool &en, bool &es) {
    int any = 0;
    for (uint8_t guard = 0; guard < 16; guard++) {
        int n = a164ReadReg(A164_REG_CHAR_LEN); // modifier(1) + payload length
        if (n < 0) return -1;                    // absent
        if (n == 0 || n == 0xFF) break;          // empty
        if (n > 16) n = 16;
        uint8_t buf[16];
        if (a164ReadBlock(A164_REG_CHAR_EVENT, buf, (uint8_t)n) < 0) return -1;
        uint8_t mod = buf[0];
        const uint8_t *pl = &buf[1];
        uint8_t pl_len = (uint8_t)(n - 1);
        if (mod & 0x01) k.ctrl = true;
        if (mod & 0x04) k.alt = true;
        any = 1;
        if (pl_len == 1) {
            k.word.emplace_back((char)pl[0]);
        } else if (pl_len > 1) {
            if (a164NameIs(pl, pl_len, "enter")) { k.enter = true; k.exit_key = true; en = true; }
            else if (a164NameIs(pl, pl_len, "esc")) { k.exit_key = true; es = true; }
            else if (a164NameIs(pl, pl_len, "backspace") || a164NameIs(pl, pl_len, "del")) { k.del = true; }
            else if (a164NameIs(pl, pl_len, "tab")) { k.word.emplace_back((char)0xB3); }
            else if (a164NameIs(pl, pl_len, "up")) { up = true; }
            else if (a164NameIs(pl, pl_len, "down")) { dn = true; }
            else if (a164NameIs(pl, pl_len, "left")) { lf = true; }
            else if (a164NameIs(pl, pl_len, "right")) { rt = true; }
            else { for (uint8_t i = 0; i < pl_len; i++) k.word.emplace_back((char)pl[i]); }
        }
    }
    a164WriteReg(A164_REG_INT_STAT, 0x00); // release latched INT (harmless if unused)
    return any;
}

/***************************************************************************************
** _setup_gpio()
***************************************************************************************/
void _setup_gpio() {
    M5.begin();
    setSysI2CBus(M5.In_I2C.getPort() == I2C_NUM_1 ? &Wire1 : &Wire);
    kbIdle(); // park the bit-banged KB bus (G0/G1) released/high
}

/***************************************************************************************
** getBattery()  (1-100)
***************************************************************************************/
int getBattery() {
    int percent = M5.Power.getBatteryLevel();
    return (percent < 0) ? 1 : (percent >= 100) ? 100 : percent;
}

/*********************************************************************
** setBrightness
**********************************************************************/
void _setBrightness(uint8_t brightval) { M5.Display.setBrightness(brightval); }

/*********************************************************************
** InputHandler
**********************************************************************/
void InputHandler(void) {
    static unsigned long tm = 0;

    // ---- A164 keyboard (software I2C on G0/G1; polled, hot-plug tolerant) ----
    {
        keyStroke key;
        bool up = false, dn = false, lf = false, rt = false, en = false, es = false;
        int r = -1;
        if (tab5KbReady) {
            r = tab5KbDrain(key, up, dn, lf, rt, en, es);
            if (r < 0) tab5KbReady = false; // hot-unplugged
        } else if (millis() >= tab5KbNextProbe) {
            tab5KbNextProbe = millis() + 750; // re-probe a detachable keyboard periodically
            if (a164ReadReg(A164_REG_CHAR_LEN) >= 0) { // ACKed -> present
                a164WriteReg(A164_REG_MODE, A164_MODE_CHARACTER);
                tab5KbReady = true;
            }
        }
        if (r > 0) {
            // Apply everything only AFTER the wake-gate so the wake keypress doesn't also act.
            if (!wakeUpScreen()) {
                AnyKeyPress = true;
                if (up) UpPress = true;
                if (dn) DownPress = true;
                if (lf) PrevPress = true;
                if (rt) NextPress = true;
                if (en) SelPress = true;
                if (es) EscPress = true;
                if (key.word.size() || key.del || key.enter || key.exit_key || key.ctrl || key.alt) {
                    key.pressed = true;
                    KeyStroke = key;
                }
            }
        }
    }

    // ---- Touch (throttled ~200 ms; shares the sys I2C bus) ----
    if (millis() - tm < 200 && !LongPress) return;
    if (!trylockSysI2CBus()) return;
    M5.update();
    unlockSysI2CBus();
    auto t = M5.Touch.getDetail();
    if (t.isPressed() || t.isHolding()) {
        tm = millis();
        if (!wakeUpScreen()) AnyKeyPress = true;
        else return;
        // M5.Touch.getDetail() is already rotation-aware (setRotation(3) -> 1280x720),
        // so NO manual transform (adding one would double-rotate).
        touchPoint.x = t.x;
        touchPoint.y = t.y;
        touchPoint.pressed = true;
        touchHeatMap(touchPoint);
    } else {
        touchPoint.pressed = false;
    }
}

/*********************************************************************
** powerOff / goToDeepSleep / checkReboot
**********************************************************************/
void powerOff() { M5.Power.powerOff(); }
void goToDeepSleep() { M5.Power.deepSleep(); }
void checkReboot() {}

/***************************************************************************************
** isCharging()  — use M5Unified's CHG_STAT (I/O-expander) line, not the shunt-current sign
***************************************************************************************/
bool isCharging() {
    auto st = M5.Power.isCharging();
    return st == decltype(st)::is_charging;
}
