#pragma once
// The SDK memory manager: nothing to manage on a computer.
#include <cstddef>
#include <cstdint>
#include <functional>
namespace freeink {
enum class MemPool : uint8_t { Internal, Psram, Default };
struct CacheSink {
  const char* name = nullptr;
  uint8_t priority = 128;
  std::function<size_t(size_t)> evict;
};
class MemoryManager {
 public:
  static MemoryManager& instance() {
    static MemoryManager m;
    return m;
  }
  int registerSink(const CacheSink&) { return 0; }
  size_t freeBytes(MemPool = MemPool::Default) const { return 1 << 20; }
  size_t largestFreeBlock(MemPool = MemPool::Default) const { return 1 << 20; }
};
}  // namespace freeink
