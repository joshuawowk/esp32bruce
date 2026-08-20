#pragma once
// ---------------------------------------------------------------------------
// ESP32-P4 BLE power compatibility shim (Tab5 port)
//
// The P4 has no local BT controller; it runs the IDF's hosted NimBLE over the
// ESP32-C6 (esp-hosted). The legacy Bluedroid TX-power API (esp_power_level_t,
// ESP_PWR_LVL_*, esp_ble_tx_power_set) does not exist there. NimBLEDevice's
// modern setPower(int8_t dBm) overload IS available, so map the legacy level
// enums to plain dBm integers. On all other targets this header is a no-op and
// the real esp_bt.h symbols are used unchanged.
// ---------------------------------------------------------------------------
#ifdef CONFIG_IDF_TARGET_ESP32P4
#ifndef BRUCE_BLE_PWR_COMPAT
#define BRUCE_BLE_PWR_COMPAT
typedef int esp_power_level_t;
typedef int esp_ble_power_type_t;
enum {
    ESP_PWR_LVL_N24 = -24, ESP_PWR_LVL_N21 = -21, ESP_PWR_LVL_N18 = -18,
    ESP_PWR_LVL_N15 = -15, ESP_PWR_LVL_N12 = -12, ESP_PWR_LVL_N9  = -9,
    ESP_PWR_LVL_N6  = -6,  ESP_PWR_LVL_N3  = -3,  ESP_PWR_LVL_N0  = 0,
    ESP_PWR_LVL_P3  = 3,   ESP_PWR_LVL_P6  = 6,   ESP_PWR_LVL_P9  = 9,
    ESP_PWR_LVL_P12 = 12,  ESP_PWR_LVL_P15 = 15,  ESP_PWR_LVL_P18 = 18,
    ESP_PWR_LVL_P20 = 20,  ESP_PWR_LVL_P21 = 20
};
#define ESP_BLE_PWR_TYPE_ADV 9
#endif // BRUCE_BLE_PWR_COMPAT
#endif // CONFIG_IDF_TARGET_ESP32P4
