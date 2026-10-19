/* ═══════════════════════════════════════════════════════════════════════════
   LUMO  ·  ambient room light  ·  XIAO ESP32-C6  ·  821 × WS2812B on D9 (GPIO20)
   (formerly "Ambient LED · BLE edition" — same protocol, settings carry over)

   v11  · LED output moved to SPI + DMA (LumoOutput.h): no more flicker or stray
          pixels while BLE is busy or settings are being saved
        · each wall lights ONE exact LED range (first..last) instead of 4 quarters
        · Police Walls is always red / blue
        · power-off = low-power standby: output stopped, CPU 80 MHz, loop idles;
          'On' from the app wakes it instantly (optional strip power MOSFET pin)
        · firmware update over Bluetooth (OTA) with automatic rollback
        · dev commands: !info  !test  !reboot  !factory
        · Flash (identify a wall) is steady and everything else is dark

   v10  · brightness is a true 0–100 again (no 8 → 0 cutoff); colours keep their
          hue at low brightness (16-bit rounding, separate output buffer)
        · each wall split into 4 zones — 16 zones, light any combination
        · presets are reported back to the phone ('L'), can be deleted ('J')
        · sleep timer reports minutes left and clears itself when it fires
        · compact 20-byte audio frame for phones stuck at the 23-byte MTU

   Wi-Fi access point removed entirely. Control is over Bluetooth LE from the
   Lumo Android app.  No certificates, no captive portal, no http/https,
   no TLS heap — that whole class of failure is gone.

   BLE    Nordic UART Service (NUS)
          RX 6E400002  phone → board   write / write-without-response
          TX 6E400003  board → phone   notify  (state JSON, newline-terminated)
   Name   "LUMO"            up to 2 phones at once

   Room   Wall 1  1-209 · Wall 2  210-413 · Wall 3  414-640 · Wall 4  641-821

   BUILD  Board XIAO_ESP32C6 · esp32 core 3.3.x · FastLED 3.9.16
          Partition: Default works · No other libraries (BLE = core NimBLE).
   ═══════════════════════════════════════════════════════════════════════════ */

#include <Preferences.h>
#include "LumoState.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_mac.h"

#define FASTLED_INTERNAL
#include <FastLED.h>
#include "LumoOutput.h"


/* ───────────────────────── HARDWARE ───────────────────────── */
#define DATA_PIN      20
#define STRIP_PWR_PIN -1         // optional: GPIO driving a MOSFET/relay that cuts strip 5 V in standby
                                 // (821 dark WS2812s still draw ~0.8 A). -1 = not fitted.
#define STRIP_PWR_ON  HIGH       // level that switches the strip ON
#define MAX_LEDS      1500       // longest strip the firmware will drive (set in the app)
#define DEF_LEDS      821        // factory layout
#define LED_PAD       16         // slack after the buffers: effects may peek a few pixels past a wall
uint16_t nLeds = DEF_LEDS;       // live strip length — set from the app, saved on the board
#define NUM_LEDS nLeds
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB

#define PSU_VOLTS     5
#define PSU_MILLIAMP  15000      // ◄── SET TO YOUR SUPPLY
#define BRI_MAX       100        // hard brightness ceiling (UI + firmware), 0..100
#define BRI_SOFT      46         // floor the eye-comfort party mode lifts to

/* ───────────────────────── BLUETOOTH ───────────────────────── */
#define BLE_NAME      "LUMO"
#define SVC_UUID      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // phone → board
#define TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // board → phone
#define MAX_CLIENTS   2
#define STATUS_LED    15          // XIAO C6 user LED, active LOW (blink = advertising, solid = connected)
#define FW_VERSION    11

#define FRAME_MS      25         // 821 LEDs = 24.6 ms to clock out → ~40 fps
uint16_t frameMs = FRAME_MS;     // grows with the strip: WS2812 needs 30 µs per LED

/* ───────────────────────── ROOM GEOMETRY ───────────────────────── */
/* the layout is set from the app ('O' total, 'H' wall range) and kept in NVS key "geo" */
const uint16_t DEF_START[NWALL] = {   0, 209, 413, 640 };
const uint16_t DEF_LEN  [NWALL] = { 209, 204, 227, 181 };
uint16_t WSTART[NWALL] = {   0, 209, 413, 640 };
uint16_t WLEN  [NWALL] = { 209, 204, 227, 181 };
#define CORNER WSTART                       // each wall starts at its corner
bool geoGaps = false;                       // some LEDs belong to no wall → they stay dark
/* the "two opposite corners" pair: C2 = join of wall 1+2, C4 = join of wall 3+4 */
#define PAIR_A  1
#define PAIR_B  3
#define PAIR_MASK ((1 << PAIR_A) | (1 << PAIR_B))      // 0b1010 = 10

CRGB leds[MAX_LEDS + LED_PAD];      // what the effects draw (and read back next frame)
CRGB outLeds[MAX_LEDS + LED_PAD];   // what actually goes down the wire — effects never see it
CRGB prevFrame[MAX_LEDS + LED_PAD]; // temporal smoothing for eye-comfort mode
uint8_t zoneGain[MAX_LEDS + LED_PAD]; // per-pixel lit mask: 255 = inside a lit wall range, 0 = dark
uint8_t heat[MAX_LEDS + LED_PAD];

static inline uint16_t wrapIdx(int32_t i) {
  while (i < 0) i += NUM_LEDS;
  while (i >= NUM_LEDS) i -= NUM_LEDS;
  return (uint16_t)i;
}
/* shortest distance between two points around the loop */
static inline uint16_t loopDist(uint16_t a, uint16_t b) {
  uint16_t d = (a > b) ? (a - b) : (b - a);
  return (d > NUM_LEDS / 2) ? (NUM_LEDS - d) : d;
}
static inline void fillWall(uint8_t w, const CRGB &c) {
  for (uint16_t i = 0; i < WLEN[w]; i++) leds[WSTART[w] + i] = c;
}

/* ───────────────────────── STATE (persisted) ───────────────────────── */
#define STATE_MAGIC 0xB2
#define STATE_VER   9          // v9 = one lit LED range per wall (v7 / v8 blobs are migrated)
#define N_EFFECTS   50
#define N_AMODES    24

#define ZONES_ALL  0xFFFF      // fw-10 'U' command: 16 quarter-zones (still accepted)
#define RANGE_END  0xFFFF      // "to the end of the wall"
bool zonesMasked = false;      // true when any LED must be forced dark by walls/ranges/gaps

LedState S;
Preferences prefs;

bool     dirty = false, blackout = false, uiDirty = false, zonesDirty = true, wantPresets = false;
uint32_t dirtyAt = 0, lastCast = 0, autoAt = 0;

/* ── the exact data set WLED's audio-reactive effects consume ──
   fftResult[16] · volumeSmth · volumeRaw · samplePeak · FFT_MajorPeak · magnitude */
volatile uint8_t  fftRx[16] = {0};
volatile uint8_t  aVolSmth = 0, aVolRaw = 0, aMag = 0, aConf = 0, aBpm = 0;
volatile uint16_t aPeakHz = 0;
volatile bool     aBeat = false, aMusic = false;
volatile uint32_t aLast = 0, aMusicAt = 0;

uint8_t  fftResult[16] = {0};          // smoothed copy the effects read
float    volumeSmth   = 0.0f;
int16_t  volumeRaw    = 0;
float    FFT_MajorPeak = 60.0f;
float    my_magnitude  = 0.0f;
bool     samplePeak    = false;
uint8_t  env = 0, strobeEnv = 0;
#define AUDIO_TIMEOUT 600
portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;   // guards the audio block (BLE task ↔ loop)
#define MAX_FREQ_LOG10 4.04238f

/* ───────────────────────── RUNTIME ───────────────────────── */

uint32_t lastFrame = 0, fxStart = 0, sleepEnd = 0, beatAt = 0;
uint8_t  lastMode = 255;
uint8_t  partySub = 0;
uint32_t curBri = 0;                    // master level, 8.8 fixed point

/* output + power + OTA state (declared early: layout code and BLE callbacks use it) */
bool     useSpi = false;               // SPI+DMA driver up (else FastLED RMT fallback)
uint8_t  darkFrames = 0;
bool     standby = false;
uint32_t frameCount = 0;
uint32_t renderUs = 0, renderUsMax = 0;   // time spent computing the last / slowest frame
volatile bool     otaActive = false;   // ── shared with the BLE task ──
volatile uint32_t otaExpected = 0;     // next byte offset the BLE task accepts
volatile bool     otaResync = false;   // a chunk arrived out of order / didn't fit
volatile bool     otaAbortReq = false;
uint32_t otaTotal = 0, otaWritten = 0, otaCrc = 0, otaCrcWant = 0, otaLastAck = 0, otaLastData = 0;
StreamBufferHandle_t otaStream = nullptr;
volatile uint16_t otaConn = 0xFFFF;      // the phone running the update
volatile uint16_t devAuthConn = 0xFFFF;  // the phone that entered the dev PIN
uint16_t cmdConn = 0xFFFF;               // the phone that sent the command being handled
uint32_t devAuthUntil = 0;
void pushOut();
uint16_t pos16 = 0, ripplePos = 0;
uint8_t  hue8 = 0;
CRGBPalette16 curPal;

/* ═══════════════════════ helpers ═══════════════════════ */
static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) { c ^= *d++; for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}
static inline CRGB C1() { return CRGB(S.r1, S.g1, S.b1); }
static inline CRGB C2() { return CRGB(S.r2, S.g2, S.b2); }
static inline CRGB C3() { return CRGB(S.r3, S.g3, S.b3); }
static inline uint8_t bpmOf(uint8_t lo, uint8_t hi) { return map(S.speed, 1, 255, lo, hi); }
static inline uint8_t gam(uint8_t v) { return scale8(v, v); }        // gamma ≈ 2.0

CRGBPalette16 palFor(uint8_t i) {
  switch (i) {
    case 0:  return CRGBPalette16(CRGB(255,180, 90), CRGB(255,215,160), CRGB(255,165, 75), CRGB(255,200,130));
    case 1:  return CRGBPalette16(CRGB(255,120,  0), CRGB(255,190, 60), CRGB(255,150, 20), CRGB(255,230,140));
    case 2:  return OceanColors_p;
    case 3:  return ForestColors_p;
    case 4:  return LavaColors_p;
    case 5:  return PartyColors_p;
    case 6:  return CRGBPalette16(CRGB(255,  0,160), CRGB(  0,255,209), CRGB(120,  0,255), CRGB(  0,200,255));
    case 7:  return CRGBPalette16(CRGB( 43, 11, 63), CRGB(255, 94, 58), CRGB(255,197,107), CRGB(120, 40, 90));
    case 8:  return RainbowColors_p;
    default: return CRGBPalette16(CRGB(191,240,255), CRGB( 46,134,255), CRGB(220,250,255), CRGB( 80,160,255));
  }
}
/* custom colour mode re-skins EVERY palette effect with your 3 colours */
void rebuildPal() {
  curPal = (S.colorMode == 1) ? CRGBPalette16(C1(), C2(), C3(), C1()) : palFor(S.pal);
}
static inline CRGB pcol(uint8_t idx, uint8_t v = 255) {
  return ColorFromPalette(curPal, idx, v, LINEARBLEND);
}

void defaults() {
  S.magic = STATE_MAGIC; S.version = STATE_VER;
  S.power = 1; S.mode = 0; S.bri = 80; S.speed = 128; S.inten = 160; S.pal = 1;
  S.r1 = 255; S.g1 = 170; S.b1 = 30;
  S.r2 = 0;   S.g2 = 140; S.b2 = 255;
  S.r3 = 255; S.g3 = 0;   S.b3 = 128;
  S.amode = 0; S.sens = 128; S.colorMode = 0; S.corners = 0x0F; S.walls = 0x0F; S.smooth = 1;
  S.autoCycle = 0; S.timer = 0;
  for (uint8_t w = 0; w < NWALL; w++) { S.lo[w] = 0; S.hi[w] = RANGE_END; }
}
/* fw-10 quarter-zone mask (bit = wall*4 + quarter) → one range per wall:
   the range spans from the first to the last lit quarter of that wall      */
static void applyZoneMask(LedState &t, uint16_t z) {
  t.walls = 0;
  for (uint8_t w = 0; w < NWALL; w++) {
    uint8_t q = (z >> (w * 4)) & 0x0F;
    t.lo[w] = 0; t.hi[w] = RANGE_END;
    if (!q) continue;
    t.walls |= 1 << w;
    if (q == 0x0F) continue;
    uint8_t a = 0, b = 3;
    while (!(q & (1 << a))) a++;
    while (!(q & (1 << b))) b--;
    t.lo[w] = (uint32_t)WLEN[w] * a / 4;
    t.hi[w] = (b == 3) ? RANGE_END : (uint16_t)((uint32_t)WLEN[w] * (b + 1) / 4 - 1);
  }
  if (!t.walls) t.walls = 0x0F;
}
/* read a state blob in either layout; false if it isn't one of ours */
static bool readBlob(const char *key, LedState &out) {
  size_t n = prefs.getBytesLength(key);
  if (n == sizeof(LedState)) {
    LedState t;
    if (prefs.getBytes(key, &t, sizeof(t)) != sizeof(t)) return false;
    if (t.magic != STATE_MAGIC || t.version != STATE_VER) return false;
    if (t.crc != crc16((uint8_t *)&t, sizeof(t) - 2)) return false;
    out = t; return true;
  }
  if (n == sizeof(LedStateV8)) {                            // fw 10: quarter-zones → ranges
    LedStateV8 o;
    if (prefs.getBytes(key, &o, sizeof(o)) != sizeof(o)) return false;
    if (o.magic != STATE_MAGIC || o.version != 8) return false;
    LedState t;
    memcpy(&t, &o, offsetof(LedStateV8, timer));            // identical prefix
    t.version = STATE_VER; t.timer = o.timer;
    applyZoneMask(t, o.zones ? o.zones : ZONES_ALL);
    out = t; return true;
  }
  if (n == sizeof(LedStateV7)) {                            // fw 9: whole walls
    LedStateV7 o;
    if (prefs.getBytes(key, &o, sizeof(o)) != sizeof(o)) return false;
    if (o.magic != STATE_MAGIC || o.version != 7) return false;
    LedState t;
    memcpy(&t, &o, offsetof(LedStateV7, timer));
    t.version = STATE_VER; t.timer = o.timer;
    t.walls = o.walls ? (o.walls & 0x0F) : 0x0F;
    for (uint8_t w = 0; w < NWALL; w++) { t.lo[w] = 0; t.hi[w] = RANGE_END; }
    out = t; return true;
  }
  return false;
}
static void sanitize(LedState &t) {
  if (t.bri > BRI_MAX) t.bri = BRI_MAX;
  if (t.mode >= N_EFFECTS) t.mode = 0;
  if (t.amode > N_AMODES) t.amode = 0;
  if (t.pal > 9) t.pal = 0;
  if (!t.speed) t.speed = 1;
  if (!t.inten) t.inten = 1;
  if (!t.sens) t.sens = 1;
  if (!(t.corners & 0x0F)) t.corners = 0x0F;
  t.walls &= 0x0F;
  if (!t.walls) t.walls = 0x0F;
  for (uint8_t w = 0; w < NWALL; w++)
    if (t.lo[w] > t.hi[w]) { uint16_t x = t.lo[w]; t.lo[w] = t.hi[w]; t.hi[w] = x; }
}
void saveState() {
  S.crc = crc16((uint8_t *)&S, sizeof(S) - 2);
  LedState old;
  if (prefs.getBytes("st", &old, sizeof(old)) == sizeof(old) && old.crc == S.crc) return;
  prefs.putBytes("st", &S, sizeof(S));
  Serial.println(F("[nvs] saved"));
}
void loadState() {
  LedState t;
  if (readBlob("st", t)) { S = t; Serial.println(F("[nvs] restored")); }
  else { defaults(); Serial.println(F("[nvs] defaults")); }
  S.amode = 0; S.timer = 0;
  sanitize(S);
  zonesDirty = true;
}
void touchState() { dirty = true; dirtyAt = millis(); uiDirty = true; }

/* ═══════ STRIP LAYOUT — total length + one LED range per wall ═══════ */
#define GEO_MAGIC 0x6E
struct __attribute__((packed)) Geo { uint8_t magic, ver; uint16_t n; uint16_t st[NWALL], ln[NWALL]; uint16_t crc; };
bool    geoDirty = false;
int8_t  identW = -1;
uint32_t identUntil = 0;

