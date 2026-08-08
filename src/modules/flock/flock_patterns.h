/**
 * @file flock_patterns.h
 * @brief Flock-You BLE detection pattern tables.
 *
 * Ported from the flock-you `dev` branch (BLE detector). Only the BLE-side
 * signatures live here — WiFi promiscuous / IE-fingerprint detection runs on
 * the dedicated ESP32-S3 co-processor and reaches the host as ready-made
 * JSON `detection` lines over the UART link (see flockyou.cpp).
 */

#ifndef __FLOCK_PATTERNS_H__
#define __FLOCK_PATTERNS_H__

#include <stdint.h>
#include <stddef.h>

// ---- MAC address prefixes (OUIs), lowercase, colon-separated ----

// Flock Safety — high-confidence OUIs (direct registration or exclusive use).
static const char *FLOCK_MAC_PREFIXES[] = {
    // FS Ext Battery devices
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    // Flock WiFi devices
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea",
    // Flock Safety (direct IEEE registration)
    "b4:1e:52"
};

// Flock Safety contract manufacturers — lower confidence alone (Liteon / USI
// also ship unrelated consumer/enterprise gear).
static const char *FLOCK_MFR_MAC_PREFIXES[] = {
    "f4:6a:dd", "f8:a2:d6", "e0:0a:f6", "00:f4:8d", "d0:39:57", "e8:d0:fc"
};

// SoundThinking (formerly ShotSpotter) — acoustic gunshot sensors.
static const char *SOUNDTHINKING_MAC_PREFIXES[] = {
    "d4:11:d6"
};

// ---- BLE device-name substrings (case-insensitive) ----
static const char *DEVICE_NAME_PATTERNS[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision"
};

// ---- BLE manufacturer company IDs ----
// Source: wgreenberg/flock-you — XUNTONG ID associated with Flock devices.
static const uint16_t BLE_MANUFACTURER_IDS[] = {
    0x09C8   // XUNTONG
};

// ---- Raven (gunshot detector) BLE service UUIDs ----
#define RAVEN_DEVICE_INFO_SERVICE   "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE           "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE         "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE       "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE        "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE         "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE    "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE  "00001819-0000-1000-8000-00805f9b34fb"

static const char *RAVEN_SERVICE_UUIDS[] = {
    RAVEN_DEVICE_INFO_SERVICE, RAVEN_GPS_SERVICE,   RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,     RAVEN_UPLOAD_SERVICE, RAVEN_ERROR_SERVICE,
    RAVEN_OLD_HEALTH_SERVICE,  RAVEN_OLD_LOCATION_SERVICE
};

// Count helpers.
#define FLOCK_MAC_COUNT          (sizeof(FLOCK_MAC_PREFIXES) / sizeof(FLOCK_MAC_PREFIXES[0]))
#define FLOCK_MFR_MAC_COUNT      (sizeof(FLOCK_MFR_MAC_PREFIXES) / sizeof(FLOCK_MFR_MAC_PREFIXES[0]))
#define SOUNDTHINKING_MAC_COUNT  (sizeof(SOUNDTHINKING_MAC_PREFIXES) / sizeof(SOUNDTHINKING_MAC_PREFIXES[0]))
#define DEVICE_NAME_COUNT        (sizeof(DEVICE_NAME_PATTERNS) / sizeof(DEVICE_NAME_PATTERNS[0]))
#define BLE_MFR_ID_COUNT         (sizeof(BLE_MANUFACTURER_IDS) / sizeof(BLE_MANUFACTURER_IDS[0]))
#define RAVEN_UUID_COUNT         (sizeof(RAVEN_SERVICE_UUIDS) / sizeof(RAVEN_SERVICE_UUIDS[0]))

#endif // __FLOCK_PATTERNS_H__
