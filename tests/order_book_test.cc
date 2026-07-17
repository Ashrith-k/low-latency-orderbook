#include "lob/order_book.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <unordered_set>

#include "lob/types.h"

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
constexpr std::uint32_t kRadius = 16;

OrderBook MakeBook(std::uint32_t pool_capacity = 32) {
  return OrderBook(kAnchor, kRadius, pool_capacity);
}

TEST(OrderBookAdd, EmptyBookHasNoBestAndNoOrders) {
  const OrderBook book = MakeBook();
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor), 0u);
  EXPECT_EQ(book.orders_at(Side::kSell, kAnchor), 0u);
}

TEST(OrderBookAdd, RestsBid) {
  OrderBook book = MakeBook();
  const OrderId id = book.add_limit(Side::kBuy, kAnchor - 1, 10);

  ASSERT_NE(id, kInvalidOrderId);
  EXPECT_TRUE(book.contains(id));
  EXPECT_EQ(book.best_bid(), kAnchor - 1);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 1), 10u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor - 1), 1u);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookAdd, RestsAsk) {
  OrderBook book = MakeBook();
  const OrderId id = book.add_limit(Side::kSell, kAnchor + 1, 7);

  ASSERT_NE(id, kInvalidOrderId);
  EXPECT_EQ(book.best_ask(), kAnchor + 1);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 7u);
}

TEST(OrderBookAdd, RejectsZeroQty) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add_limit(Side::kBuy, kAnchor, 0), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBookAdd, RejectsNonPositivePrice) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add_limit(Side::kSell, 0, 5), kInvalidOrderId);
  EXPECT_EQ(book.add_limit(Side::kSell, -3, 5), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookAdd, PoolExhaustionRejectsAndLeavesBookIntact) {
  OrderBook book = MakeBook(/*pool_capacity=*/2);
  const OrderId a = book.add_limit(Side::kBuy, kAnchor - 1, 1);
  const OrderId b = book.add_limit(Side::kSell, kAnchor + 1, 2);
  ASSERT_NE(a, kInvalidOrderId);
  ASSERT_NE(b, kInvalidOrderId);

  EXPECT_EQ(book.add_limit(Side::kBuy, kAnchor - 2, 3), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 2u);
  EXPECT_EQ(book.best_bid(), kAnchor - 1);
  EXPECT_EQ(book.best_ask(), kAnchor + 1);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 2), 0u);
}

TEST(OrderBookAdd, BestTracksBetterPrices) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor - 5, 1);
  book.add_limit(Side::kBuy, kAnchor - 2, 1);  // better bid
  book.add_limit(Side::kBuy, kAnchor - 8, 1);  // worse bid
  EXPECT_EQ(book.best_bid(), kAnchor - 2);

  book.add_limit(Side::kSell, kAnchor + 5, 1);
  book.add_limit(Side::kSell, kAnchor + 2, 1);  // better ask
  book.add_limit(Side::kSell, kAnchor + 8, 1);  // worse ask
  EXPECT_EQ(book.best_ask(), kAnchor + 2);
}

TEST(OrderBookAdd, SamePriceAggregates) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor, 10);
  book.add_limit(Side::kBuy, kAnchor, 20);
  book.add_limit(Side::kBuy, kAnchor, 30);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor), 60u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor), 3u);
  EXPECT_EQ(book.open_orders(), 3u);
}

// Pre-matching semantics: crossing prices rest, exactly like
// NaiveBook::add_limit — the add/cancel differential harness depends on it.
TEST(OrderBookAdd, CrossedPricesRestWithoutMatching) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor + 5, 10);
  book.add_limit(Side::kSell, kAnchor - 5, 10);

  EXPECT_EQ(book.best_bid(), kAnchor + 5);
  EXPECT_EQ(book.best_ask(), kAnchor - 5);
  EXPECT_EQ(book.open_orders(), 2u);
}

TEST(OrderBookAdd, OutOfBandPricesRestViaOverflow) {
  OrderBook book = MakeBook();
  const PriceTicks deep_bid = kAnchor - kRadius - 200;
  const PriceTicks high_bid = kAnchor + kRadius + 200;
  book.add_limit(Side::kBuy, deep_bid, 4);
  const OrderId top = book.add_limit(Side::kBuy, high_bid, 6);

  ASSERT_NE(top, kInvalidOrderId);
  EXPECT_EQ(book.qty_at(Side::kBuy, deep_bid), 4u);
  EXPECT_EQ(book.qty_at(Side::kBuy, high_bid), 6u);
  EXPECT_EQ(book.best_bid(), high_bid);  // better-side overflow wins
}

TEST(OrderBookAdd, MintedIdsAreUnique) {
  OrderBook book = MakeBook();
  std::unordered_set<OrderId> ids;
  for (int i = 0; i < 20; ++i) {
    const OrderId id = book.add_limit(Side::kBuy, kAnchor - (i % 4), 1);
    ASSERT_NE(id, kInvalidOrderId);
    EXPECT_TRUE(ids.insert(id).second);
  }
}