static void fitWalls() {                  // keep every wall inside the strip
  for (uint8_t w = 0; w < NWALL; w++) {
    if (WSTART[w] >= nLeds) { WSTART[w] = nLeds - 1; WLEN[w] = 0; }
    if ((uint32_t)WSTART[w] + WLEN[w] > nLeds) WLEN[w] = nLeds - WSTART[w];
  }
  geoGaps = false;
  for (uint16_t i = 0; i < nLeds && !geoGaps; i++) {
    bool in = false;
    for (uint8_t w = 0; w < NWALL && !in; w++) in = (i >= WSTART[w] && i < WSTART[w] + WLEN[w]);
    if (!in) geoGaps = true;
  }
  zonesDirty = true;
}
void loadGeo() {
  Geo g;
  if (prefs.getBytes("geo", &g, sizeof(g)) == sizeof(g) && g.magic == GEO_MAGIC &&
      g.crc == crc16((uint8_t *)&g, sizeof(g) - 2) && g.n >= 8 && g.n <= MAX_LEDS) {
    nLeds = g.n;
    for (uint8_t w = 0; w < NWALL; w++) { WSTART[w] = g.st[w]; WLEN[w] = g.ln[w]; }
    Serial.printf("[geo] %u LEDs (custom layout)\n", nLeds);
  }
  fitWalls();
  frameMs = max<uint16_t>(FRAME_MS, (uint32_t)nLeds * 30 / 1000 + 3);
}
void saveGeo() {
  Geo g; g.magic = GEO_MAGIC; g.ver = 1; g.n = nLeds;
  for (uint8_t w = 0; w < NWALL; w++) { g.st[w] = WSTART[w]; g.ln[w] = WLEN[w]; }
  g.crc = crc16((uint8_t *)&g, sizeof(g) - 2);
  prefs.putBytes("geo", &g, sizeof(g));
  Serial.println(F("[geo] saved"));
}
/* change the strip length live: blank the old length first, so LEDs past the new end
   don't freeze on their last colour, then point the controller at the new count */
void setStripLength(uint16_t n) {
  n = constrain(n, 8, MAX_LEDS);
  if (n != nLeds) {
    memset(outLeds, 0, sizeof(outLeds));
    pushOut();                                   // blank the OLD length (driver waits for it)
    nLeds = n;
    if (!useSpi) FastLED[0].setLeds(outLeds, nLeds);
    memset(leds, 0, sizeof(leds)); memset(heat, 0, sizeof(heat)); memset(prevFrame, 0, sizeof(prevFrame));
    lastMode = 255;
    frameMs = max<uint16_t>(FRAME_MS, (uint32_t)nLeds * 30 / 1000 + 3);
  }
  fitWalls();
}
void defaultGeo() {
  for (uint8_t w = 0; w < NWALL; w++) { WSTART[w] = DEF_START[w]; WLEN[w] = DEF_LEN[w]; }
  setStripLength(DEF_LEDS);
}

/* ── lit mask: one byte per pixel, rebuilt only when walls / ranges / layout change.
      Exact edges — the LEDs you pick are the LEDs that light, no fades eating into them. */
void buildZoneGain() {
  zonesDirty = false;
  memset(zoneGain, 0, sizeof(zoneGain));            // LEDs outside every wall stay dark
  zonesMasked = geoGaps;
  for (uint8_t w = 0; w < NWALL; w++) {
    if (!WLEN[w]) continue;
    if (!(S.walls & (1 << w))) { zonesMasked = true; continue; }
    uint16_t last = WLEN[w] - 1;
    uint16_t a = min<uint16_t>(S.lo[w], last);
    uint16_t b = (S.hi[w] >= last) ? last : S.hi[w];
    if (a > b) a = b;
    if (a != 0 || b != last) zonesMasked = true;
    memset(zoneGain + WSTART[w] + a, 255, b - a + 1);
  }
}
/* internal metronome for the BPM effects (no mic needed) */
bool internalBeat() {
  uint32_t per = 60000UL / bpmOf(60, 175);
  if (millis() - beatAt >= per) { beatAt = millis(); return true; }
  return false;
}
/* Live purely means "frames are arriving".  Whether it sounds like music
   only scales the output — it can never silence the strip.               */
bool audioLive() { return S.amode && (millis() - aLast < AUDIO_TIMEOUT); }

/* 0.45 … 1.0 — quiet room gets a calmer room, music gets full swing */
static inline uint8_t musicGain(uint8_t v) {
  uint16_t g = 115 + (uint16_t)aConf / 2;        // 115..242 of 255
  return scale8(v, (uint8_t)min<uint16_t>(255, g));
}

/* ═══════════════════════ AMBIENT EFFECTS ═══════════════════════ */
void fxSolid()   { fill_solid(leds, NUM_LEDS, C1()); }

void fxBreathe() { uint8_t v = beatsin8(bpmOf(3, 30), 12, 255);
                   CRGB c = C1(); c.nscale8_video(gam(v)); fill_solid(leds, NUM_LEDS, c); }

void fxGradient(){ uint8_t off = (millis() / (1 + (266 - S.speed) / 6)) & 0xFF;
                   for (uint16_t i = 0; i < NUM_LEDS; i++)
                     leds[i] = blend(C1(), C2(), sin8((uint8_t)((uint32_t)i * 255 / NUM_LEDS) + off)); }

void fxCandle()  { uint32_t t = millis();
                   uint8_t g = 150 + (inoise8(t * 3) >> 1);
                   for (uint16_t i = 0; i < NUM_LEDS; i++) {
                     uint8_t v = qadd8(scale8(inoise8(i * 22, t * 4), 70), 120);
                     leds[i] = CRGB(255, 118, 18); leds[i].nscale8_video(scale8(v, g)); } }

void fxFire() {
  uint8_t cool = map(S.inten, 1, 255, 8, 40), spark = map(S.speed, 1, 255, 60, 200);
  for (uint16_t i = 0; i < NUM_LEDS; i++) heat[i] = qsub8(heat[i], random8(0, cool / 4 + 2));
  for (uint16_t k = NUM_LEDS - 1; k >= 2; k--) heat[k] = (heat[k-1] + heat[k-2] + heat[k-2]) / 3;
  for (uint8_t w = 0; w < NWALL; w++)
    if (random8() < spark / 3) { uint16_t y = WSTART[w] + random8(14); heat[y] = qadd8(heat[y], random8(170, 255)); }
  for (uint16_t j = 0; j < NUM_LEDS; j++) leds[j] = HeatColor(heat[j]);
}

/* Sunrise/Sunset length: 45 s at Speed 128, 8 s fastest, 180 s slowest */
static uint32_t sunDur() {
  uint32_t d = 45000UL * 128UL / (S.speed ? S.speed : 1);
  return constrain(d, 8000UL, 180000UL);
}
/* FIXED v1: was 15-30 min and started at brightness 6 — looked broken */
void fxSunrise() {
  static const CRGBPalette16 sp(CRGB(20,0,0), CRGB(140,20,0), CRGB(255,95,0), CRGB(255,210,140));
  uint32_t dur = sunDur();                       // 45 s at default speed
  uint32_t el  = millis() - fxStart;
  uint8_t  p   = (el >= dur) ? 255 : (uint8_t)((uint64_t)el * 255 / dur);
  CRGB c = ColorFromPalette(sp, p, 255, LINEARBLEND);
  c.nscale8_video(qadd8(ease8InOutQuad(p), 14));                     // never fully black
  fill_solid(leds, NUM_LEDS, c);
  if (el < dur / 3)                                                  // soft shimmer at dawn
    for (uint16_t i = 0; i < NUM_LEDS; i++)
      leds[i].nscale8_video(200 + (inoise8(i * 9, millis() / 3) >> 2));
}
void fxSunset() {
  static const CRGBPalette16 sp(CRGB(20,0,0), CRGB(140,20,0), CRGB(255,95,0), CRGB(255,210,140));
  uint32_t dur = sunDur();
  uint32_t el  = millis() - fxStart;
  uint8_t  p   = (el >= dur) ? 0 : 255 - (uint8_t)((uint64_t)el * 255 / dur);
  CRGB c = ColorFromPalette(sp, p, 255, LINEARBLEND);
  c.nscale8_video(ease8InOutQuad(p));
  fill_solid(leds, NUM_LEDS, c);
}

void fxDrift()   { fill_solid(leds, NUM_LEDS, pcol((millis() / (1 + (266 - S.speed) / 8)) & 0xFF)); }

/* one full rainbow wrapped around the whole room, rotating */
void fxRainbowRoom() { for (uint16_t i = 0; i < NUM_LEDS; i++)
                         leds[i] = CHSV(hue8 + (uint8_t)((uint32_t)i * 255 / NUM_LEDS), 255, 255); }

void fxRainChase()   { uint8_t d = map(S.inten, 1, 255, 1, 9);
                       for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i] = CHSV(hue8 + i * d, 255, 255); }

void fxComet()   { fadeToBlackBy(leds, NUM_LEDS, map(S.inten, 1, 255, 6, 44));
                   uint8_t len = map(S.inten, 1, 255, 10, 60);
                   for (uint8_t i = 0; i < len; i++)
                     leds[wrapIdx(pos16 + i)] += pcol(hue8 + i * 3, 255 - i * (200 / len)); }

/* two comets launched from opposite corners, closing on each other */
void fxDualComet() {
  fadeToBlackBy(leds, NUM_LEDS, map(S.inten, 1, 255, 6, 44));
  uint8_t len = map(S.inten, 1, 255, 8, 40);
  for (uint8_t i = 0; i < len; i++) {
    uint8_t f = 255 - i * (200 / len);
    /* PAIR corners lead; the other two only join in if switched on */
    if (S.corners & (1 << PAIR_A)) leds[wrapIdx(CORNER[PAIR_A] + pos16 + i)] += pcol(hue8 + i * 3, f);
    if (S.corners & (1 << PAIR_B)) leds[wrapIdx(CORNER[PAIR_B] - pos16 - i)] += pcol(hue8 + 128 + i * 3, f);
    if (S.corners & 0b0001)        leds[wrapIdx(CORNER[0] + pos16 + i)] += pcol(hue8 + 64 + i * 3, f);
    if (S.corners & 0b0100)        leds[wrapIdx(CORNER[2] - pos16 - i)] += pcol(hue8 + 192 + i * 3, f);
  }
}

/* rings expanding outward from every selected corner */
void fxCornerBurst() {
  fadeToBlackBy(leds, NUM_LEDS, 26);
  uint16_t r = (millis() / (2 + (266 - S.speed) / 10)) % (NUM_LEDS / 2);
  uint8_t  w = map(S.inten, 1, 255, 3, 26);
  for (uint8_t c = 0; c < NWALL; c++) {
    if (!(S.corners & (1 << c))) continue;
    CRGB col = (S.colorMode == 1) ? (c & 1 ? C2() : C1()) : pcol(hue8 + c * 64);
    for (int8_t k = -w; k <= w; k++) {
      uint8_t fade = 255 - abs(k) * (220 / (w + 1));
      leds[wrapIdx(CORNER[c] + r + k)] += col.scale8(fade);
      leds[wrapIdx(CORNER[c] - r - k)] += col.scale8(fade);
    }
  }
}

/* runners leave each corner together and race around the perimeter */
void fxCornerRace() {
  fadeToBlackBy(leds, NUM_LEDS, 30);
  uint8_t len = 14;
  for (uint8_t c = 0; c < NWALL; c++) {
    if (!(S.corners & (1 << c))) continue;
    CRGB col = (S.colorMode == 1) ? (c & 1 ? C2() : C3()) : pcol(hue8 + c * 64);
    for (uint8_t i = 0; i < len; i++)
      leds[wrapIdx(CORNER[c] + pos16 + i)] += col.scale8(255 - i * (220 / len));
  }
}

/* corners flash one after another, chasing around the room */
void fxCornerFlash() {
  fadeToBlackBy(leds, NUM_LEDS, 40);
  uint8_t step = (millis() / (60 + (266 - S.speed))) % NWALL;
  uint8_t w = map(S.inten, 1, 255, 6, 46);
  if (!(S.corners & (1 << step))) return;
  CRGB col = (S.colorMode == 1) ? C1() : pcol(hue8 + step * 64);
  for (int8_t k = -w; k <= w; k++)
    leds[wrapIdx(CORNER[step] + k)] += col.scale8(255 - abs(k) * (230 / (w + 1)));
}

/* fills the room wall by wall, then empties it again */
void fxWallWipe() {
  uint32_t per = map(S.speed, 1, 255, 1400, 140);
  uint8_t  ph  = (millis() / per) % (NWALL * 2);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t w = 0; w < NWALL; w++) {
    bool on = (ph < NWALL) ? (w <= ph) : (w > (ph - NWALL));
    if (on) fillWall(w, (S.colorMode == 1) ? (w & 1 ? C2() : C1()) : pcol(hue8 + w * 64));
  }
}
/* one wall lit at a time, hopping around the ceiling */
void fxWallChase() {
  uint32_t per = map(S.speed, 1, 255, 1200, 90);
  uint8_t  w   = (millis() / per) % NWALL;
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  fillWall(w, (S.colorMode == 1) ? C1() : pcol(hue8 + w * 64));
}
/* all four walls breathe, 90° out of phase — very calm, very good */
void fxWallPulse() {
  uint8_t bpm = bpmOf(4, 40);
  for (uint8_t w = 0; w < NWALL; w++) {
    uint8_t v = beatsin8(bpm, 20, 255, 0, w * 64);
    CRGB col = (S.colorMode == 1) ? (w & 1 ? C2() : C1()) : pcol(hue8 + w * 40);
    col.nscale8_video(gam(v));
    fillWall(w, col);
  }
}
/* POLICE — opposite walls slam red / blue against each other */
void fxPoliceWalls() {
  uint32_t per = map(S.speed, 1, 255, 900, 90);
  uint8_t  ph  = (millis() / (per / 2)) % 4;
  const CRGB A(255, 0, 20);                      // police is always red / blue,
  const CRGB B(0, 40, 255);                      // whatever colours or palette are set
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  switch (ph) {
    case 0: fillWall(0, A); fillWall(2, A); break;
    case 1: fill_solid(leds, NUM_LEDS, CRGB::Black); break;
    case 2: fillWall(1, B); fillWall(3, B); break;
    default: break;
  }
}

void fxTheater()  { uint8_t step = (millis() / (4 + (266 - S.speed) / 4)) % 3;
                    for (uint16_t i = 0; i < NUM_LEDS; i++)
                      leds[i] = (i % 3 == step) ? pcol(hue8 + i) : CRGB::Black; }

void fxScanner()  { fadeToBlackBy(leds, NUM_LEDS, map(S.inten, 1, 255, 8, 60));
                    uint16_t p = beatsin16(bpmOf(3, 34), 0, NUM_LEDS - 1);
                    uint8_t  w = map(S.inten, 1, 255, 4, 30);
                    for (uint8_t i = 0; i < w; i++) leds[wrapIdx(p - w / 2 + i)] += C1(); }

void fxMeteor()   { for (uint16_t i = 0; i < NUM_LEDS; i++) if (random8() < 96) leds[i].fadeToBlackBy(random8(30, 80));
                    for (uint8_t i = 0; i < 16; i++) leds[wrapIdx(pos16 + i)] = pcol(hue8); }

void fxRunning()  { uint8_t off = (millis() / (2 + (266 - S.speed) / 6)) & 0xFF;
                    uint8_t d = map(S.inten, 1, 255, 2, 14);
                    for (uint16_t i = 0; i < NUM_LEDS; i++)
                      leds[i] = pcol(hue8 + i / 4, gam(sin8(i * d + off))); }

void fxPlasma()   { uint32_t t = millis() * (1 + S.speed / 24);
                    for (uint16_t i = 0; i < NUM_LEDS; i++)
                      leds[i] = pcol(inoise8(i * map(S.inten,1,255,4,30), t / 12) + hue8); }

void fxTwinkle()  { fadeToBlackBy(leds, NUM_LEDS, map(S.speed, 1, 255, 2, 24));
                    uint8_t n = map(S.inten, 1, 255, 1, 24);
                    for (uint8_t i = 0; i < n; i++)
                      if (random8() < 130) leds[random16(NUM_LEDS)] = pcol(random8()); }

