/**
 * @file flockyou.cpp
 * @brief Combined Flock-You detector: on-device BLE + WiFi co-processor link.
 * @see flockyou.h, flock_patterns.h, docs/COMBINED_BUILD.md (flock-you repo)
 */

#if !defined(LITE_VERSION)

#include "flockyou.h"
#include "flock_patterns.h"

#include "core/display.h"      // tft, drawMainBorderWithTitle, padprintln, displayError/Success
#include "core/mykeyboard.h"   // input helpers
#include "core/sd_functions.h" // getFsStorage, FS/File, FILE_WRITE/APPEND
#include <globals.h>           // check(), EscPress, returnToMenu, tftWidth/Height, bruceConfig

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <string>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// ============================================================================
// CONFIG
// ============================================================================

#define FY_MAX          200      // unified table capacity
#define FY_LINK_BAUD    115200
#define FY_LINK_STALE_MS 6000    // co-proc considered offline after this
#define FY_ALERT_MS     3000     // red banner duration after a new hit
#define FY_REDRAW_MS    500      // UI refresh cadence
#define FY_ROWS         7        // detection rows shown on screen

// Co-processor UART link pins. Default to the board's exposed Grove/SERIAL
// header (SERIAL_RX/SERIAL_TX from pins_arduino.h); override in build_flags
// if you wired the link elsewhere. UART1 is free on T-Display-S3 (UART2=GPS).
#ifndef FLOCK_LINK_RX
#define FLOCK_LINK_RX   SERIAL_RX
#endif
#ifndef FLOCK_LINK_TX
#define FLOCK_LINK_TX   SERIAL_TX
#endif

// ============================================================================
// UNIFIED DETECTION TABLE  (shared: BLE task + app loop -> mutex-guarded)
// ============================================================================

struct FYDet {
    char     mac[18];
    char     name[40];
    char     method[28];
    char     proto;        // 'W' = wifi_2_4ghz, 'B' = bluetooth_le
    int      rssi;
    uint8_t  channel;      // WiFi only
    uint32_t firstSeen;
    uint32_t lastSeen;
    int      count;
    bool     isRaven;
    char     ravenFW[12];
    bool     hasGPS;
    double   lat;
    double   lon;
};

static FYDet             g_det[FY_MAX];
static int               g_detCount = 0;
static SemaphoreHandle_t g_mutex    = nullptr;

// Latest GPS fix from the co-proc link (host applies it to all detections).
// All three are written and read only under g_mutex (see fyHandleLine/fyAdd),
// because the reader runs in the NimBLE host task and doubles are not atomic.
#define FY_GPS_STALE_MS 5000   // drop the fix if no fresh valid GPS within this window
static bool     g_gpsValid = false;
static double   g_gpsLat = 0, g_gpsLon = 0;
static uint32_t g_gpsFixMs = 0;

// Co-proc link status (from `status` lines). App-loop task only.
static bool          g_linkSeen   = false;
static unsigned long g_linkLastMs = 0;
static uint8_t       g_linkCh     = 0;
static int           g_linkDet    = 0;

// Alert timestamp (read by the UI without the mutex; advisory only).
static volatile unsigned long g_lastAlertMs = 0;

// Set when the table hits FY_MAX and a new device had to be dropped (advisory).
static volatile bool g_tableFull = false;

// True only while a valid fix is fresh. Caller must hold g_mutex.
static inline bool fyGpsFresh() { return g_gpsValid && (millis() - g_gpsFixMs) < FY_GPS_STALE_MS; }

// ============================================================================
// SMALL HELPERS
// ============================================================================

// Case-insensitive substring (avoids relying on non-portable strcasestr).
static const char *fy_stristr(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return hay;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
        if (!*n) return hay;
    }
    return nullptr;
}

static bool fy_prefixMatch(const char *mac, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (strncasecmp(mac, list[i], 8) == 0) return true;
    return false;
}

