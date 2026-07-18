#include "lob/spsc_queue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "lob/types.h"

namespace lob {
namespace {

// Day 4 task 1: single-threaded correctness only. The two-thread stress test
// under TSan is task 2; these tests pin down ring semantics (FIFO, full/empty,
// wraparound, cache-refresh paths) where failures are deterministic.

TEST(SPSCQueue, ConstructedEmpty) {
  SPSCQueue<std::uint64_t> q(8);
  EXPECT_EQ(q.capacity(), 8u);
  EXPECT_EQ(q.size(), 0u);
  EXPECT_TRUE(q.empty());
  std::uint64_t out = 0;
  EXPECT_FALSE(q.try_pop(out));
}

TEST(SPSCQueue, PushPopRoundTrip) {
  SPSCQueue<std::uint64_t> q(8);
  ASSERT_TRUE(q.try_push(0xDEADBEEFCAFEF00Du));
  EXPECT_EQ(q.size(), 1u);
  EXPECT_FALSE(q.empty());
  std::uint64_t out = 0;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 0xDEADBEEFCAFEF00Du);
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, FifoOrderAcrossFullFill) {
  SPSCQueue<std::uint64_t> q(8);
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q.try_push(100 + i));
  }
  for (std::uint64_t i = 0; i < 8; ++i) {
    std::uint64_t out = 0;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, 100 + i);
  }
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, PushFailsWhenFullAndCorruptsNothing) {
  SPSCQueue<std::uint64_t> q(4);
  for (std::uint64_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_push(i));
  }
  EXPECT_EQ(q.size(), 4u);
  EXPECT_FALSE(q.try_push(999));
  EXPECT_FALSE(q.try_push(999));
  EXPECT_EQ(q.size(), 4u);
  // The rejected pushes must not have disturbed the ring contents.
  for (std::uint64_t i = 0; i < 4; ++i) {
    std::uint64_t out = 0;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, i);
  }
}

TEST(SPSCQueue, PopFailsWhenEmptyAndQueueStaysUsable) {
  SPSCQueue<std::uint64_t> q(4);
  std::uint64_t out = 0;
  EXPECT_FALSE(q.try_pop(out));
  ASSERT_TRUE(q.try_push(7));
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 7u);
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_TRUE(q.empty());
}

// The producer caches head_: a push that fails on the cached value must
// refresh and succeed once the consumer has freed a slot.
TEST(SPSCQueue, ProducerCacheRefreshSeesFreedSlot) {
  SPSCQueue<std::uint64_t> q(4);
  for (std::uint64_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_push(i));
  }
  EXPECT_FALSE(q.try_push(4));  // full; head_cache_ now current
  std::uint64_t out = 0;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 0u);
  EXPECT_TRUE(q.try_push(4));  // stale cache says full; refresh must see space
  for (std::uint64_t i = 1; i <= 4; ++i) {
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, i);
  }
}

// The consumer caches tail_: a pop that fails on the cached value must
// refresh and succeed once the producer has published a slot.
TEST(SPSCQueue, ConsumerCacheRefreshSeesNewSlot) {
  SPSCQueue<std::uint64_t> q(4);
  std::uint64_t out = 0;
  EXPECT_FALSE(q.try_pop(out));  // empty; tail_cache_ now current
  ASSERT_TRUE(q.try_push(42));
  EXPECT_TRUE(q.try_pop(out));  // stale cache says empty; refresh must see it
  EXPECT_EQ(out, 42u);
}

// Thousands of push/pop cycles through a capacity-4 ring held near-full, so
// the masked indices cross the wrap boundary continuously. Any masking or
// index-arithmetic bug breaks the sequence.
TEST(SPSCQueue, WrapAroundManyTimes) {
  SPSCQueue<std::uint64_t> q(4);
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (; next_in < 3; ++next_in) {
    ASSERT_TRUE(q.try_push(next_in));
  }
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE(q.try_push(next_in));
    ++next_in;
    std::uint64_t out = 0;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, next_out);
    ++next_out;
  }
  for (; next_out < next_in; ++next_out) {
    std::uint64_t out = 0;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, next_out);
  }
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, CapacityOneQueue) {
  SPSCQueue<std::uint64_t> q(1);
  EXPECT_EQ(q.capacity(), 1u);
  ASSERT_TRUE(q.try_push(11));
  EXPECT_FALSE(q.try_push(22));
  std::uint64_t out = 0;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 11u);
  EXPECT_FALSE(q.try_pop(out));
  ASSERT_TRUE(q.try_push(22));
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 22u);
}

TEST(SPSCQueue, SizeTracksMixedOps) {
  SPSCQueue<std::uint64_t> q(8);
  std::uint64_t out = 0;
  ASSERT_TRUE(q.try_push(1));
  ASSERT_TRUE(q.try_push(2));
  ASSERT_TRUE(q.try_push(3));
  EXPECT_EQ(q.size(), 3u);
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(q.size(), 2u);
  ASSERT_TRUE(q.try_push(4));
  ASSERT_TRUE(q.try_push(5));
  EXPECT_EQ(q.size(), 4u);
  while (q.try_pop(out)) {
  }
  EXPECT_EQ(q.size(), 0u);
}

// Command and Event are the rings' real cargo. Both have unique object
// representations (static_asserted in types.h), so memcmp equality proves the
// ring transported every byte.
TEST(SPSCQueue, TransportsCommandPod) {
  SPSCQueue<Command> q(2);
  Command in{};
  in.kind = CommandType::kNew;
  in.side = Side::kSell;
  in.type = OrderType::kIoc;
  in.qty = 250;
  in.price_ticks = 10'050;
  in.order_id = 0;
  ASSERT_TRUE(q.try_push(in));
  Command out{};
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(std::memcmp(&in, &out, sizeof(Command)), 0);
}

TEST(SPSCQueue, TransportsEventPod) {
  SPSCQueue<Event> q(2);
  Event in{};
  in.kind = EventType::kTraded;
  in.side = Side::kBuy;
  in.reason = RejectReason::kNone;
  in.qty = 40;
  in.remaining = 60;
  in.price_ticks = 9'975;
  in.order_id = 0x0000000300000011ull;  // generation 3, index 17
  ASSERT_TRUE(q.try_push(in));
  Event out{};
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(std::memcmp(&in, &out, sizeof(Event)), 0);
}

// Runtime mirror of the header's layout static_asserts: the shared, producer,
// and consumer regions must occupy disjoint cache lines or the queue false-
// shares under load (task 3 measures exactly this).
TEST(SPSCQueue, LayoutSpansThreeCacheLines) {
  EXPECT_EQ(alignof(SPSCQueue<Event>), 64u);
  EXPECT_EQ(sizeof(SPSCQueue<Event>) % 64, 0u);
  EXPECT_GE(sizeof(SPSCQueue<Event>), 3u * 64u);
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(SPSCQueueDeathTest, NonPowerOfTwoCapacityAsserts) {
  EXPECT_DEATH(SPSCQueue<std::uint64_t> q(3), "power of two");
  EXPECT_DEATH(SPSCQueue<std::uint64_t> q(0), "power of two");
}
#endif

}  // namespace
}  // namespace lob
