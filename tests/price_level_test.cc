#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/order_pool.h"
#include "lob/price_ladder.h"
#include "lob/types.h"

namespace lob {
namespace {

// Allocates an order and gives it a quantity, returning its pool index.
std::uint32_t AllocOrder(OrderPool& pool, Qty qty, Qty remaining) {
  const OrderId id = pool.alloc();
  EXPECT_NE(id, kInvalidOrderId);
  Order& order = *pool.find(id);
  order.qty = qty;
  order.remaining = remaining;
  return index_of(id);
}

std::uint32_t AllocOrder(OrderPool& pool, Qty qty) { return AllocOrder(pool, qty, qty); }

// Walks the FIFO front-to-back via next_idx.
std::vector<std::uint32_t> WalkForward(const OrderPool& pool, const PriceLevel& level) {
  std::vector<std::uint32_t> out;
  for (std::int32_t i = level.head_idx; i != kNullIdx;
       i = pool[static_cast<std::uint32_t>(i)].next_idx) {
    out.push_back(static_cast<std::uint32_t>(i));
  }
  return out;
}

// Walks back-to-front via prev_idx; must mirror WalkForward exactly.
std::vector<std::uint32_t> WalkBackward(const OrderPool& pool, const PriceLevel& level) {
  std::vector<std::uint32_t> out;
  for (std::int32_t i = level.tail_idx; i != kNullIdx;
       i = pool[static_cast<std::uint32_t>(i)].prev_idx) {
    out.push_back(static_cast<std::uint32_t>(i));
  }
  return out;
}

TEST(PriceLevelLayout, SizeAlignmentOffsets) {
  EXPECT_EQ(sizeof(PriceLevel), 24u);
  EXPECT_EQ(alignof(PriceLevel), 8u);
  EXPECT_EQ(offsetof(PriceLevel, head_idx), 0u);
  EXPECT_EQ(offsetof(PriceLevel, tail_idx), 4u);
  EXPECT_EQ(offsetof(PriceLevel, order_count), 8u);
  EXPECT_EQ(offsetof(PriceLevel, total_qty), 16u);
}

TEST(PriceLevel, DefaultIsEmpty) {
  const PriceLevel level;
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.head_idx, kNullIdx);
  EXPECT_EQ(level.tail_idx, kNullIdx);
  EXPECT_EQ(level.order_count, 0u);
  EXPECT_EQ(level.total_qty, 0u);
}

TEST(PriceLevel, PushBackSingleOrder) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t idx = AllocOrder(pool, 10);

  level.push_back(pool, idx);
  EXPECT_FALSE(level.empty());
  EXPECT_EQ(level.head_idx, static_cast<std::int32_t>(idx));
  EXPECT_EQ(level.tail_idx, static_cast<std::int32_t>(idx));
  EXPECT_EQ(level.order_count, 1u);
  EXPECT_EQ(level.total_qty, 10u);
  EXPECT_EQ(pool[idx].prev_idx, kNullIdx);
  EXPECT_EQ(pool[idx].next_idx, kNullIdx);
}

TEST(PriceLevel, FifoOrderPreservedBothDirections) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t a = AllocOrder(pool, 1);
  const std::uint32_t b = AllocOrder(pool, 2);
  const std::uint32_t c = AllocOrder(pool, 3);
  level.push_back(pool, a);
  level.push_back(pool, b);
  level.push_back(pool, c);

  EXPECT_EQ(WalkForward(pool, level), (std::vector<std::uint32_t>{a, b, c}));
  EXPECT_EQ(WalkBackward(pool, level), (std::vector<std::uint32_t>{c, b, a}));
  EXPECT_EQ(level.order_count, 3u);
  EXPECT_EQ(level.total_qty, 6u);
}

TEST(PriceLevel, AggregatesTrackRemainingNotOriginalQty) {
  OrderPool pool(4);
  PriceLevel level;
  // Partially filled order: 10 original, 4 still open.
  const std::uint32_t idx = AllocOrder(pool, 10, 4);
  level.push_back(pool, idx);
  EXPECT_EQ(level.total_qty, 4u);
  level.unlink(pool, idx);
  EXPECT_EQ(level.total_qty, 0u);
}