static bool fy_nameMatch(const char *name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < DEVICE_NAME_COUNT; i++)
        if (fy_stristr(name, DEVICE_NAME_PATTERNS[i])) return true;
    return false;
}

static bool fy_mfrMatch(uint16_t id) {
    for (size_t i = 0; i < BLE_MFR_ID_COUNT; i++)
        if (BLE_MANUFACTURER_IDS[i] == id) return true;
    return false;
}

// ============================================================================
// TABLE OPS  (all under g_mutex)
// ============================================================================

// Insert or update by MAC. Returns true if this was a brand-new MAC.
static bool fyAdd(const char *mac, const char *name, const char *method,
                  char proto, int rssi, uint8_t channel,
                  bool isRaven, const char *ravenFW) {
    if (!g_mutex || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    uint32_t now = millis();
    for (int i = 0; i < g_detCount; i++) {
        if (strcasecmp(g_det[i].mac, mac) == 0) {
            g_det[i].count++;
            g_det[i].lastSeen = now;
            g_det[i].rssi = rssi;
            if (channel) g_det[i].channel = channel;
            if (name && name[0] && !g_det[i].name[0])
                strlcpy(g_det[i].name, name, sizeof(g_det[i].name));
            if (fyGpsFresh()) { g_det[i].hasGPS = true; g_det[i].lat = g_gpsLat; g_det[i].lon = g_gpsLon; }
            xSemaphoreGive(g_mutex);
            return false;
        }
    }

    bool isNew = false;
    if (g_detCount < FY_MAX) {
        FYDet &d = g_det[g_detCount];
        memset(&d, 0, sizeof(d));
        strlcpy(d.mac, mac, sizeof(d.mac));
        if (name)   strlcpy(d.name, name, sizeof(d.name));
        if (method) strlcpy(d.method, method, sizeof(d.method));
        d.proto = proto;
        d.rssi = rssi;
        d.channel = channel;
        d.firstSeen = now;
        d.lastSeen = now;
        d.count = 1;
        d.isRaven = isRaven;
        if (ravenFW) strlcpy(d.ravenFW, ravenFW, sizeof(d.ravenFW));
        if (fyGpsFresh()) { d.hasGPS = true; d.lat = g_gpsLat; d.lon = g_gpsLon; }
        g_detCount++;
        isNew = true;
    } else {
        g_tableFull = true;   // capacity reached — a new device was dropped
    }
    xSemaphoreGive(g_mutex);
    return isNew;
}

static void fyNoteHit(bool isNew, bool highConf) {
    (void)isNew;
    if (highConf) g_lastAlertMs = millis();  // keeps the banner lit while a target is present
}

// ============================================================================
// BLE DETECTION  (NimBLE 2.x scan callback — runs in the NimBLE host task)
// ============================================================================

static const char *fyRavenFW(const NimBLEAdvertisedDevice *dev) {
    bool newGps = false, oldLoc = false, power = false;
    if (!dev->haveServiceUUID()) return "?";
    size_t n = dev->getServiceUUIDCount();
    for (size_t i = 0; i < n; i++) {
        std::string u = dev->getServiceUUID(i).toString();
        if (strcasecmp(u.c_str(), RAVEN_GPS_SERVICE) == 0)          newGps = true;
        if (strcasecmp(u.c_str(), RAVEN_OLD_LOCATION_SERVICE) == 0) oldLoc = true;
        if (strcasecmp(u.c_str(), RAVEN_POWER_SERVICE) == 0)        power = true;
    }
    if (oldLoc && !newGps) return "1.1.x";
    if (newGps && !power)  return "1.2.x";
    if (newGps && power)   return "1.3.x";
    return "?";
}

static bool fyRavenMatch(const NimBLEAdvertisedDevice *dev) {
    if (!dev->haveServiceUUID()) return false;
    size_t n = dev->getServiceUUIDCount();
    for (size_t i = 0; i < n; i++) {
        std::string u = dev->getServiceUUID(i).toString();
        for (size_t j = 0; j < RAVEN_UUID_COUNT; j++)
            if (strcasecmp(u.c_str(), RAVEN_SERVICE_UUIDS[j]) == 0) return true;
    }
    return false;
}

class FYScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (!dev) return;

        std::string addr = dev->getAddress().toString();
        char macPrefix[9];
        snprintf(macPrefix, sizeof(macPrefix), "%.8s", addr.c_str());

        int rssi = dev->getRSSI();
        if (rssi == 0) rssi = -100;
        std::string name = dev->getName();

        const char *method = nullptr;
        bool highConf = true;
        bool isRaven = false;
        const char *ravenFW = "";

        if (fy_prefixMatch(macPrefix, FLOCK_MAC_PREFIXES, FLOCK_MAC_COUNT)) {
            method = "mac_prefix";
        } else if (fy_prefixMatch(macPrefix, SOUNDTHINKING_MAC_PREFIXES, SOUNDTHINKING_MAC_COUNT)) {
            method = "mac_prefix_soundthinking";
        } else if (fy_prefixMatch(macPrefix, FLOCK_MFR_MAC_PREFIXES, FLOCK_MFR_MAC_COUNT)) {
            method = "mac_prefix_mfr";
            highConf = false;
        } else if (!name.empty() && fy_nameMatch(name.c_str())) {
            method = "device_name";
        } else if (dev->haveManufacturerData()) {
            std::string m = dev->getManufacturerData();
            if (m.length() >= 2) {
                const uint8_t *d = (const uint8_t *)m.data();
                uint16_t code = ((uint16_t)d[1] << 8) | (uint16_t)d[0];  // little-endian
                if (fy_mfrMatch(code)) method = "ble_mfr_id";
            }
        }
        if (!method && fyRavenMatch(dev)) {
            method = "raven_uuid";
            isRaven = true;
            ravenFW = fyRavenFW(dev);
        }
        if (!method) return;

        bool isNew = fyAdd(addr.c_str(), name.c_str(), method, 'B', rssi, 0, isRaven, ravenFW);
        fyNoteHit(isNew, highConf);
    }
};

