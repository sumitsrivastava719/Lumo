# Hardware & firmware internals

This page is for whoever builds or modifies the controller: electrical design,
power, signal integrity, and how the firmware is put together.

---

## 1. Bill of materials

| Part | Notes |
|---|---|
| **Seeed XIAO ESP32-C6** | RISC-V @ 160 MHz, 512 KB SRAM, 4 MB flash, BLE 5 (+9 dBm used) |
| **WS2812B strip**, 5 V | 821 px in the reference room (60/m). Firmware handles 8 – 1500 px. |
| **5 V PSU** | size it with §3, typically 15–20 A for 821 px |
| **74AHCT125 / SN74AHCT1G125** | 3.3 V → 5 V data level shifter (recommended, §4) |
| 330 Ω resistor | series on the data line, at the first pixel |
| 1000 µF / 10 V electrolytic | across 5 V/GND at the strip input |
| Fuses, one per injection point | 5 A blade fuse per wall feed |
| Wire | 14 AWG trunk, 16–18 AWG injection drops, 22–24 AWG data |

---

## 2. Wiring

```
                 ┌──────────────────────── 5 V PSU ────────────────────────┐
                 │ +5V                                               GND   │
                 │                                                    │    │
     ┌───────────┼──────────┬──────────────┬──────────────┬───────────┼────┤
     │ F1 5A     │ F2 5A    │ F3 5A        │ F4 5A        │           │    │
     v           v          v              v              v           │    │
  ┌──────┐  inject W1   inject W2      inject W3      inject W4      │    │
  │ XIAO │   (LED 1)    (LED 210)      (LED 414)      (LED 641)      │    │
  │ 5V <─┼── (USB or a dedicated 5 V; never back-feed USB from the PSU)│    │
  │      │                                                            │    │
  │ D9 ──┼──> 74AHCT125 (VCC = 5 V) ──> 330 Ω ──> DIN of LED 1         │    │
  │GPIO20│        ^ OE tied to GND                                    │    │
  │ GND ─┼────────┴────────────────────── common ground ──────────────┴────┘
  └──────┘                                    1000 µF across +5V/GND at LED 1
```

**Rules that matter**

- **Common ground** between the XIAO, the level shifter, the strip and the PSU. Without it the data line has no reference, and the result is random colours and flicker.
- **Don't power 800 LEDs through the XIAO.** Its 5 V pin is USB VBUS. Feed the strip straight from the PSU.
- **Inject power at the start of every wall** (§3). Feeding only one end gives a brown-to-red fade along the strip.
- Keep the data run from D9 to the first pixel short: under 1 m, or use the level shifter at the strip end.

---

## 3. Power budget

WS2812B draws about 20 mA per channel at full scale (≈ 60 mA per pixel, full
white) plus about 1 mA quiescent. The firmware limits the real draw three ways:

1. **Brightness ceiling:** `BRI_MAX = 100` of 255 (39 %).
2. **Colour correction** (`TypicalLEDStrip` 255/176/240) scales G and B down.
3. **FastLED power limiter:** `setMaxPowerInVoltsAndMilliamps(5, PSU_MILLIAMP)` scales each frame so its estimated draw stays under your PSU.

| LEDs | Theoretical full white | **Lumo worst case** (100 %, white) | Idle (all off) |
|---:|---:|---:|---:|
| 300 | 18.0 A | 6.2 A | 0.3 A |
| **821** | 49.3 A | **16.9 A** | 0.8 A |
| 1200 | 72.0 A | 24.8 A | 1.2 A |
| 1500 | 90.0 A | 31.0 A | 1.5 A |

**PSU sizing:** take the worst case, add about 20 % headroom, and set
`PSU_MILLIAMP` to the PSU's continuous rating. The limiter then dims frames that
would exceed it, so an undersized supply never browns out.

**Voltage drop per wall feed** (209 px ≈ 3.5 m drawing about 4.3 A, 3 m run, out and back):

| Wire | Drop |
|---|---:|
| 18 AWG | 0.54 V |
| 16 AWG | 0.34 V |
| 14 AWG | 0.21 V |

WS2812B is fine down to about 4.5 V at the pixel. Use 16 AWG or heavier for drops longer than 2 m.

---

## 4. Data signal

The ESP32-C6 drives **3.3 V**. WS2812B at 5 V needs V<sub>IH</sub> ≥ 0.7 × V<sub>DD</sub> = **3.5 V**,
so a direct connection is out of spec. It often works on the bench, then fails
when the strip warms up or the supply rises.