// ---------------------------------------------------------------------------
// Cancel (task 7): O(1) removal via the id's pool index, no hash map.
// ---------------------------------------------------------------------------

TEST(OrderBookCancel, RemovesRestingOrder) {
  OrderBook book = MakeBook();
  const OrderId id = book.add_limit(Side::kBuy, kAnchor - 1, 10);

  EXPECT_TRUE(book.cancel(id));
  EXPECT_FALSE(book.contains(id));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 1), 0u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor - 1), 0u);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookCancel, UnknownIdsAreRejected) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kSell, kAnchor + 1, 5);

  EXPECT_FALSE(book.cancel(kInvalidOrderId));
  EXPECT_FALSE(book.cancel(make_order_id(1, 100)));  // index beyond capacity
  EXPECT_FALSE(book.cancel(make_order_id(99, 0)));   // forged generation
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookCancel, DoubleCancelIsRejected) {
  OrderBook book = MakeBook();
  const OrderId id = book.add_limit(Side::kBuy, kAnchor, 5);
  EXPECT_TRUE(book.cancel(id));
  EXPECT_FALSE(book.cancel(id));
  EXPECT_EQ(book.open_orders(), 0u);
}

// The generation bits' whole purpose: a stale id must not cancel the
// unrelated order that now occupies its recycled pool slot.
TEST(OrderBookCancel, StaleIdAfterSlotReuseIsRejected) {
  OrderBook book = MakeBook();
  const OrderId old_id = book.add_limit(Side::kBuy, kAnchor - 1, 10);
  EXPECT_TRUE(book.cancel(old_id));

  const OrderId new_id = book.add_limit(Side::kSell, kAnchor + 1, 20);
  ASSERT_EQ(index_of(new_id), index_of(old_id));  // LIFO free list reused the slot

  EXPECT_FALSE(book.cancel(old_id));
  EXPECT_TRUE(book.contains(new_id));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 20u);
}

TEST(OrderBookCancel, BestFallsToNextLevel) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor - 5, 1);
  const OrderId best_bid = book.add_limit(Side::kBuy, kAnchor - 1, 1);
  book.add_limit(Side::kSell, kAnchor + 5, 1);
  const OrderId best_ask = book.add_limit(Side::kSell, kAnchor + 1, 1);

  EXPECT_TRUE(book.cancel(best_bid));
  EXPECT_EQ(book.best_bid(), kAnchor - 5);
  EXPECT_TRUE(book.cancel(best_ask));
  EXPECT_EQ(book.best_ask(), kAnchor + 5);
}

TEST(OrderBookCancel, MiddleOfFifoLeavesRestIntact) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor, 1);
  const OrderId middle = book.add_limit(Side::kBuy, kAnchor, 2);
  book.add_limit(Side::kBuy, kAnchor, 4);

  EXPECT_TRUE(book.cancel(middle));
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor), 5u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor), 2u);
  EXPECT_EQ(book.best_bid(), kAnchor);
}

TEST(OrderBookCancel, OverflowOrderCancelErasesLevel) {
  OrderBook book = MakeBook();
  const PriceTicks oob = kAnchor + kRadius + 100;
  book.add_limit(Side::kBuy, kAnchor, 1);
  const OrderId top = book.add_limit(Side::kBuy, oob, 6);
  EXPECT_EQ(book.best_bid(), oob);

  EXPECT_TRUE(book.cancel(top));
  EXPECT_EQ(book.qty_at(Side::kBuy, oob), 0u);
  EXPECT_EQ(book.best_bid(), kAnchor);
}

TEST(OrderBookCancel, FreedSlotIsReusable) {
  OrderBook book = MakeBook(/*pool_capacity=*/1);
  const OrderId first = book.add_limit(Side::kBuy, kAnchor, 1);
  ASSERT_NE(first, kInvalidOrderId);
  EXPECT_EQ(book.add_limit(Side::kBuy, kAnchor, 1), kInvalidOrderId);  // exhausted

  EXPECT_TRUE(book.cancel(first));
  const OrderId second = book.add_limit(Side::kSell, kAnchor + 2, 3);
  ASSERT_NE(second, kInvalidOrderId);
  EXPECT_EQ(book.best_ask(), kAnchor + 2);
}

// ---------------------------------------------------------------------------
// Matching (Day 3 task 1): aggressive limit orders sweep the opposite side
// best price first, FIFO within a level, partial fills; the remainder rests.
// NaiveBook::add is the executable spec; until task 3 emits events, these
// tests verify through book state alone.
// ---------------------------------------------------------------------------

