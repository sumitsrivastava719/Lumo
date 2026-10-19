// Persisted state layouts. Lives in a header so Arduino's auto-generated
// function prototypes (inserted above the sketch body) can already see the types.
#pragma once
#include <stdint.h>

#define NWALL 4

/* v9 (fw 11): every wall has an on/off bit (walls) and ONE lit range, in LEDs counted
   from the start of that wall: lo..hi inclusive, hi = 0xFFFF means "to the end".   */
struct __attribute__((packed)) LedState {
  uint8_t  magic, version;
  uint8_t  power, mode, bri, speed, inten, pal;
  uint8_t  r1,g1,b1,  r2,g2,b2,  r3,g3,b3;
  uint8_t  amode, sens;
  uint8_t  colorMode;     // 0 = palette, 1 = custom 3-colour
  uint8_t  corners;       // bitmask of active corners
  uint8_t  walls;         // bitmask of walls that are lit at all
  uint8_t  smooth;        // 1 = eye-comfort music mode (slew-limited, no white strobes)
  uint8_t  autoCycle;     // seconds between effect changes, 0 = off
  uint16_t timer;
  uint16_t lo[NWALL];     // first lit LED of each wall (offset inside the wall)
  uint16_t hi[NWALL];     // last lit LED of each wall, inclusive (0xFFFF = wall end)
  uint16_t crc;
};                        // 44 bytes

/* fw 10: 16 quarter-zones */
struct __attribute__((packed)) LedStateV8 {
  uint8_t  magic, version;
  uint8_t  power, mode, bri, speed, inten, pal;
  uint8_t  r1,g1,b1,  r2,g2,b2,  r3,g3,b3;
  uint8_t  amode, sens, colorMode, corners, walls, smooth, autoCycle;
  uint16_t timer;
  uint16_t zones;
  uint16_t crc;
};                        // 30 bytes

/* fw 9 (Ambient LED) */
struct __attribute__((packed)) LedStateV7 {   // 28 bytes
  uint8_t  magic, version;
  uint8_t  power, mode, bri, speed, inten, pal;
  uint8_t  r1,g1,b1,  r2,g2,b2,  r3,g3,b3;
  uint8_t  amode, sens, colorMode, corners, walls, smooth, autoCycle;
  uint16_t timer;
  uint16_t crc;
};
