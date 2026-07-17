#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "event_test_util.h"
#include "lob/order_book.h"
#include "lob/types.h"

// Day 3 task 6: deterministic edge cases for the matching engine. The
// differential harness brushes these paths randomly; these tests pin each one
// down by name. Market-on-empty-book, basic pool exhaustion, and
// cancel-after-fill landed with tasks 1-3; the bulk here is the
// sweep-through-band / overflow-map interplay. Every scenario ends with the
// full invariant walk.

namespace lob {
namespace {

using namespace testutil;  // NOLINT(google-build-using-namespace): test-local

constexpr PriceTicks kAnchor = 10'000;
constexpr std::uint32_t kRadius = 16;
constexpr PriceTicks kBandLow = kAnchor - kRadius;   // lowest in-band price
constexpr PriceTicks kBandHigh = kAnchor + kRadius;  // highest in-band price

OrderBook MakeBook(std::uint32_t pool_capacity = 64) {
  return OrderBook(kAnchor, kRadius, pool_capacity);
}

// A buy sweeps in-band asks, crosses the band edge, and keeps filling
// overflow levels in ascending price order until it is done.
TEST(MatchingEdge, SweepThroughBandIntoOverflowAsks) {
  OrderBook book = MakeBook();
  const OrderId a = book.add(Side::kSell, OrderType::kLimit, kAnchor + 14, 5);
  const OrderId b = book.add(Side::kSell, OrderType::kLimit, kBandHigh, 5);        // band edge
  const OrderId c = book.add(Side::kSell, OrderType::kLimit, kBandHigh + 50, 5);   // overflow
  const OrderId d = book.add(Side::kSell, OrderType::kLimit, kBandHigh + 300, 5);  // deeper
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kBandHigh + 300, 20, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 20, kBandHigh + 300, taker),
                           Traded(Side::kSell, 5, 0, kAnchor + 14, a),
                           Traded(Side::kBuy, 5, 15, kAnchor + 14, taker),
                           Traded(Side::kSell, 5, 0, kBandHigh, b),
                           Traded(Side::kBuy, 5, 10, kBandHigh, taker),
                           Traded(Side::kSell, 5, 0, kBandHigh + 50, c),
                           Traded(Side::kBuy, 5, 5, kBandHigh + 50, taker),
                           Traded(Side::kSell, 5, 0, kBandHigh + 300, d),
                           Traded(Side::kBuy, 5, 0, kBandHigh + 300, taker),
                       });
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_FALSE(book.contains(taker));
  EXPECT_EQ(book.open_orders(), 0u);
  book.check_invariants();
}

// Mirror on the bid side: a sell sweeps down through the band-low edge into
// the overflow region below it.
TEST(MatchingEdge, SweepThroughBandIntoOverflowBids) {
  OrderBook book = MakeBook();
  const OrderId a = book.add(Side::kBuy, OrderType::kLimit, kAnchor - 14, 3);
  const OrderId b = book.add(Side::kBuy, OrderType::kLimit, kBandLow, 3);       // band edge
  const OrderId c = book.add(Side::kBuy, OrderType::kLimit, kBandLow - 50, 3);  // overflow
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kSell, OrderType::kLimit, kBandLow - 50, 9, sink);

  ExpectEvents(events, {
                           Accepted(Side::kSell, 9, kBandLow - 50, taker),
                           Traded(Side::kBuy, 3, 0, kAnchor - 14, a),
                           Traded(Side::kSell, 3, 6, kAnchor - 14, taker),
                           Traded(Side::kBuy, 3, 0, kBandLow, b),
                           Traded(Side::kSell, 3, 3, kBandLow, taker),
                           Traded(Side::kBuy, 3, 0, kBandLow - 50, c),
                           Traded(Side::kSell, 3, 0, kBandLow - 50, taker),
                       });
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
  book.check_invariants();
}