TEST(OrderBookMatch, NonCrossingLimitRests) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 10);
  const OrderId bid = book.add(Side::kBuy, OrderType::kLimit, kAnchor - 2, 5);

  ASSERT_NE(bid, kInvalidOrderId);
  EXPECT_TRUE(book.contains(bid));
  EXPECT_EQ(book.best_bid(), kAnchor - 2);
  EXPECT_EQ(book.best_ask(), kAnchor + 2);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 2), 10u);
  EXPECT_EQ(book.open_orders(), 2u);
}

TEST(OrderBookMatch, ValidationRejects) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kLimit, kAnchor, 0), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kLimit, 0, 5), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kLimit, -3, 5), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
}

// A taker at exactly the best opposing price crosses (<=, not <).
TEST(OrderBookMatch, ExactFillRemovesMakerAndTaker) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 10);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(maker));
  EXPECT_FALSE(book.contains(taker));  // fully filled, never rested
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.orders_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookMatch, PartialFillLeavesMakerRemainder) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 3, 4);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_TRUE(book.contains(maker));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 6u);
  EXPECT_EQ(book.orders_at(Side::kSell, kAnchor + 1), 1u);
  EXPECT_EQ(book.best_ask(), kAnchor + 1);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookMatch, TakerRemainderRestsAtLimitPrice) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 2, 15);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_TRUE(book.contains(taker));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), kAnchor + 2);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor + 2), 5u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor + 2), 1u);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookMatch, FifoWithinLevel) {
  OrderBook book = MakeBook();
  const OrderId first = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  const OrderId second = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 7);
  const OrderId third = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 9);

  book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 8);  // 5 fill first, 3 partial second

  EXPECT_FALSE(book.contains(first));
  EXPECT_TRUE(book.contains(second));
  EXPECT_TRUE(book.contains(third));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 13u);  // 4 + 9
  EXPECT_EQ(book.orders_at(Side::kSell, kAnchor + 1), 2u);
}

TEST(OrderBookMatch, SweepsLevelsBestFirstAndStopsAtLimit) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 5);
  const OrderId untouched = book.add(Side::kSell, OrderType::kLimit, kAnchor + 4, 5);

  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 2, 12);

  // 5 @ +1 then 5 @ +2 fill; +4 does not cross, so the remainder 2 rests.
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 2), 0u);
  EXPECT_TRUE(book.contains(untouched));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 4), 5u);
  EXPECT_EQ(book.best_ask(), kAnchor + 4);
  EXPECT_TRUE(book.contains(taker));
  EXPECT_EQ(book.best_bid(), kAnchor + 2);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor + 2), 2u);
  EXPECT_EQ(book.open_orders(), 2u);
}

TEST(OrderBookMatch, SellAggressorSweepsBidsMirror) {
  OrderBook book = MakeBook();
  book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 5);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor - 2, 5);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor - 4, 5);

  const OrderId taker = book.add(Side::kSell, OrderType::kLimit, kAnchor - 2, 12);

  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 1), 0u);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 2), 0u);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 4), 5u);
  EXPECT_EQ(book.best_bid(), kAnchor - 4);
  EXPECT_TRUE(book.contains(taker));
  EXPECT_EQ(book.best_ask(), kAnchor - 2);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor - 2), 2u);
}

TEST(OrderBookMatch, FilledMakerIdIsStaleForCancel) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 5);

  EXPECT_FALSE(book.cancel(maker));
  EXPECT_EQ(book.open_orders(), 0u);
}

// Exercises the by-hand total_qty debit: if the partial fill forgot it, this
// cancel would leave phantom quantity (or trip unlink's aggregate assert).
TEST(OrderBookMatch, CancelAfterPartialFillRemovesRemainder) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 4);

  ASSERT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 6u);
  EXPECT_TRUE(book.cancel(maker));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.orders_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookMatch, RestedRemainderIsMatchableAndCancelable) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 2, 15);  // 5 rest @ +2

  // The rested remainder is a first-class resting order: it can be hit...
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 3);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor + 2), 2u);
  // ...and canceled.
  EXPECT_TRUE(book.cancel(taker));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
}

// The slot is minted before the sweep, so a full pool rejects even an order
// that would have fully filled without resting.
TEST(OrderBookMatch, PoolExhaustionRejectsBeforeMatching) {
  OrderBook book = MakeBook(/*pool_capacity=*/1);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);

  EXPECT_EQ(book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 10), kInvalidOrderId);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 10u);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookMatch, FilledSlotsAreRecycled) {
  OrderBook book = MakeBook(/*pool_capacity=*/2);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 5);
  ASSERT_NE(taker, kInvalidOrderId);
  ASSERT_EQ(book.open_orders(), 0u);

  // Maker and taker slots are both free again: two fresh orders fit.
  EXPECT_NE(book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 1), kInvalidOrderId);
  EXPECT_NE(book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 1), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 2u);
}

}  // namespace
}  // namespace lob
