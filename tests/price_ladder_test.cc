#include "lob/price_ladder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/order_pool.h"
#include "lob/types.h"

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
constexpr std::uint32_t kRadius = 16;  // small band keeps edge tests readable

// Allocates an order at a price, returning its pool index.
std::uint32_t AllocOrder(OrderPool& pool, Side side, PriceTicks price, Qty qty) {
  const OrderId id = pool.alloc();
  EXPECT_NE(id, kInvalidOrderId);
  Order& order = *pool.find(id);
  order.price_ticks = price;
  order.qty = qty;
  order.remaining = qty;
  order.side = side;
  order.type = OrderType::kLimit;
  return index_of(id);
}

// Allocates and links in one step; returns the pool index.
std::uint32_t Push(PriceLadder& ladder, OrderPool& pool, PriceTicks price, Qty qty = 1) {
  const std::uint32_t idx = AllocOrder(pool, ladder.side(), price, qty);
  ladder.push_order(pool, idx);
  return idx;
}

TEST(PriceLadder, BandBoundsAreInclusiveOfBothEdges) {
  const PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  EXPECT_TRUE(ladder.in_band(kAnchor));
  EXPECT_TRUE(ladder.in_band(kAnchor - kRadius));
  EXPECT_TRUE(ladder.in_band(kAnchor + kRadius));
  EXPECT_FALSE(ladder.in_band(kAnchor - kRadius - 1));
  EXPECT_FALSE(ladder.in_band(kAnchor + kRadius + 1));
}

TEST(PriceLadder, NewLadderIsEmpty) {
  const PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  EXPECT_TRUE(ladder.empty());
  EXPECT_EQ(ladder.level_at(kAnchor).order_count, 0u);
}

TEST(PriceLadder, PushSetsBestAndLevelIdx) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t idx = Push(ladder, pool, kAnchor + 3, 10);

  EXPECT_FALSE(ladder.empty());
  EXPECT_EQ(ladder.best_price(), kAnchor + 3);
  EXPECT_EQ(ladder.best_level().total_qty, 10u);
  // level_idx is the band offset of the order's price.
  EXPECT_EQ(pool[idx].level_idx, static_cast<std::int32_t>(kRadius + 3));
}

TEST(PriceLadder, BidsBestIsHighestPrice) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  Push(ladder, pool, kAnchor);
  EXPECT_EQ(ladder.best_price(), kAnchor);
  Push(ladder, pool, kAnchor + 5);  // better
  EXPECT_EQ(ladder.best_price(), kAnchor + 5);
  Push(ladder, pool, kAnchor + 2);  // worse than best: no move
  EXPECT_EQ(ladder.best_price(), kAnchor + 5);
}

TEST(PriceLadder, AsksBestIsLowestPrice) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  Push(ladder, pool, kAnchor);
  EXPECT_EQ(ladder.best_price(), kAnchor);
  Push(ladder, pool, kAnchor - 5);  // better
  EXPECT_EQ(ladder.best_price(), kAnchor - 5);
  Push(ladder, pool, kAnchor - 2);  // worse than best: no move
  EXPECT_EQ(ladder.best_price(), kAnchor - 5);
}

TEST(PriceLadder, RemoveNonBestLeavesBestAlone) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t worse = Push(ladder, pool, kAnchor - 4);
  Push(ladder, pool, kAnchor + 4);

  ladder.remove_order(pool, worse);
  EXPECT_EQ(ladder.best_price(), kAnchor + 4);
  EXPECT_EQ(pool[worse].level_idx, kNullIdx);
}

TEST(PriceLadder, RemoveFromBestLevelWithDepthKeepsBest) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t first = Push(ladder, pool, kAnchor, 3);
  Push(ladder, pool, kAnchor, 7);  // same level, still non-empty after removal

  ladder.remove_order(pool, first);
  EXPECT_EQ(ladder.best_price(), kAnchor);
  EXPECT_EQ(ladder.best_level().total_qty, 7u);
}

TEST(PriceLadder, EmptiedBestScansDownAcrossGapForBids) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  Push(ladder, pool, kAnchor - 6);  // next best, 6 ticks of empty gap between
  const std::uint32_t best = Push(ladder, pool, kAnchor);

  ladder.remove_order(pool, best);
  EXPECT_EQ(ladder.best_price(), kAnchor - 6);
}

TEST(PriceLadder, EmptiedBestScansUpAcrossGapForAsks) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  Push(ladder, pool, kAnchor + 6);
  const std::uint32_t best = Push(ladder, pool, kAnchor);

  ladder.remove_order(pool, best);
  EXPECT_EQ(ladder.best_price(), kAnchor + 6);
}

TEST(PriceLadder, DrainToEmptyAndReuse) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  const std::uint32_t a = Push(ladder, pool, kAnchor - 1);
  const std::uint32_t b = Push(ladder, pool, kAnchor + 1);

  ladder.remove_order(pool, a);
  ladder.remove_order(pool, b);
  EXPECT_TRUE(ladder.empty());

  Push(ladder, pool, kAnchor + 2);
  EXPECT_EQ(ladder.best_price(), kAnchor + 2);
}

TEST(PriceLadder, WorksAtBandEdges) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t low = Push(ladder, pool, kAnchor - kRadius);
  Push(ladder, pool, kAnchor + kRadius);

  EXPECT_EQ(ladder.best_price(), kAnchor + kRadius);
  EXPECT_EQ(pool[low].level_idx, 0);
  ladder.remove_order(pool, low);
  EXPECT_EQ(ladder.level_at(kAnchor - kRadius).order_count, 0u);
}

TEST(PriceLadder, EmptyingTheWholeBandEdgeToEdge) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  // Best at the low edge; emptying it must scan the full band up to the high
  // edge, then removing that too must leave the side empty (cursor hits end).
  const std::uint32_t lowest = Push(ladder, pool, kAnchor - kRadius);
  const std::uint32_t highest = Push(ladder, pool, kAnchor + kRadius);

  ladder.remove_order(pool, lowest);
  EXPECT_EQ(ladder.best_price(), kAnchor + kRadius);
  ladder.remove_order(pool, highest);
  EXPECT_TRUE(ladder.empty());
}

TEST(PriceLadder, FifoPreservedWithinALevel) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t a = Push(ladder, pool, kAnchor, 1);
  const std::uint32_t b = Push(ladder, pool, kAnchor, 2);
  const std::uint32_t c = Push(ladder, pool, kAnchor, 3);

  const PriceLevel& level = ladder.best_level();
  EXPECT_EQ(level.head_idx, static_cast<std::int32_t>(a));
  EXPECT_EQ(pool[a].next_idx, static_cast<std::int32_t>(b));
  EXPECT_EQ(pool[b].next_idx, static_cast<std::int32_t>(c));
  EXPECT_EQ(level.tail_idx, static_cast<std::int32_t>(c));
  EXPECT_EQ(level.total_qty, 6u);
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(PriceLadderDeathTest, OutOfBandPushAsserts) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t idx = AllocOrder(pool, Side::kBuy, kAnchor + kRadius + 1, 1);
  EXPECT_DEATH(ladder.push_order(pool, idx), "out-of-band");
}
#endif

}  // namespace
}  // namespace lob