void fxSparkle()  { fill_solid(leds, NUM_LEDS, CRGB::Black);
                    uint8_t n = map(S.inten, 1, 255, 3, 70);
                    for (uint8_t i = 0; i < n; i++) leds[random16(NUM_LEDS)] = CRGB::White; }

/* bursts detonate at random points, biased to the corners */
void fxFireworks() {
  fadeToBlackBy(leds, NUM_LEDS, map(S.speed, 1, 255, 3, 22));
  if (random8() < map(S.inten, 1, 255, 3, 40)) {
    uint16_t c = (random8() < 140) ? CORNER[random8(NWALL)] : random16(NUM_LEDS);
    CRGB col = pcol(random8());
    uint8_t w = random8(10, 40);
    for (uint8_t i = 0; i < w; i++) {
      uint8_t f = 255 - i * (230 / w);
      leds[wrapIdx(c + i)] += col.scale8(f);
      leds[wrapIdx(c - i)] += col.scale8(f);
    }
  }
}

void fxStrobe()   { uint16_t per = map(S.speed, 1, 255, 500, 45);
                    bool on = (millis() % per) < (per / map(S.inten, 1, 255, 8, 2));
                    fill_solid(leds, NUM_LEDS, on ? C1() : CRGB::Black); }

void fxLightning(){ fadeToBlackBy(leds, NUM_LEDS, 70);
                    if (random8() < map(S.inten, 1, 255, 2, 26)) {
                      uint8_t w = random8(NWALL);
                      fillWall(w, CRGB::White);           // a whole wall flashes
                    } }

/* drops land and ripple outward, reflecting round the loop */
void fxRipple() {
  fadeToBlackBy(leds, NUM_LEDS, 18);
  static uint16_t centre = 0; static uint16_t r = 0;
  if (r == 0 || r > NUM_LEDS / 2) { centre = random16(NUM_LEDS); r = 1; hue8 += 40; }
  r += 1 + S.speed / 40;
  uint8_t f = 255 - (uint32_t)r * 255 / (NUM_LEDS / 2);
  CRGB col = pcol(hue8, f);
  leds[wrapIdx(centre + r)] += col;
  leds[wrapIdx(centre - r)] += col;
  leds[wrapIdx(centre + r / 2)] += col.scale8(90);
  leds[wrapIdx(centre - r / 2)] += col.scale8(90);
}

/* a colour wheel rotating around the room like a lighthouse */
void fxSpiral() {
  uint8_t off = (millis() / (1 + (266 - S.speed) / 5)) & 0xFF;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint8_t a = (uint8_t)((uint32_t)i * 255 / NUM_LEDS) + off;
    leds[i] = pcol(a, gam(sin8(a * 2)));
  }
}

void fxAurora() { uint32_t t = millis() / 8;
                  for (uint16_t i = 0; i < NUM_LEDS; i++) {
                    uint8_t n1 = inoise8(i * 5, t);
                    uint8_t n2 = inoise8(i * 3 + 9000, t / 2);
                    leds[i] = pcol(n1 + hue8, gam(qadd8(n2, 40))); } }

/* rain falling down each wall independently */
void fxMatrix() {
  fadeToBlackBy(leds, NUM_LEDS, map(S.speed, 1, 255, 8, 50));
  for (uint8_t w = 0; w < NWALL; w++)
    if (random8() < map(S.inten, 1, 255, 10, 90))
      leds[WSTART[w] + random16(WLEN[w])] = (S.colorMode == 1) ? C1() : pcol(hue8 + w * 40);
}

/* lub-dub, lub-dub — the whole room */
void fxHeartbeat() {
  uint32_t per = 60000UL / bpmOf(40, 120);
  uint32_t t = millis() % per;
  uint8_t v = 0;
  if      (t < 130)               v = 255 - (t * 255 / 130);
  else if (t > 210 && t < 330)    v = 200 - ((t - 210) * 200 / 120);
  CRGB c = C1(); c.nscale8_video(gam(qadd8(v, 12)));
  fill_solid(leds, NUM_LEDS, c);
}

/* ── BPM effects: musical-looking WITHOUT a microphone ── */
void fxDiscoBPM() {
  fadeToBlackBy(leds, NUM_LEDS, 40);
  if (internalBeat()) { hue8 += 53; partySub = random8(NWALL); }
  uint32_t per = 60000UL / bpmOf(60, 175);
  uint8_t  v = 255 - min<uint32_t>(255, (millis() - beatAt) * 255 / (per / 2));
  CRGB col = (S.colorMode == 1) ? C1() : pcol(hue8);
  col.nscale8_video(gam(v));
  fillWall(partySub, col);
  fillWall((partySub + 2) % NWALL, col);
}
void fxRaveBPM() {
  if (internalBeat()) hue8 += 37;
  uint32_t per = 60000UL / bpmOf(70, 180);
  uint32_t t = millis() - beatAt;
  if (t < 60) fill_solid(leds, NUM_LEDS, CRGB::White);
  else {
    fadeToBlackBy(leds, NUM_LEDS, 60);
    uint8_t v = 255 - min<uint32_t>(255, t * 255 / per);
    for (uint16_t i = 0; i < NUM_LEDS; i++)
      if ((i / 12 + (t / 40)) % 3 == 0) leds[i] = pcol(hue8 + i / 8, gam(v));
  }
}
void fxBeatWallsBPM() {
  static uint8_t w = 0;
  if (internalBeat()) { w = (w + 1) % NWALL; hue8 += 41; }
  fadeToBlackBy(leds, NUM_LEDS, 45);
  uint32_t per = 60000UL / bpmOf(60, 175);
  uint8_t v = 255 - min<uint32_t>(255, (millis() - beatAt) * 255 / per);
  CRGB col = (S.colorMode == 1) ? (w & 1 ? C2() : C1()) : pcol(hue8);
  col.nscale8_video(gam(v));
  fillWall(w, col);
}

/* ═══════════════════════ PARTY MODE ═══════════════════════
   Auto-mixes aggressive patterns. Locks to real beats if the mic is
   running, otherwise falls back to the internal metronome.          */
/* the new colour is injected at one point on the strip and sweeps outward,
   so the room re-colours like a wave instead of snapping all at once        */
uint16_t partyOrigin = 0, partyR = 0;
CRGB partyOld = CRGB::Black, partyNew = CRGB::Black;
bool  partyFlip = false;

static CRGB partyColAt(uint16_t i, uint8_t v) {
  uint16_t d = loopDist(i, partyOrigin);
  CRGB c;
  if (d + 30 < partyR)      c = partyNew;                       // swept
  else if (d > partyR)      c = partyOld;                       // not yet
  else                      c = blend(partyOld, partyNew,       // soft edge
                                      255 - (uint8_t)((uint32_t)(d + 30 - partyR) * 255 / 30));
  c.nscale8_video(gam(v));
  return c;
}

void fxParty() {
  bool beat = audioLive() ? samplePeak : internalBeat();
  if (beat) {
    aBeat = false;
    hue8 += 43;
    partyOld = partyNew;
    partyNew = pcol(hue8);
    partyFlip = !partyFlip;
    partyOrigin = CORNER[partyFlip ? PAIR_A : PAIR_B];   // alternate launch corners
    partyR = 0;
    if (random8() < (S.smooth ? 40 : 70)) partySub = random8(S.smooth ? 5 : 6);
  }
  partyR += 6 + S.speed / 10;                            // wavefront speed
  uint32_t per = audioLive() ? 500 : (60000UL / bpmOf(70, 180));
  uint8_t  dk  = 255 - min<uint32_t>(255, (millis() - beatAt) * 255 / per);
  uint8_t  lvl = audioLive() ? max<uint8_t>((uint8_t)volumeSmth, dk) : dk;
  if (S.smooth && lvl >= 8) lvl = qadd8(scale8(lvl, 200), BRI_SOFT);   // soften, but 0 stays 0

  switch (partySub) {
    case 0: {                                             // wall hit, colour swept
      fadeToBlackBy(leds, NUM_LEDS, S.smooth ? 26 : 50);
      uint8_t w = partyFlip ? PAIR_A : PAIR_B;
      for (uint16_t i = 0; i < WLEN[w]; i++) {
        uint16_t x = WSTART[w] + i; leds[x] = partyColAt(x, lvl); }
      break; }
    case 1:                                               // alternating walls
      for (uint8_t w = 0; w < NWALL; w++)
        for (uint16_t i = 0; i < WLEN[w]; i++) {
          uint16_t x = WSTART[w] + i;
          leds[x] = partyColAt(x, (w & 1) ? scale8(lvl, 120) : lvl); }
      break;
    case 2:                                               // corner plumes
      fadeToBlackBy(leds, NUM_LEDS, S.smooth ? 24 : 44);
      for (uint8_t c = 0; c < NWALL; c++) {
        if (!(S.corners & (1 << c))) continue;
        for (uint8_t k = 0; k < 40; k++) {
          uint16_t x = wrapIdx(CORNER[c] + k);
          leds[x] += partyColAt(x, scale8(lvl, 255 - k * 6)); }
      }
      break;
    case 3:                                               // travelling dashes
      fadeToBlackBy(leds, NUM_LEDS, S.smooth ? 32 : 60);
      for (uint16_t i = 0; i < NUM_LEDS; i += 7) {
        uint16_t x = wrapIdx(i + pos16); leds[x] = partyColAt(x, lvl); }
      break;
    default:                                              // full sweep
      for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i] = partyColAt(i, lvl);
      break;
  }
  if (!S.smooth && partySub == 5) {                       // the wild one, opt-in
    fadeToBlackBy(leds, NUM_LEDS, 70);
    for (uint8_t i = 0; i < 40; i++) {
      uint16_t x = random16(NUM_LEDS); leds[x] = partyColAt(x, lvl); }
  }
}

/* ═══════════════════════════════════════════════════════════════════════
   AUDIO-REACTIVE EFFECTS
   Ported from WLED (wled00/FX.cpp) — original routines by Andrew Tuline,
   Andreas Pleschung, Will Tatam and the WLED contributors.
   Adapted to: one 821-pixel loop, FastLED primitives, our palette system.
   ═══════════════════════════════════════════════════════════════════════ */

/* WLED's fade_out(r): higher r = slower fade */
static inline void fadeOutW(uint8_t r) { fadeToBlackBy(leds, NUM_LEDS, 255 - r); }
/* color_blend(black, col, amt) collapses to a video-scale */
static inline CRGB blendB(const CRGB &c, uint8_t amt) { CRGB o = c; o.nscale8_video(amt); return o; }
static inline uint8_t gamma8inv(uint8_t b) { return (uint8_t)(sqrt16((uint16_t)b * 255)); }

/* shift the whole strip one pixel outward from the centre (Pixelwave / DJ Light / Freqwave) */
static void shiftOutFromCentre() {
  const uint16_t mid = NUM_LEDS / 2;
  for (uint16_t i = NUM_LEDS - 1; i > mid; i--) leds[i] = leds[i - 1];
  for (uint16_t i = 0; i < mid; i++)            leds[i] = leds[i + 1];
}
/* shift the whole strip one pixel left (Matripix / Waterfall) */
static void shiftLeft() {
  for (uint16_t i = 0; i < NUM_LEDS - 1; i++) leds[i] = leds[i + 1];
}
/* WLED drives these off micros(); same idea, framerate-independent */
static bool tick(uint8_t &store, uint8_t div) {
  uint8_t h = (uint8_t)((micros() / ((256 - S.speed) * 500UL)) % div);
  if (store != h) { store = h; return true; }
  return false;
}

/* ── GRAVITY FAMILY — Gravcenter / Gravcentric / Gravimeter / Gravfreq ── */
static int16_t gravTop = -1;
static uint8_t gravCnt = 0;
static void amGravBase(uint8_t mode) {
  fadeOutW(251);
  float segAvg = volumeSmth * (float)S.inten / 255.0f;
  int tempsamp;
  if (mode == 2) {                                   // Gravimeter
    segAvg *= 0.25f;
    tempsamp = constrain((int)(segAvg * 2.0f * (NUM_LEDS - 1) / 64.0f), 0, NUM_LEDS - 1);
  } else {
    segAvg *= 0.125f;
    tempsamp = constrain((int)(segAvg * 2.0f * (NUM_LEDS / 2) / 32.0f), 0, NUM_LEDS / 2);
  }
  uint8_t gravity = 8 - S.speed / 32;
  int offset = (mode == 2) ? 0 : 1;
  if (tempsamp >= gravTop + offset) gravTop = tempsamp - offset;
  else if (gravity && gravCnt % gravity == 0) gravTop--;
  const uint16_t mid = NUM_LEDS / 2;

  if (mode == 1) {                                   // Gravcentric
    for (int i = 0; i < tempsamp; i++) {
      uint8_t idx = (uint8_t)(segAvg * 24) + (millis() / 200);
      leds[mid + i] = pcol(idx); leds[mid - 1 - i] = pcol(idx);
    }
    if (gravTop >= 0 && gravTop < (int)mid) {
      leds[mid + gravTop] = CRGB(128,128,128); leds[mid - 1 - gravTop] = CRGB(128,128,128); }
  } else if (mode == 2) {                            // Gravimeter
    for (int i = 0; i < tempsamp; i++) {
      uint8_t idx = inoise8(i * (uint8_t)segAvg + millis(), 5000 + i * (uint8_t)segAvg);
      leds[i] = blendB(pcol(idx), (uint8_t)min(255.0f, segAvg * 8));
    }
    if (gravTop > 0 && gravTop < NUM_LEDS) leds[gravTop] = pcol(millis());
  } else if (mode == 3) {                            // Gravfreq
    uint8_t idx = (uint8_t)((log10f(max(1.0f, FFT_MajorPeak)) - (MAX_FREQ_LOG10 - 1.78f)) * 255);
    for (int i = 0; i < tempsamp; i++) { leds[mid + i] = pcol(idx); leds[mid - 1 - i] = pcol(idx); }
    if (gravTop >= 0 && gravTop < (int)mid) {
      leds[mid + gravTop] = CRGB(128,128,128); leds[mid - 1 - gravTop] = CRGB(128,128,128); }
  } else {                                           // Gravcenter
    for (int i = 0; i < tempsamp; i++) {
      uint8_t idx = inoise8(i * (uint8_t)segAvg + millis(), 5000 + i * (uint8_t)segAvg);
      CRGB c = blendB(pcol(idx), (uint8_t)min(255.0f, segAvg * 8));
      leds[mid + i] = c; leds[mid - 1 - i] = c;
    }
    if (gravTop >= 0 && gravTop < (int)mid) {
      leds[mid + gravTop] = pcol(millis()); leds[mid - 1 - gravTop] = pcol(millis()); }
  }
  gravCnt = (gravCnt + 1) % (gravity ? gravity : 1);
}

/* ── JUGGLES ── */
static void amJuggles() {
  fadeOutW(224);
  uint8_t agc = (uint8_t)constrain(volumeSmth, 0.0f, 255.0f);
  for (uint8_t i = 0; i < S.inten / 32 + 1; i++)
    leds[beatsin16(S.speed / 4 + i * 2, 0, NUM_LEDS - 1)] =
      blendB(pcol(millis() / 4 + i * 2), agc);
}

/* ── MATRIPIX ── */
static uint8_t tkMatri = 0;
static void amMatripix() {
  if (tick(tkMatri, 16)) {
    int pixBri = volumeRaw * S.inten / 64;
    shiftLeft();
    leds[NUM_LEDS - 1] = blendB(pcol(millis()), (uint8_t)constrain(pixBri, 0, 255));
  }
}

/* ── MIDNOISE ── */
static uint16_t nzX = 0, nzY = 0;
static void amMidnoise() {
  fadeOutW(S.speed); fadeOutW(S.speed);
  float t = volumeSmth * (float)S.inten / 256.0f;
  t *= (float)S.inten / 128.0f;
  uint16_t maxLen = (uint16_t)(t * (NUM_LEDS / 2) / 127.0f);
  if (maxLen > NUM_LEDS / 2) maxLen = NUM_LEDS / 2;
  for (uint16_t i = NUM_LEDS / 2 - maxLen; i < NUM_LEDS / 2 + maxLen; i++)
    leds[i] = pcol(inoise8(i * (uint8_t)volumeSmth + nzX, nzY + i * (uint8_t)volumeSmth));
  nzX += beatsin8(5, 0, 10); nzY += beatsin8(4, 0, 10);
}