// The sweep stops inside overflow at the taker's limit; the unfilled
// remainder rests OUT of band on the taker's own side.
TEST(MatchingEdge, RemainderRestsOutOfBandAfterOverflowSweep) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 14, 5);
  book.add(Side::kSell, OrderType::kLimit, kBandHigh, 5);
  book.add(Side::kSell, OrderType::kLimit, kBandHigh + 50, 5);
  const OrderId deeper = book.add(Side::kSell, OrderType::kLimit, kBandHigh + 300, 5);

  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kBandHigh + 50, 20);

  EXPECT_TRUE(book.contains(taker));
  EXPECT_EQ(book.best_bid(), kBandHigh + 50);  // rests as a bid-side overflow level
  EXPECT_EQ(book.qty_at(Side::kBuy, kBandHigh + 50), 5u);
  EXPECT_TRUE(book.contains(deeper));  // beyond the limit: untouched
  EXPECT_EQ(book.best_ask(), kBandHigh + 300);
  EXPECT_EQ(book.qty_at(Side::kSell, kBandHigh + 300), 5u);
  book.check_invariants();
}

// Partial fill of an overflow maker adjusts the map node's aggregates in
// place; canceling the remainder erases the node eagerly.
TEST(MatchingEdge, OverflowMakerPartialFillThenCancel) {
  OrderBook book = MakeBook();
  const PriceTicks oob = kBandHigh + 100;
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, oob, 10);

  book.add(Side::kBuy, OrderType::kMarket, 0, 4);

  EXPECT_EQ(book.qty_at(Side::kSell, oob), 6u);
  EXPECT_EQ(book.orders_at(Side::kSell, oob), 1u);
  book.check_invariants();

  EXPECT_TRUE(book.cancel(maker));
  EXPECT_EQ(book.qty_at(Side::kSell, oob), 0u);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  book.check_invariants();
}

// An overflow level on the NEAR side of the band (an ask below band-low, a
// bid above band-high) is the best price and must be swept before any
// in-band level — the map-vs-band best merge, mid-sweep.
TEST(MatchingEdge, NearSideOverflowIsBestAndSweptFirst) {
  OrderBook asks_book = MakeBook();
  const OrderId low_ask = asks_book.add(Side::kSell, OrderType::kLimit, kBandLow - 100, 2);
  const OrderId band_ask = asks_book.add(Side::kSell, OrderType::kLimit, kAnchor - 2, 2);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId buyer = asks_book.add(Side::kBuy, OrderType::kLimit, kAnchor, 4, sink);

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 4, kAnchor, buyer),
                           Traded(Side::kSell, 2, 0, kBandLow - 100, low_ask),
                           Traded(Side::kBuy, 2, 2, kBandLow - 100, buyer),
                           Traded(Side::kSell, 2, 0, kAnchor - 2, band_ask),
                           Traded(Side::kBuy, 2, 0, kAnchor - 2, buyer),
                       });
  asks_book.check_invariants();

  OrderBook bids_book = MakeBook();
  const OrderId high_bid = bids_book.add(Side::kBuy, OrderType::kLimit, kBandHigh + 100, 2);
  const OrderId band_bid = bids_book.add(Side::kBuy, OrderType::kLimit, kAnchor + 2, 2);
  events.clear();

  const OrderId seller = bids_book.add(Side::kSell, OrderType::kLimit, kAnchor, 4, sink);

  ExpectEvents(events, {
                           Accepted(Side::kSell, 4, kAnchor, seller),
                           Traded(Side::kBuy, 2, 0, kBandHigh + 100, high_bid),
                           Traded(Side::kSell, 2, 2, kBandHigh + 100, seller),
                           Traded(Side::kBuy, 2, 0, kAnchor + 2, band_bid),
                           Traded(Side::kSell, 2, 0, kAnchor + 2, seller),
                       });
  bids_book.check_invariants();
}

// A market order drains every level — near-side overflow, both band edges,
// and far-side overflow — leaving the side truly empty; the remainder is
// canceled at price 0.
TEST(MatchingEdge, MarketDrainsBandAndOverflowToEmpty) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kBandLow - 200, 2);  // overflow, near side
  book.add(Side::kSell, OrderType::kLimit, kBandLow, 2);        // band edge (offset 0)
  book.add(Side::kSell, OrderType::kLimit, kAnchor, 2);
  book.add(Side::kSell, OrderType::kLimit, kBandHigh, 2);        // band edge (offset 2r)
  book.add(Side::kSell, OrderType::kLimit, kBandHigh + 200, 2);  // overflow, far side
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId taker = book.add(Side::kBuy, OrderType::kMarket, 0, 15, sink);

  ASSERT_EQ(events.size(), 12u);  // Accepted + 5 maker/taker pairs + Canceled
  // Fills ascend through the whole structure; spot-check the seams.
  EXPECT_TRUE(EventEq(Traded(Side::kBuy, 2, 13, kBandLow - 200, taker), events[2]));
  EXPECT_TRUE(EventEq(Traded(Side::kBuy, 2, 7, kBandHigh, taker), events[8]));
  EXPECT_TRUE(EventEq(Canceled(Side::kBuy, 5, 0, taker), events[11]));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
  book.check_invariants();
}

