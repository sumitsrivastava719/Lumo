# LUMO Bluetooth protocol

Firmware 11. Plain BLE, no pairing or bonding. Anything that can write to a GATT
characteristic can drive the lights.

## Service

Nordic UART Service (NUS). The board advertises as **`LUMO`**; firmware 9 and
earlier used `Ambient-LED`.

| | UUID | Direction |
|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | phone → board · write / write-without-response |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | board → phone · notify |

- Up to **two phones** at once. Both receive every state change.
- MTU is negotiated up to 247. Replies are split across notifications and end with `\n`.

## Try it without the app

Any BLE tool that can write text works, for example **nRF Connect** (Android / iOS):

1. Scan and connect to **LUMO**.
2. Enable notifications on **TX** (`6E400003…`) to see the replies.
3. Write text to **RX** (`6E400002…`), e.g. `M41` then `B60`. The room switches to Borealis at 60 %.

Only two devices can be connected at once, so disconnect the app first if two phones are already on.

## Commands

Short ASCII text, one letter plus a number. Several can share one write, separated
by `\n`. The board replies to each batch with the full state.

### Light

| Cmd | Range | Meaning |
|---|---|---|
| `P` | `0` / `1` | power off (fades) / on |
| `Z` | — | blackout: instant off |
| `B` | `0`–`100` | brightness, `0` = dark with power left on |
| `C` `D` `E` | `RRGGBB` | colour 1 / 2 / 3, e.g. `CFF9829` |
| `N` | `0` / `1` | use colours 1–3 in every palette effect |
| `T` | `0`–`600` | sleep timer in minutes, `0` = off |

### Effects

| Cmd | Range | Meaning |
|---|---|---|
| `M` | `0`–`49` | effect (38–49 = room effects added in fw 11) |
| `S` | `1`–`255` | speed |
| `I` | `1`–`255` | intensity |
| `K` | `0`–`9` | palette |
| `Y` | `0`–`240` | auto mode: seconds per effect, `0` = off |

### Room

| Cmd | Range | Meaning |
|---|---|---|
| `X` | `1`–`15` | walls on/off, bit 0 = wall 1 |
| `g` | `w,lo,hi` | **lit range of wall w** (1–4): LEDs `lo`…`hi` counted from that wall's first LED, 0-based, inclusive. `hi` = `65535` means to the end of the wall. |
| `U` | `1`–`65535` | firmware-10 quarter-zone mask, still accepted and converted to ranges |
| `R` | `1`–`15` | corners that corner effects launch from |
| `O` | `8`–`1500` | **total LEDs**. `O0` restores the factory 821-LED layout. |
| `H` | `w,first,last` | **wall range**: wall 1–4, LEDs 1-based and inclusive. `last` = `0` leaves the wall unused. |
| `F` | `1`–`4` | flash a wall for 5 s: white, first LED green, last LED red, everything else dark |

### Music

| Cmd | Range | Meaning |
|---|---|---|
| `A` | `0`–`24` | reactive mode, `0` = off |
| `G` | `1`–`255` | sensitivity |
| `Q` | `0` / `1` | easy on the eyes: no strobes or white flashes |

### Presets and sync

| Cmd | Range | Meaning |
|---|---|---|
| `V` | `0`–`7` | save the current look into a slot |
| `W` | `0`–`7` | apply a slot |
| `J` | `0`–`7` | delete a slot |
| `L` | — | send the preset list |
| `?` | — | send the state |

### Diagnostics

