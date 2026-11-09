#pragma once
// PSRAM helpers.
//
// Why: on this board mbedTLS is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC,
// i.e. every TLS handshake needs ~40-50 KB of *internal* RAM (in contiguous
// blocks). Anything else that lives in internal RAM (LVGL objects, JSON
// documents, HTTP bodies) competes with it and ends in
//   "(-32512) SSL - Memory allocation failed".
// Everything big or long-lived therefore goes to PSRAM.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

inline void *psramMalloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}
inline void *psramCalloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
}
inline void *psramRealloc(void *ptr, size_t size) {
  void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
}

// ArduinoJson allocator that keeps JsonDocument pools in PSRAM.
struct SpiRamJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override { return psramMalloc(size); }
  void deallocate(void *pointer) override { heap_caps_free(pointer); }
  void *reallocate(void *ptr, size_t new_size) override { return psramRealloc(ptr, new_size); }
  static SpiRamJsonAllocator *instance() {
    static SpiRamJsonAllocator allocator;
    return &allocator;
  }
};

// Growable byte buffer in PSRAM usable as an Arduino Stream sink, so
// HTTPClient::writeToStream() can download (and de-chunk) a response
// without building a String in internal RAM.
class PsramBuffer : public Stream {
 public:
  explicit PsramBuffer(size_t maxBytes) : max_(maxBytes) {}
  ~PsramBuffer() { if (data_) heap_caps_free(data_); }
  void clear() { len_ = 0; overflow_ = false; }
  bool reserve(size_t bytes) {
    if (bytes <= cap_) return true;
    if (bytes > max_) bytes = max_;
    uint8_t *p = (uint8_t *)psramRealloc(data_, bytes + 1);
    if (!p) return false;
    data_ = p; cap_ = bytes;
    return true;
  }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t size) override {
    if (len_ + size > cap_) {
      size_t want = cap_ ? cap_ * 2 : 16384;
      while (want < len_ + size) want *= 2;
      if (!reserve(want) || len_ + size > cap_) { overflow_ = true; return 0; }
    }
    memcpy(data_ + len_, buf, size);
    len_ += size;
    data_[len_] = 0;
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  const uint8_t *data() const { return data_; }
  const char *c_str() const { return (const char *)data_; }
  size_t length() const { return len_; }
  bool overflowed() const { return overflow_; }

 private:
  uint8_t *data_ = nullptr;
  size_t cap_ = 0, len_ = 0, max_;
  bool overflow_ = false;
};

// Internal RAM diagnostics: the largest free block is what decides whether a
// TLS handshake can succeed, not the total free size.
inline void logInternalHeap(const char *tag) {
  Serial.printf("[mem] %s: interno livre %u (maior bloco %u), PSRAM livre %u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