static FYScanCallbacks g_scanCb;
static NimBLEScan     *g_scan = nullptr;
static bool            g_bleOwned = false;   // true only if WE initialized NimBLE

static void fyBleStart() {
    // Only take ownership (and thus responsibility to deinit) if NimBLE wasn't
    // already up — otherwise we'd tear down a stack another feature owns.
    g_bleOwned = !NimBLEDevice::isInitialized();
    if (g_bleOwned) NimBLEDevice::init("");
    g_scan = NimBLEDevice::getScan();
    g_scan->setScanCallbacks(&g_scanCb);
    g_scan->setActiveScan(true);
    g_scan->setInterval(100);
    g_scan->setWindow(99);
    g_scan->setDuplicateFilter(false);
    g_scan->setMaxResults(0);         // callback-driven: don't buffer in RAM
    g_scan->start(0, false);          // 0 = scan continuously until stopped
}

static void fyBleStop() {
    if (g_scan) { g_scan->stop(); g_scan->clearResults(); g_scan = nullptr; }
    if (g_bleOwned) { NimBLEDevice::deinit(true); g_bleOwned = false; }
}

// ============================================================================
// WiFi CO-PROCESSOR LINK  (UART1, newline-delimited JSON)
// ============================================================================

static HardwareSerial FYLink(1);
static char   g_lineBuf[512];
static size_t g_lineLen = 0;
static bool   g_linkOpen = false;