/* ── NOISEFIRE — volume-reactive fire ── */
static void amNoisefire() {
  static const CRGBPalette16 firePal(
    CHSV(0,255,20), CHSV(0,255,30), CHSV(0,255,40), CHSV(0,255,44),
    CHSV(0,255,64), CRGB::Red, CRGB::Red, CRGB::Red,
    CRGB::DarkOrange, CRGB::DarkOrange, CRGB::Orange, CRGB::Orange,
    CRGB::Yellow, CRGB::Orange, CRGB::Yellow, CRGB::Yellow);
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint16_t idx = inoise8(i * S.speed / 64, millis() * S.speed / 64 * NUM_LEDS / 255);
    idx = (255 - i * 256 / NUM_LEDS) * idx / (256 - S.inten);
    leds[i] = ColorFromPalette(firePal, (uint8_t)idx,
                               (uint8_t)min(255.0f, volumeSmth * 2), LINEARBLEND);
  }
}

/* ── NOISEMETER ── */
static void amNoisemeter() {
  fadeOutW(map(S.speed, 0, 255, 200, 254));
  float t = volumeRaw * 2.0f * (float)S.inten / 255.0f;
  uint16_t maxLen = (uint16_t)(t * NUM_LEDS / 255.0f);
  if (maxLen > NUM_LEDS) maxLen = NUM_LEDS;
  for (uint16_t i = 0; i < maxLen; i++)
    leds[i] = pcol(inoise8(i * (uint8_t)volumeSmth + nzX, nzY + i * (uint8_t)volumeSmth));
  nzX += beatsin8(5, 0, 10); nzY += beatsin8(4, 0, 10);
}

/* ── PIXELWAVE ── */
static uint8_t tkPixw = 0;
static void amPixelwave() {
  if (tick(tkPixw, 16)) {
    uint8_t pixBri = (uint8_t)constrain(volumeRaw * S.inten / 64, 0, 255);
    leds[NUM_LEDS / 2] = blendB(pcol(millis()), pixBri);
    shiftOutFromCentre();
  }
}

/* ── PLASMOID ── */
static int16_t phA = 0, phB = 0;
static void amPlasmoid() {
  fadeToBlackBy(leds, NUM_LEDS, 32);
  phA += beatsin8(6, 0, 8) - 4;
  phB += beatsin8(7, 0, 8) - 4;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint8_t b = cubicwave8(((i * (1 + (3 * S.speed / 32))) + phA) & 0xFF) / 2;
    b += cos8(((i * (97 + (5 * S.speed / 32))) + phB) & 0xFF) / 2;
    uint8_t ci = b;
    if (volumeSmth * S.inten / 64 < b) b = 0;
    leds[i] += blendB(pcol(ci), b);
  }
}

/* ── PUDDLES / PUDDLEPEAK ── */
static void amPuddlesBase(bool peakMode) {
  uint16_t size = 0;
  fadeOutW(map(S.speed, 0, 255, 224, 254));
  uint16_t pos = random16(NUM_LEDS);
  if (peakMode) {
    if (samplePeak) { size = (uint16_t)(volumeSmth * S.inten / 256 / 4) + 1;
                      if (pos + size >= NUM_LEDS) size = NUM_LEDS - pos; }
  } else {
    if (volumeRaw > 1) { size = (uint16_t)(volumeRaw * S.inten / 256 / 8) + 1;
                         if (pos + size >= NUM_LEDS) size = NUM_LEDS - pos; }
  }
  for (uint16_t i = 0; i < size; i++) leds[pos + i] = pcol(millis());
}

/* ── RIPPLE PEAK ── */
#define MAXSTEPS 16
struct Ripple { uint8_t state; uint8_t color; uint16_t pos; };
static Ripple ripples[16];
static void amRipplePeak() {
  fadeOutW(240); fadeOutW(240);
  for (uint8_t i = 0; i < S.inten / 16 && i < 16; i++) {
    if (samplePeak) ripples[i].state = 255;
    switch (ripples[i].state) {
      case 254: break;
      case 255:
        ripples[i].pos = random16(NUM_LEDS);
        ripples[i].color = (FFT_MajorPeak > 1) ? (uint8_t)(log10f(FFT_MajorPeak) * 128) : 0;
        ripples[i].state = 0;
        break;
      case 0:
        leds[ripples[i].pos] = pcol(ripples[i].color);
        ripples[i].state++;
        break;
      case MAXSTEPS: ripples[i].state = 254; break;
      default: {
        uint8_t amt = (uint8_t)(2 * 255 / ripples[i].state);
        leds[wrapIdx(ripples[i].pos + ripples[i].state)] = blendB(pcol(ripples[i].color), amt);
        leds[wrapIdx(ripples[i].pos - ripples[i].state)] = blendB(pcol(ripples[i].color), amt);
        ripples[i].state++;
      } break;
    }
  }
}

/* ── BLURZ ── */
static uint8_t blurzBin = 0;
static uint32_t blurzStep = 0;
static void amBlurz() {
  int fd = (256 - S.speed) / 32;
  static uint32_t call = 0; call++;
  if (fd <= 1 || (call % fd) == 0) fadeOutW(S.speed);
  blurzStep += frameMs;
  if (blurzStep > 20) {
    uint16_t loc = random16(NUM_LEDS);
    uint8_t v = fftResult[blurzBin % 16];
    leds[loc] = blendB(pcol((uint8_t)(2 * v * 240 / max(1, NUM_LEDS - 1))), (uint8_t)min(255, 2 * v));
    blurzBin = (blurzBin + 1) % 16;
    blurzStep = 1;
    blur1d(leds, NUM_LEDS, S.inten);
  }
}

/* ── DJ LIGHT ── */
static uint8_t tkDj = 0;
static void amDJLight() {
  if (tick(tkDj, 64)) {
    CRGB c = CRGB(gamma8inv(fftResult[15] / 2), gamma8inv(fftResult[5] / 2), gamma8inv(fftResult[0] / 2));
    leds[NUM_LEDS / 2] = c.fadeToBlackBy(map(fftResult[4], 0, 255, 255, 4));
    shiftOutFromCentre();
  }
}

/* ── FREQMAP ── */
static void amFreqmap() {
  static uint32_t call = 0; call++;
  int fd = (256 - S.speed) / 32;
  if (fd <= 1 || (call % fd) == 0) fadeOutW(S.speed);
  float mp = max(1.0f, FFT_MajorPeak);
  int locn = (int)((log10f(mp) - 1.78f) * (float)NUM_LEDS / (MAX_FREQ_LOG10 - 1.78f));
  locn = constrain(locn, 0, NUM_LEDS - 1);
  uint8_t pixCol = (mp < 61.0f) ? 0 : (uint8_t)((log10f(mp) - 1.78f) * 255.0f / (MAX_FREQ_LOG10 - 1.78f));
  leds[locn] = blendB(pcol(S.inten + pixCol), (uint8_t)constrain(my_magnitude / 4.0f, 0.0f, 255.0f));
}

/* ── FREQMATRIX ── */
static uint8_t tkFm = 0;
static void amFreqmatrix() {
  if (tick(tkFm, 16)) {
    float sensitivity = 1.0f + (float)S.inten / 64.0f;
    float pixVal = min(255.0f, volumeSmth * sensitivity);
    CRGB color = CRGB::Black;
    if (FFT_MajorPeak >= 80.0f) {
      uint8_t i = (uint8_t)map((long)constrain(FFT_MajorPeak, 80.0f, 5000.0f), 80, 5000, 0, 255);
      color = CHSV(i, 240, gamma8inv((uint8_t)pixVal));
    }
    leds[0] = color;
    for (uint16_t i = NUM_LEDS - 1; i > 0; i--) leds[i] = leds[i - 1];
  }
}

/* ── FREQWAVE ── */
static uint8_t tkFw = 0;
static void amFreqwave() {
  if (tick(tkFw, 16)) {
    float sensitivity = 1.0f + (float)S.inten / 48.0f;
    float pixVal = min(255.0f, volumeSmth * sensitivity);
    CRGB color = CRGB::Black;
    if (FFT_MajorPeak >= 80.0f) {
      uint8_t i = (uint8_t)map((long)constrain(FFT_MajorPeak, 80.0f, 5000.0f), 80, 5000, 0, 255);
      color = CHSV(i, 240, gamma8inv((uint8_t)pixVal));
    }
    leds[NUM_LEDS / 2] = color;
    shiftOutFromCentre();
  }
}

/* ── NOISEMOVE ── */
static void amNoisemove() {
  static uint32_t call = 0; call++;
  int fd = (256 - S.speed) / 96;
  if (fd <= 1 || (call % fd) == 0) fadeToBlackBy(leds, NUM_LEDS, 4 + S.speed / 4);
  uint8_t numBins = map(S.inten, 0, 255, 0, 16);
  for (uint8_t i = 0; i < numBins; i++) {
    uint16_t locn = inoise16(millis() * S.speed + i * 50000, millis() * S.speed);
    locn = map(constrain(locn, 7500, 58000), 7500, 58000, 0, NUM_LEDS - 1);
    leds[locn] = blendB(pcol(i * 64), (uint8_t)min(255, fftResult[i % 16] * 4));
  }
}

/* ── ROCKTAVES — same note in any octave gets the same colour ── */
static void amRocktaves() {
  fadeToBlackBy(leds, NUM_LEDS, 16);
  float frTemp = FFT_MajorPeak;
  uint8_t octCount = 0, volTemp = 0;
  float mag = my_magnitude / 16.0f;
  volTemp = (uint8_t)constrain(32.0f + mag * 1.5f, 0.0f, 255.0f);
  if (mag < 48) volTemp = 0;
  if (mag > 144) volTemp = 255;
  while (frTemp > 249) { octCount++; frTemp /= 2; }
  frTemp -= 132.0f;
  frTemp = fabsf(frTemp * 2.1f);
  uint16_t i = map(beatsin8(8 + octCount * 4, 0, 255, 0, octCount * 8), 0, 255, 0, NUM_LEDS - 1);
  leds[constrain(i, 0, NUM_LEDS - 1)] += blendB(pcol((uint8_t)frTemp), volTemp);
}

/* ── WATERFALL ── */
static uint8_t tkWf = 0;
static void amWaterfall() {
  if (tick(tkWf, 16)) {
    uint8_t pixCol = (FFT_MajorPeak < 182.0f) ? 0 : (uint8_t)((log10f(FFT_MajorPeak) - 2.26f) * 150);
    shiftLeft();
    if (samplePeak) leds[NUM_LEDS - 1] = CHSV(92, 92, gamma8inv(92));
    else leds[NUM_LEDS - 1] = blendB(pcol(pixCol + S.inten),
                                     (uint8_t)constrain(my_magnitude / 8.0f, 0.0f, 255.0f));
  }
}

/* ── BINMAP — every FFT bin mapped across the whole room ── */
static void amBinmap() {
  const uint16_t perBin = NUM_LEDS / 16;
  for (uint8_t b = 0; b < 16; b++) {
    uint8_t v = fftResult[b];
    for (uint16_t i = 0; i < perBin; i++)
      leds[b * perBin + i] = blendB(pcol((uint8_t)(b * 16 + hue8)), v);
  }
}

/* ── GEQ BARS — the classic 16-band spectrum wrapped round the room ── */
static void amGEQ() {
  for (uint8_t b = 0; b < 16; b++) {
    uint16_t a = (uint32_t)b * NUM_LEDS / 16, e = (uint32_t)(b + 1) * NUM_LEDS / 16;
    uint8_t v = fftResult[b];
    CRGB c = (S.colorMode == 1) ? ((b < 6) ? C1() : (b < 11) ? C2() : C3())
                                : (CRGB)CHSV(map(b, 0, 15, 0, 200), 245, 255);
    c.nscale8_video(gam(v));
    for (uint16_t i = a; i < e; i++) leds[i] = c;
  }
}
/* ═══════════════════════ ROOM EFFECTS (fw 11) ═══════════════════════
   Designed for a closed ceiling loop: they use the walls, the corners and the true
   angle around the room, and avoid a visible seam where the strip closes on itself.
   Pacifica and TwinkleFox are ported from the FastLED examples
   (MIT, © Mark Kriegsman / Mary Corey March); the rest are original to Lumo.        */

/* which wall a pixel belongs to (gaps → the wall before them) */
static uint8_t wallOf(uint16_t i) {
  for (uint8_t w = 0; w < NWALL; w++) if (WLEN[w] && i >= WSTART[w] && i < WSTART[w] + WLEN[w]) return w;
  uint8_t best = 0;
  for (uint8_t w = 0; w < NWALL; w++) if (i >= WSTART[w] && WSTART[w] >= WSTART[best]) best = w;
  return best;
}
/* angle around the room, 0..65535, corners at exactly 0 / ¼ / ½ / ¾ however long each wall is */
static uint16_t roomAng(uint16_t i) {
  uint8_t w = wallOf(i);
  uint16_t len = WLEN[w] ? WLEN[w] : 1;
  uint16_t off = (i >= WSTART[w]) ? min<uint16_t>(i - WSTART[w], len - 1) : 0;
  return (uint16_t)(w * 16384u + (uint32_t)off * 16384u / len);
}
/* 3-D noise sampled on a circle → seamless all the way round; cells = features per lap */
static uint16_t loopNoise(uint16_t a, uint16_t cells, uint32_t z) {
  int32_t R = (int32_t)cells * 41;
  return inoise16((uint32_t)((1L << 24) + (((int32_t)cos16(a) * R) >> 7)),
                  (uint32_t)((1L << 24) + (((int32_t)sin16(a) * R) >> 7)), z);
}
static inline uint8_t stretchN(uint16_t n) { int32_t v = ((int32_t)n - 12000) >> 7; return (uint8_t)constrain(v, 0, 255); }
/* the effect's two colours: your colours 1 / 2 in "my colours" mode, else the palette */
static inline CRGB colA() { return S.colorMode ? C1() : pcol(hue8); }
static inline CRGB colB() { return S.colorMode ? C2() : pcol(hue8 + 110); }
/* soft dot (raised cosine), wraps round the loop */
static void drawSoft(int32_t pos, uint8_t w, const CRGB &c) {
  if (!w) w = 1;
  for (int16_t d = -(int16_t)w; d <= (int16_t)w; d++) {
    uint8_t amp = cos8((uint8_t)((uint16_t)abs(d) * 128 / (w + 1)));
    CRGB k = c; k.nscale8(amp);
    leds[wrapIdx(pos + d)] += k;
  }
}

/* ── 38 PACIFICA — layered ocean swells with white caps (FastLED example, MIT) ── */
static const CRGBPalette16 pacP1(0x000507, 0x000409, 0x00030B, 0x00030D, 0x000210, 0x000212, 0x000114, 0x000117,
                                 0x000019, 0x00001C, 0x000026, 0x000031, 0x00003B, 0x000046, 0x14554B, 0x28AA50);
static const CRGBPalette16 pacP2(0x000507, 0x000409, 0x00030B, 0x00030D, 0x000210, 0x000212, 0x000114, 0x000117,
                                 0x000019, 0x00001C, 0x000026, 0x000031, 0x00003B, 0x000046, 0x0C5F52, 0x19BE5F);
static const CRGBPalette16 pacP3(0x000208, 0x00030E, 0x000514, 0x00061A, 0x000820, 0x000927, 0x000B2D, 0x000C33,
                                 0x000E39, 0x001040, 0x001450, 0x001860, 0x001C70, 0x002080, 0x1040BF, 0x2060FF);
