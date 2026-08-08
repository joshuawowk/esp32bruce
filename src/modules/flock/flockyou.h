/**
 * @file flockyou.h
 * @brief Combined Flock-You surveillance detector app for Bruce.
 *
 * Merges two detection paths into one on-device view:
 *   - BLE   : scanned on-device (NimBLE) — Flock / Raven / SoundThinking /
 *             manufacturer-ID / device-name matching.
 *   - WiFi  : promiscuous IE-fingerprint detection performed by a dedicated
 *             ESP32-S3 co-processor, delivered as JSON `detection` lines over
 *             a UART link (see docs/COMBINED_BUILD.md in the flock-you repo).
 *
 * Both streams merge into one MAC-keyed table, rendered on the TFT and
 * exportable to SD/LittleFS.
 */

#ifndef __FLOCKYOU_H__
#define __FLOCKYOU_H__

#if !defined(LITE_VERSION)

// Full-screen app entry points (called from FlockMenu).
void flockyou_run_all();   // BLE + WiFi co-processor link
void flockyou_run_ble();   // BLE only
void flockyou_run_wifi();  // WiFi co-processor link only

// Session utilities (usable from the menu without entering the live view).
void flockyou_export();    // write current session to /BruceFlock on SD/LittleFS
void flockyou_clear();     // clear the in-memory session table

#endif // LITE_VERSION
#endif // __FLOCKYOU_H__
