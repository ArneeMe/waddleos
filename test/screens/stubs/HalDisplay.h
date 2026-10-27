#pragma once
// The T5's 960x540 e-ink panel as a frame buffer in memory (1 bit per pixel,
// MSB first, 0 = black). The program writes the buffer to a PNG.
#include <BoardConfig.h>
#include <GrayscaleCapabilities.h>

#include <cstdint>
#include <cstring>

class HalDisplay {
 public:
  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleEncoding = freeink::GrayscaleEncoding;
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  static constexpr uint16_t DISPLAY_WIDTH = 960;
  static constexpr uint16_t DISPLAY_HEIGHT = 540;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  void begin(bool = false) {}
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode = GrayscaleMode::Overlay) const { return {}; }
  void clearScreen(uint8_t color = 0xFF) const { std::memset(buffer_, color, BUFFER_SIZE); }
  void drawImage(const uint8_t* data, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool = false) const {
    for (uint16_t r = 0; r < h && y + r < DISPLAY_HEIGHT; r++) {
      for (uint16_t c = 0; c < w / 8 && x / 8 + c < DISPLAY_WIDTH_BYTES; c++) {
        buffer_[(y + r) * DISPLAY_WIDTH_BYTES + x / 8 + c] = data[r * (w / 8) + c];
      }
    }
  }
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) { frames++; }
  void displayBufferAsync(RefreshMode mode = FAST_REFRESH) { displayBuffer(mode); }
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  bool supportsAsyncGrayscaleBase() const { return false; }
  bool isInverted() const { return false; }
  uint8_t* getFrameBuffer() const { return buffer_; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    if (size) *size = BUFFER_SIZE;
    return buffer_;
  }
  void returnFrameBufferStorage() {}
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void displayGrayscaleBase(RefreshMode = HALF_REFRESH, bool = false) {}
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode = HALF_REFRESH, bool = false) { return false; }
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) {}
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
  bool supportsStripGrayscale() const { return false; }
  bool combinesGrayscaleBase() const { return false; }
  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }

  int frames = 0;  // displayBuffer() calls

 private:
  static inline uint8_t buffer_[BUFFER_SIZE];
};

extern HalDisplay display;