static void pacLayer(const CRGBPalette16 &p, uint16_t cistart, uint16_t wavescale, uint8_t bri, uint16_t ioff) {
  uint16_t ci = cistart, waveangle = ioff, half = (wavescale / 2) + 20;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    waveangle += 250;
    uint16_t s16 = sin16(waveangle) + 32768;
    uint16_t cs = scale16(s16, half) + half;
    ci += cs;
    uint16_t sindex16 = sin16(ci) + 32768;
    leds[i] += ColorFromPalette(p, scale16(sindex16, 240), bri, LINEARBLEND);
  }
}
void fxPacifica() {
  static uint16_t c1, c2, c3, c4; static uint32_t last = 0;
  uint32_t now = millis(), dms = (now - last) * (S.speed + 32) / 160; last = now;
  if (dms > 200) dms = 200;
  uint32_t d1 = dms * beatsin16(3, 179, 269) / 256, d2 = dms * beatsin16(4, 179, 269) / 256, d21 = (d1 + d2) / 2;
  c1 += d1 * beatsin88(1011, 10, 13); c2 -= d21 * beatsin88(777, 8, 11);
  c3 -= d1 * beatsin88(501, 5, 7);    c4 -= d2 * beatsin88(257, 4, 6);
  fill_solid(leds, NUM_LEDS, CRGB(2, 6, 10));
  pacLayer(pacP1, c1, beatsin16(3, 11 * 256, 14 * 256), beatsin8(10, 70, 130), 0 - beat16(301));
  pacLayer(pacP2, c2, beatsin16(4, 6 * 256, 9 * 256), beatsin8(17, 40, 80), beat16(401));
  pacLayer(pacP3, c3, 6 * 256, beatsin8(9, 10, 38), 0 - beat16(503));
  pacLayer(pacP3, c4, 5 * 256, beatsin8(8, 10, 28), beat16(601));
  uint8_t base = beatsin8(9, 55, 65), wave = beat8(7);
  for (uint16_t i = 0; i < NUM_LEDS; i++) {                       // white caps where the layers pile up
    uint8_t th = scale8(sin8(wave), 20) + base; wave += 7;
    uint8_t l = leds[i].getAverageLight();
    if (l > th) { uint8_t o = l - th, o2 = qadd8(o, o); leds[i] += CRGB(o, o2, qadd8(o2, o2)); }
    leds[i].blue = scale8(leds[i].blue, 145); leds[i].green = scale8(leds[i].green, 200);
    leds[i] |= CRGB(2, 5, 7);
  }
}

/* ── 39 TWINKLEFOX — every LED its own slow fairy-light rhythm, cooling as it dims
      like an incandescent bulb (FastLED example, MIT). Stateless. ── */
void fxTwinkleFox() {
  uint16_t prng = 11337;
  uint32_t clock32 = millis();
  uint8_t spd = map(S.speed, 1, 255, 1, 6);            // original TWINKLE_SPEED 0..8 (4 at speed 192)
  uint8_t dens = map(S.inten, 1, 255, 1, 8);           // original TWINKLE_DENSITY 0..8
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    prng = (uint16_t)(prng * 2053) + 1384;
    uint16_t clockoff = prng;
    prng = (uint16_t)(prng * 2053) + 1384;
    uint8_t mulQ53 = ((((prng & 0xFF) >> 4) + (prng & 0x0F)) & 0x0F) + 0x08;
    uint32_t myclock = (uint32_t)((clock32 * mulQ53) >> 3) + clockoff;
    uint8_t salt = prng >> 8;
    uint16_t ticks = myclock >> (8 - spd);
    uint8_t fast = ticks;
    uint16_t slow = (ticks >> 8) + salt;
    slow += sin8(slow);
    slow = (slow * 2053) + 1384;
    uint8_t slow8 = (slow & 0xFF) + (slow >> 8);
    CRGB c = CRGB::Black;
    if (((slow8 & 0x0E) / 2) < dens) {
      uint8_t b = fast < 86 ? fast * 3 : 255 - ((fast - 86) + ((fast - 86) >> 1));
      if (b) {
        c = ColorFromPalette(curPal, slow8 - salt, b, NOBLEND);
        if (fast >= 128) { uint8_t cool = (fast - 128) >> 4; c.g = qsub8(c.g, cool); c.b = qsub8(c.b, cool * 2); }
      }
    }
    leds[i] = c;
  }
}

/* ── 40 COLOR WAVES — broad palette bands that swell, narrow and drift, never repeating
      (after Kriegsman's ColorWaves; mirrored round the loop so there is no seam) ── */
void fxColorWaves() {
  static uint16_t pseudo = 0, hue16 = 0; static uint32_t last = 0;
  uint32_t now = millis(); uint16_t dms = (uint16_t)min<uint32_t>((now - last) * S.speed / 256, 400); last = now;   // calm drift for a ceiling
  uint8_t depth = beatsin88(341, 96, 224), msm = beatsin88(147, 23, 60);
  /* the original is tuned for ~100-LED strips; a room-size loop needs bands ~5× wider */
  uint16_t thInc = beatsin88(203, 5 * 256, 8 * 256);
  uint16_t hInc = (uint16_t)((uint32_t)beatsin88(113, 60, 300) * S.inten / 160);
  pseudo += dms * msm;
  hue16 += dms * beatsin88(400, 5, 9);
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint16_t u = loopDist(i, 0);
    uint16_t h16 = hue16 + hInc * u;
    uint16_t b16 = sin16(pseudo + thInc * u) + 32768;
    uint16_t bri16 = (uint32_t)b16 * b16 / 65536;
    uint8_t bri = (uint32_t)bri16 * depth / 65536 + (255 - depth);
    uint16_t h7 = h16 >> 7;
    uint8_t h8 = (h7 & 0x100) ? 255 - (h7 >> 1) : (h7 >> 1);
    nblend(leds[i], ColorFromPalette(curPal, scale8(h8, 240), bri), 128);
  }
}

/* ── 41 BOREALIS — northern lights with depth: far violet haze, green curtains,
      fast teal rays, each layer drifting at its own pace ── */
void fxBorealis() {
  uint32_t z = millis() * (uint32_t)S.speed;
  uint8_t cut = 255 - S.inten;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint16_t a = roomAng(i);
    uint8_t far = stretchN(loopNoise(a, 3, z >> 5));
    uint8_t mid = stretchN(loopNoise(a, 7, z >> 4));
    uint8_t nr  = stretchN(loopNoise(a, 16, z >> 3));
    CRGB c = CRGB(22, 0, 34); c.nscale8(far);
    c += CHSV(96, 255, qsub8(mid, cut >> 1));
    uint8_t ray = qsub8(nr, cut >> 1); c += CHSV(118, 160, scale8(ray, 190));
    leds[i] = c;
  }
}

/* ── 42 NIGHT SKY — sparse twinkling stars on deep blue, the whole sky slowly turning
      round the room; the odd shooting star ── */
static CRGB starAt(uint16_t k, uint32_t t) {
  uint32_t h = (uint32_t)k * 2654435761u;
  if ((h >> 24) > (uint32_t)(S.inten / 6 + 4)) return CRGB::Black;
  uint8_t b = 70 + ((h >> 8) & 127);
  b = scale8(b, 170 + (sin8(t / 14 + (h >> 16)) / 3));
  return (h & 1) ? CRGB(b, b, b * 7 / 8) : CRGB(b * 7 / 8, b * 7 / 8, b);
}
void fxNightSky() {
  static uint32_t off = 0; static int16_t shoot = -1; static uint8_t life = 0; static int8_t dir = 1;
  uint32_t t = millis();
  off += max<uint8_t>(1, S.speed / 8);                  // 8.8 fixed-point sub-pixels
  uint16_t N = NUM_LEDS, ip = (off >> 8) % N; uint8_t fr = off & 0xFF;
  for (uint16_t i = 0; i < N; i++) {
    uint16_t k = (i + N - ip) % N;
    CRGB a = starAt(k, t), b = starAt((k + N - 1) % N, t);
    a.nscale8(255 - fr); b.nscale8(fr);
    leds[i] = CRGB(0, 0, 4) + a + b;
  }
  if (!life && random16() < 6) { shoot = random16(N); life = 36; dir = random8() & 1 ? 1 : -1; }
  if (life) {
    for (uint8_t j = 0; j < 12; j++) { uint8_t v = scale8(life * 7, 255 - j * 20);
      leds[wrapIdx(shoot - dir * j)] += CRGB(v, v, v); }
    shoot = wrapIdx(shoot + dir * 3); life--;
  }
}

/* ── 43 LAVA LAMP — wax blobs drift, swell and merge (1-D metaballs) ── */
void fxLavaLamp() {
  struct Blob { uint32_t p; uint8_t r, seed; };
  static Blob b[10]; static bool init = false;
  static uint8_t F[MAX_LEDS + LED_PAD];
  uint16_t N = NUM_LEDS;
  if (!init) { init = true; for (uint8_t k = 0; k < 10; k++) { b[k].p = (uint32_t)random16(N) << 16; b[k].r = random8(10, 26); b[k].seed = random8(); } }
  memset(F, 0, N);
  uint32_t t = millis(); uint8_t nb = 3 + S.inten / 37;
  for (uint8_t k = 0; k < nb; k++) {
    int32_t v = (int32_t)inoise8(t >> 4, b[k].seed * 97) - 128;
    int64_t np = (int64_t)b[k].p + (int64_t)v * S.speed * 4 + ((int64_t)N << 16);
    b[k].p = (uint32_t)(np % ((int64_t)N << 16));
    uint8_t r = b[k].r + (sin8(t / 60 + b[k].seed * 40) >> 5);
    int32_t c = b[k].p >> 16;
    for (int16_t d = -(int16_t)r; d <= (int16_t)r; d++) {
      uint8_t w = 255 - (uint8_t)((uint32_t)d * d * 255 / ((uint32_t)r * r));
      uint16_t x = wrapIdx(c + d); F[x] = qadd8(F[x], scale8(w, w));
    }
  }
  CRGB lo = colB(); lo.nscale8(35); CRGB hi = colA();
  for (uint16_t i = 0; i < N; i++) {
    uint8_t f = F[i], a = f < 70 ? 0 : f > 150 ? 255 : ease8InOutQuad((uint16_t)(f - 70) * 255 / 80);
    leds[i] = blend(lo, hi, a);
    if (f > 220) leds[i] += CRGB(f - 220, (f - 220) / 2, 0);
  }
}

/* ── 44 FIREFLIES — yellow-green motes drift lazily and glow in slow pulses ── */
void fxFireflies() {
  struct Fly { uint32_t p; uint16_t ph, rate; uint8_t seed; };
  static Fly fl[24]; static bool init = false;
  uint16_t N = NUM_LEDS;
  if (!init) { init = true; for (uint8_t k = 0; k < 24; k++) { fl[k].p = (uint32_t)random16(N) << 16; fl[k].ph = random16(); fl[k].rate = random16(250, 600); fl[k].seed = random8(); } }
  fadeToBlackBy(leds, N, 64);
  uint32_t t = millis(); uint8_t n = 6 + S.inten / 14;
  for (uint8_t k = 0; k < n && k < 24; k++) {
    Fly &f = fl[k];
    int32_t v = (int32_t)inoise8(t >> 3, f.seed * 61) - 128;
    int64_t np = (int64_t)f.p + (int64_t)v * S.speed + ((int64_t)N << 16);
    f.p = (uint32_t)(np % ((int64_t)N << 16));
    uint16_t before = f.ph; f.ph += f.rate;
    if (f.ph < before) f.rate = random16(250, 600);      // new rhythm every cycle
    uint8_t p8 = f.ph >> 8;
    uint8_t br = p8 < 96 ? quadwave8((uint16_t)p8 * 85 / 32) : 0;
    if (br) drawSoft(f.p >> 16, 3, CHSV(60 + (f.seed & 15), 220, br));
  }
}

/* ── 45 SHEEN — a solid colour with one broad soft highlight gliding round the room,
      glinting as it passes each corner ── */
void fxSheen() {
  uint32_t per = 4000 + (uint32_t)(255 - S.speed) * 120;
  uint16_t N = NUM_LEDS, pos = (uint32_t)(millis() % per) * N / per, W = 8 + S.inten / 2;
  CRGB base = S.colorMode ? C1() : pcol(hue8 / 4), hl = S.colorMode ? C2() : CRGB(255, 240, 220);
  for (uint16_t i = 0; i < N; i++) {
    leds[i] = base;
    uint16_t d = loopDist(i, pos);
    if (d < W) { uint8_t h = qsub8(cos8((uint8_t)((uint32_t)d * 128 / W)), 128) * 2; h = scale8(h, h); leds[i] = blend(base, hl, h); }
  }
  for (uint8_t w = 0; w < NWALL; w++)
    if (WLEN[w] && loopDist(pos, WSTART[w]) < 6) leds[WSTART[w]] += CRGB(50, 50, 50);
}

/* ── 46 LEAF CANOPY — warm light dappled by two drifting layers of leaf shadow,
      with the odd gust of wind ── */
void fxLeafCanopy() {
  static uint32_t gust = 0, last = 0;
  uint32_t t = millis(), dt = min<uint32_t>(t - last, 200); last = t;
  uint32_t calm = t * S.speed;
  gust += dt * S.speed * (1 + (inoise8(t >> 6) >> 6));        // accumulated, so a gust never jumps
  uint8_t dens = 255 - S.inten;
  CRGB sun = S.colorMode ? C1() : CRGB(255, 168, 80), leaf = S.colorMode ? C2() : CRGB(36, 80, 14);
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint16_t a = roomAng(i);
    uint8_t s1 = stretchN(loopNoise(a, 10, calm >> 5)), s2 = stretchN(loopNoise(a, 24, gust >> 4));
    uint8_t shade = qsub8(qadd8(scale8(s1, s2), (s1 + s2) >> 2), dens);
    leds[i] = blend(sun, leaf, shade);
  }
}

/* ── 47 SLOSH — a tide of light seesaws from one side of the room to the other,
      the tilt axis slowly turning. Intensity > half: walls 1+3 against 2+4. ── */
void fxSlosh() {
  static uint16_t ph = 0; ph += (uint16_t)S.speed * 3;   // ≈ 5 s per swing at speed 128
  uint16_t phi = millis() / 40;
  uint8_t k = S.inten > 127 ? 2 : 1;
  int32_t sw = sin16(ph);
  CRGB lo = colB(); lo.nscale8(40); CRGB hi = colA();
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    int32_t v = ((int32_t)cos16((uint16_t)(k * roomAng(i) - phi)) * sw) >> 15;
    uint8_t lvl = ease8InOutQuad((uint8_t)constrain(128 + (v >> 8), 0, 255));
    leds[i] = blend(lo, hi, lvl);
  }
}

/* ── 48 CORNER MEET — colour 1 grows out of walls 1 & 3, colour 2 out of walls 2 & 4;
      they meet and mix in every corner with a soft glow, then ebb ── */
void fxCornerMeet() {
  static uint16_t ph = 0; ph += S.speed;
  uint8_t f = ease8InOutQuad(triwave8(ph >> 8));
  uint8_t soft = map(S.inten, 1, 255, 8, 1);
  CRGB A = colA(), B = colB();
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t w = 0; w < NWALL; w++) {
    if (WLEN[w] < 2) continue;
    for (uint16_t j = 0; j < WLEN[w]; j++) {
      uint8_t x = (uint32_t)j * 255 / (WLEN[w] - 1);
      uint8_t d = (uint8_t)min(255, abs((int)x - 128) * 2);
      CRGB own = (w & 1) ? B : A, oth = (w & 1) ? A : B;
      CRGB c = blend(own, oth, scale8(d, d) >> 1);       // pure mid-wall, 50/50 in the corners
      int e = 128 + ((int)f - (int)d) * soft;
      c.nscale8((uint8_t)constrain(e, 0, 255));
      if (f > 235 && d > 225) { uint8_t g = f - 235; c += CRGB(g * 3, g * 3, g * 3); }
      leds[WSTART[w] + j] = c;
    }
  }
}

/* ── 49 PING PONG — a pulse leaves the middle of one wall, splits both ways round the
      room, and both halves land on the opposite wall at the same instant and bloom;
      every other volley it switches to the other pair of walls ── */
void fxPingPong() {
  static uint8_t src = 0, vol = 0, bloom = 0; static uint32_t pr = 0; static uint16_t at = 0;
  uint16_t N = NUM_LEDS;
  uint8_t dst = (src + 2) % NWALL;
  uint16_t S0 = WSTART[src] + WLEN[src] / 2, T0 = WSTART[dst] + WLEN[dst] / 2;
  uint16_t dcw = (T0 + N - S0) % N, dccw = N - dcw;
  uint8_t w = map(S.inten, 1, 255, 6, 30);
  pr += (uint32_t)S.speed * 12;
  uint16_t e = ease16InOutQuad((uint16_t)min<uint32_t>(pr, 65535));
  fadeToBlackBy(leds, N, 48);
  CRGB A = colA(), B = colB();
  drawSoft(S0 + ((uint32_t)dcw * e >> 16), w / 3 + 2, A);
  drawSoft((int32_t)S0 - (int32_t)((uint32_t)dccw * e >> 16), w / 3 + 2, A);
  if (pr >= 65535) { bloom = 255; at = T0; pr = 0; src = dst; if (!(++vol & 1)) src = (src + 1) % NWALL; }
  if (bloom) {
    uint8_t rad = (255 - bloom) / 6 + w;
    for (int16_t d = -(int16_t)rad; d <= (int16_t)rad; d++) {
      CRGB k = B; k.nscale8(scale8(bloom, 255 - (uint8_t)((uint32_t)abs(d) * 255 / (rad + 1))));
      leds[wrapIdx(at + d)] += k;
    }
    bloom = qsub8(bloom, 6);
  }
}

