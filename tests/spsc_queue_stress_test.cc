#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#include "lob/spsc_queue.h"
#include "lob/types.h"

namespace lob {
namespace {

// Day 4 task 2: two-thread producer/consumer stress. These tests are the
// reason the TSan CI job exists — a data race in the queue's release/acquire
// protocol shows up here (and only here: everything else in the suite is
// single-threaded so far). They run under plain debug/asan too, where they
// still catch FIFO and payload-integrity bugs under real contention.
//
// Reporting pattern: the consumer thread records failures into atomics and
// the main thread asserts after join() — join gives the main thread a
// happens-before over everything the workers wrote, so every load below the
// joins can be relaxed (documented per site). Consumers always drain all N
// items even after a mismatch so the producer can never wedge on a full ring.

// Small capacities on purpose: the more often the ring is full/empty, the
// more often the threads cross the cached-index refresh paths and the
// release/acquire edges — that is where the bugs would live.

TEST(SPSCQueueStress, FifoSequenceUnderContention) {
  constexpr std::uint64_t kOps = 1'000'000;
  SPSCQueue<std::uint64_t> q(64);

  // relaxed everywhere below: cross-thread visibility to the main thread is
  // established by thread::join, not by these atomics themselves.
  std::atomic<std::uint64_t> mismatches{0};
  std::atomic<std::uint64_t> first_bad_index{0};
  std::atomic<std::uint64_t> first_bad_value{0};

  std::thread producer([&q] {
    for (std::uint64_t i = 0; i < kOps; ++i) {
      while (!q.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    for (std::uint64_t expected = 0; expected < kOps; ++expected) {
      std::uint64_t out = 0;
      while (!q.try_pop(out)) {
        std::this_thread::yield();
      }
      if (out != expected && mismatches.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_bad_index.store(expected, std::memory_order_relaxed);
        first_bad_value.store(out, std::memory_order_relaxed);
      }
    }
  });
  producer.join();
  consumer.join();

  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0u)
      << "first mismatch at #" << first_bad_index.load(std::memory_order_relaxed) << ": got "
      << first_bad_value.load(std::memory_order_relaxed);
  EXPECT_TRUE(q.empty());
}

// Capacity 1: every single op crosses the empty<->full boundary, so both
// sides take their cached-index refresh path on essentially every call.
TEST(SPSCQueueStress, CapacityOneBoundaryChurn) {
  constexpr std::uint64_t kOps = 200'000;
  SPSCQueue<std::uint64_t> q(1);

  std::atomic<std::uint64_t> mismatches{0};
  std::atomic<std::uint64_t> first_bad_index{0};
  std::atomic<std::uint64_t> first_bad_value{0};

  std::thread producer([&q] {
    for (std::uint64_t i = 0; i < kOps; ++i) {
      while (!q.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    for (std::uint64_t expected = 0; expected < kOps; ++expected) {
      std::uint64_t out = 0;
      while (!q.try_pop(out)) {
        std::this_thread::yield();
      }
      if (out != expected && mismatches.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_bad_index.store(expected, std::memory_order_relaxed);
        first_bad_value.store(out, std::memory_order_relaxed);
      }
    }
  });
  producer.join();
  consumer.join();

  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0u)
      << "first mismatch at #" << first_bad_index.load(std::memory_order_relaxed) << ": got "
      << first_bad_value.load(std::memory_order_relaxed);
  EXPECT_TRUE(q.empty());
}

// Batched consumer against a single-push producer: try_pop_batch's one
// release store per batch must hand back only fully-read slots, and FIFO
// order must hold across batch boundaries of every size the race produces.
TEST(SPSCQueueStress, BatchedConsumerFifoUnderContention) {
  constexpr std::uint64_t kOps = 1'000'000;
  SPSCQueue<std::uint64_t> q(64);

  std::atomic<std::uint64_t> mismatches{0};
  std::atomic<std::uint64_t> first_bad_index{0};
  std::atomic<std::uint64_t> first_bad_value{0};

  std::thread producer([&q] {
    for (std::uint64_t i = 0; i < kOps; ++i) {
      while (!q.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    std::array<std::uint64_t, 64> buf;
    std::uint64_t expected = 0;
    while (expected < kOps) {
      const std::size_t n = q.try_pop_batch(buf.data(), buf.size());
      if (n == 0) {
        std::this_thread::yield();
        continue;
      }
      for (std::size_t i = 0; i < n; ++i, ++expected) {
        if (buf[i] != expected && mismatches.fetch_add(1, std::memory_order_relaxed) == 0) {
          first_bad_index.store(expected, std::memory_order_relaxed);
          first_bad_value.store(buf[i], std::memory_order_relaxed);
        }
      }
    }
  });
  producer.join();
  consumer.join();

  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0u)
      << "first mismatch at #" << first_bad_index.load(std::memory_order_relaxed) << ": got "
      << first_bad_value.load(std::memory_order_relaxed);
  EXPECT_TRUE(q.empty());
}

// Every Event field is a pure function of the sequence number, so the
// consumer can recompute the expected 32 bytes and memcmp. A sequence check
// alone only proves the indices are right; this proves the slot bytes were
// neither torn nor reordered past the publishing store.
Event event_for(std::uint64_t i) {
  Event e{};  // value-init zeroes the explicit padding, keeping memcmp legal
  e.kind = static_cast<EventType>(i % 4);
  e.side = static_cast<Side>(i % 2);
  e.reason = static_cast<RejectReason>(i % 5);
  e.qty = static_cast<Qty>(i * 2654435761u);
  e.remaining = static_cast<Qty>(~i);
  e.price_ticks = static_cast<PriceTicks>(i ^ 0x5DEECE66Dull);
  e.order_id = i * 0x9E3779B97F4A7C15ull;
  return e;
}

TEST(SPSCQueueStress, EventPayloadIntegrity) {
  constexpr std::uint64_t kOps = 500'000;
  SPSCQueue<Event> q(16);

  std::atomic<std::uint64_t> mismatches{0};
  std::atomic<std::uint64_t> first_bad_index{0};

  std::thread producer([&q] {
    for (std::uint64_t i = 0; i < kOps; ++i) {
      const Event e = event_for(i);
      while (!q.try_push(e)) {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    for (std::uint64_t i = 0; i < kOps; ++i) {
      Event out{};
      while (!q.try_pop(out)) {
        std::this_thread::yield();
      }
      const Event expected = event_for(i);
      if (std::memcmp(&out, &expected, sizeof(Event)) != 0 &&
          mismatches.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_bad_index.store(i, std::memory_order_relaxed);
      }
    }
  });
  producer.join();
  consumer.join();

  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0u)
      << "first corrupted Event at #" << first_bad_index.load(std::memory_order_relaxed);
  EXPECT_TRUE(q.empty());
}

}  // namespace
}  // namespace lob