| Option | Verdict |
|---|---|
| **74AHCT125 / 74HCT245 at 5 V** | ✅ recommended: clean edges, drives long runs |
| "Sacrificial pixel": first LED powered at about 4.3 V through a diode | works, costs one pixel |
| Direct 3.3 V | ⚠️ unreliable, bench use only |

The 330 Ω series resistor at DIN damps ringing on the cable. The 1000 µF cap
absorbs the inrush when the strip goes from black to full.

**Timing:** 800 kHz NRZ, 24 bits per pixel = **30 µs per LED**.

**Driver: SPI + DMA (firmware 11, `LumoOutput.h`).** FastLED's RMT driver on the
C6 has no DMA and only a 48-symbol buffer, which is two LEDs. The CPU refills it
from an interrupt every ~60 µs. When the BLE stack holds the CPU longer, or an
NVS write briefly suspends the flash cache, the waveform breaks and dark pixels
latch random colours. Firmware 10 showed this as flicker and stray LEDs outside
the selected zones.

Firmware 11 encodes the whole frame into RAM, then **GPSPI2 clocks it out by
DMA** with no CPU involvement:

| | |
|---|---|
| SPI clock | 2.5 MHz → 0.4 µs per SPI bit |
| Encoding | 3 SPI bits per WS2812 bit: `1` = `110` (0.8 µs high), `0` = `100` (0.4 µs high), 1.2 µs period |
| Frame | 4 lead bytes low, 9 bytes per LED (G R B), 100 reset bytes = 320 µs latch (WS2812B-V5 needs > 280 µs) |
| Buffers | 2 × 13.6 KB DMA-capable (double buffer: encode one while the other is on the wire) |
| Fallback | if the SPI bus can't start, FastLED RMT is used and `!info` reports `drv: rmt` |

| LEDs | Wire time | Frame period | Frame rate |
|---:|---:|---:|---:|
| 300 | 9.0 ms | 25 ms | 40 fps |
| 821 | 24.6 ms | 27 ms | 36 fps |
| 1200 | 36.0 ms | 39 ms | 26 fps |
| 1500 | 45.0 ms | 48 ms | 21 fps |

The frame period is `max(25 ms, n × 30 µs + 3 ms)`, recomputed whenever the LED
count changes from the app.

---

## 5. Board pins

| Pin | Use |
|---|---|
| D9 / GPIO 20 | LED data (`DATA_PIN`) |
| GPIO 15 | on-board user LED, active low: **blink** = advertising, **solid** = phone connected |
| USB-C | power, flashing, serial log @ 115200 (USB-Serial/JTAG, no UART bridge) |

---

## 6. Firmware architecture

```
 NimBLE host task                         Arduino loop()  (one thread owns all state)
┌───────────────────────┐   FreeRTOS    ┌──────────────────────────────────────────────┐
│ onWrite(RX)           │   queue (24)  │ 1. drain ≤16 commands → handleCmd()          │
│  ├ text  → enqueue ───┼──────────────>│ 2. reply state/presets (coalesced, ≥120 ms)  │
│  └ audio → copy under │               │ 3. render() every frameMs                    │
│    spinlock           │               │      effect → leds[]                         │
│ onConnect/Disconnect  │               │      output stage → outLeds[] → RMT          │
│  → flags only         │               │ 4. NVS save 2 s after the last change        │
└───────────────────────┘               │ 5. watchdog: re-arm advertising every 3 s    │
                                        └──────────────────────────────────────────────┘
```

- **The BLE callbacks never touch state.** Text goes into a FreeRTOS queue, and 24-byte audio frames are copied under a `portMUX` spinlock. `loop()` applies both. This keeps the NimBLE task non-blocking, as the core's BLE docs require.
- **Advertising can't stop:** it restarts on disconnect, and a 3 s watchdog re-arms it if anything else stopped it.
- **Connection parameters:** 7.5–15 ms interval (for audio), 4 s supervision timeout, so a vanished phone frees its slot quickly. MTU up to 256. Max 2 centrals.

### Render pipeline

```
effect / music mode ──> leds[]   (effects read it back next frame: trails, fades, shifts)
                           │
          output stage     v   per channel:  out = round(c × level × corr_ch × zone / 2³²)
                        outLeds[] ──> PSU limiter ──> SPI encode ──> DMA ──> strip
```

`leds[]` and `outLeds[]` are separate on purpose. Firmware 9 thresholded and
scaled `leds[]` in place, which caused two bugs:

1. **Hue shift when dim.** The weak channels of a colour truncated to 0 first. Warm orange `FFAA1E` at 10 % came out as `(8,0,0)`, pure red.
2. **Broken trails.** Effects read their own buffer back the next frame, so the zeroing cut every fade short.

