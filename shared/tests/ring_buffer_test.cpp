#include "internal/ring_buffer.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

using sentlog::internal::RingBuffer;

TEST(RingBufferTest, EmptyOnConstruction) {
    RingBuffer rb(16);
    EXPECT_EQ(rb.available(), 0u);
    EXPECT_EQ(rb.overflow_count(), 0u);
    EXPECT_EQ(rb.capacity(), 16u);
}

TEST(RingBufferTest, WriteThenRead) {
    RingBuffer rb(16);
    std::array<float, 4> in = {1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_EQ(rb.write(in.data(), in.size()), 4u);
    EXPECT_EQ(rb.available(), 4u);

    std::array<float, 4> out{};
    EXPECT_EQ(rb.read(out.data(), out.size()), 4u);
    EXPECT_EQ(out[0], 1.0f);
    EXPECT_EQ(out[1], 2.0f);
    EXPECT_EQ(out[2], 3.0f);
    EXPECT_EQ(out[3], 4.0f);
    EXPECT_EQ(rb.available(), 0u);
}

TEST(RingBufferTest, ReadEmptyReturnsZero) {
    RingBuffer rb(16);
    std::array<float, 4> out{};
    EXPECT_EQ(rb.read(out.data(), out.size()), 0u);
}

TEST(RingBufferTest, WriteOverflowDropsNewestAndCounts) {
    RingBuffer rb(8);
    std::vector<float> in(16, 1.0f);
    const size_t written = rb.write(in.data(), in.size());
    EXPECT_EQ(written, 8u);
    EXPECT_EQ(rb.overflow_count(), 8u);
    EXPECT_EQ(rb.available(), 8u);
}

TEST(RingBufferTest, WrapAroundManyCycles) {
    RingBuffer rb(8);
    std::array<float, 4> in  = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> out{};
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(rb.write(in.data(), in.size()), 4u);
        EXPECT_EQ(rb.read(out.data(), out.size()), 4u);
        EXPECT_EQ(out[0], 1.0f);
        EXPECT_EQ(out[3], 4.0f);
    }
    EXPECT_EQ(rb.overflow_count(), 0u);
}

TEST(RingBufferTest, NullPointerSafe) {
    RingBuffer rb(8);
    EXPECT_EQ(rb.write(nullptr, 4), 0u);
    EXPECT_EQ(rb.read(nullptr, 4), 0u);
}

TEST(RingBufferTest, ResetClears) {
    RingBuffer rb(8);
    std::array<float, 4> in = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(in.data(), in.size());
    rb.reset();
    EXPECT_EQ(rb.available(), 0u);
    EXPECT_EQ(rb.overflow_count(), 0u);
}

TEST(RingBufferTest, PeekDoesNotAdvance) {
    RingBuffer rb(16);
    std::array<float, 4> in = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(in.data(), in.size());

    std::array<float, 4> out{};
    EXPECT_EQ(rb.peek(out.data(), out.size()), 4u);
    EXPECT_EQ(out[0], 1.0f);
    EXPECT_EQ(out[3], 4.0f);
    EXPECT_EQ(rb.available(), 4u) << "peek must not consume";

    // Peeking twice yields the same data.
    std::array<float, 4> out2{};
    EXPECT_EQ(rb.peek(out2.data(), out2.size()), 4u);
    EXPECT_EQ(out2[0], 1.0f);

    // A real read after peek still gets the same samples.
    std::array<float, 4> consumed{};
    EXPECT_EQ(rb.read(consumed.data(), consumed.size()), 4u);
    EXPECT_EQ(consumed[0], 1.0f);
    EXPECT_EQ(rb.available(), 0u);
}

TEST(RingBufferTest, PeekRespectsCapacity) {
    RingBuffer rb(8);
    std::array<float, 4> in = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(in.data(), in.size());

    std::array<float, 10> out{};
    EXPECT_EQ(rb.peek(out.data(), out.size()), 4u);
}

TEST(RingBufferTest, PeekNullPointerSafe) {
    RingBuffer rb(8);
    EXPECT_EQ(rb.peek(nullptr, 4), 0u);
}

// Lock-free producer/consumer correctness — two threads, one writing
// counters, the other reading and checking strict ordering.
TEST(RingBufferTest, ConcurrentProducerConsumer) {
    constexpr size_t kCapacity = 1024;
    constexpr size_t kTotal    = 100'000;
    RingBuffer rb(kCapacity);

    std::atomic<bool> producer_done{false};
    std::vector<float> received;
    received.reserve(kTotal);

    std::thread producer([&] {
        for (size_t i = 0; i < kTotal; ) {
            const float v = static_cast<float>(i);
            const size_t n = rb.write(&v, 1);
            if (n == 1) {
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (received.size() < kTotal) {
            float v = 0.0f;
            if (rb.read(&v, 1) == 1) {
                received.push_back(v);
            } else if (producer_done.load(std::memory_order_acquire) &&
                       rb.available() == 0) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kTotal);
    for (size_t i = 0; i < kTotal; ++i) {
        ASSERT_EQ(received[i], static_cast<float>(i)) << "at index " << i;
    }
    EXPECT_EQ(rb.overflow_count(), 0u);
}