| Cmd | Meaning |
|---|---|
| `!info` | controller report, see [Replies](#replies-tx) |
| `!test` | test pattern for 10 s: walls red / green / blue / yellow, LED 1 white. Checks wiring and wall order. |
| `!lock` | ends developer mode for this connection |

Firmware updates and maintenance (restart, reset, PIN) run over a separate,
**PIN-protected** channel used by the app's hidden developer panel. They aren't part
of the public protocol.

## Reference values

<details>
<summary><b>Effects</b> (<code>M</code>)</summary>

| ID | Effect | ID | Effect | ID | Effect |
|---:|---|---:|---|---:|---|
| 0 | Solid | 17 | Wall Pulse | 34 | Disco BPM |
| 1 | Breathe | 18 | Police Walls | 35 | Rave BPM |
| 2 | Gradient | 19 | Theater | 36 | Beat Walls |
| 3 | Candle | 20 | Scanner | 37 | Party |
| 4 | Fireplace | 21 | Meteor | 38 | Pacifica |
| 5 | Sunrise | 22 | Running | 39 | TwinkleFox |
| 6 | Sunset | 23 | Plasma | 40 | Color Waves |
| 7 | Drift | 24 | Twinkle | 41 | Borealis |
| 8 | Rainbow Room | 25 | Sparkle | 42 | Night Sky |
| 9 | Rain Chase | 26 | Fireworks | 43 | Lava Lamp |
| 10 | Comet | 27 | Strobe | 44 | Fireflies |
| 11 | Dual Comet | 28 | Lightning | 45 | Sheen |
| 12 | Corner Burst | 29 | Ripple | 46 | Leaf Canopy |
| 13 | Corner Race | 30 | Spiral | 47 | Slosh |
| 14 | Corner Flash | 31 | Aurora | 48 | Corner Meet |
| 15 | Wall Wipe | 32 | Matrix | 49 | Ping Pong |
| 16 | Wall Chase | 33 | Heartbeat | | |

34–36 keep time with an internal metronome (Speed sets the BPM), so they look musical with no microphone.
</details>

<details>
<summary><b>Palettes</b> (<code>K</code>)</summary>

| ID | Palette | ID | Palette |
|---:|---|---:|---|
| 0 | Warm White | 5 | Party |
| 1 | Gold | 6 | Cyber |
| 2 | Ocean | 7 | Sunset |
| 3 | Forest | 8 | Rainbow |
| 4 | Lava | 9 | Ice |
</details>

<details>
<summary><b>Music-reactive modes</b> (<code>A</code>, need music sync from the app)</summary>

| ID | Mode | ID | Mode | ID | Mode |
|---:|---|---:|---|---:|---|
| 0 | off | 9 | Noisefire | 18 | Freqmap |
| 1 | GEQ Bars | 10 | Noisemeter | 19 | Freqmatrix |
| 2 | Gravcenter | 11 | Pixelwave | 20 | Freqwave |
| 3 | Gravcentric | 12 | Plasmoid | 21 | Noisemove |
| 4 | Gravimeter | 13 | Puddles | 22 | Rocktaves |
| 5 | Gravfreq | 14 | Puddlepeak | 23 | Waterfall |
| 6 | Juggles | 15 | Ripple Peak | 24 | Binmap |
| 7 | Matripix | 16 | Blurz | | |
| 8 | Midnoise | 17 | DJ Light | | |
</details>

<details>
<summary><b>Wall and corner masks</b> (<code>X</code>, <code>R</code>)</summary>

Add the values of the walls you want: wall 1 = `1`, wall 2 = `2`, wall 3 = `4`, wall 4 = `8`.

| Mask | Walls |
|---:|---|
| `15` | all four |
| `5` | 1 + 3 |
| `10` | 2 + 4 |
| `3` | 1 + 2 |
| `1` | 1 only |

Corners use the same values: C1 (wall 4 / wall 1) = `1`, C2 = `2`, C3 = `4`, C4 = `8`.
</details>

## Examples

| You want | Send |
|---|---|
| Warm white at 40 % | `M0` `CFF9829` `B40` |
| Movie light: dim warm glow | `M0` `CFF7A1A` `B14` |
| Night light: deep amber, barely on | `M0` `CFF3A00` `B4` |
| Ocean on the ceiling | `M38` `B60` |
| Northern lights, calm | `M41` `S80` `I170` |
| Your two colours meeting in the corners | `C00C8FF` `DFF4D8D` `N1` `M48` |
| Only walls 1 and 3 | `X5` |
| Middle third of wall 2 only | `g2,68,135` |
| Wall 2 back to full length | `g2,0,65535` |
| Check where wall 3 is | `F3` |
| Fade out in 30 minutes | `T30` |
| Save the look into preset 1, then recall it | `V0` … `W0` |
| Instant off / back on | `Z` … `P1` |

Separate commands with `\n` to send them in one write, e.g. `M41\nB60`.

## Replies (TX)

Lines starting with `~` are short status replies, e.g. `~ok test`.


**State**, sent after every command batch and on `?`:

```json
{"power":1,"mode":3,"bri":72,"speed":128,"inten":160,"pal":1,
 "r1":255,"g1":154,"b1":60, "r2":0,"g2":140,"b2":255, "r3":255,"g3":0,"b3":128,
 "amode":0,"sens":128,"cm":0,"cor":15,"wal":15,"sm":1,"auto":0,
 "timer":30,"tl":23,"blk":0,"pre":7,"heap":212,"fw":11,"max":100,
 "n":821,"maxn":1500,"geo":[1,209,210,413,414,640,641,821],
 "rng":[0,208,40,143,0,226,0,180],"sb":0}
```

| Field | Meaning |
|---|---|
| `wal` | walls on/off mask |
| `rng` | lit range per wall: `lo, hi` × 4, 0-based inside the wall, clamped to its length |
| `sb` | 1 = in standby |
| `tl` | sleep minutes left |
| `blk` | blackout active |
| `pre` | bitmask of filled preset slots |
| `n` / `maxn` | current / maximum LED count |
| `geo` | first, last LED for walls 1–4 (1-based, last = 0 means unused) |

**Controller report**, sent on `!info`:

```json
{"t":"info","fw":11,"build":"Oct 19 2026 12:00:00","core":"3.3.2","up":5432,"rr":1,
 "heap":201000,"minheap":188000,"cpu":160,"drv":"spi-dma","frame":27,"frames":9000,
 "rus":6100,"rusmax":11800,"mode":41,"part":"app0","mac":"…","links":1,"mtu":247,"auth":0}
```

| Field | Meaning |
|---|---|
| `up` / `rr` | seconds since boot / ESP-IDF reset reason (1 = power-on, 4 = panic, 9 = brown-out) |
| `heap` / `minheap` | free memory now / lowest since boot, bytes |
| `drv` | LED driver: `spi-dma`, or `rmt` if the fallback had to be used |
| `frame` / `rus` / `rusmax` | frame period in ms / time to compute the last and the slowest frame, µs |
| `part` | running app slot (`app0` / `app1` alternate with each firmware update) |

**Presets**, sent on `L` and after every save or delete:

```json
{"t":"pre","p":[[0,1,0,72,"FF9A3C008CFFFF0080",15], null, …]}
```

Each slot is `[effect, palette, useMyColours, brightness, "colour1colour2colour3", wallsMask]`,
or `null` when the slot is empty.

## Audio frames (RX, binary)

The phone does all the listening and streams band data to the board:

| Frame | Layout |
|---|---|
| `0xA3` · 24 bytes | `[A3][16 bands][volSmooth][volRaw][flags][magnitude][peakHz hi][peakHz lo][bpm/2]` |
| `0xA4` · 20 bytes | `[A4][16 bands][vol][flags][peakHz/32]`, for phones stuck at the 23-byte MTU |

`flags`: bit 0 = beat, bit 2 = music detected.
