#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace hoodie::audio {

// Single-producer / single-consumer lock-free ring buffer.
//
// One thread may call write()/clear(); one (different) thread may call read().
// Capacity is rounded up to a power of two; one slot is kept free so that
// full and empty are distinguishable. Safe to use from a real-time audio
// callback on the consumer side (no allocation, no locks).
template <typename T>
class SpscRingBuffer {
 public:
  explicit SpscRingBuffer(std::size_t min_capacity) {
    std::size_t cap = 2;
    while (cap < min_capacity + 1) cap <<= 1;
    buf_.resize(cap);
    mask_ = cap - 1;
  }

  SpscRingBuffer(const SpscRingBuffer&) = delete;
  SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

  std::size_t capacity() const { return buf_.size() - 1; }

  std::size_t size() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }

  std::size_t space() const { return capacity() - size(); }
  bool empty() const { return size() == 0; }

  // Producer only. Returns the number of items actually written.
  std::size_t write(const T* src, std::size_t count) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t avail = capacity() - (head - tail);
    const std::size_t n = count < avail ? count : avail;
    for (std::size_t i = 0; i < n; ++i) buf_[(head + i) & mask_] = src[i];
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  // Consumer only. Returns the number of items actually read.
  std::size_t read(T* dst, std::size_t count) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t avail = head - tail;
    const std::size_t n = count < avail ? count : avail;
    for (std::size_t i = 0; i < n; ++i) dst[i] = buf_[(tail + i) & mask_];
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

  // Discard all buffered data. Only safe when the consumer is stopped.
  void clear() {
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
  }

 private:
  std::vector<T> buf_;
  std::size_t mask_ = 0;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

}  // namespace hoodie::audio
