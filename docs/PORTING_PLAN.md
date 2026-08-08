# Bruce → M5Stack Tab5 (ESP32‑P4) — End‑to‑End Porting Plan

**Author:** Josh (with Claude)  ·  **Date:** 2026‑08‑08  ·  **Status:** Draft v3 — Launcher‑validated (§17) + adversarially reviewed (§18); scope revised (offense de‑prioritized, D1/D7/D8). **Read §18 first — it overrides conflicting earlier text.**
**Target device:** M5Stack Tab5 (SKU C145/K145) + Tab5 Keyboard (A164)
**Source firmware:** Bruce (`/home/jwowk/Repos/esp32bruce`, pioarduino/Arduino)

---

## 1. Executive summary

Bruce is a pure **Arduino‑framework** firmware built with the **pioarduino** fork of `platform-espressif32`. The Tab5's main SoC is the **ESP32‑P4** (RISC‑V), which M5Stack **officially supports under that same pioarduino Arduino toolchain** (M5Unified + M5GFX). This is the key enabler: we do **not** rewrite Bruce into ESP‑IDF/LVGL. We add a new board to Bruce, drive the display/touch/board through Bruce's existing **`m5gfx` HAL backend + M5Unified**, and solve the one genuinely hard problem — the P4 has **no radio** and offloads Wi‑Fi/BLE to an on‑board **ESP32‑C6** over SDIO (esp‑hosted).

Per scope decisions (2026‑08‑08):

1. **Offensive radio de‑prioritized (revised 2026‑08‑08).** Stock esp‑hosted does **not** expose promiscuous RX / raw 802.11 TX on the internal C6, and rather than build custom C6 firmware, red‑team monitor/injection will be **offloaded to an external companion ESP32** (UART/SPI radio co‑processor) if/when wanted. The internal C6 is used only for **connectivity** Wi‑Fi/BLE. This removes the longest pole from the critical path.
2. **External‑module config hooks now** — the board definition wires pin/bus hooks for CC1101, NRF24, PN532/RFID, IR, LoRa, FM, GPS via the Grove Port.A / M5‑Bus / Stamp / GPIO_EXT headers, so those menus work when a module is attached.
3. **M5Unified/M5GFX‑assisted** display, touch, IMU, RTC, power, audio bring‑up, bridged into Bruce's HAL globals.
4. **Hardware on hand = external modules only** (no Tab5 / A164 / C6‑flash rig yet). Execution is therefore **compile‑first**: every phase must build cleanly for `esp32p4`; on‑device validation follows the bring‑up checklist in §13 when hardware arrives.

