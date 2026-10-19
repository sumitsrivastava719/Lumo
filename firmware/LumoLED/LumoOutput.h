// ─────────────────────────────────────────────────────────────────────────────
//  WS2812B output over SPI + DMA — glitch-free next to BLE and flash writes.
//
//  Why not FastLED's RMT driver: on the ESP32-C6 it runs without DMA in a
//  48-symbol buffer (two LEDs). The CPU must refill it from an interrupt every
//  ~60 µs; whenever the BLE stack holds the CPU longer — or a settings save to
//  flash briefly suspends the cache — the waveform breaks and pixels that should
//  be dark latch random colours (flicker, light outside the selected range).
//
//  Here the whole frame is encoded into RAM first and the GPSPI2 peripheral
//  clocks it out by DMA with no CPU involvement at all.
//
//  Encoding: SPI at 2.5 MHz = 0.4 µs per SPI bit, 3 SPI bits per WS2812 bit
//     '1' → 110  (0.8 µs high, 0.4 µs low)      '0' → 100  (0.4 µs high, 0.8 µs low)
//  = exactly the WS2812B datasheet timing, 1.2 µs per bit, 9 bytes per LED.
//  The frame starts and ends with zero bytes: line low before the first bit and
//  a ≥ 300 µs latch/reset after the last (WS2812B-V5 needs > 280 µs).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <Arduino.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

namespace ledout {

static const uint32_t SPI_HZ      = 2500000;
static const size_t   LEAD_BYTES  = 4;      // 12.8 µs low before the first bit
static const size_t   RESET_BYTES = 100;    // 320 µs latch

static spi_device_handle_t dev = nullptr;
static uint8_t *buf[2] = { nullptr, nullptr };   // double buffer: encode one while DMA sends the other
static uint8_t  cur = 0;
static bool     inFlight = false;
static spi_transaction_t tx;
static uint8_t  lut[256][3];                     // byte → 24 SPI bits
static size_t   capBytes = 0;

/* true on success; false → caller falls back to another driver */
static bool begin(int pin, uint16_t maxLeds) {
  for (int v = 0; v < 256; v++) {
    uint32_t p = 0;
    for (int b = 7; b >= 0; b--) p = (p << 3) | (((v >> b) & 1) ? 0b110 : 0b100);
    lut[v][0] = (p >> 16) & 0xFF; lut[v][1] = (p >> 8) & 0xFF; lut[v][2] = p & 0xFF;
  }
  capBytes = LEAD_BYTES + (size_t)maxLeds * 9 + RESET_BYTES;
  for (int i = 0; i < 2; i++) {
    buf[i] = (uint8_t *)heap_caps_calloc(1, capBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf[i]) return false;
  }
  spi_bus_config_t bus = {};
  bus.mosi_io_num = pin;
  bus.miso_io_num = -1;
  bus.sclk_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = (int)capBytes;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t d = {};
  d.clock_speed_hz = SPI_HZ;
  d.mode = 0;
  d.spics_io_num = -1;          // no chip-select
  d.queue_size = 1;
  if (spi_bus_add_device(SPI2_HOST, &d, &dev) != ESP_OK) return false;
  return true;
}

/* block until the previous frame has left the wire (≈ n × 28.8 µs after it started) */
static void wait() {
  if (!inFlight) return;
  spi_transaction_t *r;
  spi_device_get_trans_result(dev, &r, portMAX_DELAY);
  inFlight = false;
}

/* encode + start DMA; returns immediately, the next call waits for this one */
static void show(const CRGB *px, uint16_t n) {
  if (!dev) return;
  uint8_t *b = buf[cur];
  size_t o = LEAD_BYTES;
  memset(b, 0, LEAD_BYTES);
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t g = px[i].g, r = px[i].r, bl = px[i].b;     // WS2812 wire order: G R B
    memcpy(b + o, lut[g], 3);  o += 3;
    memcpy(b + o, lut[r], 3);  o += 3;
    memcpy(b + o, lut[bl], 3); o += 3;
  }
  memset(b + o, 0, RESET_BYTES); o += RESET_BYTES;
  wait();
  memset(&tx, 0, sizeof(tx));
  tx.length = o * 8;
  tx.tx_buffer = b;
  if (spi_device_queue_trans(dev, &tx, portMAX_DELAY) == ESP_OK) inFlight = true;
  cur ^= 1;
}

}  // namespace ledout
