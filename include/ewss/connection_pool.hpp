#ifndef EWSS_CONNECTION_POOL_HPP_
#define EWSS_CONNECTION_POOL_HPP_

#include "vocabulary.hpp"

#include <cstdint>
#include <cstring>

#include <array>
#include <atomic>
#include <new>

namespace ewss {

// ============================================================================
// ObjectPool - O(1) acquire/release, zero heap allocation at runtime
// ============================================================================

template <typename T, size_t MaxSlots>
class alignas(kCacheLine) ObjectPool {
 public:
  ObjectPool() { reset(); }

  // Pre-initialize pool (call once at startup)
  void reset() {
    for (size_t i = 0; i < MaxSlots; ++i) {
      free_list_[i] = i;
      slot_active_[i] = false;
    }
    free_count_ = MaxSlots;
  }

  // Acquire a slot index, returns -1 if pool exhausted
  int32_t acquire() {
    if (free_count_ == 0)
      return -1;
    --free_count_;
    int32_t idx = static_cast<int32_t>(free_list_[free_count_]);
    slot_active_[idx] = true;
    return idx;
  }

  // Release a slot back to pool
  void release(int32_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= MaxSlots)
      return;
    if (!slot_active_[idx])
      return;
    slot_active_[idx] = false;
    free_list_[free_count_] = static_cast<size_t>(idx);
    ++free_count_;
  }

  // Get raw storage for slot (placement new by caller)
  void* storage(int32_t idx) { return &slots_[static_cast<size_t>(idx) * sizeof(T)]; }

  // Get typed pointer
  T* get(int32_t idx) { return reinterpret_cast<T*>(storage(idx)); }

  // Status
  size_t available() const { return free_count_; }
  size_t capacity() const { return MaxSlots; }
  size_t in_use() const { return MaxSlots - free_count_; }
  bool is_active(int32_t idx) const { return idx >= 0 && static_cast<size_t>(idx) < MaxSlots && slot_active_[idx]; }

 private:
  // Storage for objects (aligned)
  alignas(kCacheLine) uint8_t slots_[MaxSlots * sizeof(T)]{};

  // Free list (stack-based)
  std::array<size_t, MaxSlots> free_list_{};
  size_t free_count_ = 0;

  // Active tracking
  std::array<bool, MaxSlots> slot_active_{};
};

// ============================================================================
// ServerStats - Atomic performance counters
// ============================================================================

struct alignas(kCacheLine) ServerStats {
  // Throughput counters
  std::atomic<uint64_t> total_messages_in{0};
  std::atomic<uint64_t> total_messages_out{0};
  std::atomic<uint64_t> total_bytes_in{0};
  std::atomic<uint64_t> total_bytes_out{0};

  // Connection counters
  std::atomic<uint64_t> total_connections{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> rejected_connections{0};

  // Error counters
  std::atomic<uint64_t> handshake_errors{0};
  std::atomic<uint64_t> socket_errors{0};
  std::atomic<uint64_t> buffer_overflows{0};

  // Latency tracking (microseconds)
  std::atomic<uint64_t> last_poll_latency_us{0};
  std::atomic<uint64_t> max_poll_latency_us{0};

  // Pool usage
  std::atomic<uint64_t> pool_acquires{0};
  std::atomic<uint64_t> pool_releases{0};
  std::atomic<uint64_t> pool_exhausted{0};

  void reset() {
    total_messages_in = 0;
    total_messages_out = 0;
    total_bytes_in = 0;
    total_bytes_out = 0;
    total_connections = 0;
    active_connections = 0;
    rejected_connections = 0;
    handshake_errors = 0;
    socket_errors = 0;
    buffer_overflows = 0;
    last_poll_latency_us = 0;
    max_poll_latency_us = 0;
    pool_acquires = 0;
    pool_releases = 0;
    pool_exhausted = 0;
  }

  // Check overload condition (>90% pool usage)
  bool is_overloaded(size_t pool_capacity) const {
    uint64_t active = active_connections.load(std::memory_order_relaxed);
    return active > (pool_capacity * 9 / 10);
  }
};

}  // namespace ewss

#endif  // EWSS_CONNECTION_POOL_HPP_