The output stage now combines brightness (8.8 fixed point), colour correction and
the per-pixel zone gain into a single 0.16 factor per channel and **rounds** it.
The same orange at 10 % is now `(10,5,1)`. The master level eases toward its
target by 1/8 per frame, so brightness changes and the sleep fade are smooth.

### Lit ranges & layout

- Each wall has an on/off bit and **one exact lit range** (`lo`…`hi`, LEDs counted from that wall's first LED, `hi = 0xFFFF` = to the end). Firmware 10's 16 quarter-zones are migrated: the range spans from the first to the last lit quarter.
- `zoneGain[]` holds one byte per pixel (255 lit / 0 dark) and is rebuilt only when walls, ranges or the layout change. Edges are exact, with no fade eating into the LEDs you picked.
- Pixels outside every wall range get gain 0, so gaps at the corners stay dark.
- The strip length changes at runtime: the old length is blanked first, so pixels past the new end don't freeze on their last colour.
- Buffers are statically sized for `MAX_LEDS + 16` (the pad lets effects peek past a wall end safely).

### Memory

| | |
|---|---|
| Pixel buffers (`leds`, `outLeds`, `prevFrame`, `heat`, `zoneGain`, scratch) | 12 B × 1516 ≈ 18 KB, static |
| SPI DMA frame buffers | 2 × 13.6 KB, heap |
| Total static RAM | 47 KB of 320 KB |
| Flash | 838 KB (63 % of the 1.25 MB app slot; the other slot receives OTA updates) |

### NVS (namespace `led7`)

| Key | Contents | Size |
|---|---|---:|
| `st` | `LedState` v9: the current look incl. per-wall ranges, CRC-16 protected | 44 B |
| `p0`…`p7` | presets, same struct | 44 B each |
| `geo` | LED count + 4 wall positions, CRC-16 | 22 B |
| `pin` | developer PIN, once changed from the app | ≤ 12 B |

Older blobs are recognised by size and migrated: v8 (fw 10, 30 B, quarter-zones)
and v7 (fw 9, 28 B, whole walls). Writes are debounced by 2 s and
skipped when the CRC is unchanged, which saves flash wear during slider drags.

### Fail-safes

| Event | Behaviour |
|---|---|
| Phone disappears | 4 s supervision timeout → slot freed → advertising again |
| App sends garbage | non-ASCII and over-length writes dropped; every value `constrain()`ed |
| Corrupt NVS | CRC or magic mismatch → defaults, then sanitised |
| Undersized PSU | the FastLED power limiter scales the frame down |
| Sleep timer fires | linear fade over the last 60 s, then power off and timer cleared |
| Bad firmware update | CRC-32 + image validation before switching; a new image must run 20 s before it's marked valid, otherwise the bootloader rolls back |
| OTA link drops | the board aborts after 15 s without data (or when no phone is left); the running firmware is untouched |

### Standby (power off)

Deep sleep would switch the radio off, and then the app couldn't turn the lights
back on. The core build also has no BLE light-sleep. Power-off is therefore a
**connectable standby**:

- after the fade-out, a few black frames are sent, then LED output stops
- CPU drops from 160 to 80 MHz, and `loop()` sleeps 20 ms per pass instead of spinning
- Bluetooth stays advertising and connected, so **On** from the app is instant
- optional `STRIP_PWR_PIN`: drive a logic-level MOSFET or relay that cuts the strip's 5 V. This is the big saving, because 821 dark WS2812Bs still draw about 0.8 A of quiescent current

### Firmware update over Bluetooth

New firmware can be installed from the app. The controller only accepts a valid
ESP32-C6 app image that fits the update slot and passes a CRC check. A new image has to
run for 20 s before it's kept; if it crashes or reboots earlier, the controller rolls
back to the previous firmware by itself. Updating is PIN-protected.

---

## 7. Changing the hardware

| What | Where |
|---|---|
| Data pin | `#define DATA_PIN` |
| Strip type / colour order | `LED_TYPE`, `COLOR_ORDER` (e.g. SK6812 / GRB) |
| PSU limit | `PSU_MILLIAMP` |
| Brightness ceiling | `BRI_MAX` (keep it ≤ 100 unless you've redone §3) |
| Max strip length | `MAX_LEDS` (RAM grows 12 B per LED) |
| Default room layout | `DEF_LEDS`, `DEF_START[]`, `DEF_LEN[]` |
| Strip power switch | `STRIP_PWR_PIN`, `STRIP_PWR_ON` (−1 = not fitted) |
| Factory developer PIN | `firmware/LumoLED/LumoSecrets.h` (git-ignored; copy the `.example`) |
