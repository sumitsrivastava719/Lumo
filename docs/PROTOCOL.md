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

### Developer channel

Firmware updates and diagnostics run over a separate, **PIN-protected** channel used
by the app's hidden developer panel. It isn't part of the public control protocol.

## Replies (TX)

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