static void fyHandleLine(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '{') return;  // ignore human-readable co-proc log lines

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;

    const char *ev = doc["event"] | "";
    if (strcmp(ev, "detection") == 0) {
        const char *mac    = doc["mac_address"] | "";
        if (!mac[0]) return;
        const char *method = doc["detection_method"] | "wifi";
        const char *ssid   = doc["ssid"] | "";
        int rssi = doc["rssi"] | 0;
        int ch   = doc["channel"] | 0;
        bool isNew = fyAdd(mac, ssid, method, 'W', rssi, (uint8_t)ch, false, "");
        fyNoteHit(isNew, true);   // co-proc detections are IE-verified => high confidence
    } else if (strcmp(ev, "gps") == 0) {
        // Parse outside the lock, then publish under the SAME mutex fyAdd's
        // reads hold (the reader runs in the NimBLE host task and doubles are
        // not atomically loadable on Xtensa). Publish g_gpsValid LAST.
        bool   valid = doc["valid"] | false;
        double lat   = doc["lat"] | 0.0;
        double lon   = doc["lon"] | 0.0;
        if (g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (valid) {
                g_gpsLat = lat;
                g_gpsLon = lon;
                g_gpsFixMs = millis();
                g_gpsValid = true;
            } else {
                g_gpsValid = false;   // explicit loss of fix
            }
            xSemaphoreGive(g_mutex);
        }
    } else if (strcmp(ev, "status") == 0) {
        g_linkSeen   = true;
        g_linkLastMs = millis();
        g_linkCh     = (uint8_t)(doc["channel"] | 0);
        g_linkDet    = doc["det"] | 0;
    }
}

static void fyLinkStart() {
    FYLink.setRxBufferSize(1024);  // absorb bursts while the UI redraws
    FYLink.begin(FY_LINK_BAUD, SERIAL_8N1, FLOCK_LINK_RX, FLOCK_LINK_TX);
    g_linkOpen = true;
    g_lineLen = 0;
}

static void fyLinkStop() {
    if (g_linkOpen) { FYLink.end(); g_linkOpen = false; }
}

static void fyLinkPump() {
    if (!g_linkOpen) return;
    while (FYLink.available() > 0) {
        char c = (char)FYLink.read();
        if (c == '\n' || c == '\r') {
            if (g_lineLen > 0) { g_lineBuf[g_lineLen] = '\0'; fyHandleLine(g_lineBuf); g_lineLen = 0; }
        } else if (g_lineLen < sizeof(g_lineBuf) - 1) {
            g_lineBuf[g_lineLen++] = c;
        } else {
            g_lineLen = 0;  // overrun — drop line
        }
    }
    // Mark the co-proc offline if `status` lines stop arriving.
    if (g_linkSeen && millis() - g_linkLastMs > FY_LINK_STALE_MS) g_linkSeen = false;
}

// ============================================================================
// DISPLAY
// ============================================================================

struct FYRow { char mac[18]; char name[24]; char proto; int rssi; int count; bool raven; };

