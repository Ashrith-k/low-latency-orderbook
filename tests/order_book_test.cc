#include "lob/order_book.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "event_test_util.h"
#include "lob/naive_book.h"
#include "lob/types.h"

namespace lob {
namespace {

using namespace testutil;  // NOLINT(google-build-using-namespace): test-local

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

// ---------------------------------------------------------------------------
// Market + IOC (Day 3 task 2): market sweeps at any price and ignores the
// price argument; IOC sweeps within its limit; neither ever rests — the
// remainder is dropped (Canceled events land with task 3).
// ---------------------------------------------------------------------------

TEST(OrderBookMarketIoc, MarketBuySweepsBestFirstAndNeverRests) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  const OrderId far = book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 5);

  const OrderId taker = book.add(Side::kBuy, OrderType::kMarket, 0, 8);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 2), 2u);  // 5 @ +1, then 3 of 5 @ +2
  EXPECT_TRUE(book.contains(far));
  EXPECT_EQ(book.best_ask(), kAnchor + 2);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookMarketIoc, MarketSellSweepsBidsMirror) {
  OrderBook book = MakeBook();
  book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 5);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor - 2, 5);

  const OrderId taker = book.add(Side::kSell, OrderType::kMarket, 0, 8);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 1), 0u);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 2), 2u);
  EXPECT_EQ(book.best_bid(), kAnchor - 2);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

// The price argument is ignored entirely: negative, zero, and "non-crossing"
// prices all sweep the same. (NaiveBook: market ignores price.)
TEST(OrderBookMarketIoc, MarketIgnoresPriceArgument) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 5, 6);

  // A limit buy at kAnchor - 5 would not cross; a market buy carrying that
  // price sweeps anyway. So does one with a negative price.
  const OrderId low = book.add(Side::kBuy, OrderType::kMarket, kAnchor - 5, 2);
  ASSERT_NE(low, kInvalidOrderId);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 5), 4u);

  const OrderId negative = book.add(Side::kBuy, OrderType::kMarket, -7, 2);
  ASSERT_NE(negative, kInvalidOrderId);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 5), 2u);
}

// Lack of liquidity is acceptance-then-cancel, not a reject: the id is real,
// the book untouched.
TEST(OrderBookMarketIoc, MarketOnEmptyBookIsAcceptedNotRejected) {
  OrderBook book = MakeBook();
  const OrderId taker = book.add(Side::kBuy, OrderType::kMarket, 0, 10);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
  EXPECT_FALSE(book.cancel(taker));  // never rested, permanently stale
}

TEST(OrderBookMarketIoc, MarketRemainderVanishesAfterSweepingAllLiquidity) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 3, 5);

  const OrderId taker = book.add(Side::kBuy, OrderType::kMarket, 0, 25);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), std::nullopt);  // remainder 15 dropped, nothing rests
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookMarketIoc, MarketZeroQtyRejected) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kMarket, 0, 0), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookMarketIoc, IocFillsWithinLimitAndRemainderIsDropped) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 5);
  const OrderId untouched = book.add(Side::kSell, OrderType::kLimit, kAnchor + 4, 5);

  const OrderId taker = book.add(Side::kBuy, OrderType::kIoc, kAnchor + 2, 12);

  // Fills 5 @ +1 and 5 @ +2 exactly like a limit; +4 does not cross; the
  // remainder 2 is dropped instead of resting.
  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 0u);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 2), 0u);
  EXPECT_TRUE(book.contains(untouched));
  EXPECT_EQ(book.best_ask(), kAnchor + 4);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookMarketIoc, IocPartialMakerFillLeavesRemainderResting) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);

  const OrderId taker = book.add(Side::kBuy, OrderType::kIoc, kAnchor + 1, 4);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_TRUE(book.contains(maker));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 6u);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookMarketIoc, IocNonCrossingIsAcceptedAndDropped) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 5, 10);

  const OrderId taker = book.add(Side::kBuy, OrderType::kIoc, kAnchor - 5, 4);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 5), 10u);  // untouched
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 1u);
}

// IOC validates price exactly like a limit — only market ignores it.
TEST(OrderBookMarketIoc, IocValidationRejects) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kIoc, kAnchor, 0), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kIoc, 0, 5), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kIoc, -3, 5), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookMarketIoc, NeverRestingTakerSlotsAreRecycled) {
  OrderBook book = MakeBook(/*pool_capacity=*/2);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);

  // Each taker borrows the second slot and returns it.
  ASSERT_NE(book.add(Side::kBuy, OrderType::kMarket, 0, 2), kInvalidOrderId);
  ASSERT_NE(book.add(Side::kBuy, OrderType::kIoc, kAnchor + 1, 2), kInvalidOrderId);
  ASSERT_NE(book.add(Side::kBuy, OrderType::kIoc, kAnchor - 1, 2), kInvalidOrderId);

  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 6u);
  EXPECT_EQ(book.open_orders(), 1u);
  // The freed slot still rests a real order afterwards.
  EXPECT_NE(book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 1), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 2u);
}