**Feasibility verdict:** Green for the platform/UI/storage/non‑radio features (they ride Bruce's SoC‑agnostic bus HAL) — including the **directly‑attached SPI/UART modules Josh is adding (nRF24, CC1101, GT‑U7 GPS)**. Yellow→Green for connectivity Wi‑Fi/BLE and native USB‑HID (dependency‑gated on the pioarduino P4 core maturity). **Deferred** (optional, via companion ESP32): Wi‑Fi/BLE monitor+injection (deauth/sniffer/karma/pwnagotchi/BLE‑spam).

---

## 2. Scope & decisions of record

| # | Decision | Consequence for the plan |
|---|---|---|
| D1 | *(revised 2026‑08‑08)* Offensive Wi‑Fi/BLE **de‑prioritized**; when wanted, offloaded to an **external companion ESP32**, not the internal C6 | Phase 8 leaves the critical path → optional later track (§11). Internal C6 stays for **connectivity** Wi‑Fi/BLE only |
| D7 | External radios hang **directly off the Tab5**: **nRF24** + **CC1101** (SPI), **GT‑U7** GPS (UART) | Promote module hooks to a near‑term phase; seed buses in `_post_setup_gpio` on M5‑Bus SPI (MOSI G18/MISO G19/SCK G5) + a UART for GPS |
| D8 | Red‑team monitor+injection via **companion ESP32** (UART/SPI radio co‑processor) | Sidesteps the hard custom esp‑hosted CustomRpc work entirely; integrate later over a serial command link |
| D2 | Keep Bruce in Arduino/pioarduino (no IDF/LVGL rewrite) | Add board via Bruce's 4‑file board pattern; reuse `m5gfx` display backend |
| D3 | M5Unified + M5GFX for board/display bring‑up | Add `M5Unified`/`M5GFX` to board `lib_deps`; bridge to Bruce globals in `interface.cpp` |
| D4 | External radio modules = config hooks now | Define `CC1101_*`, `NRF24_*`, `PN532_*`, `IR_*`, `RF_*`, `LORA_*`, `FM_*`, `GPS_*` pin groups on Grove/M5‑Bus |
| D5 | Compile‑first validation (no Tab5 on hand) | CI builds `esp32p4`; host unit tests for keyboard decode & battery curve; device checklist deferred |
| D6 | Primary input = A164 detachable keyboard | New I2C keyboard driver (addr `0x6D`, INT `G50`) implementing `_getKeyPress()` |

---

## 3. Target hardware reference — M5Stack Tab5

### 3.1 Core

| Item | Value |
|---|---|
| Main SoC | ESP32‑P4NRW32, RISC‑V 32‑bit dual‑core @360 MHz + LP core @40 MHz |
| Wireless SoC | ESP32‑C6‑MINI‑1U (Wi‑Fi 6 2.4 GHz, BLE 5, Thread/Zigbee) over SDIO |
| Flash | 16 MB (QIO) |
| PSRAM | 32 MB Octal (200 MHz, XIP capable) |
| Display | 5″ IPS 1280×720, MIPI‑DSI 2‑lane; panel/touch IC varies by revision (see §3.4) |
| Camera | SC2356 2 MP (1600×1200), MIPI‑CSI |
| Audio | ES8388 codec + ES7210 AEC (dual‑mic), NS4150B 1 W speaker, 3.5 mm jack |
| IMU | BMI270 6‑axis (INT wake) |
| RTC | RX8130CE (+ 70 000 µF supercap) |
| Power | NP‑F550 7.4 V 2‑cell 2000 mAh; MP4560 buck‑boost; IP2326 charger; INA226 monitor |
| USB | Type‑C USB2.0 OTG (native HS) + Type‑A Host |
| Other | RS‑485 (SIT3088), microSD, M5‑Bus, Grove Port.A (HY2.0‑4P), GPIO_EXT, Stamp pads, tripod nut |

### 3.2 Pin map (authoritative — from Tab5 datasheet PDF)

**Internal system I²C (I2C0): `SDA=G31`, `SCL=G32`, 100 kHz** — shared by nearly everything:

| Device | 7‑bit addr | Notes |
|---|---|---|
| ES8388 codec (DAC/spkr/HP) | 0x10 | I²S out |
| GT911 touch | 0x14 | *rev‑A panel only* (backup addr) |
| ST7123 / ST7121 touch | 0x55 | *rev‑B/C integrated panel* |
| RX8130CE RTC | 0x32 | INT → PMS150G wake MCU |
| SC2356 camera (SCCB) | ~0x36 | auto‑detected |
| ES7210 mic ADC | 0x40 | I²S TDM in |
| INA226 power monitor | 0x41 | 5 mΩ shunt |
| PI4IOE5V6408‑1 I/O exp | 0x43 | rail/reset control (E1) |
| PI4IOE5V6408‑2 I/O exp | 0x44 | power/charge control (E2) |
| BMI270 IMU | 0x68 | INT1 → PMS150G wake |

**Display (MIPI‑DSI, dedicated lanes):** backlight `LEDA=G22` (LEDC PWM). **Touch INT=`G23`**, TP_RST via expander **E1.P5**.
**Camera (MIPI‑CSI, dedicated lanes):** SCCB shares I2C0 (`CAM_SCL=G32`,`CAM_SDA=G31`), `CAM_MCLK=G36` (24 MHz LEDC), CAM_RST via **E1.P6**.
**Audio I²S (I2S1):** `MCLK=G30`, `BCLK=G27`, `WS/LRCK=G29`, `DOUT=G26→ES8388`, `DIN=G28←ES7210`. Speaker amp NS4150B enable = **E1.P1 (SPK_EN)**; headphone‑detect = **E1.P7**.
**microSD (shared pins):** SDIO 4‑bit → `CLK=G43,CMD=G44,D0=G39,D1=G40,D2=G41,D3=G42`; or SPI → `SCK=G43,MOSI=G44,MISO=G39,CS=G42`. Card power = on‑chip **LDO_VO4 @3.3 V**.
**ESP32‑C6 (SDIO2, 4‑bit 40 MHz):** `D0=G11,D1=G10,D2=G9,D3=G8,CMD=G13,CLK=G12`, `RESET=G15`, `IO2=G14`. C6 3V3 gated by **E2.P0 (WLAN_PWR_EN)**; antenna int/ext switch = **E1.P0**.
**RS‑485 (SIT3088):** `RX=G21,TX=G20,DIR=G34` (6–24 V).
**Grove Port.A (HY2.0‑4P):** `SDA=G53,SCL=G54`, 5 V gated by **E2.P3?/E1.P2 (EXT5V_EN)**.
**Tab5 Keyboard (A164, Ext.Port1):** `SDA=G0,SCL=G1,INT=G50`, I²C addr **0x6D** (see §10).

**I/O‑expander control lines:**

| Expander | Pin | Function |
|---|---|---|
| E1 (0x43) | P0 | RF antenna int/ext switch (low=internal) |
| E1 (0x43) | P1 | SPK_EN (NS4150B) |
| E1 (0x43) | P2 | EXT5V_EN (M5‑Bus/Grove 5 V) |
| E1 (0x43) | P4 | LCD_RST |
| E1 (0x43) | P5 | TP_RST |
| E1 (0x43) | P6 | CAM_RST |
| E1 (0x43) | P7 | HP_DET (in) |
| E2 (0x44) | P0 | WLAN_PWR_EN (C6 power) |
| E2 (0x44) | P3 | USB5V_EN (Type‑A host) |
| E2 (0x44) | P4 | PWROFF_PULSE (device power‑off) |
| E2 (0x44) | P5 | nCHG_QC_EN (quick‑charge) |
| E2 (0x44) | P6 | CHG_STAT_LED |
| E2 (0x44) | P7 | CHG_EN |

> M5Unified encapsulates most of E1/E2, INA226, panel, touch, IMU and audio bring‑up. We use it rather than re‑deriving these registers, but the table above is the fallback truth for anything M5Unified doesn't expose.

### 3.3 Power & battery model

2‑cell pack: **full ≈ 8.23 V, shutdown ≈ 6.0 V**. Battery voltage/current come from **INA226 bus voltage** (no coulomb counter) — percentage is derived from a 2S voltage curve in `getBattery()`. Charging is gated by IP2326 via `CHG_EN`/`nCHG_QC_EN` (E2), and **only works when the device is powered on**. `powerOff()` pulses **E2.P4**. Sleep/wake uses RX8130 alarm + BMI270 INT (via the PMS150G helper MCU).

### 3.4 Display‑revision caveat (must handle)

| Ship date | Display + touch | Detect |
|---|---|---|
| 2025‑05 | ILI9881C + GT911 (0x14) | GT911 present |
| 2025‑10 | ST7123 integrated (touch 0x55) | FW‑ver reg = 3 |
| 2026‑04 | ST7121 integrated (touch 0x55) | FW‑ver reg = 1 |

Current‑production units (Aug 2026) are ST7121/ST7123. **M5GFX autodetects all three** — this is a primary reason for decision **D3**. Any native‑driver fallback must replicate this probe.

---

## 4. Bruce architecture recap (what a port must satisfy)

- **Build:** pioarduino `platform-espressif32` (Bruce pins `55.03.39`; M5's Tab5 example uses `54.03.21`), Arduino core 3.3.x / ESP‑IDF 5.5, custom `esp32-arduino-lib-builder` libs (exFAT), LittleFS internal FS, per‑board `custom_*.csv` partitions, LTO. Already ships **RISC‑V boards (ESP32‑C5)** — the riscv32 toolchain path is proven.
- **Board = 4 files + 2 edits:** `boards/_boards_json/<b>.json` (`mcu`, `variant:"pinouts"`, identity `-D`), `boards/<b>/<b>.ini` (`[env:<b>]`), `boards/<b>/pins_arduino.h` (pins/feature `#define`s), `boards/<b>/interface.cpp` (board C++). Plus an `#elif` in `boards/pinouts/pins_arduino.h` and a `default_envs` entry.
- **HAL seam:** app talks to hardware through **globals** (`include/globals.h`) + **weak board functions** (`include/interface.h`, `src/core/mykeyboard.h`). A board implements `_setup_gpio()/_post_setup_gpio()`, `InputHandler()` (sets `*Press` flags, fills `KeyStroke`/`touchPoint`), `getBattery()`, `isCharging()`, `_setBrightness()`, `powerOff()`, `_getKeyPress()`, optional `goToDeepSleep()/checkReboot()`.
- **Display:** fully wrapped — `tft_logger → tft_display` re‑exports ~60 methods; **~2 500 call sites are backend‑agnostic** (no raw `TFT_eSPI`). Backends: `tftespi`/`ardgfx`/`lovyan`/**`m5gfx`**/`remote_canvas`. We select **`m5gfx`**.
- **Buses:** `BruceConfigPins` holds runtime `SPIPins/I2CPins/UARTPins` for every external radio (`CC1101_bus`, `NRF24_bus`, `PN532_bus`, `ST25R_bus`, `SDCARD_bus`, `W5500_bus`, `LoRa_bus`, `sys_i2c`, `i2c_bus`, `uart_bus`, `gps_bus`) — SoC‑agnostic; a port just supplies correct pins.
- **Radios/features:** `src/modules/{NRF24,ble,ble_api,badusb_ble,ir,rf,lora,rfid,fm,gps,wifi,pwnagotchi,flock,ethernet,reverseShell,bjs_interpreter,others}`. Wi‑Fi via Arduino `WiFi.h` (20 files) incl. **`esp_wifi_80211_tx` + promiscuous in 11 files**; BLE via **NimBLE** (15 files); BadUSB via native TinyUSB **or** CH9329 UART **or** BLE‑HID; JS = `mquickjs` (pure C, PSRAM).

---

## 5. Gap analysis & feasibility

### 5.1 Feature status matrix

| Feature area | Tab5 mechanism | Status | Effort |
|---|---|---|---|
| Display / UI / menus | M5GFX MIPI‑DSI (autodetect) | **Green** | Med (backend wiring) |
| Touch input | GT911 / ST7123 via M5GFX/M5Unified | **Green** | Low |
| A164 keyboard | New I²C driver (0x6D @ G0/G1, INT G50) | **Green** | Med |
| microSD + LittleFS | SDMMC 4‑bit or SPI, Arduino `FS` | **Green** | Low |
| Battery / power / brightness | INA226 + expanders via M5Unified | **Green** | Med |
| RTC / sleep / wake | RX8130 + BMI270 INT | **Green** | Med |
| IMU apps | BMI270 (M5Unified) | **Green** | Low |
| Camera | SC2356 MIPI‑CSI (IDF esp_video) | **Yellow** | High (Arduino exposure) |
| Audio (spkr/mic) | ES8388/ES7210 (M5Unified) | **Green→Yellow** | Med |
| RS‑485 | UART1 + DIR (G20/21/34) | **Green** | Low |
| **Wi‑Fi connectivity** (STA/AP, portals, web UI, files, NTP, OTA) | esp‑hosted + esp_wifi_remote → C6 | **Yellow→Green** | Med (link bring‑up) |
| **Wi‑Fi offense** (deauth, sniffer, karma, wardrive, pwnagotchi) | **Custom C6 CustomRpc fw** | **Red→Green** | **High (critical path)** |
| BLE connectivity / GATT / BLE‑HID | Hosted HCI → NimBLE‑over‑hosted | **Yellow** | High |
| BLE offense (spam/sniff) | Hosted controller limits | **Yellow/Red** | High |
| BadUSB / native USB‑HID/MSC | P4 USB‑OTG HS + TinyUSB | **Yellow** | Med (core support) |
| USB‑A HID host | IDF usb_host_hid | **Yellow** | Med |
| Sub‑GHz (CC1101), NRF24, IR, RFID/NFC, LoRa, FM, GPS | **External modules** via Grove/M5‑Bus/Stamp | **Green (hooks)** | Low–Med per module |
| JS interpreter (mquickjs) | riscv32 = 32‑bit LE, headers regenerate | **Green** | Low |

### 5.2 The wireless problem (why it's the critical path)

The P4 has no radio. Wi‑Fi/BLE go to the C6 over **SDIO (4‑bit, 40 MHz)** using Espressif **esp‑hosted** with **esp_wifi_remote** on the host, plus a prebuilt **`network_adapter`** slave image on the C6 (~36 Mbps real‑world).

- **Connectivity works unchanged:** standard `esp_wifi_*` / Arduino `WiFi.h` STA+AP are transparently routed to the C6. Bruce's portals, web UI, file sharing, NTP, OTA, WireGuard, reverse shell → portable once the link is up and the C6 is flashed.
- **Offense does not (stock):** **raw 802.11 TX and promiscuous/monitor callbacks are not exposed by stock esp‑hosted RPC** (stubs are commented out). Bruce's `esp_wifi_80211_tx()` + `esp_wifi_set_promiscuous()` calls (deauth/sniffer/karma/jam/channel‑analyzer/pwnagotchi) will not link/function against `esp_wifi_remote` as‑is.
- **The fix (D1):** build **custom C6 slave firmware** that registers **esp‑hosted CustomRpc** handlers calling the C6's native `esp_wifi_set_promiscuous[_rx_cb]()` / `esp_wifi_80211_tx()` / channel / MAC‑override, streaming captured frames back over SDIO; and a **P4‑side shim** that presents Bruce the `esp_wifi_80211_tx`/promiscuous API it expects but marshals over CustomRpc. Reference: `r4d10n/esp32p4-c6-wifi-test` (promiscuous RX + injection via CustomRpc) and Espressif esp‑hosted‑mcu docs.

### 5.3 Other notable gaps

- **USB‑HID on P4:** not Xtensa‑locked (P4 has USB‑OTG HS + TinyUSB). Gate is whether the pioarduino P4 core exposes `USB.h`/`USBHIDKeyboard`. **CH9329 UART is a guaranteed fallback** for BadUSB.
- **SDIO contention:** C6 uses one SDIO controller (G8–13), microSD can use SDMMC on G39–44. Confirm they're **separate controllers**; if not, run microSD in **SPI mode** (conflict‑free).
- **Camera under Arduino:** SC2356 pipeline is IDF `esp_video`/V4L2 + ISP — heavy to surface in Arduino. Treat camera as a **late, optional** phase; M5Unified may expose enough.
- **GPIO0 strap:** keyboard SDA is on **G0** (a boot strap). Verify it's reusable as I²C post‑boot on P4 (expected yes).

---

## 6. Target architecture

```
┌──────────────────────────── ESP32‑P4 (Bruce, Arduino/pioarduino) ────────────────────────────┐
│                                                                                               │
│  Bruce app + modules (UI, RF, RFID, IR, BadUSB, JS, portals…) — UNCHANGED                     │
│        │ globals + weak interface fns                                                          │
│  ┌─────┴───────────────┐   ┌───────────────────┐   ┌──────────────────────────────────────┐  │
│  │ interface.cpp (NEW) │   │ tft HAL: m5gfx     │   │ BruceConfigPins bus HAL (unchanged)  │  │
│  │  InputHandler       │   │ backend →  M5GFX   │   │  CC1101/NRF24/PN532/IR/LoRa/FM/GPS    │  │
│  │  _getKeyPress (kbd) │   │ (DSI autodetect)   │   │  on Grove/M5‑Bus/Stamp                │  │
│  │  getBattery/power   │   └───────────────────┘   └──────────────────────────────────────┘  │
│  │  → M5Unified board  │                                                                       │
│  └─────────┬───────────┘                                                                       │
│   A164 kbd │ I²C 0x6D (G0/G1,INT G50)          Wi‑Fi/BLE via WiFi.h / NimBLE                    │
│            ▼                                    │                                               │
│      STM32F030 keyboard          esp_wifi_remote → esp‑hosted (SDIO 4‑bit @ G8‑13/15) ──────────┼──▶ ESP32‑C6
│                                  + P4 CustomRpc shim (promiscuous/inject)                       │     (custom
└────────────────────────────────────────────────────────────────────────────────────────────────┘     esp‑hosted fw)
```

**Key decisions:**

1. **Framework:** pioarduino `platform-espressif32` targeting `esp32p4`, `framework=arduino`, `board_build.mcu=esp32p4`, `-DBOARD_HAS_PSRAM`, USB‑CDC‑on‑boot. Start from M5's known‑good `esp32-p4-evboard` board JSON; pin the pioarduino release that has the healthiest P4 + `esp_wifi_remote` support (candidate: the Tab5 example's `54.03.21`; reconcile against Bruce's `55.03.39` — see §16 V1).
2. **Display/board:** Bruce `USE_M5GFX` backend + **M5Unified** for `begin()` (panel autodetect, touch, IMU, RTC, INA226, expanders, audio). `interface.cpp` bridges M5Unified state → Bruce globals.
3. **Networking:** link `esp_wifi_remote` + `esp_hosted`; flash C6 `network_adapter` for connectivity; **custom C6 fw + P4 CustomRpc shim** for offense (Phase 8).
4. **Input:** new A164 I²C keyboard driver → `KeyStroke`; GT911/ST7123 touch → `touchPoint`.
5. **Storage:** microSD (SPI‑mode default to dodge SDIO contention; SDMMC as optimization) + LittleFS.
6. **Dependencies added to the board env:** `M5Unified`, `M5GFX`, `esp_wifi_remote`/`esp_hosted` (via core or component), keyboard driver (in‑tree). Everything else stays as Bruce's existing `lib_deps`.

**Licensing:** Bruce is GPL‑family; M5Unified/M5GFX are MIT/FreeBSD (compatible). Keep the new C6 firmware and board code under a license compatible with both (note in `THIRD_PARTY.md`).

---

## 7. Board scaffolding — exact files

Create board id **`m5stack-tab5`** (identity macro `-DM5STACK_TAB5`).

| Path | Action | Contents |
|---|---|---|
| `boards/_boards_json/m5stack-tab5.json` | new | `mcu:esp32p4`, `variant:"pinouts"`, `f_cpu:360000000L`, 16 MB flash, `extra_flags:-DM5STACK_TAB5 -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`, partitions `custom_16Mb.csv` |
| `boards/m5stack-tab5/m5stack-tab5.ini` | new | `[env:m5stack-tab5]`, `board=m5stack-tab5`, `build_src_filter += +<../boards/m5stack-tab5>`, `-DUSE_M5GFX -DBOARD_HAS_PSRAM`, feature `-D`s, `lib_deps=${env.lib_deps} m5stack/M5Unified m5stack/M5GFX m5stack/M5Unit-KEYBOARD`, **`lib_ignore = TFT_eSPI, LovyanGFX, GFX Library for Arduino, SensorLib, XPowersLib`** (§18.2 H‑odr) |
| `boards/m5stack-tab5/pins_arduino.h` | new | all pins/feature flags (§3.2): sys I²C, kbd I²C, SD, audio, RS‑485, expander addresses, external‑module hook pins (Grove/M5‑Bus), `HAS_SCREEN`, `HAS_KEYBOARD`, `HAS_TOUCH`, `BACKLIGHT=G22`, battery via INA226 |
| `boards/m5stack-tab5/interface.cpp` | new | `_setup_gpio`, `_post_setup_gpio` (seed `BruceConfigPins`), `InputHandler`, `_getKeyPress` (kbd), `getBattery/isCharging/_setBrightness/powerOff`, touch read |
| `boards/m5stack-tab5/conf.h` *(if pattern used)* | new | board menu/feature conf |
| `boards/pinouts/pins_arduino.h` | edit | add `#elif defined(M5STACK_TAB5) #include "../m5stack-tab5/pins_arduino.h"` |
| `include/precompiler_flags.h` | edit | add `CONFIG_IDF_TARGET_ESP32P4` GPIO/default block; PSRAM‑scaled stacks |
| `platformio.ini` | edit | add `m5stack-tab5` to `default_envs`; include glob already covers `boards/*/*.ini` |
| `custom_16Mb.csv` | reuse/new | confirm app ≥ 4.5 MB, LittleFS data, coredump; add a P4 variant if offsets differ |
| `lib/HAL/display/m5gfx.*` | verify | ensure the existing m5gfx backend compiles for P4 + Tab5 panel; extend if needed |

---

## 8. Phased implementation plan

Each phase lists **goal → work → files → acceptance**. "Acceptance (compile)" is enforced now; "Acceptance (device)" is the deferred bring‑up check (§13). Phases 1–7 unblock a genuinely usable device; Phase 8 is the offense critical path and can proceed in parallel on the C6 side.

### Phase ‑1 — Arduino‑libs bundle (GATING, no device) *(NEW — §18.1 B3)*
- **Goal:** a `framework-arduinoespressif32-libs` tarball for `esp32p4` that links a trivial app using **esp‑hosted/esp_wifi_remote + exFAT + Bruce's crypto** config.
- **Work:** (a) *immediate* — adopt Launcher's proven P4/esp‑hosted bundle **as‑is** (FAT32+LittleFS, no exFAT) so Phase 0 can start; (b) *parallel from source* — rebuild with `esp32-arduino-lib-builder` (IDF 5.5, esp‑hosted component, exFAT + Bruce crypto sdkconfig, C++ exceptions/RTTI matched to Bruce), pinning the platform (`55.03.32`↔`55.03.39`) that carries it.
- **Acceptance:** a stub `esp32p4` app links `esp_wifi_remote` + exFAT. **No later phase's compile‑acceptance is meaningful until (a) lands.**

### Phase 0 — Toolchain & scaffold *(no device needed)*
- **Goal:** an `esp32p4` Bruce env in which the **full `lib_deps` set compiles** (not a stub) and merges to a flashable binary.
- **Work:** on the Phase ‑1 bundle, create the §7 files **with the `lib_ignore` set** (§18.2 H‑odr); add **`esp32p4: 0x2000`** to `build.py` BOOT_OFFSETS + a P4 partition CSV (big app + LittleFS, app @0x10000); get `pio run -e m5stack-tab5` green **for the full module set** — patch/gate RISC‑V‑hostile libs incl. the vendored `lib/TFT_eSPI` (§18.2 H‑lib); run mquickjs header‑gen on an **x86** host (§18.4).
- **Acceptance (compile):** clean build; `Bruce-m5stack-tab5.bin` produced.

### Phase 1 — Display bring‑up (M5GFX)
- **Goal:** Bruce boots to its menu on the Tab5 screen.
- **Work:** enable `USE_M5GFX`; `M5.begin()`/`M5GFX` init in `_setup_gpio`; map Bruce `tft` wrapper → M5GFX; set rotation to landscape 1280×720; backlight via `_setBrightness` (G22 LEDC / M5Unified); validate DSI panel autodetect (ILI9881C/ST7123/ST7121).
- **Files:** `interface.cpp`, `lib/HAL/display/m5gfx.*`, `pins_arduino.h`.
- **Acceptance (compile):** builds with display on. **(device):** boot logo + main menu render; brightness changes.

### Phase 2 — Input: touch + A164 keyboard
- **Goal:** navigate the whole UI with keyboard and touch.
- **Work:** implement `InputHandler()` to (a) read touch (GT911/ST7123 via M5Unified) → `touchPoint` + directional flags; (b) read the A164 keyboard over I²C (see §10) → `KeyStroke`. Implement `_getKeyPress()` per the cardputer reference. Wire `NextPress/PrevPress/Sel/Esc` etc. Gate I²C reads on the INT line (G50).
- **Files:** `interface.cpp`, new `boards/m5stack-tab5/keyboard_a164.{h,cpp}` (or inline).
- **Acceptance (compile):** host unit test decodes canned A164 register dumps → expected chars/modifiers. **(device):** arrow/enter/esc navigate; text entry works; shortcuts (Ctrl/Alt) fire.

### Phase 3 — Storage: microSD + LittleFS
- **Goal:** SD mount, file browser, config persistence.
- **Work:** configure `SDCARD_bus` (SPI mode: SCK G43/MOSI G44/MISO G39/CS G42) in `_post_setup_gpio`; mount LittleFS; verify `getFsStorage()` picks SD‑else‑LittleFS; seed `sd_files/` assets.
- **Files:** `interface.cpp`, `pins_arduino.h`.
- **Acceptance (compile):** builds. **(device):** SD lists files; settings persist across reboot.

### Phase 4 — Power, battery, sleep
- **Goal:** correct battery %, charge state, brightness, safe power‑off, deep sleep.
- **Work:** `getBattery()` from INA226 bus voltage mapped through a **2S NP‑F550 curve** (8.23 V→100 %, 6.0 V→0 %); `isCharging()` from IP2326/expander; `powerOff()` pulses **E2.P4**; `goToDeepSleep()` + RX8130 alarm / BMI270 INT wake. Use M5Unified power API where available.
- **Files:** `interface.cpp`, host test for the voltage curve.
- **Acceptance (compile):** curve unit test passes. **(device):** % tracks a discharge; charging shows; double‑press‑equivalent power‑off; wake works.

### Phase 5 — Wi‑Fi connectivity (esp‑hosted)
- **Goal:** STA connect + SoftAP + web UI + evil portal + file transfer + NTP/OTA.
- **Work:** enable C6 power (E2.P0), reset sequence (G15); bring up `esp_wifi_remote`/`esp_hosted` over SDIO (G8–13); confirm Arduino `WiFi.h` resolves to the remote backend; document/flash the C6 `network_adapter` image; smoke‑test STA scan/connect and AP.
- **Files:** `interface.cpp` (radio power/reset), board `.ini` (component deps), `remote_display`/web assets unaffected.
- **Acceptance (compile):** links against `esp_wifi_remote`. **(device):** scan/associate; portal serves; file upload works.

### Phase 6 — BLE connectivity
- **Goal:** BLE scan, GATT server, BLE‑HID.
- **Work:** route NimBLE onto the hosted C6 controller (HCI‑over‑hosted); validate `ble_api` GATT + BLE‑HID. Feasibility‑gate; if hosted BLE is immature in the chosen release, mark BLE features "pending" and continue.
- **Acceptance (compile):** NimBLE builds for P4. **(device):** BLE scan sees devices; HID keyboard pairs.

### Phase 7 — USB‑HID / BadUSB
- **Goal:** BadUSB (native) + mass storage; USB‑A HID host optional.
- **Work:** if pioarduino P4 exposes `USB.h`/`USBHIDKeyboard`, set `-DUSB_as_HID`; else default to **CH9329 UART** fallback and document the wiring. Validate `massStorage.cpp`/`u2f` under whichever path.
- **Acceptance (compile):** chosen path builds. **(device):** Ducky script types on a host PC.

### Phase 8 — Offensive Wi‑Fi/BLE *(DEFERRED — optional, OFF the critical path)*
- **Goal:** deauth, sniffer, karma, beacon/jam, channel analyzer, wardriving, pwnagotchi/pwngrid, BLE spam/sniff — **only if/when wanted**.
- **Approach (revised 2026‑08‑08):** do **not** build custom internal‑C6 firmware up front. Offload to a **companion ESP32** (Option A, recommended) or later custom C6 CustomRpc (Option B) — both detailed in §11.
- **Near‑term work:** keep Bruce's ~11 raw‑radio files (`esp_wifi_80211_tx`/promiscuous) `#ifdef`‑guarded **out** of the Tab5 build so connectivity Wi‑Fi still links cleanly; expose a `companion_radio` hook for later.
- **Acceptance (compile):** connectivity build links with offense compiled out. **(later):** companion path validated per §11.

### Phase 9 — Offensive BLE *(feasibility‑gated)*
- **Goal:** BLE spam/sniff.
- **Work:** determine hosted‑controller limits for BLE advertising/scan injection; extend CustomRpc if the C6 controller allows; otherwise document as constrained.
- **Acceptance:** best‑effort per controller capability.

### Phase 10 — External‑module hooks (D4)
- **Goal:** RF/sub‑GHz (CC1101), NRF24, RFID/NFC (PN532/RC522/ST25R), IR TX/RX, LoRa, FM (Si4713), GPS — usable when a module is attached.
- **Work:** define `CC1101_*`/`NRF24_*`/`PN532_*`/`IR_*`/`RF_*`/`LORA_*`/`FM_*`/`GPS_*` pin groups on **Grove Port.A (G53/G54 I²C)**, **M5‑Bus SPI (MOSI G18/MISO G19/SCK G5)** and UART (G6/G7), and Stamp/GPIO_EXT; seed `BruceConfigPins` in `_post_setup_gpio`; expose the pin‑config UI so users can remap. Verify each upstream lib compiles for `esp32p4` (RMT for IR/RF; SPI for CC1101/NRF24/LoRa).
- **Acceptance (compile):** each module lib builds for P4. **(device):** with a module attached, its menu scans/tx/rx.

### Phase 11 — Onboard extras
- **Goal:** IMU apps, audio (spkr/mic), RTC clock, RS‑485, RGB (keyboard LEDs), camera *(optional/last)*.
- **Work:** BMI270 → Bruce IMU consumers (M5Unified); ES8388/ES7210 for tone/record; RX8130 clock/alarm UI; RS‑485 on UART1 (G20/21/34); A164 RGB via kbd regs 0x60–0x67; camera via M5Unified/esp_video if time permits.
- **Acceptance:** per‑feature device checks.

### Phase 12 — Parity sweep, JS, polish
- **Goal:** validate mquickjs bindings; walk every Bruce menu; fix P4‑specific asserts/stack sizes; finalize docs.
- **Acceptance:** full menu walk with no crashes; JS sample scripts run; README/board docs updated.

---

## 9. Driver‑by‑driver mapping

| Bruce needs | Tab5 chip | Bus / pins | Implementation |
|---|---|---|---|
| Framebuffer / `tft` | ILI9881C/ST7123/ST7121 | MIPI‑DSI 2‑lane; BL G22 | `USE_M5GFX` → M5GFX autodetect |
| Touch → `touchPoint` | GT911 0x14 / ST7123·ST7121 0x55 | I2C0 G31/G32; INT G23 | M5Unified touch → `InputHandler` |
| Keyboard → `KeyStroke` | STM32F030 (A164) | I²C 0x6D G0/G1; INT G50 | new driver (§10) |
| `getBattery/isCharging` | INA226 0x41 + IP2326 | I2C0; expander E2 | INA226 read + 2S curve |
| `_setBrightness` | backlight | G22 LEDC | M5Unified / LEDC |
| `powerOff` | expander E2.P4 | I2C0 0x44 | pulse PWROFF |
| Sleep/wake | RX8130 0x32 + BMI270 INT | I2C0 | RTC alarm + IMU INT |
| SD / files | microSD | SPI G43/44/39/42 (or SDMMC) | Bruce SD HAL |
| Wi‑Fi (connect) | ESP32‑C6 | SDIO G8‑13/15 | esp_wifi_remote + hosted |
| Wi‑Fi (offense) | ESP32‑C6 | SDIO | **custom CustomRpc fw + shim** |
| BLE | ESP32‑C6 | SDIO (HCI) | NimBLE‑over‑hosted |
| BadUSB | P4 USB‑OTG / CH9329 | USB‑C / UART | `USB_as_HID` or CH9329 |
| IMU | BMI270 0x68 | I2C0 | M5Unified |
| Audio | ES8388 0x10 / ES7210 0x40 | I2S1 G26‑30 | M5Unified / esp_codec_dev |
| RS‑485 | SIT3088 | UART1 G20/21/34 | Bruce serial |
| CC1101/NRF24/PN532/IR/LoRa/FM/GPS | external | Grove/M5‑Bus/Stamp | `BruceConfigPins` hooks |

## 10. A164 keyboard driver spec

- **Bus:** dedicated I²C master on **G0 (SDA) / G1 (SCL)**, ≤400 kHz, **clock‑stretching enabled**; slave **0x6D**; **INT G50** active‑low, latching.
- **Init:** read reg `0xFE` (version) to confirm presence; set reg `0x10 = 2` (Character mode) for text, or `1` (HID) for shortcuts/BadUSB capture.
- **Service (on INT low, or poll `0x02` EVENT_NUM>0):**
  - *Character:* `N=read8(0x40)`; if `N>0` read `N` bytes @`0x50` → `[modifier, chars…]`; `ctrl=mod&0x01`, `alt=mod&0x04`.
  - *HID:* read 2 bytes @`0x30` → `[modifier, usage]` (0xFF,0xFF = empty).
  - *Normal:* read 1 byte @`0x20` → `pressed(bit7)|row(6:4)|col(3:0)` (0xFF empty).
  - After draining, **write `0x00` to `0x01`** to release INT.
- **Map to `KeyStroke`:** chars → `key.word`; `"enter"→key.enter`, `"backspace"/"del"→key.del`, `"esc"→exit`, arrows → Bruce arrow codes; `key.ctrl/alt` from modifier; `key.pressed=(N>0)`. HID mode → `key.hid_keys[]` + `key.modifiers` (cardputer `interface.cpp` is the reference). No dedicated Fn — optionally map **Sym** to Bruce `fn` using Normal mode.
- **RGB (optional):** brightness reg `0x03`; custom colors regs `0x60–0x67` (mode `0x11=1`).
- **Firmware update:** `0xFD` triggers IAP; out of scope for the port (document only).

## 11. Offensive‑radio workstream *(deferred, optional)*

Red‑team monitor/injection is **off the critical path** (D1/D8). Two options; **Option A recommended**.

### Option A — Companion ESP32 radio co‑processor (recommended)
Since Josh is already hanging modules off the Tab5, add a small **companion ESP32** (S3/C5 — e.g. a Marauder‑class board or a Bruce‑headless node) that owns Wi‑Fi/BLE monitor+injection, linked to the Tab5 over **UART (or SPI)**:
- **Protocol:** a simple framed command set (scan, set‑channel, sniff‑stream, deauth, beacon, pwnagotchi) built on Bruce's existing serial‑command infrastructure (`src/core/serial_commands.*`) and the `bruce-remote`/remote_display precedent.
- **Tab5 side:** a `companion_radio` module that issues commands and ingests captured frames/handshakes over UART, surfacing the same Bruce menus.
- **Pros:** zero custom esp‑hosted work; the companion runs stock Bruce/Marauder attack code on real radio silicon; hot‑swappable; keeps the internal C6 free for connectivity. **Cons:** one extra board + a UART link.

### Option B — Custom internal‑C6 esp‑hosted firmware (fallback, harder)
Deliver as a sibling project `tab5-c6-esp-hosted-offense/` (ESP‑IDF, target `esp32c6`):

1. **Baseline:** start from Espressif esp‑hosted **slave** (`network_adapter`) matching the host `esp_hosted` version used on P4.
2. **CustomRpc handlers:** `offense_init` (register callbacks), `set_channel(ch)`, `promisc_enable(filter_mgmt|data)`, `promisc_rx_cb` (stream captured frame + RSSI to host), `raw_tx(buf,len)`, `set_mac(iface,mac)`.
3. **Host shim (P4):** `src/core/wifi/hosted_offense.{h,cpp}` exposing `esp_wifi_80211_tx()`, `esp_wifi_set_promiscuous()`, `esp_wifi_set_promiscuous_rx_cb()`, `esp_wifi_set_channel()`, `esp_wifi_set_mac()` signatures that marshal to CustomRpc. `#ifdef USE_ESP_HOSTED_OFFENSE` swaps Bruce's includes to this shim.
4. **Version lockstep:** embed a protocol version; host refuses mismatched C6 fw with a clear message.
5. **Flashing:** `esptool.py write_flash 0x0 c6_offense.bin` over the P4‑bridged path (document the exact route on Tab5).
6. **Validation:** promiscuous capture count > 0; deauth drops a known test client; injection frames observed on a monitor NIC.

> Risk: some raw‑radio ops may exceed what the C6 controller/hosted transport allows at speed. Treat throughput/timing (deauth flood rate, capture drop rate) as a tuning task, not a guaranteed match to a native Xtensa Bruce.

## 12. Risks & mitigations

| Risk | Impact | Likelihood | Mitigation |
|---|---|---|---|
| Custom C6 offense fw effort/limits | High | Med | Phase 8 isolated + parallel; reference impl exists; fall back to connectivity‑only if blocked |
| pioarduino P4 core maturity (USB/WiFi glue) | High | Med | Pin a known‑good release; CH9329 USB fallback; verify `WiFi.h`↔`esp_wifi_remote` early (V1) |
| M5Unified ↔ Bruce HAL impedance | Med | Med | Thin bridge in `interface.cpp`; keep M5Unified only for bring‑up, not app logic |
| Panel‑revision variance | Med | Low | M5GFX autodetect (D3) |
| SDIO (C6) vs microSD contention | Med | Low | microSD in SPI mode by default |
| GPIO0 strap reuse for kbd SDA | Low | Low | Verify post‑boot I²C on G0; if bad, bit‑bang or relocate |
| Camera under Arduino | Low | High | Defer to Phase 11, optional |
| BLE‑over‑hosted immaturity | Med | Med | Gate BLE features; ship Wi‑Fi first |
| No device on hand | Med | High | Compile‑first CI + host unit tests + §13 checklist |

## 13. Test & validation strategy

- **Compile‑first CI (now):** `pio run -e m5stack-tab5` must stay green every phase; add to `.github/workflows`. Also build `env_light`/4 MB variants if targeted.
- **Host unit tests (now):** (a) A164 register‑dump → `KeyStroke` decoder; (b) INA226 2S voltage→% curve; (c) CustomRpc frame (de)serialization. Run in the Linux sandbox, no device.
- **Static checks:** `-Werror=odr`, LTO, clang‑format; verify partition/app‑size fit.
- **On‑device bring‑up checklist (when hardware arrives):** flash → serial boot log → display/menu → backlight → touch → A164 keys → SD → battery %/charge → power‑off/wake → Wi‑Fi scan/connect/AP/portal → BLE scan → BadUSB type → (C6 offense) capture/deauth → external module per attached hardware → audio/IMU/RTC/RS‑485 → full menu walk.
- **High‑stakes verification:** before declaring Phase 8 done, verify offense on real RF with a dedicated monitor capture, not just host stubs.

## 14. Dependencies & versions

| Component | Version target | Notes |
|---|---|---|
| pioarduino platform‑espressif32 | reconcile `54.03.21` (M5 Tab5) vs `55.03.39` (Bruce) | pick the release with best P4 + `esp_wifi_remote`; **V1** |
| Arduino‑ESP32 core | 3.3.x (ESP‑IDF 5.5) | P4 beta (Apr 2026) — verify USB/WiFi surface |
| M5Unified / M5GFX | latest (ST7123/ST7121‑adapted) | display autodetect, board bring‑up |
| esp_wifi_remote / esp_hosted | match C6 slave fw | connectivity + offense transport |
| C6 `network_adapter` | version‑locked to host | connectivity |
| NimBLE‑Arduino | 2.5.x | over hosted HCI |
| mquickjs | BruceDevices 0.0.6 | regenerate headers (`gcc -m32`) |

## 15. Milestone sequencing

```
P0 scaffold ─▶ P1 display ─▶ P2 input ─▶ P3 storage ─▶ P4 power ─▶ P5 WiFi ─▶ P10 ext modules ─▶ P6 BLE ─▶ P7 USB ─▶ P11 extras ─▶ P12 polish
                                          └▶ P10 (nRF24/CC1101/GT‑U7 GPS) can begin right after P3
P8 offensive radio (companion ESP32, §11 Option A) ───── optional, any time after P5 ─────▶
```
**Usable device** after P1–P5. **Josh's target config** (connectivity Wi‑Fi/BLE + nRF24 + CC1101 + GT‑U7 GPS directly on the Tab5) after **P10**. **Red‑team** is an optional later add via the companion ESP32 (§11 Option A).

## 16. Open items to resolve during execution

> Several items below were **resolved by the Launcher reference (§17)**; the new #1 risk is the custom Arduino‑libs bundle (§17.7 R‑A).
- **V0 — Arduino‑libs bundle (NEW, highest priority):** obtain/build a `framework-arduinoespressif32-libs` bundle that has **P4 + esp‑hosted** *and* Bruce's needs (exFAT, crypto). Start from Launcher's Tab5 bundle; add Bruce's deltas. Blocks all builds.
- **V1 — pioarduino release: RESOLVED** → `55.03.39` (same as Bruce; proven by Launcher, §17.1).
- **V2 — WiFi.h routing: RESOLVED** → esp‑hosted SDIO init recipe + crash‑guard from Launcher `src/idf/idf_wifi.*` (§17.5); Arduino `WiFi.h` shares the netifs.
- **V9 — P4 silicon revision:** confirm Josh's Tab5 P4 rev vs the libs/core build target (`esp32p4_es` pre‑rev.300 in Launcher's board JSON) — §17.7 R‑B.
- **V10 — companion ESP32:** choose the offense co‑processor board + UART/SPI command protocol (§11 Option A) when red‑team work is scheduled.
- **V3 — SDIO controllers:** confirm C6 SDIO and microSD SDMMC are independent; else lock microSD to SPI.
- **V4 — USB.h on P4:** does the chosen core expose `USBHIDKeyboard`? If not, default CH9329.
- **V5 — M5Unified power/expander coverage:** how much of INA226/charge/E1/E2 does it expose vs. manual.
- **V6 — GPIO0 strap:** validate G0 as keyboard I²C SDA post‑boot.
- **V7 — mquickjs host gen:** ensure `gcc -m32` available in the build image (Docker fallback).
- **V8 — Camera:** decide if SC2356 is in scope for v1 or deferred.

---

## 17. Reference implementation — Launcher (PROVEN, supersedes assumptions)

The `Launcher` repo (`/home/jwowk/Repos/Launcher`, M5Launcher lineage, **same Bruce board architecture**) ships a **working `m5stack-tab5` ESP32‑P4 board**. Its files are the single best reference and **resolve several open items**. Where this section conflicts with earlier assumptions, **this section wins.**

### 17.1 Proven toolchain (resolves V1)
- **Platform:** pioarduino **`55.03.39`** — *identical to Bruce's* `[env]`. No platform change needed.
- **The enabler is the Arduino‑libs bundle:** Launcher overrides `platform_packages = framework-arduinoespressif32-libs @ …bmorcelli/myLibBuilder/releases/download/builds/launcher_esp32-arduino-libs-20260807-031549.tar.gz`. A Tab5‑specific variant also exists (`…esp32-arduino-lib-builder/…/launcher_tab5_esp32-arduino-libs-20260519…`, with platform `55.03.32`). **P4 + esp‑hosted Wi‑Fi support lives in this custom bundle, not in app code.** Bruce currently uses a *different* bundle (`…esp32-arduino-lib-builder…20260715` "with exFAT"). **Action:** the port must use a bundle that has **both** P4/esp‑hosted **and** Bruce's needs (exFAT, etc.) — either adopt Launcher's Tab5 bundle and add exFAT, or rebuild a merged bundle. *This is now the #1 dependency risk.*
- **Board JSON (`boards/_jsonfiles/m5stack-tab5.json`):** `core:esp32`, `mcu:esp32p4`, `chip_variant:esp32p4_es`, `variant:pinouts`, `f_cpu 360000000L`, `f_flash 40000000L`, `f_psram 200000000L`, `psram_mode:oct`, `flash_mode:qio`, `-DBOARD_HAS_PSRAM -DARDUINO_M5STACK_TAB5=1 -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`, `maximum_ram_size 512000`. **`esp32p4_es` = engineering‑sample silicon (pre‑rev.300)** — the lib/core build must match Josh's actual P4 revision (new risk, see §17.6).
- **Flash offsets (from `support_files/merge.py`, corrects the plan):** ESP32‑P4 **bootloader `0x2000`**, partition table `0x8000`, **app `0x10000`** (`board_upload.offset_address=0x10000`). Add a P4 entry to Bruce's `build.py` BOOT_OFFSETS = `0x2000`.
- **Partitions:** Launcher's `custom_16Mb_p4.csv` is a *launcher* layout (app0 factory `0x10000`/`0x180000`=1.5 MB, coredump, **no data FS**). **Bruce cannot reuse it** — Bruce needs a big app (~4.5 MB) **+ LittleFS** P4 CSV, keeping app at `0x10000`.

### 17.2 Proven display (confirms D3)
`-DUSE_M5GFX=1` + **M5Unified `^0.2.17`**, `-DROTATION=3`, native `720×1280` → landscape. Drawing via `M5.Display`; brightness via `M5.Display.setBrightness()`. **M5GFX autodetects the panel** (ILI9881C/ST7123/ST7121). ⚠️ The board `.ini`'s `TFT_*`/`BACKLIGHT=27` pins **overlap with SD/SDIO pins and are largely vestigial** when M5GFX drives the panel — do **not** treat them as authoritative; trust M5Unified + the §3.2 datasheet map.

### 17.3 Proven A164 keyboard (simplifies Phase 2 — big win)
Launcher's `boards/m5stack-tab5/interface.cpp` drives the **detachable A164 keyboard via M5Stack's `M5Unit-KEYBOARD` library** — no hand‑written 0x6D register driver needed:
- Includes `<M5UnitUnifiedKEYBOARD.h>`; `namespace kb = m5::unit::tab5_keyboard`; objects `m5::unit::UnitTab5Keyboard tab5Kb` + `UnitUnified tab5KbUnits`.
- **Bus:** `Wire1.begin(SDA=0, SCL=1, clock)`, INT = **GPIO50**; config `kb::Mode::Normal`, `cfg.irq_pin=GPIO_NUM_50` (library drains events on INT).
- **Hot‑plug:** if absent at boot, arm a FALLING ISR on GPIO50; `InputHandler()` retries `begin()` on the flag.
- **Decode:** `tab5Kb.wasPressed(kidx)`, `row=kidx/COLS`, `col=kidx%COLS`, `map = isSym()? keyMatrixToHidSym : keyMatrixToHidBase` → HID codes → nav globals: **Left 0x50→PrevPress, Right 0x4F→NextPress, Up 0x52→UpPress, Down 0x51→DownPress, Enter 0x28→SelPress, Esc 0x29→EscPress**.
- `lib_deps: m5stack/M5Unit-KEYBOARD`. Also supports an **external CardKB** on Grove (`-DUSE_CARDKB2 CARDKB2_SDA=53 CARDKB2_SCL=54`).
- **Revised Phase 2:** integrate `M5Unit-KEYBOARD` for the A164 (Character/HID mode for full text via the register map in §10 as reference); keep §10's raw protocol only as fallback/validation.

### 17.4 Proven power/RTC/touch (simplifies Phase 4 — big win)
All via **M5Unified**, no custom drivers:
- `getBattery()` → `M5.Power.getBatteryLevel()` (M5Unified owns the INA226 + 2S curve — **drop the custom voltage curve**).
- `_setBrightness()` → `M5.Display.setBrightness()`; `powerOff()` → `M5.Power.powerOff()`.
- `reboot()`/power‑off pulse → `M5.getIOExpander(1).digitalWrite(4, …)` (E2.P4), RTC alarm via `M5.Rtc` (`getDateTime/setAlarmIRQ/clearIRQ`).
- Touch → `M5.Touch.getDetail()` (`isPressed/isHolding/x/y`) → `touchPoint`, throttled ~200 ms (see Launcher `InputHandler`).

### 17.5 Proven Wi‑Fi connectivity recipe (resolves V2) — `src/idf/idf_wifi.{h,cpp}`
- Boot calls `wifiInitHostedSdioGuarded(CLK=12, CMD=13, D0=11, D1=10, D2=9, D3=8, RST=15)` **before** any Wi‑Fi use; guarded by `CONFIG_ESP_HOSTED_ENABLED` + `#include "esp32-hal-hosted.h"` (both from the custom libs bundle).
- Then **standard** `esp_netif`/`esp_wifi_*` (`WIFI_INIT_CONFIG_DEFAULT`, `cfg.nvs_enable=false`, `esp_wifi_set_storage(WIFI_STORAGE_RAM)`, STA/AP netifs, event handler with retry/auto‑reconnect). Arduino `WiFi.h` shares these netifs.
- **Crash‑guard (copy this):** wrong/absent C6 firmware makes the hosted stack **spin ~20 s then panic → board never boots**. Launcher writes an "attempting" flag to **NVS** before init and clears it after; a flag still set at next boot ⇒ mark `hostedWifiAvailable=false` and skip. Expose a `hostedResetGuard()` to call **after flashing new C6 firmware**. Bruce must adopt this or risk unbootable devices.
- Optional **ESP‑AT** backend exists (`idf_wifi_at.*`) for factory‑AT co‑processors — not needed for Tab5's esp‑hosted C6, but informative.

### 17.6 What Launcher does NOT solve (still net‑new for Bruce)
- **Offensive radio (Phase 8) — nothing.** Zero promiscuous/`esp_wifi_80211_tx`/CustomRpc anywhere; Launcher is a launcher, connectivity‑only. The custom C6 firmware + P4 shim remains the critical path, unaided.
- **BLE** — not exercised in the Tab5 board; hosted‑HCI BLE maturity still unverified (Phase 6/9 risk stands).
- **BadUSB / native USB‑HID** — not present; verify `USB.h` on the chosen bundle or use CH9329 (Phase 7).
- **External radio modules** — Launcher wires none on Tab5; Bruce's §8 Phase 10 hooks are still ours to add.
- **Camera / audio apps** — not in Launcher's Tab5 board.
- **Bruce‑specific partitions/LittleFS, mquickjs headers, and the many Bruce modules** — all still ours.

### 17.7 New risks surfaced by Launcher
- **R‑A (High): custom Arduino‑libs bundle.** Must contain P4 + esp‑hosted **and** Bruce's exFAT/crypto needs. Likely requires building a merged bundle. Blocks everything.
- **R‑B (Med): P4 silicon revision** (`esp32p4_es` vs rev3) mismatch vs the libs/core build → boot/PSRAM issues. Confirm Josh's Tab5 P4 rev.
- **R‑C (Med): wrong‑C6‑fw boot hang** — mitigated by copying Launcher's NVS crash‑guard.
- **R‑D (Low): vestigial board `-D` pins** in Launcher's `.ini` — don't blindly copy; trust M5Unified + datasheet.

## 18. Adversarial review — corrections applied (v3, authoritative)

Two independent adversarial reviews (toolchain lens + wireless/input lens) read this plan **and the Bruce/Launcher source**. The accepted findings below **override any conflicting text earlier in this document.** Each cites evidence and the fix.

### 18.1 Blocking (must be resolved at Phase ‑1/0)
- **B1 — `build.py` produces an unbootable P4 image.** `esp32bruce/build.py` `BOOT_OFFSETS` has no `esp32p4` key → falls back to `0x0000`; P4's 2nd‑stage bootloader must be at **`0x2000`** (Launcher `merge.py` confirms). **Fix:** add `"esp32p4": 0x2000` (and `"esp32c6": 0x0000`) to `build.py`; the Phase 0 acceptance must read **0x2000**, not 0x0. Also `build.py`'s size guard keys on an `ota_0` subtype; a `factory`‑only P4 CSV silently skips the check — add an `ota_0`/`test` row or adapt the guard. *(Overrides Phase 0 text.)*
- **B2 — A connectivity‑only build will NOT link.** The raw‑injection primitive `wifiRawTx()` → `esp_wifi_80211_tx()` lives in the **core** file `src/core/wifi/wifi_common.cpp` (not in the "~11 offense files"), which is included across `main/settings/ConnectMenu/webInterface/serial_commands`; and `src/core/menu_items/WifiMenu.cpp` registers the offense menus directly. `esp_wifi_remote` does **not** export `esp_wifi_80211_tx` → undefined‑reference at Phase 5. **Fix:** wrap the *body* of `wifiRawTx()` under `#ifdef BRUCE_RAW_RADIO` (return `ESP_ERR_NOT_SUPPORTED` otherwise); guard the offense `#include`s + `options.push_back(...)` in `WifiMenu.cpp` under the same macro; *then* the offense `.cpp`s can be `build_src_filter`‑excluded. `wifi_mac.cpp`'s `esp_wifi_set_mac` **is** remoted (leave it). *(Overrides §5.2, §8 Phase 8, §17.6.)*
- **B3 — The merged Arduino‑libs bundle is a from‑source IDF rebuild, and it gates every build.** "Add exFAT to Launcher's tarball" is impossible (exFAT lives in the precompiled IDF libs). Bruce's bundle (`…lib-builder…20260715` exFAT) and Launcher's (`…myLibBuilder…20260807` P4+esp‑hosted) are different prebuilt tarballs. **Fix:** make this **Phase ‑1** (below). Build Phase 0 on **Launcher's existing P4/esp‑hosted bundle as‑is** (FAT32+LittleFS, no exFAT) to get P4 linking now; spin the merged P4+esp‑hosted+exFAT+crypto bundle from source (`esp32-arduino-lib-builder`, IDF 5.5, esp‑hosted component, matching `CONFIG_COMPILER_CXX_EXCEPTIONS` to Bruce) as a parallel task; swap it in when its link‑test passes. **Downgrade V1 from "RESOLVED":** Launcher's Tab5 `.ini` actually comments `55.03.32` + a *Tab5‑specific* bundle, so reconcile `55.03.32`↔`55.03.39` against whichever bundle carries esp‑hosted. *(Overrides §14 V0/V1, §17.1.)*

### 18.2 High
- **H‑lib — Phase 0 is not an "empty‑ish app."** PlatformIO compiles every `#include`d lib; Bruce pulls RadioLib, RF24, CC1101, NimBLE, FastLED, ESP8266Audio/SAM, PN532/ST25R stacks, and a **vendored `lib/TFT_eSPI` with no `esp32p4` processor path**. All must compile for riscv32. **Fix:** redefine Phase 0's "done" as *the full `lib_deps` set compiles for `esp32p4`*; expect per‑lib patches/forks; optionally `#ifdef`‑gate whole modules (RF/NFC/audio) off for the first green build, then re‑enable in Phase 10/11.
- **H‑odr — Missing `lib_ignore` → `-Werror=odr` hard failure.** Bruce builds with `-flto -Werror=odr`; M5GFX bundles its own LovyanGFX, and Bruce also ships `lovyan`/`ardgfx` backends → duplicate `lgfx` namespace = ODR error. **Fix:** the Tab5 `.ini` must set `lib_ignore = TFT_eSPI, LovyanGFX, GFX Library for Arduino, SensorLib, XPowersLib` so only M5GFX's `lgfx` survives. *(Adds the missing §7 `lib_ignore` row.)*
- **H‑init — Display/bus init ordering hazards.** `main.cpp` runs `ioExpander.init(&Wire)` + `initCC1101once()` at boot **before** any `M5.begin()`, on Arduino `Wire`; but Tab5 sys‑I²C (G31/G32) is owned by M5Unified and its expanders are **PI4IOE5V6408, not AW9523** (Bruce's probe). D7's "seed radios in `_post_setup_gpio`" runs *after* that first CC1101 init. **Fix:** single guarded `M5.begin()`; seed SPI/UART radio buses in `_setup_gpio()` **before** the CC1101 init; run hosted‑C6 SDIO bring‑up in `_post_setup_gpio()` after the final `M5.begin()`; keep Bruce's `Wire` off G31/G32 and make the AW9523 probe no‑op safely on Tab5. *(Refines §6, Phase 1/5, D7.)*
- **H‑kbd — Standardize the A164 on Normal mode and FILL `KeyStroke.word`; §10↔§17.3 were contradictory and Launcher's path can't type.** Launcher's `tab5KbPoll()` sets only nav booleans, never `KeyStroke.word`; Bruce's text/shortcut model (`checkShortcutPress`, text entry) consumes `key.word` (ASCII). Library class is `m5::unit::UnitTab5Keyboard` (constants in `tab5_keyboard::`); `keyMatrixToChar(row,col)` is **Normal‑mode‑only**. **Fix:** use **Normal mode**; in `InputHandler()` fill **both** `KeyStroke.word` (via `keyMatrixToChar`) for printables **and** special/nav keys via `keyMatrixToHidBase/Sym` (Enter 0x28→`enter`, Backspace 0x2A→`del`, Esc 0x29→`exit_key`, arrows→Bruce arrow codes), mapping Sym/Aa/Ctrl/Alt→`ctrl/alt/fn`. This is **net‑new code, not a "big win."** *(Overrides §10 & §17.3; Phase 2 effort ↑.)*
- **H‑companion — The companion‑ESP32 link needs a real binary protocol, not Bruce's ASCII CLI.** `serial_commands`/`serialcmds.cpp` is a 512‑byte line REPL with a bool ack + 20 ms timeout — wrong shape for sniffer/pwnagotchi frame streaming. The binary precedent is `boards/bruce-remote-master` + `remote_display` (SPI canvas push). **Fix:** define a **new framed binary protocol** (length‑prefixed, seq/ack, CRC, backpressure) on a **dedicated high‑baud UART or SPI**; assign its pins and reconcile against the GPS UART & RS‑485; state a throughput/drop‑policy acceptance. *(Refines §11 Option A.)*

### 18.3 Medium
- **M‑pins — External‑module pins (Josh's actual deliverable) are unassigned in §3.2.** Add authoritative rows (see 18.6): M5‑Bus SPI (MOSI G18/MISO G19/SCK G5 — from the datasheet M5‑Bus table), a GPS UART, **CC1101 GDO0**, **nRF24 CE/CSN**, companion link, plus a current budget for the EXT5V/3V3 rail feeding nRF24(+PA/LNA)/CC1101/GT‑U7/companion. Until assigned, Phase 10 "Green" is unsupported.
- **M‑spi — Shared SPI constraints + static‑init footgun.** `bus_HAL.cpp acquireSPIBus` allows one shared `AUX_SPI` only if CC1101 & nRF24 use **identical SCK/MISO/MOSI** (distinct CS/CE) and are used **modally**; the global `RF24` object is constructed at static‑init before pins are seeded; and `acquireSPIBus` mis‑routes to the DSI backend if the board leaves the **vestigial `TFT_MOSI=15`** defined. **Fix:** document the shared‑bus/mutual‑exclusion constraint; leave `TFT_MOSI`≤0 on Tab5; re‑`begin()` `RF24` after seeding.
- **M‑rmt — CC1101/OOK RMT assumes 80 MHz APB and `ESP_ERROR_CHECK`‑aborts.** `rf_utils.h` hardcodes `RMT_CLK_DIV 80` / `RMT_1US_TICKS`; P4's RMT clock tree differs → wrong pulse math + a crash‑on‑failure. **Fix:** derive the timebase from the P4 RMT source clock (or set `resolution_hz`), replace `ESP_ERROR_CHECK` with graceful failure, re‑validate the GDO0‑ISR vs 1280×720 DSI redraw load. RF effort → **Med**. *(Refines §5.1.)*
- **M‑batt — Battery model is contradictory (§4 curve vs §17.4 drop it) and M5Unified's NP‑F550 accuracy is unverified;** `isCharging()` has no M5.Power impl in Launcher. **Fix:** default to `M5.Power.getBatteryLevel()` but **keep the INA226 2S curve as fallback**; verify against a real discharge before deleting; source charge‑state from IP2326/expander E2. Keep the §13 curve unit‑test only if the curve is retained.
- **M‑metrics — The `m5gfx` wrapper's text metrics are stubs** (`textWidth=len*6`, `fontHeight=size*8`, `setTextFont` no‑op, `native()` returns `nullptr`) → misaligned UI on 1280×720. **Fix:** delegate metrics to `M5.Display.textWidth/fontHeight/drawString`; audit any future `tft.native()` users.
- **M‑sd — "SDIO contention" is overstated.** C6 SDIO (G8–15) and microSD (G39–44) are disjoint and Launcher names the C6 bus `SDIO2_*` (separate controller). **Fix:** correct the rationale; SPI‑SD is an acceptable conservative default, but prefer **SDMMC 4‑bit** for throughput once the controller assignment is confirmed. *(Overrides §5.3 contention claim.)*

### 18.4 Low
- **USB‑HID:** `USB_as_HID` is proven on Bruce's **S3** boards, not P4; Launcher (no HID) doesn't validate it. Verify `USBHIDKeyboard` links for `esp32p4` early; keep **CH9329** the default BadUSB path. *(§17.6, V4.)*
- **mquickjs:** `-m32` is a **host‑arch** requirement (x86 + gcc‑multilib), not a P4 issue; run header‑gen in a Dockerized x86 image or set `MQJS_HOST_CC`. *(V7.)*
- **Vestigial pins:** Launcher's `TFT_RST=12/SCLK=13/MOSI=15` collide with the C6 `SDIO2_CLK/CMD/RST` — the Tab5 `pins_arduino.h` must emit **no** real SPI/LEDC init on them; brightness only via `M5.Display.setBrightness()`. *(Reinforces §17.2 R‑D.)*
- **Drop V6:** G0 as keyboard SDA is already proven in Launcher production (`Wire1.begin(0,1)`). Remove from open items.

### 18.5 Revised phase spine (supersedes §15)
```
Phase ‑1 (libs bundle, GATING) ─▶ P0 scaffold+full‑lib compile ─▶ P1 display ─▶ P2 input ─▶ P3 storage ─▶ P4 power
   ├─ PRIMARY (Josh's target, SoC‑agnostic, no esp‑hosted): ─▶ P10 nRF24 + CC1101 + GT‑U7 GPS  ─▶ P11 extras ─▶ P12 polish
   └─ PARALLEL (connectivity):  P5 Wi‑Fi(esp‑hosted) ─▶ P6 BLE ─▶ P7 USB ;   P8 offense via companion ESP32 (optional, after P5)
```
Josh's deliverable (connectivity **+ nRF24 + CC1101 + GT‑U7**) rides the **P3→P10** spine, which needs neither the C6 nor the offense track — sequence it first.

### 18.6 External‑module bus map (new authoritative table)
| Signal | Candidate Tab5 pin(s) | Source / note |
|---|---|---|
| Aux SPI SCK / MISO / MOSI | **G5 / G19 / G18** (M5‑Bus) | Tab5 datasheet M5‑Bus (SCK G5, MISO G19, MOSI G18). Shared by CC1101 + nRF24 |
| CC1101 CS | free M5‑Bus GPIO (e.g. **G16**) | assign; distinct from nRF24 CS |
| CC1101 GDO0 (TX bit‑bang + RX IRQ) | free M5‑Bus GPIO (e.g. **G17/G45**) | required by `rf_utils/rf_send/rf_listen`; verify not a strap/input‑only |
| nRF24 CE / CSN | free M5‑Bus GPIOs (e.g. **G2 / G48**) | assign; RF24 needs both |
| nRF24 IRQ (opt) | free M5‑Bus GPIO | optional |
| GT‑U7 GPS TX/RX (UART) | **G6 / G7** (M5‑Bus "PC" UART) or Grove | pick a free P4 UART; reconcile vs RS‑485 (G20/G21) & companion link |
| Companion‑ESP32 link | dedicated UART **or** SPI | new binary protocol (H‑companion); assign after UART audit |
| Grove Port.A I²C | **G53 / G54** | for I²C modules (e.g. PN532) |

> All candidate GPIOs above must be confirmed **free and not input‑only/strap** on ESP32‑P4 before Phase 10 wiring; M5‑Bus GPIOs (G2/G3/G4/G16/G45/G47/G48/G51…) are the pool.

---

## 19. Execution log

### Session 2026‑08‑08 — Phase ‑1(a) interim bundle + Phase 0 scaffolding (branch `port-M5Stack-Tab5`)
Landed in `esp32bruce` (static‑validated: JSON/ini parse, brace balance, macro presence — all pass):
- **`build.py`** — added `esp32p4: 0x2000` and `esp32c6: 0x0000` to `BOOT_OFFSETS` (fixes the unbootable‑image blocker B1).
- **New board `m5stack-tab5`** — `boards/_boards_json/m5stack-tab5.json` (`mcu esp32p4`, `chip_variant esp32p4_es`, `variant pinouts`, octal PSRAM, USB‑CDC), `boards/m5stack-tab5/m5stack-tab5.ini`, `pins_arduino.h`, `interface.cpp`.
- **Dispatcher** `boards/pinouts/pins_arduino.h` routes `ARDUINO_M5STACK_TAB5`; **`platformio.ini`** `default_envs` gains `;m5stack-tab5`.
- **Interim libs bundle** — the env overrides `platform_packages` to Launcher's proven P4/esp‑hosted bundle (`launcher_esp32-arduino-libs-20260807`). The merged exFAT+P4 bundle (Phase ‑1(b)) is still TODO.
- **Display/power/touch** via `USE_M5GFX` + M5Unified 0.2.17 (`M5.Display`, `M5.Power`, `M5.Touch`).
- **A164 keyboard** — self‑contained raw‑I²C **Character‑mode** driver on `Wire1` (G0/G1, INT G50, 0x6D) that fills `KeyStroke.word` + nav flags. *Deviation from §17.3:* chose a raw driver over `M5Unit-KEYBOARD` to avoid the M5UnitUnified dependency chain and to satisfy §18.2 H‑kbd (must fill `KeyStroke.word`) directly.
- **Connectivity‑first** — `-DDISABLE_RAW_RADIO` + `wifiRawTx()` guarded to return `ESP_ERR_NOT_SUPPORTED` (foundation for B2).
- **Partitions** — reuse `custom_16Mb.csv` (its offsets are independent of the P4 bootloader offset).

### Build attempt 1 — 2026‑08‑08, on Josh's local toolchain (via desktop‑commander)
Ran `pio run -e m5stack-tab5` for real (PlatformIO 6.1.19). Findings:
- **Toolchain OK** — pioarduino `55.03.39` + `toolchain-riscv32-esp 14.2.0` + all 90 libs installed and the board builds.
- **BREAKTHROUGH — Phase ‑1 is largely moot (resolves R‑A / B3):** Bruce's **root** `platform_packages` bundle (`main_esp32-arduino-libs-20260715`, "exFAT") **already ships an `esp32p4` build** with `CONFIG_BT_ENABLED`/`CONFIG_BT_NIMBLE_ENABLED`, **`CONFIG_ESP_HOSTED_ENABLED` (C6, SDIO 4‑bit @40 MHz)**, and exFAT all enabled. So the Tab5 needs **no Launcher bundle and no custom merged bundle** — the same libs every Bruce board uses already target P4 with hosted WiFi+BLE+exFAT. The Tab5 `.ini` override was removed.
- **Bruce's own code compiles clean for RISC‑V** — with that bundle, **all `src/` + `boards/m5stack-tab5/` objects compiled with zero errors** (my board files, the M5GFX/M5Unified bridge, and the A164 driver included).
- **One remaining wall: NimBLE‑Arduino.** The library itself fails on P4 (34 errors, *all* library‑internal): `NimBLEDevice.cpp` calls a **local** BT controller (`esp_bt_controller_enable`, `ESP_BT_MODE_BLE`, `esp_bt_controller_config_t`) that P4 doesn't have, plus `nimble_npl` OS‑layer conflicts. This is the known arduino‑esp32 gap — NimBLE‑Arduino has no P4/hosted‑BLE controller path yet. **Zero Bruce‑side errors.**
- **build.py P4 offset (0x2000)** and the board scaffolding are validated by a real toolchain run.

### Decision point — BLE on P4
NimBLE‑Arduino won't compile for P4 without library‑level work. Options: **(A, recommended, matches connectivity‑first)** disable BLE for the Tab5 build now — `lib_ignore = NimBLE-Arduino`, `-DDISABLE_BLE`, exclude `src/modules/ble/*` + `badusb_ble`/`flock`/`ble_api` via `build_src_filter`, and guard the ~handful of core touchpoints (`BleMenu`, `ble_common`, `wifi_common.cpp`'s ble include) under `#ifndef DISABLE_BLE` — yielding a green, flashable connectivity build (display + A164 + SD + WiFi + external radios), BLE returning in Phase 6. **(B)** invest now in a P4/hosted‑compatible NimBLE (patch or alternate version) — higher effort, uncertain.

### Immediate next steps
1. **`pio run -e m5stack-tab5`** on a machine with the toolchain (or CI). First run downloads pioarduino `55.03.39` + the interim P4 libs bundle.
2. **Iterate the compile** (Phase 0 "done" = full module set compiles for RISC‑V, §18.2 H‑lib): expect RISC‑V‑hostile libs and, at link, the **offense‑module** errors — complete B2 by guarding the offense `#include`s + `options.push_back(...)` in `src/core/menu_items/WifiMenu.cpp` under `#ifndef DISABLE_RAW_RADIO` and excluding those `.cpp` (sniffer, wifi_atks, karma_attack, channel_analyzer, jam_detect, pwnagotchi/*) via `build_src_filter` in the Tab5 `.ini`. (The `wifiRawTx` guard is already in place.)
3. **Phase ‑1(b)** — build the merged exFAT + P4 + esp‑hosted Arduino‑libs bundle from source and swap it into `platform_packages`.
4. **Flash + on‑device bring‑up** per the §13 checklist (display → touch → A164 → SD → battery → Wi‑Fi(esp‑hosted) → modules).

---

### GREEN BUILD — 2026‑08‑08 (Phase 0 complete, on Josh's local toolchain via desktop‑commander)
`pio run -e m5stack-tab5` **SUCCEEDS**: `Bruce-m5stack-tab5.bin` (3.95 MB) merged at 0x2000/0x8000/0x10000; **RAM 14.5%, Flash 21.2%** (fits the 4.5 MB app partition). Fixes, iterated against the real compiler:
- **No custom bundle needed** — Bruce's root `main_esp32-arduino-libs` (exFAT) already targets P4 with hosted WiFi/BLE; removed the Tab5 `platform_packages` override (resolves R‑A/B3).
- **BLE on P4 (hardest):** NimBLE‑Arduino 2.5.1 defaults to bundled/local‑controller mode via `USING_NIMBLE_ARDUINO_HEADERS`, which fails on the radio‑less P4. Fixed by running it in **IDF / esp‑nimble‑cpp mode** on P4 — patch `nimconfig.h` (don't define that macro on P4) + a `library.json` `srcFilter` excluding the 163 bundled NimBLE C sources — so the C++ wrapper links the IDF's **hosted** NimBLE (`libbt.a`) over the C6. **⚠️ These edits currently live in `.pio/libdeps` and MUST be baked into `patch_library_conflicts.py` for reproducibility (top follow‑up).**
- **BLE TX‑power:** `src/modules/ble/ble_pwr_compat.h` maps legacy `ESP_PWR_LVL_*`→dBm for `NimBLEDevice::setPower(int8_t)` on P4; the two Bluedroid `esp_ble_tx_power_set` calls guarded.
- **ESP‑NOW absent on P4:** board‑local `esp_now_stub.cpp` (connect feature no‑ops).
- **Touch:** `-DHAS_TOUCH=1` (defines `touchHeatMap`, enables touch UI). **USB HID:** native `-DUSB_as_HID=1` (P4 core ships `USBHIDKeyboard`; `CONFIG_TINYUSB_HID_ENABLED=1`) → BadUSB over USB‑C OTG.
- **Offense modules compile AND link** (`esp_wifi_remote` exports `esp_wifi_80211_tx`); whether they *function* over hosted is unverified — companion‑ESP32 plan (§11) stands.

### Emulator smoke‑test — esp‑emu v0.38.0 (Espressif RISC‑V emulator)
The merged image boots in the emulator: **P4 ROM boots → reads flash → loads the 2nd‑stage bootloader at 0x2000 and jumps to it** (validates the `build.py` 0x2000 fix + partition table end‑to‑end); loaded directly, the app image (`7 segments, chip_id=0x12`) resets at its entry and begins IDF startup. Full execution isn't possible — esp‑emu v0.38 doesn't model the Tab5's octal PSRAM / full P4 peripherals (early load fault). **Conclusion: image structurally valid and boots; runtime validation requires real hardware.**

### Top follow‑ups (post‑green)
1. **Persist the NimBLE P4 IDF‑mode patch** in `patch_library_conflicts.py` (currently manual in `.pio` → lost on clean build).
2. Address adversarial‑review findings (§20).
3. On‑device bring‑up per §13 (display → touch → A164 → SD → battery → WiFi/BLE → modules).

---

*End of plan (v3 — Launcher‑validated, adversarially reviewed; **Phase 0 GREEN 2026‑08‑08**, §19). Josh's target config rides the P3→P10 spine; connectivity (P5/P6) works via hosted C6; optional companion‑ESP32 offense (P8) later.*


