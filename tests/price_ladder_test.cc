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
  ASSERT_NE(ladder.find_level(kAnchor), nullptr);  // in-band levels always exist
  EXPECT_EQ(ladder.find_level(kAnchor)->order_count, 0u);
  EXPECT_EQ(ladder.find_level(kAnchor + kRadius + 50), nullptr);  // no overflow resting
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
  EXPECT_EQ(ladder.find_level(kAnchor - kRadius)->order_count, 0u);
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

// ---------------------------------------------------------------------------
// Overflow region: out-of-band prices resting in the fallback map (task 5).
// ---------------------------------------------------------------------------

TEST(PriceLadderOverflow, OutOfBandPushRoutesToOverflow) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const PriceTicks oob = kAnchor + kRadius + 100;
  const std::uint32_t idx = Push(ladder, pool, oob, 5);

  EXPECT_EQ(pool[idx].level_idx, kOverflowIdx);
  ASSERT_NE(ladder.find_level(oob), nullptr);
  EXPECT_EQ(ladder.find_level(oob)->total_qty, 5u);
  EXPECT_FALSE(ladder.empty());
}

TEST(PriceLadderOverflow, BetterSideOverflowBeatsBandBest) {
  OrderPool pool(8);
  // Bids: an out-of-band price above the band outranks everything in band.
  PriceLadder bids(Side::kBuy, kAnchor, kRadius);
  Push(bids, pool, kAnchor);
  Push(bids, pool, kAnchor + kRadius + 10);
  EXPECT_EQ(bids.best_price(), kAnchor + kRadius + 10);
  EXPECT_EQ(bids.best_level().order_count, 1u);

  // Asks: an out-of-band price below the band outranks everything in band.
  PriceLadder asks(Side::kSell, kAnchor, kRadius);
  Push(asks, pool, kAnchor);
  Push(asks, pool, kAnchor - kRadius - 10);
  EXPECT_EQ(asks.best_price(), kAnchor - kRadius - 10);
}

TEST(PriceLadderOverflow, WorseSideOverflowDoesNotDisplaceBandBest) {
  OrderPool pool(8);
  PriceLadder bids(Side::kBuy, kAnchor, kRadius);
  Push(bids, pool, kAnchor);
  Push(bids, pool, kAnchor - kRadius - 10);  // deep out-of-band bid: worse
  EXPECT_EQ(bids.best_price(), kAnchor);

  PriceLadder asks(Side::kSell, kAnchor, kRadius);
  Push(asks, pool, kAnchor);
  Push(asks, pool, kAnchor + kRadius + 10);
  EXPECT_EQ(asks.best_price(), kAnchor);
}

TEST(PriceLadderOverflow, EmptiedOverflowLevelIsErased) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kBuy, kAnchor, kRadius);
  const PriceTicks oob = kAnchor + kRadius + 7;
  const std::uint32_t a = Push(ladder, pool, oob, 1);
  const std::uint32_t b = Push(ladder, pool, oob, 2);

  ladder.remove_order(pool, a);
  ASSERT_NE(ladder.find_level(oob), nullptr);  // still one order resting
  EXPECT_EQ(ladder.find_level(oob)->total_qty, 2u);
  EXPECT_EQ(pool[a].level_idx, kNullIdx);

  ladder.remove_order(pool, b);
  EXPECT_EQ(ladder.find_level(oob), nullptr);  // node erased, not left empty
  EXPECT_TRUE(ladder.empty());
}

TEST(PriceLadderOverflow, FifoPreservedWithinOverflowLevel) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  const PriceTicks oob = kAnchor - kRadius - 3;
  const std::uint32_t a = Push(ladder, pool, oob, 1);
  const std::uint32_t b = Push(ladder, pool, oob, 2);

  const PriceLevel& level = *ladder.find_level(oob);
  EXPECT_EQ(level.head_idx, static_cast<std::int32_t>(a));
  EXPECT_EQ(level.tail_idx, static_cast<std::int32_t>(b));
  EXPECT_EQ(pool[a].next_idx, static_cast<std::int32_t>(b));
}

TEST(PriceLadderOverflow, BestCascadesAcrossBandAndBothOverflowSides) {
  OrderPool pool(8);
  PriceLadder bids(Side::kBuy, kAnchor, kRadius);
  const std::uint32_t above = Push(bids, pool, kAnchor + kRadius + 20);
  const std::uint32_t in_band = Push(bids, pool, kAnchor);
  const std::uint32_t below = Push(bids, pool, kAnchor - kRadius - 20);

  EXPECT_EQ(bids.best_price(), kAnchor + kRadius + 20);
  bids.remove_order(pool, above);
  EXPECT_EQ(bids.best_price(), kAnchor);  // falls back to the band
  bids.remove_order(pool, in_band);
  EXPECT_EQ(bids.best_price(), kAnchor - kRadius - 20);  // then worse overflow
  bids.remove_order(pool, below);
  EXPECT_TRUE(bids.empty());
}

TEST(PriceLadderOverflow, OnlyOverflowOrdersStillCountAsNonEmpty) {
  OrderPool pool(8);
  PriceLadder ladder(Side::kSell, kAnchor, kRadius);
  const std::uint32_t idx = Push(ladder, pool, kAnchor + kRadius + 1);
  EXPECT_FALSE(ladder.empty());
  EXPECT_EQ(ladder.best_price(), kAnchor + kRadius + 1);
  ladder.remove_order(pool, idx);
  EXPECT_TRUE(ladder.empty());
}

}  // namespace
}  // namespace lob
