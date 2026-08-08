// ---------------------------------------------------------------------------
// ESP32-P4 ESP-NOW stubs (Tab5 port)
//
// The ESP32-P4 has no native Wi-Fi radio; Wi-Fi runs on the ESP32-C6 via esp-hosted,
// which does not export the ESP-NOW API. Bruce's device-to-device "connect" feature
// (src/core/connect/*) calls esp_now_* directly, so provide inert stubs to let the
// firmware link. The feature is a no-op on Tab5 until/unless esp-hosted exposes
// ESP-NOW. This file is compiled ONLY for the Tab5 board (its dir is added to
// build_src_filter for this env only), so no target guard is needed.
// ---------------------------------------------------------------------------
#include "esp_now.h"
#include <stddef.h>

extern "C" {

__attribute__((weak)) esp_err_t esp_now_init(void) { return ESP_ERR_NOT_SUPPORTED; }
__attribute__((weak)) esp_err_t esp_now_deinit(void) { return ESP_OK; }

__attribute__((weak)) esp_err_t esp_now_send(const uint8_t *peer_addr, const uint8_t *data, size_t len) {
    (void)peer_addr; (void)data; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb) { (void)cb; return ESP_ERR_NOT_SUPPORTED; }
__attribute__((weak)) esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { (void)cb; return ESP_ERR_NOT_SUPPORTED; }
__attribute__((weak)) esp_err_t esp_now_unregister_send_cb(void) { return ESP_OK; }
__attribute__((weak)) esp_err_t esp_now_unregister_recv_cb(void) { return ESP_OK; }

__attribute__((weak)) esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer) { (void)peer; return ESP_ERR_NOT_SUPPORTED; }
__attribute__((weak)) bool esp_now_is_peer_exist(const uint8_t *peer_addr) { (void)peer_addr; return false; }

} // extern "C"