// ---------------------------------------------------------------------------
// Events (Day 3 task 3): add()/cancel() emit into a caller-supplied sink, in
// NaiveBook's exact order and byte-for-byte shape. The one deliberate
// difference: add-rejects carry order_id = kInvalidOrderId (no id was
// minted; NaiveBook echoes its caller-supplied id).
// ---------------------------------------------------------------------------

TEST(OrderBookEvents, RestingLimitEmitsAccepted) {
  OrderBook book = MakeBook();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId id = book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 10, sink);

  ASSERT_NE(id, kInvalidOrderId);
  ExpectEvents(events, {Accepted(Side::kBuy, 10, kAnchor - 1, id)});
}

TEST(OrderBookEvents, ValidationRejectsCarryReasonAndNoId) {
  OrderBook book = MakeBook();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  EXPECT_EQ(book.add(Side::kBuy, OrderType::kLimit, kAnchor, 0, sink), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::kSell, OrderType::kIoc, -3, 5, sink), kInvalidOrderId);
  // Market skips the price check; a qty reject still echoes the raw price.
  EXPECT_EQ(book.add(Side::kBuy, OrderType::kMarket, kAnchor, 0, sink), kInvalidOrderId);

  ExpectEvents(events,
               {
                   Rejected(Side::kBuy, RejectReason::kInvalidQty, 0, kAnchor, kInvalidOrderId),
                   Rejected(Side::kSell, RejectReason::kInvalidPrice, 5, -3, kInvalidOrderId),
                   Rejected(Side::kBuy, RejectReason::kInvalidQty, 0, kAnchor, kInvalidOrderId),
               });
}

// No NaiveBook counterpart (it has no pool); shaped by analogy with the
// validation rejects: raw qty/price echoed, no id.
TEST(OrderBookEvents, PoolExhaustionEmitsRejected) {
  OrderBook book = MakeBook(/*pool_capacity=*/1);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 1);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  EXPECT_EQ(book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 7, sink), kInvalidOrderId);

  ExpectEvents(events, {Rejected(Side::kSell, RejectReason::kPoolExhausted, 7, kAnchor + 1,
                                 kInvalidOrderId)});
}

TEST(OrderBookEvents, FullFillEmitsMakerThenTakerAtMakerPrice) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  // Taker limit +3 is worse than the maker's +1: price improvement goes to
  // the taker — both Traded events carry the maker's price.
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 3, 10, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 10, kAnchor + 3, taker),
                           Traded(Side::kSell, 10, 0, kAnchor + 1, maker),
                           Traded(Side::kBuy, 10, 0, kAnchor + 1, taker),
                       });
}

TEST(OrderBookEvents, PartialMakerFillCarriesBothRemainings) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 4, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 4, kAnchor + 1, taker),
                           Traded(Side::kSell, 4, 6, kAnchor + 1, maker),
                           Traded(Side::kBuy, 4, 0, kAnchor + 1, taker),
                       });
}

TEST(OrderBookEvents, SweepEmitsPairsInPriceTimeOrder) {
  OrderBook book = MakeBook();
  const OrderId a = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  const OrderId b = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 3);
  const OrderId c = book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 4);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 2, 10, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 10, kAnchor + 2, taker),
                           Traded(Side::kSell, 5, 0, kAnchor + 1, a),
                           Traded(Side::kBuy, 5, 5, kAnchor + 1, taker),
                           Traded(Side::kSell, 3, 0, kAnchor + 1, b),
                           Traded(Side::kBuy, 3, 2, kAnchor + 1, taker),
                           Traded(Side::kSell, 2, 2, kAnchor + 2, c),
                           Traded(Side::kBuy, 2, 0, kAnchor + 2, taker),
                       });
}

TEST(OrderBookEvents, LimitRemainderRestsWithoutCancelEvent) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 15, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 15, kAnchor + 1, taker),
                           Traded(Side::kSell, 10, 0, kAnchor + 1, maker),
                           Traded(Side::kBuy, 10, 5, kAnchor + 1, taker),
                       });
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor + 1), 5u);  // remainder rests, no Canceled
}

TEST(OrderBookEvents, IocRemainderEmitsCanceledAtLimitPrice) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kIoc, kAnchor + 1, 15, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 15, kAnchor + 1, taker),
                           Traded(Side::kSell, 10, 0, kAnchor + 1, maker),
                           Traded(Side::kBuy, 10, 5, kAnchor + 1, taker),
                           Canceled(Side::kBuy, 5, kAnchor + 1, taker),
                       });
}