/* ═══════════════════════ RENDER ═══════════════════════ */
void renderEffect() {
  switch (S.mode) {
    case 0:  fxSolid();        break;  case 1:  fxBreathe();      break;
    case 2:  fxGradient();     break;  case 3:  fxCandle();       break;
    case 4:  fxFire();         break;  case 5:  fxSunrise();      break;
    case 6:  fxSunset();       break;  case 7:  fxDrift();        break;
    case 8:  fxRainbowRoom();  break;  case 9:  fxRainChase();    break;
    case 10: fxComet();        break;  case 11: fxDualComet();    break;
    case 12: fxCornerBurst();  break;  case 13: fxCornerRace();   break;
    case 14: fxCornerFlash();  break;  case 15: fxWallWipe();     break;
    case 16: fxWallChase();    break;  case 17: fxWallPulse();    break;
    case 18: fxPoliceWalls();  break;  case 19: fxTheater();      break;
    case 20: fxScanner();      break;  case 21: fxMeteor();       break;
    case 22: fxRunning();      break;  case 23: fxPlasma();       break;
    case 24: fxTwinkle();      break;  case 25: fxSparkle();      break;
    case 26: fxFireworks();    break;  case 27: fxStrobe();       break;
    case 28: fxLightning();    break;  case 29: fxRipple();       break;
    case 30: fxSpiral();       break;  case 31: fxAurora();       break;
    case 32: fxMatrix();       break;  case 33: fxHeartbeat();    break;
    case 34: fxDiscoBPM();     break;  case 35: fxRaveBPM();      break;
    case 36: fxBeatWallsBPM(); break;  case 37: fxParty();        break;
    case 38: fxPacifica();     break;  case 39: fxTwinkleFox();   break;
    case 40: fxColorWaves();   break;  case 41: fxBorealis();     break;
    case 42: fxNightSky();     break;  case 43: fxLavaLamp();     break;
    case 44: fxFireflies();    break;  case 45: fxSheen();        break;
    case 46: fxLeafCanopy();   break;  case 47: fxSlosh();        break;
    case 48: fxCornerMeet();   break;  case 49: fxPingPong();     break;
    default: fxSolid();
  }
}

/* ═══════════════════════ OUTPUT LAYER ═══════════════════════ */

/* PSU limit + send. The limiter uses FastLED's per-channel power model on the
   final output, so it is exact for whatever the zones / brightness left lit.  */
void pushOut() {
  const uint32_t budget = (uint32_t)PSU_VOLTS * PSU_MILLIAMP;
  uint32_t mw = calculate_unscaled_power_mW(outLeds, nLeds);
  if (mw > budget) {
    uint8_t k = (uint8_t)((uint64_t)budget * 255 / mw);
    for (uint16_t i = 0; i < nLeds; i++) outLeds[i].nscale8(k);
  }
  if (useSpi) ledout::show(outLeds, nLeds);
  else        FastLED.show();
  frameCount++;
}
static void stripPower(bool on) {
  if (STRIP_PWR_PIN < 0) return;
  digitalWrite(STRIP_PWR_PIN, on ? STRIP_PWR_ON : !STRIP_PWR_ON);
}
/* standby: LEDs dark, output stopped, CPU 160 → 80 MHz, loop sleeps 20 ms per pass.
   BLE keeps advertising / stays connected, so 'On' from the app is instant.     */
void enterStandby() {
  if (standby) return;
  if (useSpi) ledout::wait();
  stripPower(false);
  setCpuFrequencyMhz(80);
  standby = true;
  Serial.println(F("[pwr] standby"));
}
void exitStandby() {
  if (!standby) return;
  setCpuFrequencyMhz(160);
  stripPower(true);
  if (STRIP_PWR_PIN >= 0) delay(30);           // let the strip's supply settle before data
  standby = false; darkFrames = 0;
  Serial.println(F("[pwr] awake"));
}

/* OTA progress on the ceiling: a thin cyan bar grows round the room */
static void renderOtaProgress() {
  memset(outLeds, 0, sizeof(outLeds));
  uint32_t lit = otaTotal ? (uint64_t)nLeds * otaWritten / otaTotal : 0;
  for (uint32_t i = 0; i < lit && i < nLeds; i += 3) outLeds[i] = CRGB(0, 10, 14);
  pushOut();
}

void render() {
  if (S.mode != lastMode) { lastMode = S.mode; fxStart = millis();
                            fill_solid(leds, NUM_LEDS, CRGB::Black); memset(heat, 0, sizeof(heat)); }
  pos16 = wrapIdx(pos16 + 1 + S.speed / 20);
  EVERY_N_MILLISECONDS(30) { hue8++; }

  if (audioLive()) {
    /* latch the incoming frame into the variables the WLED routines read */
    portENTER_CRITICAL(&audioMux);
    uint8_t rxCopy[16]; for (uint8_t b = 0; b < 16; b++) rxCopy[b] = fftRx[b];
    uint8_t vS = aVolSmth, vR = aVolRaw, mg = aMag; uint16_t pk = aPeakHz;
    bool bt = aBeat; aBeat = false;
    portEXIT_CRITICAL(&audioMux);
    for (uint8_t b = 0; b < 16; b++) {
      uint8_t t = rxCopy[b];
      fftResult[b] = S.smooth ? (uint8_t)((fftResult[b] * 2 + t * 3) / 5) : t;
    }
    volumeSmth    = volumeSmth * 0.6f + (float)vS * 0.4f;
    volumeRaw     = vR;
    FFT_MajorPeak = (pk > 0) ? (float)pk : 60.0f;
    my_magnitude  = (float)mg * 4.0f;
    samplePeak    = bt;

    switch (S.amode) {
      case 1:  amGEQ();            break;
      case 2:  amGravBase(0);      break;   // Gravcenter
      case 3:  amGravBase(1);      break;   // Gravcentric
      case 4:  amGravBase(2);      break;   // Gravimeter
      case 5:  amGravBase(3);      break;   // Gravfreq
      case 6:  amJuggles();        break;
      case 7:  amMatripix();       break;
      case 8:  amMidnoise();       break;
      case 9:  amNoisefire();      break;
      case 10: amNoisemeter();     break;
      case 11: amPixelwave();      break;
      case 12: amPlasmoid();       break;
      case 13: amPuddlesBase(0);   break;   // Puddles
      case 14: amPuddlesBase(1);   break;   // Puddlepeak
      case 15: amRipplePeak();     break;
      case 16: amBlurz();          break;
      case 17: amDJLight();        break;
      case 18: amFreqmap();        break;
      case 19: amFreqmatrix();     break;
      case 20: amFreqwave();       break;
      case 21: amNoisemove();      break;
      case 22: amRocktaves();      break;
      case 23: amWaterfall();      break;
      case 24: amBinmap();         break;
      default: renderEffect();
    }
    /* eye-comfort: temporal blend with the previous frame */
    if (S.smooth) {
      for (uint16_t i = 0; i < NUM_LEDS; i++) nblend(leds[i], prevFrame[i], 96);
      memcpy(prevFrame, leds, sizeof(prevFrame));
    }
  } else {
    renderEffect();
  }

  if (zonesDirty) buildZoneGain();

  /* ── master level, 8.8 fixed point (0 … BRI_MAX·256), eased toward the target ── */
  uint32_t target = (blackout || !S.power) ? 0 : (uint32_t)S.bri << 8;
  if (sleepEnd && S.power) {
    int32_t left = (int32_t)(sleepEnd - millis());
    if (left <= 0)         { S.power = 0; sleepEnd = 0; S.timer = 0; touchState(); target = 0; }
    else if (left < 60000) target = target * (uint32_t)left / 60000UL;   // last minute fades out
  }
  if (blackout) curBri = 0;
  else if (curBri < target) curBri += max<uint32_t>(16, (target - curBri) / 8);
  else if (curBri > target) curBri -= max<uint32_t>(16, (curBri - target) / 8);
  if ((curBri > target && curBri - target < 16) || (curBri < target && target - curBri < 16)) curBri = target;

  /* ══ OUTPUT STAGE ══
     The v9 firmware zeroed every channel below a threshold *in the effect buffer*.
     That (a) shifted the hue at low brightness — the weaker channels of a colour
     were the first to vanish, so orange went red — and (b) cut every fading trail
     short, because effects read leds[] back next frame.
     Now: effects keep their own buffer; this stage scales a copy with one combined
     per-channel factor (brightness × colour correction × zone mask) and ROUNDS
     instead of truncating, which is what keeps a dim colour the same colour.     */
  static const uint8_t CORR[3] = { 255, 176, 240 };          // TypicalLEDStrip
  uint32_t lvl = curBri;                                      // 0 … 25600
  if (lvl == 0) {
    memset(outLeds, 0, sizeof(outLeds));
  } else {
    /* factor per channel in 0.16 fixed point: level/255 × corr/255 */
    uint32_t f[3];
    for (uint8_t k = 0; k < 3; k++) f[k] = (uint32_t)((uint64_t)lvl * CORR[k] * 65536ULL / (255ULL * 255ULL * 256ULL));
    const bool masked = zonesMasked;
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
      CRGB &o = outLeds[i];
      if (masked && !zoneGain[i]) { o = CRGB::Black; continue; }   // outside the lit ranges: hard 0
      const CRGB &c = leds[i];
      o.r = (uint8_t)min<uint32_t>(255, ((uint32_t)c.r * f[0] + 32768) >> 16);
      o.g = (uint8_t)min<uint32_t>(255, ((uint32_t)c.g * f[1] + 32768) >> 16);
      o.b = (uint8_t)min<uint32_t>(255, ((uint32_t)c.b * f[2] + 32768) >> 16);
    }
  }
  /* identify (Flash): ONLY the chosen wall lights — steady white, its FIRST LED green
     and LAST LED red — everything else is fully dark.
     identW == NWALL is the dev test pattern: walls red / green / blue / yellow, LED 1 white. */
  if (identW >= 0) {
    if ((int32_t)(identUntil - millis()) <= 0) identW = -1;
    else {
      memset(outLeds, 0, sizeof(outLeds));
      if (identW < NWALL) {
        const uint8_t w = identW;
        for (uint16_t i = 0; i < WLEN[w]; i++) outLeds[WSTART[w] + i] = CRGB(48, 48, 48);
        if (WLEN[w]) { outLeds[WSTART[w]] = CRGB(0, 110, 0); outLeds[WSTART[w] + WLEN[w] - 1] = CRGB(110, 0, 0); }
      } else {
        static const CRGB TP[NWALL] = { CRGB(60, 0, 0), CRGB(0, 60, 0), CRGB(0, 0, 60), CRGB(50, 50, 0) };
        for (uint8_t w = 0; w < NWALL; w++)
          for (uint16_t i = 0; i < WLEN[w]; i++) outLeds[WSTART[w] + i] = TP[w];
        outLeds[0] = CRGB(80, 80, 80);
      }
    }
  }

  /* standby bookkeeping: a few black frames are sent, then output stops (see loop) */
  bool dark = (curBri == 0 && target == 0 && identW < 0);
  darkFrames = dark ? (darkFrames < 255 ? darkFrames + 1 : 255) : 0;
  pushOut();
}

/* ═══════════════════════ STATE + COMMANDS ═══════════════════════ */
static char jbuf[768];
#define N_PRESETS 8
uint8_t presetMask = 0;                 // bit i = preset slot i holds something

static uint16_t sleepLeftMin() {
  if (!sleepEnd || !S.power) return 0;
  int32_t left = (int32_t)(sleepEnd - millis());
  return left <= 0 ? 0 : (uint16_t)((left + 59999) / 60000);
}

static uint16_t rhi(uint8_t w) { return WLEN[w] ? min<uint16_t>(S.hi[w], WLEN[w] - 1) : 0; }
static uint16_t rlo(uint8_t w) { return min<uint16_t>(S.lo[w], rhi(w)); }

void buildJson() {
  snprintf(jbuf, sizeof(jbuf),
    "{\"power\":%u,\"mode\":%u,\"bri\":%u,\"speed\":%u,\"inten\":%u,\"pal\":%u,"
    "\"r1\":%u,\"g1\":%u,\"b1\":%u,\"r2\":%u,\"g2\":%u,\"b2\":%u,\"r3\":%u,\"g3\":%u,\"b3\":%u,"
    "\"amode\":%u,\"sens\":%u,\"cm\":%u,\"cor\":%u,\"wal\":%u,\"sm\":%u,\"auto\":%u,"
    "\"timer\":%u,\"tl\":%u,\"blk\":%u,\"pre\":%u,\"heap\":%u,\"fw\":%u,\"max\":%u,"
    "\"n\":%u,\"maxn\":%u,\"geo\":[%u,%u,%u,%u,%u,%u,%u,%u],"
    "\"rng\":[%u,%u,%u,%u,%u,%u,%u,%u],\"sb\":%u}",
    S.power, S.mode, S.bri, S.speed, S.inten, S.pal,
    S.r1,S.g1,S.b1, S.r2,S.g2,S.b2, S.r3,S.g3,S.b3,
    S.amode, S.sens, S.colorMode, S.corners, S.walls, S.smooth, S.autoCycle,
    S.timer, sleepLeftMin(), blackout ? 1 : 0, presetMask,
    (unsigned)(ESP.getFreeHeap() / 1024), FW_VERSION, BRI_MAX, nLeds, MAX_LEDS,
    /* 1-based, inclusive; a disabled wall reads start,0 */
    WSTART[0] + 1, WLEN[0] ? WSTART[0] + WLEN[0] : 0, WSTART[1] + 1, WLEN[1] ? WSTART[1] + WLEN[1] : 0,
    WSTART[2] + 1, WLEN[2] ? WSTART[2] + WLEN[2] : 0, WSTART[3] + 1, WLEN[3] ? WSTART[3] + WLEN[3] : 0,
    /* lit range per wall, 0-based inside the wall, clamped to its real length */
    rlo(0), rhi(0), rlo(1), rhi(1), rlo(2), rhi(2), rlo(3), rhi(3), standby ? 1 : 0);
}