TEST(PriceLevel, UnlinkHeadRewires) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t a = AllocOrder(pool, 1);
  const std::uint32_t b = AllocOrder(pool, 2);
  const std::uint32_t c = AllocOrder(pool, 3);
  level.push_back(pool, a);
  level.push_back(pool, b);
  level.push_back(pool, c);

  level.unlink(pool, a);
  EXPECT_EQ(WalkForward(pool, level), (std::vector<std::uint32_t>{b, c}));
  EXPECT_EQ(WalkBackward(pool, level), (std::vector<std::uint32_t>{c, b}));
  EXPECT_EQ(level.order_count, 2u);
  EXPECT_EQ(level.total_qty, 5u);
  EXPECT_EQ(pool[a].prev_idx, kNullIdx);
  EXPECT_EQ(pool[a].next_idx, kNullIdx);
}

TEST(PriceLevel, UnlinkMiddleRewires) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t a = AllocOrder(pool, 1);
  const std::uint32_t b = AllocOrder(pool, 2);
  const std::uint32_t c = AllocOrder(pool, 3);
  level.push_back(pool, a);
  level.push_back(pool, b);
  level.push_back(pool, c);

  level.unlink(pool, b);
  EXPECT_EQ(WalkForward(pool, level), (std::vector<std::uint32_t>{a, c}));
  EXPECT_EQ(WalkBackward(pool, level), (std::vector<std::uint32_t>{c, a}));
  EXPECT_EQ(level.total_qty, 4u);
}

TEST(PriceLevel, UnlinkTailRewires) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t a = AllocOrder(pool, 1);
  const std::uint32_t b = AllocOrder(pool, 2);
  const std::uint32_t c = AllocOrder(pool, 3);
  level.push_back(pool, a);
  level.push_back(pool, b);
  level.push_back(pool, c);

  level.unlink(pool, c);
  EXPECT_EQ(WalkForward(pool, level), (std::vector<std::uint32_t>{a, b}));
  EXPECT_EQ(level.tail_idx, static_cast<std::int32_t>(b));
  EXPECT_EQ(level.total_qty, 3u);
}

TEST(PriceLevel, UnlinkOnlyOrderEmptiesLevel) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t idx = AllocOrder(pool, 5);
  level.push_back(pool, idx);
  level.unlink(pool, idx);
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.head_idx, kNullIdx);
  EXPECT_EQ(level.tail_idx, kNullIdx);
  EXPECT_EQ(level.order_count, 0u);
  EXPECT_EQ(level.total_qty, 0u);
}

TEST(PriceLevel, DrainFromFrontPreservesFifo) {
  OrderPool pool(8);
  PriceLevel level;
  std::vector<std::uint32_t> pushed;
  for (int i = 0; i < 5; ++i) {
    pushed.push_back(AllocOrder(pool, 1));
    level.push_back(pool, pushed.back());
  }
  // Pop-front loop, the matching engine's fill pattern.
  std::vector<std::uint32_t> drained;
  while (!level.empty()) {
    const auto front = static_cast<std::uint32_t>(level.head_idx);
    drained.push_back(front);
    level.unlink(pool, front);
  }
  EXPECT_EQ(drained, pushed);
}

TEST(PriceLevel, UnlinkedOrderCanBeRelinkedElsewhere) {
  OrderPool pool(4);
  PriceLevel first;
  PriceLevel second;
  const std::uint32_t anchor = AllocOrder(pool, 1);
  const std::uint32_t mover = AllocOrder(pool, 2);
  first.push_back(pool, anchor);
  first.push_back(pool, mover);

  first.unlink(pool, mover);
  second.push_back(pool, mover);
  EXPECT_EQ(WalkForward(pool, first), (std::vector<std::uint32_t>{anchor}));
  EXPECT_EQ(WalkForward(pool, second), (std::vector<std::uint32_t>{mover}));
  EXPECT_EQ(first.total_qty, 1u);
  EXPECT_EQ(second.total_qty, 2u);
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(PriceLevelDeathTest, PushBackAlreadyLinkedAsserts) {
  OrderPool pool(4);
  PriceLevel level;
  const std::uint32_t a = AllocOrder(pool, 1);
  const std::uint32_t b = AllocOrder(pool, 2);
  level.push_back(pool, a);
  level.push_back(pool, b);
  EXPECT_DEATH(level.push_back(pool, a), "already linked");
}
#endif

}  // namespace
}  // namespace lob