// Makers resting at the exact band-edge offsets (0 and 2r) match cleanly.
TEST(MatchingEdge, BandEdgeMakersMatchExactly) {
  OrderBook book = MakeBook();
  const OrderId edge_ask = book.add(Side::kSell, OrderType::kLimit, kBandHigh, 7);
  const OrderId edge_bid = book.add(Side::kBuy, OrderType::kLimit, kBandLow, 7);

  EXPECT_NE(book.add(Side::kBuy, OrderType::kLimit, kBandHigh, 7), kInvalidOrderId);
  EXPECT_FALSE(book.contains(edge_ask));
  book.check_invariants();

  EXPECT_NE(book.add(Side::kSell, OrderType::kLimit, kBandLow, 7), kInvalidOrderId);
  EXPECT_FALSE(book.contains(edge_bid));
  EXPECT_EQ(book.open_orders(), 0u);
  book.check_invariants();
}

// ABA under matching: fills free slots (maker first, then the taker — LIFO),
// new orders reuse exactly those slots, and the filled orders' stale ids
// must not touch them.
TEST(MatchingEdge, StaleIdsAfterFillFreedSlotReuse) {
  OrderBook book = MakeBook(/*pool_capacity=*/2);
  const OrderId maker = book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  const OrderId taker = book.add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 5);
  ASSERT_EQ(book.open_orders(), 0u);

  const OrderId reuse1 = book.add(Side::kBuy, OrderType::kLimit, kAnchor - 1, 4);
  const OrderId reuse2 = book.add(Side::kSell, OrderType::kLimit, kAnchor + 2, 4);
  ASSERT_EQ(index_of(reuse1), index_of(taker));  // freed last, reused first
  ASSERT_EQ(index_of(reuse2), index_of(maker));

  EXPECT_FALSE(book.cancel(maker));
  EXPECT_FALSE(book.cancel(taker));
  EXPECT_TRUE(book.contains(reuse1));
  EXPECT_TRUE(book.contains(reuse2));
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 1), 4u);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 2), 4u);
  book.check_invariants();
}

// Alloc-before-sweep applies to market orders too: a market taker never
// rests, but a full pool still rejects it up front.
TEST(MatchingEdge, PoolExhaustionRejectsMarketOrder) {
  OrderBook book = MakeBook(/*pool_capacity=*/1);
  book.add(Side::kSell, OrderType::kLimit, kAnchor + 1, 10);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  EXPECT_EQ(book.add(Side::kBuy, OrderType::kMarket, 0, 4, sink), kInvalidOrderId);

  ExpectEvents(events, {Rejected(Side::kBuy, RejectReason::kPoolExhausted, 4, 0, kInvalidOrderId)});
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 10u);
  book.check_invariants();
}

// An IOC sweeps through the band edge into overflow like a limit, but its
// remainder cancels instead of resting out-of-band; levels beyond its limit
// stay untouched.
TEST(MatchingEdge, IocSweepsIntoOverflowAndCancelsRemainder) {
  OrderBook book = MakeBook();
  book.add(Side::kSell, OrderType::kLimit, kBandHigh, 5);
  book.add(Side::kSell, OrderType::kLimit, kBandHigh + 80, 5);
  const OrderId deeper = book.add(Side::kSell, OrderType::kLimit, kBandHigh + 200, 5);

  const OrderId taker = book.add(Side::kBuy, OrderType::kIoc, kBandHigh + 80, 20);

  ASSERT_NE(taker, kInvalidOrderId);
  EXPECT_FALSE(book.contains(taker));  // remainder canceled, never rests
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_TRUE(book.contains(deeper));
  EXPECT_EQ(book.best_ask(), kBandHigh + 200);
  book.check_invariants();
}

}  // namespace
}  // namespace lob