static bool isHex(char c) { return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static uint8_t hx(char c) { return c >= 'a' ? c-'a'+10 : c >= 'A' ? c-'A'+10 : c-'0'; }
static bool parseHex(const char *p, uint8_t &r, uint8_t &g, uint8_t &b) {
  for (uint8_t i = 0; i < 6; i++) if (!isHex(p[i])) return false;   // validated input
  r = hx(p[0])<<4 | hx(p[1]); g = hx(p[2])<<4 | hx(p[3]); b = hx(p[4])<<4 | hx(p[5]);
  return true;
}

/* ── presets: whole look in NVS keys p0…p7, summarised back to the phone ── */
static void presetKey(uint8_t i, char *k) { k[0] = 'p'; k[1] = (char)('0' + i); k[2] = 0; }
void scanPresets() {
  presetMask = 0;
  char k[3]; LedState t;
  for (uint8_t i = 0; i < N_PRESETS; i++) { presetKey(i, k); if (readBlob(k, t)) presetMask |= 1 << i; }
}
void savePreset(uint8_t i) {
  char k[3]; presetKey(i, k);
  LedState t = S;
  t.power = 1;                                   // a preset is a look, never "off"
  if (t.bri == 0) t.bri = 40;
  t.crc = crc16((uint8_t *)&t, sizeof(t) - 2);
  if (prefs.putBytes(k, &t, sizeof(t)) == sizeof(t)) presetMask |= 1 << i;
  Serial.printf("[preset] saved %u\n", i);
}
void deletePreset(uint8_t i) {
  char k[3]; presetKey(i, k);
  prefs.remove(k);
  presetMask &= ~(1 << i);
}
bool loadPreset(uint8_t i) {
  char k[3]; presetKey(i, k); LedState t;
  if (!readBlob(k, t)) { presetMask &= ~(1 << i); return false; }
  /* live-only things stay as they are: music mode, sleep timer, auto-cycle */
  t.amode = S.amode; t.timer = S.timer; t.autoCycle = S.autoCycle;
  t.magic = STATE_MAGIC; t.version = STATE_VER;
  S = t; sanitize(S);
  S.power = 1; blackout = false;
  if (S.bri == 0) S.bri = 40;
  rebuildPal(); zonesDirty = true; touchState();
  return true;
}
/* {"t":"pre","p":[null,[mode,pal,cm,bri,"RRGGBBRRGGBBRRGGBB",zones],…]} */
void buildPresetJson() {
  size_t o = snprintf(jbuf, sizeof(jbuf), "{\"t\":\"pre\",\"p\":[");
  char k[3]; LedState t;
  for (uint8_t i = 0; i < N_PRESETS && o < sizeof(jbuf) - 64; i++) {
    presetKey(i, k);
    if (readBlob(k, t))
      o += snprintf(jbuf + o, sizeof(jbuf) - o, "%s[%u,%u,%u,%u,\"%02X%02X%02X%02X%02X%02X%02X%02X%02X\",%u]",
                    i ? "," : "", t.mode, t.pal, t.colorMode, t.bri,
                    t.r1, t.g1, t.b1, t.r2, t.g2, t.b2, t.r3, t.g3, t.b3, t.walls);
    else o += snprintf(jbuf + o, sizeof(jbuf) - o, "%snull", i ? "," : "");
  }
  snprintf(jbuf + o, sizeof(jbuf) - o, "]}");
}

void handleCmd(char *c, size_t len) {
  if (!len) return;
  long v = (len > 1) ? strtol(c + 1, NULL, 10) : 0;
  switch (c[0]) {
    case 'P': S.power = v ? 1 : 0; blackout = false;
              if (!v) { sleepEnd = 0; S.timer = 0; }
              else { if (S.bri == 0) S.bri = 40;              // "on" must be visible
                     if (sleepEnd && (int32_t)(sleepEnd - millis()) <= 0) { sleepEnd = 0; S.timer = 0; } }
              touchState(); break;
    case 'Z': blackout = true; S.power = 0; sleepEnd = 0; S.timer = 0; touchState(); break;
    case 'B': S.bri = constrain(v, 0, BRI_MAX); blackout = false; S.power = 1; touchState(); break;
    case 'M': S.mode  = constrain(v, 0, N_EFFECTS - 1); S.amode = 0; S.autoCycle = 0; touchState(); break;
    case 'S': S.speed = constrain(v, 1, 255); touchState(); break;
    case 'I': S.inten = constrain(v, 1, 255); touchState(); break;
    case 'K': S.pal   = constrain(v, 0, 9); rebuildPal(); touchState(); break;
    case 'A': S.amode = constrain(v, 0, N_AMODES); touchState(); break;
    case 'G': S.sens  = constrain(v, 1, 255); touchState(); break;
    case 'N': S.colorMode = v ? 1 : 0; rebuildPal(); touchState(); break;
    case 'R': S.corners = (uint8_t)(v & 0x0F); if (!S.corners) S.corners = 0x0F; touchState(); break;
    case 'X': S.walls = (uint8_t)(v & 0x0F); if (!S.walls) S.walls = 0x0F;          // walls on/off
              zonesDirty = true; touchState(); break;
    case 'U': applyZoneMask(S, (uint16_t)(v & 0xFFFF) ? (uint16_t)(v & 0xFFFF) : ZONES_ALL);   // fw-10 apps
              zonesDirty = true; touchState(); break;
    /* g<wall 1-4>,<first>,<last> — lit range inside that wall, 0-based, inclusive.
       last 65535 = to the end of the wall. Clamped to the wall at render time.     */
    case 'g': { char *p = c + 1, *q;
                long w = strtol(p, &q, 10); if (*q != ',') break; p = q + 1;
                long a = strtol(p, &q, 10); if (*q != ',') break; p = q + 1;
                long b = strtol(p, &q, 10);
                if (w < 1 || w > NWALL || a < 0 || b < 0) break;
                a = min<long>(a, 0xFFFE); b = min<long>(b, 0xFFFF);
                if (a > b) { long x = a; a = b; b = x; }
                S.lo[w - 1] = (uint16_t)a; S.hi[w - 1] = (uint16_t)b;
                zonesDirty = true; touchState(); break; }
    case '!': devCmd(c + 1); break;
    case '~': otaCmd(c + 1); break;
    case 'Q': S.smooth  = v ? 1 : 0; touchState(); break;
    case 'Y': S.autoCycle = constrain(v, 0, 240); autoAt = millis(); touchState(); break;
    case 'C': if (len >= 7 && parseHex(c+1, S.r1, S.g1, S.b1)) { S.power = 1; blackout = false;
                if (S.bri == 0) S.bri = 40; rebuildPal(); touchState(); } break;
    case 'D': if (len >= 7 && parseHex(c+1, S.r2, S.g2, S.b2)) { rebuildPal(); touchState(); } break;
    case 'E': if (len >= 7 && parseHex(c+1, S.r3, S.g3, S.b3)) { rebuildPal(); touchState(); } break;
    case 'V': if (len > 1 && v >= 0 && v < N_PRESETS) { savePreset(v); wantPresets = true; uiDirty = true; } break;
    case 'W': if (len > 1 && v >= 0 && v < N_PRESETS && !loadPreset(v)) wantPresets = true; break;
    case 'J': if (len > 1 && v >= 0 && v < N_PRESETS) { deletePreset(v); wantPresets = true; uiDirty = true; } break;
    case 'L': wantPresets = true; break;
    /* ── layout ── O<n> total LEDs (O0 = factory 821 layout) · H<wall 1-4>,<first>,<last> (1-based,
       inclusive, last 0 = wall unused) · F<wall 1-4> flash that wall for 4 s to check it */
    case 'O': if (v == 0) defaultGeo(); else setStripLength((uint16_t)constrain(v, 8, MAX_LEDS));
              geoDirty = true; touchState(); break;
    case 'H': { char *p = c + 1, *q;
                long w = strtol(p, &q, 10); if (*q != ',') break; p = q + 1;
                long a = strtol(p, &q, 10); if (*q != ',') break; p = q + 1;
                long b = strtol(p, &q, 10);
                if (w < 1 || w > NWALL || a < 1 || a > nLeds) break;
                WSTART[w - 1] = a - 1;
                WLEN[w - 1]   = (b >= a) ? (uint16_t)(min<long>(b, nLeds) - a + 1) : 0;
                fitWalls(); geoDirty = true; touchState(); break; }
    case 'F': if (v >= 1 && v <= NWALL) { identW = v - 1; identUntil = millis() + 5000; } break;
    case 'T': S.timer = S.power ? constrain(v, 0, 600) : 0;          // a timer only makes sense while lit
              sleepEnd = S.timer ? millis() + (uint32_t)S.timer * 60000UL : 0; touchState(); break;
    case '?': break;            /* handled by replyState() below */
  }
}

/* ═══════════════════════════════════════════════════════════════════════
   BLUETOOTH LE LAYER

   Design rules that make this robust:
     1. BLE callbacks run in the NimBLE host task.  They do NOTHING heavy —
        text commands go into a FreeRTOS queue, audio frames are copied under
        a spinlock.  loop() applies them.  (The ESP32 core's own BLE source
        says exactly this: "Implement onWrite() as a fast, non-blocking
        callback. Offload long-running processing to a queue.")
     2. Advertising restarts the instant a phone disconnects, and a watchdog in
        loop() re-arms it every 3 s if it ever stops for any other reason.
        "Board stopped showing up" is the #1 BLE failure — it can't happen here.
     3. MTU is negotiated up to 256, so the 24-byte audio frame fits in one
        write and the state JSON goes out in two notifications.
     4. Fast connection interval (7.5–15 ms) for audio, 4 s supervision timeout
        so a phone that vanishes without saying goodbye is dropped and the slot
        freed in 4 s, not 30.
   ═══════════════════════════════════════════════════════════════════════ */
BLEServer         *bleServer = nullptr;
BLECharacteristic *txChar    = nullptr;
volatile uint16_t  bleMtu    = 23;
volatile bool      wantState = false;
volatile uint32_t  bleLastRx = 0;
volatile uint8_t   bleLinks  = 0;
volatile bool      linkRecount = false;
QueueHandle_t      cmdQ      = nullptr;

struct CmdMsg { uint8_t len; uint16_t conn; char txt[63]; };   // conn = which phone sent it

/* per-phone MTU: notifications are cut to EACH phone's own MTU, so replies are sized
   for the smallest one connected (a 23-byte phone next to a 247-byte phone still works) */
#define NO_CONN 0xFFFF
struct PeerMtu { uint16_t conn, mtu; };
PeerMtu peers[3] = { { NO_CONN, 23 }, { NO_CONN, 23 }, { NO_CONN, 23 } };
static void recomputeMtu() {
  uint16_t m = 0;
  for (auto &p : peers) if (p.conn != NO_CONN) m = m ? min(m, p.mtu) : p.mtu;
  bleMtu = m ? m : 23;
}
static void peerSet(uint16_t conn, uint16_t mtu) {
  for (auto &p : peers) if (p.conn == conn) { p.mtu = mtu; recomputeMtu(); return; }
  for (auto &p : peers) if (p.conn == NO_CONN) { p.conn = conn; p.mtu = mtu; recomputeMtu(); return; }
}
static void peerDrop(uint16_t conn) {
  for (auto &p : peers) if (p.conn == conn) { p.conn = NO_CONN; p.mtu = 23; }
  recomputeMtu();
}

static void enqueueText(const uint8_t *d, size_t n, uint16_t conn) {
  size_t s0 = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || d[i] == '\n' || d[i] == '\r') {
      size_t L = i - s0;
      if (L > 0 && L < sizeof(CmdMsg::txt)) {
        CmdMsg m; m.len = (uint8_t)L; m.conn = conn; memcpy(m.txt, d + s0, L); m.txt[L] = 0;
        xQueueSend(cmdQ, &m, 0);              // never blocks the BLE task
      }
      s0 = i + 1;
    }
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  /* NimBLE calls the descriptor overload — it tells us WHICH phone wrote */
  void onWrite(BLECharacteristic *c, ble_gap_conn_desc *desc) override {
    const uint16_t conn = desc ? desc->conn_handle : NO_CONN;
    String v = c->getValue();
    const uint8_t *d = (const uint8_t *)v.c_str();
    size_t n = v.length();
    if (!n) return;
    bleLastRx = millis();

    /* [0xA3][fft0..15][volSmth][volRaw][flags][mag][peakHi][peakLo][bpm/2] */
    if (d[0] == 0xA3 && n >= 24) {
      portENTER_CRITICAL(&audioMux);
      for (uint8_t b = 0; b < 16; b++) fftRx[b] = d[1 + b];
      aVolSmth = d[17];
      aVolRaw  = d[18];
      if (d[19] & 1) aBeat = true;
      aMusic   = (d[19] & 4) != 0;
      aMag     = d[20];
      aPeakHz  = ((uint16_t)d[21] << 8) | d[22];
      aBpm     = d[23];
      aConf    = 255;
      aLast    = millis();
      if (aMusic) aMusicAt = millis();
      portEXIT_CRITICAL(&audioMux);
      return;
    }
    /* compact frame for phones stuck at the default 23-byte MTU (20-byte payload):
       [0xA4][fft0..15][volSmth][flags][peakHz/32]                               */
    if (d[0] == 0xA4 && n >= 19) {
      uint8_t mx = 0;
      portENTER_CRITICAL(&audioMux);
      for (uint8_t b = 0; b < 16; b++) { fftRx[b] = d[1 + b]; if (d[1 + b] > mx) mx = d[1 + b]; }
      aVolSmth = d[17];
      aVolRaw  = d[17];
      if (d[18] & 1) aBeat = true;
      aMusic   = (d[18] & 4) != 0;
      aMag     = mx;
      aPeakHz  = (n >= 20) ? (uint16_t)d[19] * 32 : 0;
      aConf    = 255;
      aLast    = millis();
      if (aMusic) aMusicAt = millis();
      portEXIT_CRITICAL(&audioMux);
      return;
    }
    /* OTA data: [0xB7][offset, 3 bytes LE][payload…]. Accepted only in order and only
       if it fits the stream buffer; otherwise the loop tells the phone where to resume. */
    if (d[0] == 0xB7 && n > 4) {
      if (!otaActive || !otaStream || conn != otaConn) return;   // only the phone that started it
      uint32_t off = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16);
      size_t len = n - 4;
      if (off != otaExpected) { if (off > otaExpected) otaResync = true; return; }   // stale repeats are dropped silently
      if (xStreamBufferSpacesAvailable(otaStream) < len) { otaResync = true; return; }
      xStreamBufferSend(otaStream, d + 4, len, 0);
      otaExpected = off + len;
      return;
    }
    if (d[0] < 0x20 || d[0] > 0x7E) return;   // not text, not audio: ignore
    enqueueText(d, n, conn);
  }
  void onSubscribe(BLECharacteristic *c, ble_gap_conn_desc *desc, uint16_t subValue) override {
    if (subValue) wantState = true;              // phone just enabled notifications
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  /* NimBLE fires BOTH onConnect overloads — only the descriptor one is overridden */
  void onConnect(BLEServer *srv, ble_gap_conn_desc *desc) override {
    bleLinks = srv->getConnectedCount() + 1;                    // the core counts this link AFTER the callback
    peerSet(desc->conn_handle, 23);
    srv->updateConnParams(desc->conn_handle, 6, 12, 0, 400);   // 7.5-15 ms, 4 s timeout
    wantState = true; wantPresets = true;
    if (bleLinks < MAX_CLIENTS) BLEDevice::startAdvertising(); // let a 2nd phone in
    Serial.printf("[ble] connect  (links %u)\n", bleLinks);
  }
  void onDisconnect(BLEServer *srv, ble_gap_conn_desc *desc) override {
    BLEDevice::startAdvertising();                              // back on air immediately
    linkRecount = true;                                         // loop() re-reads the real count
    peerDrop(desc->conn_handle);
    if (desc->conn_handle == devAuthConn) { devAuthConn = NO_CONN; devAuthUntil = 0; }   // unlock ends with that phone
    if (otaActive && desc->conn_handle == otaConn) otaAbortReq = true;
    Serial.println(F("[ble] disconnect"));
  }
  void onMtuChanged(BLEServer *srv, ble_gap_conn_desc *desc, uint16_t mtu) override {
    peerSet(desc->conn_handle, mtu);
    Serial.printf("[ble] mtu %u (using %u)\n", mtu, (unsigned)bleMtu);
  }
};

void startBLE() {
  BLEDevice::init(BLE_NAME);
  BLEDevice::setMTU(256);
  BLEDevice::setPower(ESP_PWR_LVL_P9);                // +9 dBm — whole-room range

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  bleServer->advertiseOnDisconnect(true);

  BLEService *svc = bleServer->createService(SVC_UUID);
  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->setCallbacks(new RxCallbacks());            // for onSubscribe
  BLECharacteristic *rx = svc->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println(F("[ble] advertising as \"" BLE_NAME "\""));
}

/* jbuf + '\n', chunked to the negotiated MTU */
void sendLine() {
  size_t len = strlen(jbuf);
  if (len + 1 < sizeof(jbuf)) { jbuf[len++] = '\n'; jbuf[len] = 0; }
  size_t chunk = (bleMtu > 23) ? (bleMtu - 3) : 20;
  if (chunk > 240) chunk = 240;
  for (size_t off = 0; off < len; off += chunk) {
    size_t n = (len - off < chunk) ? (len - off) : chunk;
    txChar->setValue((uint8_t *)jbuf + off, n);
    txChar->notify();
  }
}
static bool anyoneListening() {
  // ask the stack, don't trust a cached count: it can lag when one of two phones leaves
  return txChar && bleServer && bleServer->getConnectedCount() > 0;
}
void sendState()   { if (!anyoneListening()) return; buildJson();       sendLine(); }
void sendPresets() { if (!anyoneListening()) return; buildPresetJson(); sendLine(); }

/* status LED: blink while advertising, solid while a phone is connected */
void statusLed() {
  static uint32_t t = 0; static bool on = false;
  if (bleLinks) { if (!on) { digitalWrite(STATUS_LED, LOW); on = true; } return; }
  if (millis() - t > 500) { t = millis(); on = !on; digitalWrite(STATUS_LED, on ? LOW : HIGH); }
}

