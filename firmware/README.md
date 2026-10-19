# LUMO firmware

Runs on a **Seeed XIAO ESP32-C6** driving WS2812B strips: 821 LEDs by default,
up to 1500, and the layout is set from the app.

```
LumoLED/
├── LumoLED.ino     the sketch
├── LumoOutput.h    WS2812 driver: SPI + DMA, immune to BLE / flash stalls
├── LumoState.h     saved-settings layout (kept separate for Arduino's prototype generator)
└── LumoSecrets.h.example   copy to LumoSecrets.h (git-ignored) to set your factory PIN
```

## Wiring

| XIAO pin | Goes to |
|---|---|
| **D9** (GPIO 20) | 74AHCT125 → 330 Ω → strip DIN |
| GND | strip GND **and** PSU GND (common ground) |
| 5V | not the strip. Power the strip straight from the PSU, injected per wall. |

Full schematic, power budget and signal notes: **[../docs/HARDWARE.md](../docs/HARDWARE.md)**.

Set your supply before flashing:

```cpp
#define PSU_MILLIAMP 15000   // your PSU's real rating, in mA
```

## Option A: Arduino IDE

| Setting | Value |
|---|---|
| Board | **XIAO_ESP32C6** |
| Core | esp32 by Espressif **3.3.x** (3.0.x has a different BLE stack and won't build) |
| Library | **FastLED 3.9.16** |
| Partition | Default |
| USB CDC On Boot | Enabled |

Open `LumoLED/LumoLED.ino` and press Upload.

## Option B: arduino-cli

```bash
arduino-cli compile -b esp32:esp32:XIAO_ESP32C6 firmware/LumoLED
```

```bash
arduino-cli upload -b esp32:esp32:XIAO_ESP32C6 -p /dev/cu.usbmodem* firmware/LumoLED
```

## Option C: prebuilt binary, no toolchain

```bash
esptool --chip esp32c6 --port /dev/cu.usbmodem* write-flash 0x0 release/LumoLED-fw11-merged.bin
```

## First boot

The four walls flash **red → green → blue → yellow**, then the board's small LED
**blinks** while it waits for a phone. It goes **solid** once one connects.

Serial monitor at 115200:

```
=== LUMO · fw 11 ===
[led] 821 LEDs via SPI+DMA
[nvs] restored
[ble] advertising as "LUMO"
```

Upgrading from the Ambient LED firmware (fw 9) keeps your settings and presets.
They're migrated automatically.

## Option D: over Bluetooth (firmware 11 and later)

Later builds install from the phone through the app (PIN-protected). Export the
plain app image in Arduino IDE (*Sketch → Export Compiled Binary* → `LumoLED.ino.bin`,
not `.merged.bin`). A failed or crashing update rolls back by itself.

> [!IMPORTANT]
> Builds without `LumoSecrets.h`, including the files in `release/`, use the
> factory PIN `0000`. Change it from the app after flashing.

## Build stats (core 3.3.2)

| | |
|---|---|
| Flash | 839 KB (64 % of the 1.25 MB app slot) |
| RAM | 47 KB static of 320 KB |
| Warnings | 0 |
