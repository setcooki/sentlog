#include "internal/ring_buffer.h"

#include <algorithm>
#include <cassert>

namespace sentlog::internal {

namespace {
constexpr bool is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}
}  // namespace

RingBuffer::RingBuffer(size_t capacity_samples)
    : capacity_(capacity_samples),
      mask_(capacity_samples - 1),
      buffer_(capacity_samples, 0.0f) {
    assert(is_power_of_two(capacity_samples) &&
           "RingBuffer capacity must be a power of two");
}

size_t RingBuffer::write(const float* samples, size_t num_samples) noexcept {
    if (samples == nullptr || num_samples == 0) {
        return 0;
    }

    const size_t w = write_index_.load(std::memory_order_relaxed);
    const size_t r = read_index_.load(std::memory_order_acquire);
    const size_t used = w - r;
    const size_t free_slots = capacity_ - used;

    if (free_slots == 0) {
        overflow_count_.fetch_add(num_samples, std::memory_order_relaxed);
        return 0;
    }

    const size_t to_write = std::min(num_samples, free_slots);
    if (to_write < num_samples) {
        overflow_count_.fetch_add(num_samples - to_write,
                                  std::memory_order_relaxed);
    }

    for (size_t i = 0; i < to_write; ++i) {
        buffer_[(w + i) & mask_] = samples[i];
    }

    write_index_.store(w + to_write, std::memory_order_release);
    return to_write;
}

size_t RingBuffer::read(float* out, size_t num_samples) noexcept {
    if (out == nullptr || num_samples == 0) {
        return 0;
    }

    const size_t r = read_index_.load(std::memory_order_relaxed);
    const size_t w = write_index_.load(std::memory_order_acquire);
    const size_t avail = w - r;
    const size_t to_read = std::min(num_samples, avail);

    for (size_t i = 0; i < to_read; ++i) {
        out[i] = buffer_[(r + i) & mask_];
    }

    read_index_.store(r + to_read, std::memory_order_release);
    return to_read;
}

size_t RingBuffer::peek(float* out, size_t num_samples) const noexcept {
    if (out == nullptr || num_samples == 0) {
        return 0;
    }

    const size_t r = read_index_.load(std::memory_order_acquire);
    const size_t w = write_index_.load(std::memory_order_acquire);
    const size_t avail = w - r;
    const size_t to_read = std::min(num_samples, avail);

    for (size_t i = 0; i < to_read; ++i) {
        out[i] = buffer_[(r + i) & mask_];
    }
    // Intentionally do NOT advance read_index_ — that's the point.
    return to_read;
}

size_t RingBuffer::available() const noexcept {
    const size_t w = write_index_.load(std::memory_order_acquire);
    const size_t r = read_index_.load(std::memory_order_acquire);
    return w - r;
}

void RingBuffer::reset() noexcept {
    write_index_.store(0, std::memory_order_relaxed);
    read_index_.store(0, std::memory_order_relaxed);
    overflow_count_.store(0, std::memory_order_relaxed);
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
}

}  // namespace sentlog::internal