/* ═══════════════════════ DEV COMMANDS + OTA ═══════════════════════
   Dangerous commands (OTA, reboot, factory reset, PIN change) need "!auth <pin>"
   first; the unlock lasts 10 minutes. 5 wrong PINs lock it for 60 s.
   The PIN lives in NVS ("pin"); default DEV_PIN_DEFAULT — change it from the app.  */
/* the factory PIN lives in LumoSecrets.h (git-ignored, see LumoSecrets.h.example);
   public builds without it fall back to "0000" — change it from the app on first use */
#if __has_include("LumoSecrets.h")
#include "LumoSecrets.h"
#endif
#ifndef DEV_PIN_DEFAULT
#define DEV_PIN_DEFAULT "0000"
#endif
uint32_t devLockUntil = 0;
uint8_t  devFails = 0;
uint32_t rebootAt = 0;
bool     appMarkedValid = false;

/* write pending settings now (before a restart or a firmware update) */
void flushSaves() {
  if (dirty) { dirty = false; saveState(); }
  if (geoDirty) { geoDirty = false; saveGeo(); }
}

void notifyText(const char *t) {
  if (!anyoneListening()) return;
  snprintf(jbuf, sizeof(jbuf), "%s", t);
  sendLine();
}
static bool devAuthed() { return cmdConn != NO_CONN && cmdConn == devAuthConn && (int32_t)(devAuthUntil - millis()) > 0; }
static bool needAuth() { if (devAuthed()) return true; notifyText("~auth required"); return false; }

void buildInfoJson() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  snprintf(jbuf, sizeof(jbuf),
    "{\"t\":\"info\",\"fw\":%u,\"build\":\"%s %s\",\"core\":\"%s\",\"up\":%lu,\"rr\":%d,"
    "\"heap\":%u,\"minheap\":%u,\"cpu\":%u,\"drv\":\"%s\",\"frame\":%u,\"frames\":%lu,"
    "\"rus\":%lu,\"rusmax\":%lu,\"mode\":%u,\"part\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"links\":%u,\"mtu\":%u,\"auth\":%u}",
    FW_VERSION, __DATE__, __TIME__, ESP_ARDUINO_VERSION_STR, (unsigned long)(millis() / 1000), (int)esp_reset_reason(),
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)getCpuFrequencyMhz(),
    useSpi ? "spi-dma" : "rmt", frameMs, (unsigned long)frameCount,
    (unsigned long)renderUs, (unsigned long)renderUsMax, S.mode,
    run ? run->label : "?", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
    (unsigned)bleLinks, (unsigned)bleMtu, devAuthed() ? 1 : 0);
}

void devCmd(char *a) {
  if (!strncmp(a, "auth ", 5)) {
    if ((int32_t)(devLockUntil - millis()) > 0) { notifyText("~auth locked"); return; }
    String pin = prefs.getString("pin", DEV_PIN_DEFAULT);
    if (pin == String(a + 5)) { devAuthUntil = millis() + 600000UL; devAuthConn = cmdConn; devFails = 0; notifyText("~auth ok"); }
    else { if (++devFails >= 5) { devFails = 0; devLockUntil = millis() + 60000UL; }
           devAuthUntil = 0; notifyText("~auth bad"); }
    return;
  }
  if (!strcmp(a, "info"))  { if (anyoneListening()) { buildInfoJson(); sendLine(); } renderUsMax = 0; return; }
  if (!strcmp(a, "test"))  { identW = NWALL; identUntil = millis() + 10000; notifyText("~ok test"); return; }
  if (!strcmp(a, "lock"))  { if (cmdConn == devAuthConn) { devAuthUntil = 0; devAuthConn = NO_CONN; } notifyText("~auth locked by user"); return; }
  if (!strncmp(a, "pin ", 4)) {
    if (!needAuth()) return;
    const char *np = a + 4; size_t L = strlen(np); bool ok = L >= 4 && L <= 12;
    for (size_t i = 0; ok && i < L; i++) ok = isalnum((unsigned char)np[i]);
    if (!ok) { notifyText("~pin invalid (4-12 letters/digits)"); return; }
    prefs.putString("pin", np); notifyText("~pin saved"); return;
  }
  if (!strcmp(a, "reboot"))  { if (!needAuth()) return; flushSaves(); notifyText("~bye"); rebootAt = millis() + 400; return; }
  if (!strcmp(a, "factory")) { if (!needAuth()) return;
                               prefs.clear();                           // settings, presets, layout, PIN
                               dirty = false; geoDirty = false;
                               notifyText("~bye factory"); rebootAt = millis() + 400; return; }
  notifyText("~unknown");
}

/* ── OTA over BLE ──
   phone: ~B<size>,<crc32 hex>   board: ~R  (ready)  or ~ER<reason>
   phone: 0xB7 frames …          board: ~K<bytes written>  every 4 KB   ~S<offset> = resend from here
   phone: ~E                     board: ~OK (reboots into the new image)  or ~ER<reason>
   phone: ~A                     board: ~ER aborted
   The new image must stay up for 20 s before it is marked valid; if it crashes or
   reboots earlier, the bootloader rolls back to the previous firmware by itself.   */
/* standard CRC-32 (zlib / java.util.zip.CRC32), so the phone and board agree bit-for-bit */
static uint32_t crcTab[256];
static void crcInit() { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
                          for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTab[i] = c; } }
static uint32_t crcUpdate(uint32_t c, const uint8_t *p, size_t n) { while (n--) c = crcTab[(c ^ *p++) & 0xFF] ^ (c >> 8); return c; }

static void otaFail(const char *why) {
  if (Update.isRunning()) Update.abort();
  otaActive = false; otaTotal = 0;
  if (otaStream) xStreamBufferReset(otaStream);
  char m[80]; snprintf(m, sizeof(m), "~ER%s", why); notifyText(m);
  Serial.printf("[ota] failed: %s\n", why);
}
void otaCmd(char *a) {
  if (a[0] == 'B') {
    if (!needAuth()) return;
    if (otaActive) { if (Update.isRunning()) Update.abort(); otaActive = false; }   // silent: a retry must not fail itself
    flushSaves();
    char *q; unsigned long sz = strtoul(a + 1, &q, 10);
    if (*q != ',') { notifyText("~ERformat"); return; }
    otaCrcWant = strtoul(q + 1, nullptr, 16);
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (!next || sz < 1024 || sz > next->size) { notifyText("~ERsize"); return; }
    if (!otaStream) otaStream = xStreamBufferCreate(16384, 1);
    if (!otaStream) { notifyText("~ERmemory"); return; }
    xStreamBufferReset(otaStream);
    if (!Update.begin(sz, U_FLASH)) { notifyText("~ERbegin"); return; }
    if (!crcTab[1]) crcInit();
    otaTotal = sz; otaWritten = 0; otaCrc = 0xFFFFFFFFu; otaLastAck = 0; otaLastData = millis();
    otaExpected = 0; otaResync = false; otaAbortReq = false;
    otaConn = cmdConn;
    otaActive = true;
    exitStandby();
    Serial.printf("[ota] begin %lu bytes → %s\n", sz, next->label);
    notifyText("~R");
    return;
  }
  if (a[0] == 'A') { if (otaActive) otaFail(" aborted"); return; }
  if (a[0] == 'E') {
    if (!otaActive) { notifyText("~ERnot active"); return; }
    otaPump(true);
    if (otaWritten != otaTotal) { otaFail("short"); return; }
    if ((otaCrc ^ 0xFFFFFFFFu) != otaCrcWant) { otaFail("crc"); return; }
    if (!Update.end(true))      { otaFail(Update.errorString()); return; }
    otaActive = false;
    Serial.println(F("[ota] done — rebooting"));
    notifyText("~OK");
    rebootAt = millis() + 800;
    return;
  }
}
/* drain the stream buffer into flash (loop context only) */
void otaPump(bool all) {
  if (!otaActive || !otaStream) return;
  static uint8_t blk[1024];
  uint32_t t0 = millis();
  for (;;) {
    size_t n = xStreamBufferReceive(otaStream, blk, sizeof(blk), 0);
    if (!n) break;
    if (otaWritten == 0) {                         // first bytes: is this an ESP32-C6 app image?
      uint16_t chip = n >= 14 ? (uint16_t)(blk[12] | (blk[13] << 8)) : 0xFFFF;
      if (blk[0] != 0xE9 || chip != 0x000D) { otaFail("not an ESP32-C6 app image"); return; }
    }
    if (Update.write(blk, n) != n) { otaFail(Update.errorString()); return; }
    otaCrc = crcUpdate(otaCrc, blk, n);
    otaWritten += n; otaLastData = millis();
    if (!all && millis() - t0 > 30) break;        // keep the loop responsive
  }
  if (otaWritten - otaLastAck >= 4096 || (otaWritten == otaTotal && otaLastAck != otaWritten)) {
    otaLastAck = otaWritten;
    char m[24]; snprintf(m, sizeof(m), "~K%lu", (unsigned long)otaWritten); notifyText(m);
  }
  if (otaResync) {
    otaResync = false;
    char m[24]; snprintf(m, sizeof(m), "~S%lu", (unsigned long)otaExpected); notifyText(m);
  }
  /* a lost ~K / ~S notification would leave both sides waiting on each other:
     when no data has arrived for 500 ms, repeat where we are (both are idempotent) */
  static uint32_t nudgeAt = 0;
  if (millis() - otaLastData > 500 && millis() - nudgeAt > 500) {
    nudgeAt = millis();
    char m[40]; snprintf(m, sizeof(m), "~K%lu", (unsigned long)otaWritten); notifyText(m);
    otaLastAck = otaWritten;
    if (otaExpected > otaWritten || otaWritten < otaTotal) {
      snprintf(m, sizeof(m), "~S%lu", (unsigned long)otaExpected); notifyText(m);
    }
  }
  if (otaAbortReq || millis() - otaLastData > 15000) { otaAbortReq = false; otaFail("timeout"); }
}
/* the Arduino core would mark a fresh OTA image valid at boot; we wait until it has
   proven itself (BLE up, loop running) — see loop()                                   */
extern "C" bool verifyRollbackLater() { return true; }

/* ═══════════════════════ SETUP / LOOP ═══════════════════════ */
/* curated list for auto-cycle — skips strobes and the one-shot sunrise */
const uint8_t AUTO_LIST[] = {2,38,3,40,7,41,8,43,10,45,11,47,12,39,13,48,15,46,16,42,17,22,23,24,29,30,31,32,44,49};
uint8_t autoIdx = 0;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.printf("\n=== LUMO · fw %u ===\n", FW_VERSION);

  if (STRIP_PWR_PIN >= 0) { pinMode(STRIP_PWR_PIN, OUTPUT); stripPower(true); }
  prefs.begin("led7", false);
  loadGeo();

  /* LED output: SPI + DMA (immune to BLE / flash stalls). FastLED's RMT driver is kept
     only as a fallback if the SPI bus can't be set up. Brightness, colour correction and
     the PSU limit are applied by render()/pushOut(), so the driver sends values as-is. */
  useSpi = ledout::begin(DATA_PIN, MAX_LEDS);
  if (!useSpi) {
    FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(outLeds, NUM_LEDS).setCorrection(UncorrectedColor);
    FastLED.setDither(DISABLE_DITHER);
    FastLED.setBrightness(255);
  }
  Serial.printf("[led] %u LEDs via %s\n", nLeds, useSpi ? "SPI+DMA" : "RMT (fallback)");
  memset(outLeds, 0, sizeof(outLeds)); pushOut();

  loadState();
  scanPresets();
  rebuildPal();
  buildZoneGain();

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);
  cmdQ = xQueueCreate(24, sizeof(CmdMsg));
  startBLE();

  /* boot signature: one colour per wall so you can verify the mapping */
  const CRGB sig[NWALL] = { CRGB(14,0,0), CRGB(0,14,0), CRGB(0,0,14), CRGB(14,14,0) };
  for (uint8_t w = 0; w < NWALL; w++)
    for (uint16_t i = 0; i < WLEN[w]; i++) outLeds[WSTART[w] + i] = sig[w];
  pushOut(); delay(700);
  memset(outLeds, 0, sizeof(outLeds)); pushOut();
}

void loop() {
  /* ── apply queued BLE commands (max 16 per pass so rendering never starves) ── */
  CmdMsg m; uint8_t applied = 0;
  while (applied < 16 && xQueueReceive(cmdQ, &m, 0) == pdTRUE) {
    cmdConn = m.conn;
    handleCmd(m.txt, m.len);
    applied++;
  }
  if (applied) wantState = true;
  if (uiDirty) { uiDirty = false; wantState = true; }

  /* coalesce state replies: at most one every 120 ms, even during a slider drag */
  static uint32_t lastState = 0;
  if (!otaActive) {
    if (wantState && millis() - lastState > 120) { wantState = false; lastState = millis(); sendState(); }
    else if (wantPresets && millis() - lastState > 60) { wantPresets = false; lastState = millis(); sendPresets(); }
  }

  if (linkRecount) {                       // settle the count a moment after a disconnect
    static uint32_t at = 0;
    if (!at) at = millis();
    if (millis() - at > 150) {
      linkRecount = false; at = 0;
      bleLinks = bleServer->getConnectedCount();
      if (bleLinks == 0) bleMtu = 23;
    }
  }
  statusLed();

  /* ── firmware update in progress: flash writes + a progress bar, nothing else ── */
  if (otaActive) {
    otaPump(false);
    if (millis() - lastFrame >= 50) { lastFrame = millis(); renderOtaProgress(); }
    if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) ESP.restart();
    delay(1);
    return;
  }
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    flushSaves();
    memset(outLeds, 0, sizeof(outLeds)); pushOut(); if (useSpi) ledout::wait();
    ESP.restart();
  }

  /* ── standby: lights off and faded out → stop sending, slow down; wake on demand ── */
  bool wantLight = (S.power && !blackout) || identW >= 0;
  if (standby && wantLight) exitStandby();
  if (!standby && !wantLight && darkFrames >= 4) enterStandby();

  if (!standby && millis() - lastFrame >= frameMs) {
    lastFrame = millis();
    uint32_t t0 = micros(); render(); renderUs = micros() - t0;
    if (renderUs > renderUsMax) renderUsMax = renderUs;
  }

  if (dirty && millis() - dirtyAt > 2000)   { dirty = false; saveState(); if (geoDirty) { geoDirty = false; saveGeo(); } }

  /* auto-cycle through the curated effect list */
  if (S.autoCycle && !standby && !audioLive() && millis() - autoAt > (uint32_t)S.autoCycle * 1000UL) {
    autoAt = millis();
    autoIdx = (autoIdx + 1) % (sizeof(AUTO_LIST) / sizeof(AUTO_LIST[0]));
    S.mode = AUTO_LIST[autoIdx];
    uiDirty = true;
  }

  static bool wasLive = false;
  bool live = audioLive();
  if (wasLive && !live) { memset(fftResult, 0, sizeof(fftResult)); volumeSmth = 0; volumeRaw = 0;
                          memset(prevFrame, 0, sizeof(prevFrame)); lastMode = 255; }
  wasLive = live;

  /* ── a freshly updated image proves itself for 20 s, then cancels the rollback ── */
  static bool bleProven = false;
  if (!bleProven && bleServer) {
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    bleProven = (bleServer->getConnectedCount() > 0) || (adv && adv->isAdvertising());
  }
  if (!appMarkedValid && bleProven && millis() > 20000) {
    appMarkedValid = true;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println(F("[ota] new firmware confirmed"));
    }
  }

  /* ── BLE watchdog: never be invisible ── */
  EVERY_N_SECONDS(3) {
    bleLinks = bleServer ? bleServer->getConnectedCount() : 0;
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    if (bleLinks < MAX_CLIENTS && adv && !adv->isAdvertising()) {
      BLEDevice::startAdvertising();
      Serial.println(F("[ble] watchdog re-armed advertising"));
    }
  }

  EVERY_N_SECONDS(300) {
    Serial.printf("[stat] up %lus heap %u links %u mtu %u %s\n",
                  millis() / 1000, (unsigned)ESP.getFreeHeap(), (unsigned)bleLinks, (unsigned)bleMtu,
                  standby ? "standby" : "on");
  }

  /* ── idle until the next frame instead of spinning (lets the CPU sleep in WFI) ── */
  uint32_t since = millis() - lastFrame;
  uint32_t wait = standby ? 20 : (since < frameMs ? frameMs - since : 0);
  if (wait > 4) wait = standby ? 20 : 4;          // stay responsive to commands
  if (wait) delay(wait);
}
