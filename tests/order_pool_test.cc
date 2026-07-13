#include "lob/order_pool.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include "lob/types.h"

namespace lob {
namespace {

TEST(OrderLayout, IsOneCacheLine) {
  EXPECT_EQ(sizeof(Order), 64u);
  EXPECT_EQ(alignof(Order), 64u);
}

TEST(OrderIdEncoding, HelpersRoundTrip) {
  const OrderId id = make_order_id(0xDEADBEEFu, 0x0BADF00Du);
  EXPECT_EQ(generation_of(id), 0xDEADBEEFu);
  EXPECT_EQ(index_of(id), 0x0BADF00Du);
  EXPECT_EQ(make_order_id(0, 0), kInvalidOrderId);
}

TEST(OrderPool, NewPoolIsEmpty) {
  const OrderPool pool(8);
  EXPECT_EQ(pool.capacity(), 8u);
  EXPECT_EQ(pool.live_count(), 0u);
  EXPECT_EQ(pool.find(kInvalidOrderId), nullptr);
}

TEST(OrderPool, FirstAllocationsAreSequentialWithGenerationOne) {
  OrderPool pool(4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    const OrderId id = pool.alloc();
    ASSERT_NE(id, kInvalidOrderId);
    EXPECT_EQ(index_of(id), i);
    EXPECT_EQ(generation_of(id), 1u);
  }
  EXPECT_EQ(pool.live_count(), 4u);
}

TEST(OrderPool, FindReturnsTheLiveSlot) {
  OrderPool pool(4);
  const OrderId id = pool.alloc();
  Order* order = pool.find(id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_id, id);
  EXPECT_EQ(order, &pool[index_of(id)]);
}

TEST(OrderPool, AllocResetsLinkFields) {
  OrderPool pool(2);
  const OrderId first = pool.alloc();
  Order& order = *pool.find(first);
  order.prev_idx = 7;
  order.next_idx = 8;
  order.level_idx = 9;
  pool.free(index_of(first));

  const OrderId second = pool.alloc();
  const Order& reused = *pool.find(second);
  EXPECT_EQ(reused.prev_idx, kNullIdx);
  EXPECT_EQ(reused.next_idx, kNullIdx);
  EXPECT_EQ(reused.level_idx, kNullIdx);
}

TEST(OrderPool, FreedIdGoesStale) {
  OrderPool pool(4);
  const OrderId id = pool.alloc();
  EXPECT_EQ(pool.live_count(), 1u);
  pool.free(index_of(id));
  EXPECT_EQ(pool.live_count(), 0u);
  EXPECT_EQ(pool.find(id), nullptr);
}

TEST(OrderPool, FreedSlotIsReusedLifoWithBumpedGeneration) {
  OrderPool pool(4);
  const OrderId a = pool.alloc();
  const OrderId b = pool.alloc();
  pool.free(index_of(a));

  const OrderId reused = pool.alloc();
  EXPECT_EQ(index_of(reused), index_of(a));  // LIFO: the just-freed slot first
  EXPECT_EQ(generation_of(reused), generation_of(a) + 1);
  EXPECT_NE(reused, a);
  EXPECT_EQ(pool.find(a), nullptr);  // the stale id stays dead
  ASSERT_NE(pool.find(reused), nullptr);
  ASSERT_NE(pool.find(b), nullptr);
}

TEST(OrderPool, ExhaustionReturnsInvalidIdAndFreeingRecovers) {
  OrderPool pool(3);
  std::vector<OrderId> ids;
  for (int i = 0; i < 3; ++i) {
    ids.push_back(pool.alloc());
    ASSERT_NE(ids.back(), kInvalidOrderId);
  }
  EXPECT_EQ(pool.alloc(), kInvalidOrderId);
  EXPECT_EQ(pool.live_count(), 3u);

  pool.free(index_of(ids[1]));
  const OrderId recovered = pool.alloc();
  EXPECT_NE(recovered, kInvalidOrderId);
  EXPECT_EQ(index_of(recovered), index_of(ids[1]));
  EXPECT_EQ(pool.alloc(), kInvalidOrderId);
}

TEST(OrderPool, ZeroCapacityPoolIsAlwaysExhausted) {
  OrderPool pool(0);
  EXPECT_EQ(pool.capacity(), 0u);
  EXPECT_EQ(pool.alloc(), kInvalidOrderId);
}

TEST(OrderPool, FindRejectsForgedIds) {
  OrderPool pool(4);
  const OrderId id = pool.alloc();
  // Right index, wrong generation.
  EXPECT_EQ(pool.find(make_order_id(generation_of(id) + 1, index_of(id))), nullptr);
  // Bare index with zero generation (never minted).
  EXPECT_EQ(pool.find(static_cast<OrderId>(index_of(id))), nullptr);
  // Index beyond capacity.
  EXPECT_EQ(pool.find(make_order_id(1, 100)), nullptr);
}

TEST(OrderPool, GenerationWrapSkipsZero) {
  OrderPool pool(1);
  const OrderId id = pool.alloc();
  pool.free(index_of(id));
  // White-box: fast-forward the freed slot to the last generation and check
  // the next alloc wraps to 1, never minting a generation-0 (invalid-able) id.
  pool[0].generation = std::numeric_limits<std::uint32_t>::max();
  const OrderId wrapped = pool.alloc();
  EXPECT_EQ(generation_of(wrapped), 1u);
  EXPECT_NE(wrapped, kInvalidOrderId);
}

TEST(OrderPool, ChurnMintsGloballyUniqueIds) {
  OrderPool pool(8);
  std::unordered_set<OrderId> seen;
  std::vector<OrderId> live;
  for (int cycle = 0; cycle < 10'000; ++cycle) {
    const OrderId id = pool.alloc();
    ASSERT_NE(id, kInvalidOrderId);
    EXPECT_TRUE(seen.insert(id).second) << "id reused across generations";
    live.push_back(id);
    ASSERT_EQ(pool.live_count(), live.size());
    // Drain every 8th cycle so alloc exercises both fresh and reused slots.
    if (live.size() == pool.capacity()) {
      for (const OrderId dead : live) {
        pool.free(index_of(dead));
      }
      live.clear();
    }
  }
  EXPECT_LE(pool.live_count(), pool.capacity());
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(OrderPoolDeathTest, DoubleFreeAsserts) {
  OrderPool pool(2);
  const OrderId id = pool.alloc();
  pool.free(index_of(id));
  EXPECT_DEATH(pool.free(index_of(id)), "not live");
}
#endif

}  // namespace
}  // namespace lob