// Copy the N most-recently-seen rows under the mutex, plus live counters.
static int fySnapshot(FYRow *rows, int maxRows, int &total, int &wifi, int &ble, int &raven) {
    total = wifi = ble = raven = 0;
    if (!g_mutex || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

    total = g_detCount;
    for (int i = 0; i < g_detCount; i++) {
        if (g_det[i].proto == 'W') wifi++; else ble++;
        if (g_det[i].isRaven) raven++;
    }

    // Select the maxRows entries with the largest lastSeen (simple selection).
    bool used[FY_MAX] = {false};
    int n = 0;
    for (int k = 0; k < maxRows; k++) {
        int best = -1;
        uint32_t bestTs = 0;
        for (int i = 0; i < g_detCount; i++) {
            if (used[i]) continue;
            if (best < 0 || g_det[i].lastSeen >= bestTs) { best = i; bestTs = g_det[i].lastSeen; }
        }
        if (best < 0) break;
        used[best] = true;
        strlcpy(rows[n].mac, g_det[best].mac, sizeof(rows[n].mac));
        strlcpy(rows[n].name, g_det[best].name, sizeof(rows[n].name));
        rows[n].proto = g_det[best].proto;
        rows[n].rssi  = g_det[best].rssi;
        rows[n].count = g_det[best].count;
        rows[n].raven = g_det[best].isRaven;
        n++;
    }
    xSemaphoreGive(g_mutex);
    return n;
}

static void fyDraw(bool ble, bool wifi) {
    FYRow rows[FY_ROWS];
    int total, w, b, raven;
    int n = fySnapshot(rows, FY_ROWS, total, w, b, raven);

    tft.fillRect(0, 26, tftWidth, tftHeight - 26, bruceConfig.bgColor);
    tft.setTextSize(1);

    int y = 30;
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setCursor(6, y);
    tft.printf("Tot:%d%s  W:%d  B:%d  Raven:%d", total, g_tableFull ? "!" : "", w, b, raven);
    y += 12;

    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.setCursor(6, y);
    if (wifi) {
        if (g_linkSeen) tft.printf("Link:UP ch%d  ", g_linkCh);
        else            tft.printf("Link:--  ");
    } else {
        tft.printf("Link:off  ");
    }
    bool gpsFresh = g_gpsValid && (millis() - g_gpsFixMs) < FY_GPS_STALE_MS;
    tft.printf("BLE:%s  GPS:%s", ble ? "on" : "off", gpsFresh ? "fix" : "--");
    y += 14;

    // Alert banner
    if (millis() - g_lastAlertMs < FY_ALERT_MS) {
        tft.fillRect(0, y, tftWidth, 12, TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.setCursor(6, y + 2);
        tft.print("** SURVEILLANCE DEVICE DETECTED **");
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    }
    y += 14;

    if (n == 0) {
        tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
        tft.setCursor(6, y);
        tft.print("Scanning...");
        return;
    }

    for (int i = 0; i < n && y < tftHeight - 10; i++) {
        uint16_t col = rows[i].raven ? TFT_RED : bruceConfig.priColor;
        tft.setTextColor(col, bruceConfig.bgColor);
        tft.setCursor(6, y);
        const char *label = rows[i].name[0] ? rows[i].name : rows[i].mac;
        tft.printf("%c %-17.17s %ddBm x%d", rows[i].proto, label, rows[i].rssi, rows[i].count);
        y += 11;
    }
}

// ============================================================================
// SD / LittleFS EXPORT  (CSV, Flask/wardriving-friendly)
// ============================================================================

// Emit an RFC-4180-safe, formula-injection-neutralized CSV field (including the
// surrounding double quotes) into dst. BLE device names / WiFi SSIDs are
// attacker-controlled and can contain commas, quotes, newlines, or a leading
// =/+/-/@ that spreadsheets execute as a formula.
static void fyCsvField(char *dst, size_t cap, const char *src) {
    if (cap < 3) { if (cap) dst[0] = '\0'; return; }
    size_t o = 0;
    dst[o++] = '"';
    if (src && (*src == '=' || *src == '+' || *src == '-' || *src == '@' ||
                *src == '\t' || *src == '\r')) {
        if (o < cap - 2) dst[o++] = '\'';   // defuse formula
    }
    for (const char *p = src ? src : ""; *p && o < cap - 2; p++) {
        char c = *p;
        if (c == '"') { if (o < cap - 3) { dst[o++] = '"'; dst[o++] = '"'; } else break; }
        else if (c == '\n' || c == '\r') { dst[o++] = ' '; }   // strip embedded newlines
        else dst[o++] = c;
    }
    dst[o++] = '"';
    dst[o] = '\0';
}

static bool fyWriteCsv() {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || !fs) return false;

    const char *dir = "/BruceFlock";
    if (!(*fs).exists(dir)) (*fs).mkdir(dir);

    // Pick an incrementing filename.
    char path[48];
    int idx = 0;
    do {
        snprintf(path, sizeof(path), "%s/flock_%d.csv", dir, idx++);
    } while ((*fs).exists(path) && idx < 1000);

    File f = (*fs).open(path, FILE_WRITE);
    if (!f) return false;

    f.println("mac,name,protocol,method,rssi,channel,count,first_ms,last_ms,is_raven,raven_fw,lat,lon");
    if (g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        char line[320];
        char qmac[40], qname[96], qmethod[64], qfw[32];
        for (int i = 0; i < g_detCount; i++) {
            FYDet &d = g_det[i];
            const char *proto = d.proto == 'W' ? "wifi_2_4ghz" : "bluetooth_le";
            fyCsvField(qmac, sizeof(qmac), d.mac);
            fyCsvField(qname, sizeof(qname), d.name);
            fyCsvField(qmethod, sizeof(qmethod), d.method);
            fyCsvField(qfw, sizeof(qfw), d.ravenFW);
            if (d.hasGPS) {
                snprintf(line, sizeof(line),
                    "%s,%s,%s,%s,%d,%u,%d,%lu,%lu,%s,%s,%.6f,%.6f\n",
                    qmac, qname, proto, qmethod, d.rssi, (unsigned)d.channel, d.count,
                    (unsigned long)d.firstSeen, (unsigned long)d.lastSeen,
                    d.isRaven ? "true" : "false", qfw, d.lat, d.lon);
            } else {
                snprintf(line, sizeof(line),
                    "%s,%s,%s,%s,%d,%u,%d,%lu,%lu,%s,%s,,\n",
                    qmac, qname, proto, qmethod, d.rssi, (unsigned)d.channel, d.count,
                    (unsigned long)d.firstSeen, (unsigned long)d.lastSeen,
                    d.isRaven ? "true" : "false", qfw);
            }
            f.print(line);
        }
        xSemaphoreGive(g_mutex);
    }
    f.close();
    return true;
}

// ============================================================================
// APP LIFECYCLE
// ============================================================================

static void fyEnsureMutex() {
    if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
}

static void fyRun(bool ble, bool wifi) {
    fyEnsureMutex();
    returnToMenu = false;

    drawMainBorderWithTitle("Flock-You");
#if defined(HAS_TOUCH)
    // Touch-only boards (e.g. the ES3C28P host) have no physical Esc key — the
    // back gesture is a tap in the invisible top-left cell of the screen. Mark
    // it so exit is discoverable. Drawn once; fyDraw only repaints y >= 26.
    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.setCursor(6, 8);
    tft.print("< exit");
#endif
    if (wifi) fyLinkStart();
    if (ble)  fyBleStart();

    fyDraw(ble, wifi);
    unsigned long lastDraw = millis();

    for (;;) {
        if (returnToMenu || check(EscPress)) break;

        if (wifi) fyLinkPump();

        // Safety re-arm: keep a continuous BLE scan alive.
        if (ble && g_scan && !g_scan->isScanning()) g_scan->start(0, false);

        if (millis() - lastDraw >= FY_REDRAW_MS) {
            fyDraw(ble, wifi);
            lastDraw = millis();
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    if (ble)  fyBleStop();
    if (wifi) fyLinkStop();
    returnToMenu = true;
}

// ============================================================================
// PUBLIC ENTRY POINTS
// ============================================================================

void flockyou_run_all()  { fyRun(true,  true);  }
void flockyou_run_ble()  { fyRun(true,  false); }
void flockyou_run_wifi() { fyRun(false, true);  }

void flockyou_clear() {
    fyEnsureMutex();
    if (g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        g_detCount = 0;
        memset(g_det, 0, sizeof(g_det));
        xSemaphoreGive(g_mutex);
    }
    displaySuccess("Session cleared", true);
}

void flockyou_export() {
    fyEnsureMutex();
    if (g_detCount == 0) { displayError("No detections yet", true); return; }
    if (fyWriteCsv()) displaySuccess("Saved to /BruceFlock", true);
    else              displayError("Export failed", true);
}

#endif // LITE_VERSION
