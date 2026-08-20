# Bruce — Device Assembly & Flashing Guide

A complete, hands-on guide for getting Bruce running on hardware: choosing a
device, assembling or wiring it, flashing the firmware, building from source,
and preparing the SD card for first boot.

> **New to Bruce?** The fastest path is: buy a
> [supported device](#1-choose-your-device) → open the
> [Web Flasher](#method-1--web-flasher-recommended) → prepare an
> [SD card](#5-sd-card-setup--first-boot). Everything else below is for people
> assembling hardware, flashing from the command line, or building from source.

Bruce is a tool for **authorized** security testing and red-team operations only.
See the disclaimer in the [README](../README.md).

---

## Contents

1. [Choose your device](#1-choose-your-device)
2. [Hardware assembly](#2-hardware-assembly)
   - [Buy assembled vs. build it](#buy-assembled-vs-build-it)
   - [Community PCB fabrication](#a-community-pcb-fabrication)
   - [Adding radio modules to a dev board](#b-adding-radio-modules-to-a-dev-board)
3. [Flashing](#3-flashing)
   - [The one offset fact you must know](#the-one-offset-fact-you-must-know)
   - [Web Flasher](#method-1--web-flasher-recommended) · [esptool](#method-2--esptool) · [M5Burner](#method-3--m5burner) · [OTA / M5Launcher](#method-4--ota-via-m5launcher)
   - [Troubleshooting](#flashing-troubleshooting)
4. [Building from source](#4-building-from-source)
5. [SD card setup & first boot](#5-sd-card-setup--first-boot)
6. [Appendix: partition tables & env list](#appendix)

---

## 1. Choose your device

Bruce runs on ESP32 / ESP32-S3 / ESP32-C5 / ESP32-C6 hardware. Every flashable
target is a PlatformIO **environment** (env) defined under `boards/<device>/`.
All targets share the WiFi / BLE / IR / RF / RFID / BadUSB core; devices differ
in which **radios and peripherals** are present.

### Quick capability matrix

Onboard/pre-wired support (`✓`); blank means "not available or needs an external module":

| Device | MCU | Flash | CC1101 | NRF24 | PN532/NFC | Mic | BadUSB | SD slot |
|---|---|---|:--:|:--:|:--:|:--:|:--:|:--:|
| **M5Cardputer** (+ADV) | S3 | 8 MB | ✓ | ✓ | ✓ | ✓ | native | onboard |
| **M5StickC PLUS2** | ESP32 | 8 MB | ✓ | ✓ | ✓ | ✓ | bridge¹ | Grove/hat |
| **M5StickC PLUS / 1.1** | ESP32 | 4 MB | ✓ | ✓ | ✓ | ✓ | bridge¹ | Grove/hat |
| **M5Core BASIC** | ESP32 | 4/16 MB | ✓ | ✓ | ✓ | ✓ | bridge¹ | onboard |
| **M5Core2** | ESP32 | 16 MB | ✓ | ✓ | ✓ | ✓ | bridge¹ | onboard |
| **M5CoreS3 / SE** | S3 | 16 MB | ✓ | ✓ | ✓ | | native | onboard |
| **CYD-2432S028** (+ variants) | ESP32 | 4 MB | ✓ | ✓ | ✓ | | bridge¹ | onboard |
| **LilyGo T-Embed CC1101** | S3 | 16 MB | **onboard** | ✓ | ✓ | ✓ | native | onboard |
| **LilyGo T-Embed** | S3 | 16 MB | ✓ | ✓ | ✓ | ✓ | native | onboard |
| **LilyGo T-Display-S3** (+touch/mmc) | S3 | 16 MB | ✓ | ✓ | | | native | `-mmc` only |
| **LilyGo T-Deck / Plus** | S3 | 16 MB | ✓ | | | | native | onboard |
| **LilyGo T-HMI / T-LoRa Pager** | S3 | 16 MB | | | | | native | onboard |
| **Smoochiee V2** | S3 | 16 MB | **onboard** | **onboard** | ✓ | | native | onboard |
| **Bruce RF Reaper / Elecrow** | S3/ESP32 | 16/4 MB | **onboard** | **onboard** | ST25R3916² | | native | onboard |
| **ESP32-C5 DevKitC** | C5 | 8 MB | ✓ | ✓³ | ✓ | | | none (wire it) |

¹ On Core/CYD/StickC, BadUSB runs through a serial/HID **bridge**, not native USB HID.
² Reaper/Elecrow use an **ST25R3916** NFC reader, not a PN532.
³ README marks NRF24 as supported on the C5, but the in-tree `boards/ESP32-C5/ESP32-C5.ini`
currently has the NRF24 SPI block commented out — treat C5 NRF24 as not-yet-enabled.

The full per-feature matrix (FM radio, RGB LED, speaker, fuel gauge, LITE builds)
is in the [README](../README.md#specific-functions-per-device-the-ones-not-mentioned-here-are-available-to-all).

### Flash size → partition table → SD need

- **4 MB "full" builds** (CYD, Core BASIC 4 MB, StickC, TTGO, Elecrow, Marauder) use
  `custom_4Mb_full.csv`, which leaves only **192 KB** of internal storage → you
  effectively **need an SD card** for scripts, IR/RF captures, and portals.
- **8 MB / 16 MB S3 boards** (Cardputer, T-Embed, T-Deck, CoreS3, Smoochiee) have
  **3 MB / 11.4 MB** of internal LittleFS → an SD card is **optional but recommended**.
- **`LAUNCHER_*` / LITE builds** exist for 4 MB boards to fit M5Launcher; they drop
  TelNet, SSH, WireGuard, ScanHosts, RawSniffer, Brucegotchi, BLE scan/beacon, and
  the JS interpreter.

A device **boots and runs without an SD card** — Bruce keeps its UI in internal
LittleFS. SD/LittleFS only matter for storing content (see
[§5](#5-sd-card-setup--first-boot)).

---

## 2. Hardware assembly

There are two very different paths. Pick the one that matches what you have.

### Buy assembled vs. build it

| You have… | Do this |
|---|---|
| An off-the-shelf dev board (M5Stack, LilyGo, CYD, ESP32 devkit) | Nothing to assemble — go straight to [Flashing](#3-flashing). Add radio modules per [§2B](#b-adding-radio-modules-to-a-dev-board) if you want SubGHz/NFC. |
| A pre-made commercial Bruce board (**Bruce RF Reaper** from Elecrow) | Buy it assembled; skip soldering. Go to [Flashing](#3-flashing). |
| Bare Gerbers you want to fabricate (**Smoochiee V2**, **Pirata StickC**, **Ultramarines**) | Follow [§2A](#a-community-pcb-fabrication). |

> **Scope note:** This repo ships the *manufacturing data* for the community PCBs
> (Gerbers, and for two of them BOM + schematic + pick-and-place + 3D case), but
> **not** a per-component "solder R1, then C1…" walkthrough. The schematic PDFs are
> the authoritative placement/wiring reference during assembly.

### A) Community PCB fabrication

The PCB projects live under [`pcbs/`](../pcbs). See [`pcbs/README.md`](../pcbs/README.md)
for the PCBWay order link.

| Board | What it is | Files provided |
|---|---|---|
| **Smoochiee V2** (`pcbs/Bruce_PCB_smoochie/`) | Full standalone ESP32-S3 Bruce board: TSOP4838 IR RX + 8-emitter IR array, 16× WS2812 LEDs, buzzer, microSD, USB ESD protection, sockets for CC1101/NRF24, onboard PN532 support | Schematic PDF, BOM `.xlsx`, pick-and-place `.xlsx`, Gerbers (`Bruce_PCB_v2_Smoochiee.zip` full + `BRUCE_MANUAL_BUILD.zip` hand-solder), 3D case (`3d/…zip`) |
| **Pirata StickC board** (`pcbs/StickCPlus_PCB_Pirata/`) | Expander/HAT for M5StickC PLUS 1.1 / PLUS2 — bolts CC1101 + NRF24 onto the Stick (no own ESP32) | Schematic PDF, BOM `.xlsx`, Gerbers, parts list + solder cautions in `Readme.md` |
| **Ultramarines M5 Stick Extender** (`pcbs/M5Stick_Intermidiate_ultramarines/`) | Another M5Stick extender | Gerbers **only** (cross-reference the README photos) |

**General fabrication flow:**

1. **Order the bare PCB** — upload the Gerber zip to **PCBWay** (Smoochiee has a
   ready shared project linked in `pcbs/README.md`) or **JLCPCB**. Use
   `BRUCE_MANUAL_BUILD.zip` if you intend to hand-solder everything.
2. **Get the parts** from the BOM. Smoochiee's BOM has LCSC part numbers pre-filled;
   Pirata's `Readme.md` links AliExpress parts (1× CC1101, 1× NRF24L01, 2× SMD DIP
   switches, 1× BC547, 1× TF socket, 6× 10k 0805, 1× TSOP38238, a 90° 8-pin header,
   and a Grove cable to cut and solder).
3. **Populate SMD first, then through-hole.** Reflow/hand-solder the fine-pitch parts
   (resistors, caps, MOSFETs, USBLC6, WS2812s) before headers, sockets, TF socket,
   and IR receiver. For a fab-assembled board (upload BOM + CPL to JLCPCB SMT), you
   only hand-solder the through-hole/connector parts.
4. **Fit the radios and connectors**, then set the Pirata DIP switches per its schematic.
5. **Print the case** (Smoochiee) from `3d/Bruce_PCB_V2_3dCase.zip`.
6. **Flash Bruce** for the matching target (`smoochiee-board`, `m5stack-cplus2`, etc.)
   and validate each radio from the [Config menus](#step-3--tell-bruce-which-pins-you-used).

**Pirata build cautions (from its `Readme.md`):** use the *minimum* amount of solder,
and if you only have 10k resistors, place two in parallel to make ~5k at the
transistor-bias position.

### B) Adding radio modules to a dev board

The common DIY case: you have a supported dev board and want to add a radio it
lacks. Bruce is designed so **you wire the module to free GPIOs and then tell the
firmware which pins you used — no recompile needed.**

#### Step 1 — Do you even need a module?

Check the [capability matrix](#quick-capability-matrix): a `✓` means the radio is
onboard or pre-wired; blank means you need an external module. Then open
`boards/<board>/pins_arduino.h` to see which GPIOs the display/SD already use and
which are free (often the **Grove** `GROVE_SDA` / `GROVE_SCL` pins and exposed
header pins).

#### Step 2 — Wire the module (SPI or I²C)

- **CC1101 (SubGHz)** and **NRF24L01 (2.4 GHz)** are **SPI** — they share
  `SCK`/`MISO`/`MOSI` and each needs its own **CS** plus a data/enable pin
  (CC1101 `GDO0`, NRF24 `CE`).
- **PN532 (NFC/RFID)** runs as **I²C or SPI** (module mode switch); Bruce also
  supports **RC522 (SPI)** and M5's **RFID2** unit.
- **Power everything from 3.3 V, not 5 V** — NRF24 in particular is 3.3 V logic.
  A 10 µF cap across NRF24 VCC/GND fixes most brownouts.

**Generic wiring reference** — *silkscreen names vary between vendors; confirm against
your module and the board schematic before powering on:*

| Module pin | CC1101 | NRF24L01 | PN532 (SPI) | PN532 (I²C) | → ESP32 |
|---|---|---|---|---|---|
| VCC | 3.3 V | 3.3 V | 3.3 V | 3.3 V | 3V3 rail |
| GND | GND | GND | GND | GND | GND |
| SCK | SCK | SCK | SCK | — | SPI SCK |
| MISO | SO | MISO | MISO | — | SPI MISO |
| MOSI | SI | MOSI | MOSI | — | SPI MOSI |
| CS | CSN | CSN | SS/NSS | — | dedicated CS GPIO |
| Data/enable | GDO0 | CE | — | — | dedicated GPIO |
| SDA / SCL | — | — | — | SDA / SCL | I²C (e.g. Grove SDA/SCL) |

On a **shared SPI bus**, `SCK`/`MISO`/`MOSI` are common to CC1101, NRF24, SD, and
(SPI-mode) PN532/RC522; only **CS** and the per-chip data/enable pin must be unique.

**Known-good pin maps** you can copy (these are exactly what the firmware ships as
defaults, so they're the safest reference):

- **Reaper** (`boards/reaper/pins_arduino.h`) — shared SPI SCK=17 MOSI=18 MISO=8;
  CC1101 CS=9 GDO0=46; NRF24 CS=13 CE=14; Grove I²C SDA=47 SCL=48.
- **Smoochiee** (`boards/smoochiee-board/pins_arduino.h`) — dedicated radio SPI
  SCK=13 MOSI=12 MISO=11; CC1101 CS=46 GDO0=9; NRF24 CS=14 CE=21; Grove SDA=47 SCL=48.
- **M5StickC PLUS2** (`boards/m5stack-cplus2/m5stack-cplus2.ini`) — legacy set
  SCK=0 MOSI=32 MISO=33 SS=26, CC1101 GDO0=25 / NRF24 CE=25.

Reference wiring **photos** ship in [`media/connections/`](../media/connections)
(`cc1101_stick.jpg`, `pn532_i2c.jpg`, `pn532_spi.jpg`, `rc522_stick_spi.jpg`,
`spi_pins.excalidraw.png`, `T-Embed CC1101 - NRF24.png`, …).

#### Step 3 — Tell Bruce which pins you used

Set these in the on-device **Config menus** (persisted to flash — set once):

- **RF → Config**
  - **RF TX Pin** / **RF RX Pin** — for simple ASK/OOK 433 modules (out-of-range
    values fall back to `GROVE_SDA`/`GROVE_SCL`).
  - **RF Module** — choose **M5 RF433T/R** or **CC1101**. On StickC PLUS/PLUS2 this
    splits into **CC1101 (legacy)** and **CC1101 (Shared SPI)**, each loading a
    different pin set and probing the chip; on failure it shows a QR to a wiring photo.
  - **RF Frequency** — default 433.92 MHz.
- **RFID → Config → RFID Module** — **M5 RFID2**, **PN532 on I²C**, **PN532 on SPI**,
  **RC522 on SPI**, or (on Sticks) **PN532 I²C G33 / G36**.

No firmware rebuild is required — the pin/module choice is stored in `brucePins.conf`.

---

## 3. Flashing

Bruce ships as a single **merged** image named `Bruce-<device>.bin`
(e.g. `Bruce-m5stack-cardputer.bin`). This one file already contains the
bootloader + partition table + app fused together, so almost every method below
only writes that one file.

### The one offset fact you must know

**The merged `Bruce-<device>.bin` is flashed at `0x0` on *every* chip — ESP32,
ESP32-S3, and ESP32-C5 alike.**

`build.py` builds the release with esptool `merge-bin`, placing the bootloader at
the per-MCU boot offset (ESP32 `0x1000`, S3 `0x0000`, C5 `0x2000`), the partition
table at `0x8000`, and the app at `0x10000`. `merge-bin` **bakes the boot offset
into the file as leading `0xFF` padding**, producing a full-flash image whose byte 0
is flash address `0x0`. esptool itself confirms this:
`Wrote 0x…… bytes … ready to flash to offset 0x0`.

So:
- ✅ Flash the **merged single file** at `0x0`. The README one-liner
  (`write_flash 0x00000 …`) is correct for **all** devices.
- ⚠️ The per-MCU boot offset (`0x1000`/`0x0`/`0x2000`) **only** matters if you flash
  the **three separate component files** (`bootloader.bin` + `partitions.bin` +
  `firmware.bin`) instead of the merged release — see the
  [multi-file command](#optional-flashing-the-three-separate-files).

### Method 1 — Web Flasher (recommended)

The official flasher at **<https://bruce.computer/flasher>** runs in your browser via
the Web Serial API and writes the correct image at the correct offset for you.

- **Requires a Chromium browser** (Chrome, Edge, Opera, Brave) on desktop.
  **Firefox and Safari are not supported** (no Web Serial).
- Use a real **data** USB cable (not charge-only). You may need the USB-serial driver
  for your board's bridge chip (CH340, CH9102, CP210x, FTDI); native-USB S3/C5 boards
  don't need one.

**Steps:** plug in → open the flasher in Chrome/Edge → pick your exact board → click
**Connect/Install** → choose the serial port → let it erase + write → reset the device.

If it can't sync, put the board in **download mode** first: hold **BOOT / G0**, tap
**EN/RST**, release BOOT, then retry Connect. (Cardputer: hold **G0** while plugging in.)

### Method 2 — esptool

Use this with a binary from GitHub Releases or CI Actions artifacts.

```sh
pip install esptool
```

**Find the serial port:**
- **Linux:** `ls /dev/ttyUSB* /dev/ttyACM*` (bridges → `ttyUSB*`, native-USB → `ttyACM*`);
  `dmesg | grep -i tty` to watch it appear. Permission denied? `sudo usermod -aG dialout $USER` then re-login.
- **macOS:** `ls /dev/cu.*` — use the `cu.*` node, not `tty.*`.
- **Windows:** Device Manager → **Ports (COM & LPT)** → `COMx` (install the bridge driver if it's an unknown device). `pio device list` also works.

**First-time erase** (recommended when converting from other firmware):

```sh
esptool.py --chip <chip> --port <port> erase_flash
```

**Flash the merged image** (`<chip>` = `esp32` | `esp32s3` | `esp32c5`):

```sh
esptool.py --chip <chip> --port <port> --baud 921600 write_flash 0x0 Bruce-<device>.bin
```

Concrete examples — note **`0x0` for all of them**:

```sh
# ESP32   (M5StickC Plus2)
esptool.py --chip esp32   --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 Bruce-m5stack-cplus2.bin
# ESP32-S3 (M5Cardputer)
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 Bruce-m5stack-cardputer.bin
# ESP32-C5
esptool.py --chip esp32c5 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 Bruce-nm-cyd-c5.bin
```

Always pass `--chip` explicitly (avoids autodetect problems). If `921600` is unstable
on a cheap cable, drop to `460800` or `115200`.

#### Optional: flashing the three separate files

Only if you have the individual files (e.g. from `.pio/build/<env>/`). **Here the
bootloader goes at the per-MCU boot offset:**

```sh
# ESP32 (boot 0x1000):
esptool.py --chip esp32 --port <port> --baud 921600 write_flash \
  0x1000  bootloader.bin \
  0x8000  partitions.bin \
  0x10000 firmware.bin
# ESP32-S3 → use 0x0 for bootloader; ESP32-C5 → use 0x2000. Partitions/app stay 0x8000/0x10000.
```

Match the binary to the board's **flash size** — a 16 MB build won't fit a 4 MB chip.

### Method 3 — M5Burner

Best for M5Stack hardware. Download **M5Burner** (<https://docs.m5stack.com/en/download>),
pick your device category, search **`Bruce`** (official builds are uploaded by
**"owner"** and have photos), select the port/baud, and **Burn**. If it won't connect,
hold **G0/BOOT**, tap **RESET**, and burn again.

### Method 4 — OTA via M5Launcher

If your M5Stack device already runs **M5Launcher**: boot into it → connect to Wi-Fi →
open the firmware catalog → pick **Bruce** for your model → install over the air → reboot.

Because Bruce uses a **single app slot** (no A/B OTA bank), this is the Launcher
fetching and writing the app partition, not classic dual-bank OTA. For strict Launcher
compatibility, pick the **LITE / `LAUNCHER_*`** image where one exists (e.g. CYD).

### Flashing troubleshooting

| Symptom | Fix |
|---|---|
| Device won't boot after flashing the merged file | You flashed at the wrong offset. The merged `Bruce-*.bin` goes at **`0x0`** on every chip. |
| `Failed to connect … Wrong boot mode` / `No serial data received` | Not in download mode. **Hold BOOT/G0**, tap **EN/RST**, keep holding BOOT until esptool syncs. Also lower baud and close other serial monitors. |
| Brownout / resets mid-flash, or reboots when radios start | Cheap/charge-only cable or weak hub. Use a short known-good **data** cable straight into the computer; lower the baud. |
| Wrong chip autodetected | Always pass `--chip esp32`/`esp32s3`/`esp32c5` explicitly. |
| No serial port appears | Install the bridge driver (CH340 / CH9102 / CP210x / FTDI). On Linux, `dmesg \| grep tty` after plugging shows if the kernel saw it. |
| Linux `Permission denied: /dev/ttyUSB0` | `sudo usermod -aG dialout $USER`, then log out/in. |

---

## 4. Building from source

Bruce is a **PlatformIO** project. The same commands CI runs work locally.

### Prerequisites

```sh
pip install platformio requests esptool intelhex
```

- **Python 3** (CI pins 3.13; any recent 3.x is fine).
- **32-bit host compiler** — only needed the first time, and only if you modify the
  JS-interpreter stdlib (`gen_mqjs_headers.py` compiles a helper with `-m32`; a normal
  checkout ships the generated header and skips this). On Linux:
  ```sh
  sudo apt-get install -y --no-install-recommends gcc-multilib libc6-dev-i386
  ```
  On macOS `xcode-select --install`; or just use the [Docker path](#docker-build).
- Or use **VS Code + the PlatformIO IDE extension** — the repo ships a `.vscode/` folder.

The first `pio run` downloads the toolchain, platform, and all libraries into `.pio/`
(large, slow); subsequent builds are cached.

### Select a device and build

Environments are contributed by each `boards/<board>/<board>.ini` (pulled in via
`extra_configs` in `platformio.ini`). You don't need to edit anything — just pass `-e`:

```sh
pio run -e m5stack-cardputer            # build one env
pio run -e lilygo-t-embed-cc1101 -t upload   # build + flash over USB
```

Alternatively, uncomment your device in the `default_envs` list at the top of
`platformio.ini` and run a bare `pio run`. The full env list is that `default_envs`
block (see the [Appendix](#appendix)).

Each env `extends` one of three bases: `[env]` (full build), `[env_light]`
(`LITE_VERSION`, trimmed for M5Launcher), or `[env_4mb]` (restricted for 4 MB flash).

### Build artifacts

```
.pio/build/<env>/firmware.bin      # raw application
.pio/build/<env>/bootloader.bin
.pio/build/<env>/partitions.bin
Bruce-<env>.bin                    # merged image, written to the REPO ROOT by build.py
```

`build.py` runs as a post-action after `firmware.bin` is built: it merges the three
images into `Bruce-<env>.bin` and prints a partition-usage bar, e.g.
`BRUCE: [==========          ] 52.3% (used 0x… of 0x… of OTA partition)`.
If the app exceeds the app slot, the build fails.

**Two custom targets** from `build.py`:

```sh
pio run -e <env> -t build-firmware    # (re)merge into Bruce-<env>.bin without reflashing
pio run -e <env> -t upload-nobuild    # flash the already-built firmware without recompiling
```

### Docker build

Build without installing PlatformIO or the multilib toolchain on your host:

```sh
docker compose run platformio_build
# choose envs (space-separated); default_envs example ships as m5stack-cplus2:
PIO_ENVS="m5stack-cardputer lilygo-t-embed-cc1101" docker compose run platformio_build
```

The container builds envs in parallel batches and drops each `Bruce-<env>.bin` into the
repo root (visible on the host via the bind mount).

### What the build scripts do (gotchas)

`platformio.ini` runs these on every build (in order):

| Script | Purpose |
|---|---|
| `patch.py` | Weakens `ieee80211_raw_frame_sanity_check` in the framework archive (enables raw 802.11 injection); **also** gzips/minifies the WebUI into `include/webFiles.h` — this step calls an external minifier over the network, but is skipped unless the web files changed. |
| `pre_build_current_year.py` | Generates `include/current_year.h` (shows as modified after a build). |
| `gen_mqjs_headers.py` | Regenerates the JS-interpreter stdlib header **only** when `mqjs_stdlib.c` changes (needs the 32-bit compiler); skipped for LITE builds. |
| `patch_library_conflicts.py` | Renames symbols in downloaded libs that collide with the ESP32 core. |
| `flto_prep.py` | Strips `-fno-lto` so whole-program LTO works. |
| `build.py` | Merges bootloader+partitions+app → `Bruce-<env>.bin`; adds `build-firmware`/`upload-nobuild` targets. |

Locally `BRUCE_VERSION` is `"dev"` and `GIT_COMMIT_HASH` is `"Homebrew"`; CI rewrites
these to the real tag/SHA.

See [`docker/`](../docker), [`platformio.ini`](../platformio.ini),
[`boards/README.md`](../boards/README.md), and the workflows in
[`.github/workflows/`](../.github/workflows).

---

## 5. SD card setup & first boot

### Prepare the SD card

1. **Format as FAT32.** Bruce's mount code uses the Arduino `SD`/`SDMMC` stack, which
   reads FAT16/FAT32 only — **exFAT/NTFS will not mount**. Cards ≤ 32 GB format to FAT32
   natively; 64 GB+ cards default to exFAT and must be reformatted (macOS Disk Utility
   "MS-DOS (FAT)", `mkfs.vfat -F 32`, or guiformat/Rufus on Windows).
2. **Copy the *contents* of [`sd_files/`](../sd_files) to the card root** — so
   `/infrared`, `/portals`, `/themes`, `/interpreter`, etc. sit at the top level (not a
   `sd_files/` folder).

What `sd_files/` gives you:

| Folder | Contents |
|---|---|
| `infrared/` | 218 Flipper-format `.ir` remote databases (TVs, ACs, Consoles, Universal) for TV-B-Gone / Custom IR |
| `BadUSB and BlueDucky/` | DuckyScript payloads + offline Ducky-script builder pages |
| `portals/` | Evil-portal credential-capture HTML clones (`en/`, `pt-br/`) |
| `interpreter/` | 20 JS scripts (games + `ir_brute.js`, `rf_brute.js`, `wifi_brute.js`, …) + `gifs/` |
| `pwnagotchi/` | Pwngrid/Brucegotchi face & name spam lists |
| `nfc/` | Bruce-format `.rfid` dumps |
| `themes/` | Offline theme builder + 3 example themes |

### LittleFS vs. SD — devices with no card slot

Bruce is dual-storage. **SD takes priority when a card is present; otherwise everything
uses on-chip LittleFS.** Config is written to LittleFS first, then mirrored to SD if a
card is mounted. On slotless devices (T-Display-S3 base, T-Watch, …), load the same
payloads into **LittleFS** via the on-device **LittleFS Mngr** or the
[WebUI](#webui) file manager. LittleFS is small, so large IR databases realistically
want an SD card.

### Themes

A theme is a folder of menu images + a **`.json`** manifest (icon→image map, RGB565
`priColor`/`secColor`/`bgColor`, `border`, `label`, `boot_img`, `boot_sound`, LED
settings). Build one with the **Bruce Theme Builder** (`bruce.computer/build_theme.html`,
also offline at `sd_files/themes/Theme_Builder.html`) — it outputs a `.zip` you unzip
onto LittleFS/SD. Apply on-device: **Config → UI Theme → (choose FS) → select the
`.json`**. Accepted images: `.bmp`, `.jpg`, `.gif`, `.png` (PNG not on LITE). Suggested
heights: T-Embed 140px, Cardputer/StickC 105px, Core/CYD 180px. UI colors are **RGB565
hex** (`rgbcolorpicker.com/565`); LED colors are plain hex.

> `embedded_resources/web_interface/theme.css.example` is a **WebUI-only** style file
> for the browser panel — not a device theme. Don't confuse the two.

### First boot

1. **Boot splash** (~7 s "PREDATORY FIRMWARE" animation) — **any key press skips it.**
   A `boot.jpg`/`boot.gif` at the storage root (SD first, then LittleFS) overrides the
   default logo; an active theme's `boot_img` overrides that.
2. **Boot sound** — only if enabled; buzzer devices beep, speaker devices play the
   theme's sound or `/boot.wav`. LITE builds skip it.
3. **Default config auto-created** — on a fresh device Bruce writes `/bruce.conf`
   automatically.

**Where settings live** (JSON files on LittleFS, mirrored to SD — *not* NVS/EEPROM):

- **`/bruce.conf`** — brightness, dim timeout, sound, clock/NTP, keyboard layout,
  WebUI creds (`admin`/`bruce`), AP creds (`BruceNet`/`brucenet`), RGB-LED, etc.
- **`/brucePins.conf`** — hardware pin map **and screen rotation/orientation**
  (Config → Orientation persists here, per-device).

A **factory reset** renames these to `/bak.bruce.conf` and restarts.

### WebUI

Launch **WebUI** from the menu, choose **AP mode** (device hosts Wi-Fi) or **my Network**
(join existing Wi-Fi). The device screen shows the connection details:

- **AP mode:** join Wi-Fi **`BruceNet` / `brucenet`**, browse to **`http://bruce.local`**
  or the shown IP.
- **Login:** **`admin` / `bruce`** (also shown on screen). Press **Esc** to stop the server.

The WebUI includes **SD Card** and **LittleFS/SPIFFS** file managers — the intended way
to upload themes, IR files, scripts, and `boot.*` assets to LittleFS on slotless devices.

---

## Appendix

### Partition tables (repo root)

All layouts use a **single app slot** (`app0` only — no OTA A/B bank), so a firmware
update is a full reflash, not dual-bank OTA.

| CSV | App (`app0`) | LittleFS data | Coredump | Used for |
|---|---|---|---|---|
| `custom_4Mb.csv` | ~2.44 MB | 1.5 MB | – | 4 MB **LAUNCHER/LITE** builds |
| `custom_4Mb_full.csv` | 3.75 MB | 192 KB | – | 4 MB **full** builds (CYD, Core, StickC, Elecrow) |
| `custom_8Mb.csv` | ~4.88 MB | 3 MB | 64 KB | 8 MB S3 boards + StickC PLUS2 |
| `custom_16Mb.csv` | ~4.44 MB | ~11.4 MB | 64 KB | 16 MB S3 boards (T-Embed, T-Deck, CoreS3, Smoochiee, …) |

### Buildable environments

The complete env list is the `default_envs` block in
[`platformio.ini`](../platformio.ini). Families: `m5stack-*` (Cardputer, cplus1_1,
cplus2, core/core4mb/core16mb, core2, cores3, sticks3, dinmeter), `lilygo-t-*`
(embed, embed-cc1101, deck, deck-pro, display-s3 [+touch/mmc], display-S3-pro,
display-ttgo, hmi, lora-pager, watch-s3), `CYD-*` (+ `LAUNCHER_*` LITE variants),
`elecrow-*`, `Marauder-*` / `Awok-*` / `WaveSentry-R1`, `esp32-c5[-tft]`,
`esp32-s3-devkitc-1[-psram]`, `arduino-nesso-n1` (C6), `nm-cyd-c5`, `smoochiee-board`,
`reaper`, `xk404`, `Phantom_S024R`, `ES3C28P`.

### Key files

| Path | What |
|---|---|
| [`platformio.ini`](../platformio.ini) | Envs, build flags, extra scripts |
| [`build.py`](../build.py) | Merge + OTA size check + custom targets |
| [`boards/README.md`](../boards/README.md) | How to add a new device |
| [`boards/<board>/pins_arduino.h`](../boards) | Per-device pin/feature map |
| [`custom_*Mb.csv`](../) | Partition layouts |
| [`sd_files/`](../sd_files) | SD-card starter payload pack |
| [`media/connections/`](../media/connections) | Module wiring photos |
| [`pcbs/`](../pcbs) | Community PCB manufacturing files |

### More help

- **Wiki:** <https://wiki.bruce.computer/> · **FAQ:** <https://wiki.bruce.computer/faq/>
- **Discord:** <https://discord.gg/WJ9XF9czVT>
- **Boards / hardware:** <https://bruce.computer/boards>