// Accepted/Canceled carry price 0 for a market order; its Traded events
// carry the real execution (maker) price.
TEST(OrderBookEvents, MarketEventsCarryPriceZero) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 4);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kMarket, kAnchor + 9, 6, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 6, 0, taker),
                           Traded(Side::kSell, 4, 0, kAnchor + 1, maker),
                           Traded(Side::kBuy, 4, 2, kAnchor + 1, taker),
                           Canceled(Side::kBuy, 2, 0, taker),
                       });
}

TEST(OrderBookEvents, MarketOnEmptyBookAcceptedThenCanceled) {
  OrderBook book = MakeBook();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kSell, OrderType::kMarket, 0, 5, sink);

  ExpectEvents(events, {
                           Accepted(Side::kSell, 5, 0, taker),
                           Canceled(Side::kSell, 5, 0, taker),
                       });
}

TEST(OrderBookEvents, CancelEmitsCanceledWithOpenQty) {
  OrderBook book = MakeBook();
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 4);  // partial-fill to 6
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  EXPECT_TRUE(book.cancel(maker, sink));

  ExpectEvents(events, {Canceled(Side::kSell, 6, kAnchor + 1, maker)});
}

TEST(OrderBookEvents, CancelUnknownEmitsRejectedEchoingId) {
  OrderBook book = MakeBook();
  const OrderId filled = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 5);  // fills `filled`
  const OrderId forged = make_order_id(99, 7);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  EXPECT_FALSE(book.cancel(forged, sink));
  EXPECT_FALSE(book.cancel(filled, sink));

  ExpectEvents(events, {
                           Rejected(Side::kBuy, RejectReason::kUnknownOrder, 0, 0, forged),
                           Rejected(Side::kBuy, RejectReason::kUnknownOrder, 0, 0, filled),
                       });
}

// Scripted spec-conformance capstone: drive both books through a mixed
// sequence (sweeps, partial fills, market, IOC, cancels) and require
// byte-identical event streams op by op. The randomized version is Day 3
// task 5.
TEST(OrderBookEvents, ScriptedParityWithNaiveBook) {
  OrderBook book = MakeBook();
  NaiveBook naive;
  std::vector<Event> book_events;
  std::vector<Event> naive_events;
  auto sink = [&](const Event& e) { book_events.push_back(e); };

  auto add_both = [&](Side s, OrderType t, PriceTicks px, Qty q) {
    book_events.clear();
    naive_events.clear();
    const OrderId id = book.add(s, t, px, q, sink);
    if (id != kInvalidOrderId) {  // rejects mint no id to feed NaiveBook
      naive.add(id, s, t, px, q, naive_events);
      ExpectEvents(book_events, naive_events);
    }
    EXPECT_EQ(book.best_bid(), naive.best_bid());
    EXPECT_EQ(book.best_ask(), naive.best_ask());
    EXPECT_EQ(book.open_orders(), naive.open_orders());
    return id;
  };
  auto cancel_both = [&](OrderId id) {
    book_events.clear();
    naive_events.clear();
    EXPECT_EQ(book.cancel(id, sink), naive.cancel(id, naive_events));
    ExpectEvents(book_events, naive_events);
    EXPECT_EQ(book.open_orders(), naive.open_orders());
  };

  add_both(Side::kBuy, OrderType::kLimit, kAnchor - 2, 10);
  add_both(Side::kSell, OrderType::kLimit, kAnchor + 2, 8);
  add_both(Side::kSell, OrderType::kLimit, kAnchor + 2, 5);
  add_both(Side::kBuy, OrderType::kLimit, kAnchor + 2, 10);  // sweeps 8 + 2 FIFO, full fill
  // Fills the 10 resting bids; its remainder 5 rests on the ask side at -2.
  const OrderId sell15 = add_both(Side::kSell, OrderType::kLimit, kAnchor - 2, 15);
  add_both(Side::kBuy, OrderType::kMarket, 0, 3);          // takes 3 of sell15's remainder
  add_both(Side::kBuy, OrderType::kIoc, kAnchor + 2, 10);  // sweeps 2 @ -2 + 3 @ +2, cancels 5
  cancel_both(sell15);  // fully filled by the IOC: both books reject kUnknownOrder
  const OrderId resting = add_both(Side::kBuy, OrderType::kLimit, kAnchor - 5, 4);
  cancel_both(resting);                             // live cancel: Canceled on both
  cancel_both(resting);                             // double cancel: both reject
  add_both(Side::kSell, OrderType::kMarket, 0, 5);  // empty bids: accept-then-cancel
}

}  // namespace
}  // namespace lob
