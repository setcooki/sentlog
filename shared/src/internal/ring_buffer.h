// SPSC lock-free ring buffer for 32-bit float audio samples.
//
// Designed for the audio pipeline: the platform binding's real-time
// audio thread is the single producer (via sentlog_push_audio_frame),
// the SDK's pipeline thread is the single consumer. The buffer is
// pre-allocated; write() and read() never allocate and never block.
//
// Overflow policy is **drop-newest** — when the buffer is full, the
// incoming samples are silently dropped and the overflow counter
// increments. Dropping newest (vs dropping oldest) keeps the SPSC
// contract clean: the producer never touches the read index. The
// overflow counter is surfaced via the SentlogPipelineDiagnostics
// telemetry event so persistent overflow is visible to integrators.
//
// Capacity must be a power of two so we can mask instead of modulo.

#ifndef SENTLOG_INTERNAL_RING_BUFFER_H
#define SENTLOG_INTERNAL_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <vector>

namespace sentlog::internal {

class RingBuffer {
public:
    // capacity_samples must be a power of two. Construction asserts on this.
    explicit RingBuffer(size_t capacity_samples);

    // Producer side. Returns the number of samples actually written
    // (0..num_samples). Any dropped samples are counted in
    // overflow_count().
    size_t write(const float* samples, size_t num_samples) noexcept;

    // Consumer side. Returns the number of samples actually read
    // (0..num_samples). Reads less than num_samples when the buffer
    // doesn't have enough available.
    size_t read(float* out, size_t num_samples) noexcept;

    // Non-destructive read: copies up to num_samples without
    // advancing the read index. Returns the number of samples
    // actually copied. Used by sentlog_snapshot_audio so the
    // platform binding can pull the captured audio into AASIST-L
    // inference before the pipeline drains the ring.
    size_t peek(float* out, size_t num_samples) const noexcept;

    // Consumer side. Number of samples currently readable.
    size_t available() const noexcept;

    // Capacity in samples.
    size_t capacity() const noexcept { return capacity_; }

    // Number of samples dropped due to overflow since construction.
    // Read by either side. Monotonically increasing.
    size_t overflow_count() const noexcept {
        return overflow_count_.load(std::memory_order_relaxed);
    }

    // Reset the read and write indices. NOT thread-safe. Intended for
    // use between captures, when neither producer nor consumer is
    // active.
    void reset() noexcept;

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<float> buffer_;

    // Producer writes at write_index_, consumer reads at read_index_.
    // Both monotonically increasing; we mask when indexing into buffer_.
    // Using 64-bit indices means we never wrap around in practice
    // (at 16 kHz, wraps after ~36 million years).
    std::atomic<size_t> write_index_{0};
    std::atomic<size_t> read_index_{0};

    std::atomic<size_t> overflow_count_{0};
};

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_RING_BUFFER_H
