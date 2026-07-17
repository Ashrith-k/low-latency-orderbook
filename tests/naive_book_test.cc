#include "lob/naive_book.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "event_test_util.h"
#include "lob/types.h"

namespace lob {
namespace {

// Event helpers (MakeEvent, EventEq, ExpectEvents, spec-table builders) are
// shared with the OrderBook event tests and the differential harness.
using namespace testutil;  // NOLINT(google-build-using-namespace): test-local

using OptPx = std::optional<PriceTicks>;

TEST(NaiveBookAddCancel, EmptyBookHasNoState) {
  NaiveBook book;
  EXPECT_EQ(std::nullopt, book.best_bid());
  EXPECT_EQ(std::nullopt, book.best_ask());
  EXPECT_EQ(0u, book.open_orders());
  EXPECT_EQ(0u, book.qty_at(Side::kBuy, 100));
  EXPECT_EQ(0u, book.orders_at(Side::kSell, 100));
  EXPECT_FALSE(book.contains(1));
}

TEST(NaiveBookAddCancel, AddBidRestsAndEmitsAccepted) {
  NaiveBook book;
  std::vector<Event> events;
  EXPECT_TRUE(book.add_limit(1, Side::kBuy, 100, 10, events));
  ASSERT_EQ(1u, events.size());
  EXPECT_TRUE(EventEq(
      MakeEvent(EventType::kAccepted, Side::kBuy, RejectReason::kNone, 10, 10, 100, 1), events[0]));
  EXPECT_EQ(OptPx{100}, book.best_bid());
  EXPECT_EQ(std::nullopt, book.best_ask());
  EXPECT_EQ(10u, book.qty_at(Side::kBuy, 100));
  EXPECT_EQ(1u, book.orders_at(Side::kBuy, 100));
  EXPECT_EQ(1u, book.open_orders());
  EXPECT_TRUE(book.contains(1));
}

TEST(NaiveBookAddCancel, AddAskRestsAndEmitsAccepted) {
  NaiveBook book;
  std::vector<Event> events;
  EXPECT_TRUE(book.add_limit(7, Side::kSell, 105, 3, events));
  ASSERT_EQ(1u, events.size());
  EXPECT_TRUE(EventEq(
      MakeEvent(EventType::kAccepted, Side::kSell, RejectReason::kNone, 3, 3, 105, 7), events[0]));
  EXPECT_EQ(OptPx{105}, book.best_ask());
  EXPECT_EQ(std::nullopt, book.best_bid());
  EXPECT_EQ(3u, book.qty_at(Side::kSell, 105));
}

TEST(NaiveBookAddCancel, BestTracksAcrossLevels) {
  NaiveBook book;
  std::vector<Event> events;
  // Non-crossing book: bids below asks.
  book.add_limit(1, Side::kBuy, 98, 5, events);
  book.add_limit(2, Side::kBuy, 100, 5, events);
  book.add_limit(3, Side::kBuy, 99, 5, events);
  book.add_limit(4, Side::kSell, 105, 5, events);
  book.add_limit(5, Side::kSell, 103, 5, events);
  book.add_limit(6, Side::kSell, 104, 5, events);
  EXPECT_EQ(6u, events.size());
  EXPECT_EQ(OptPx{100}, book.best_bid());  // highest bid
  EXPECT_EQ(OptPx{103}, book.best_ask());  // lowest ask
  EXPECT_EQ(6u, book.open_orders());
}

TEST(NaiveBookAddCancel, SamePriceLevelAggregates) {
  NaiveBook book;
  std::vector<Event> events;
  book.add_limit(1, Side::kSell, 105, 10, events);
  book.add_limit(2, Side::kSell, 105, 7, events);
  EXPECT_EQ(17u, book.qty_at(Side::kSell, 105));
  EXPECT_EQ(2u, book.orders_at(Side::kSell, 105));
  EXPECT_EQ(OptPx{105}, book.best_ask());
}

TEST(NaiveBookAddCancel, RejectsZeroQty) {
  NaiveBook book;
  std::vector<Event> events;
  EXPECT_FALSE(book.add_limit(1, Side::kBuy, 100, 0, events));
  ASSERT_EQ(1u, events.size());
  EXPECT_TRUE(
      EventEq(MakeEvent(EventType::kRejected, Side::kBuy, RejectReason::kInvalidQty, 0, 0, 100, 1),
              events[0]));
  EXPECT_EQ(0u, book.open_orders());
  EXPECT_EQ(std::nullopt, book.best_bid());
}

TEST(NaiveBookAddCancel, RejectsNonPositivePrice) {
  NaiveBook book;
  std::vector<Event> events;
  EXPECT_FALSE(book.add_limit(1, Side::kSell, 0, 5, events));
  EXPECT_FALSE(book.add_limit(2, Side::kSell, -10, 5, events));
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(
      EventEq(MakeEvent(EventType::kRejected, Side::kSell, RejectReason::kInvalidPrice, 5, 0, 0, 1),
              events[0]));
  EXPECT_TRUE(EventEq(
      MakeEvent(EventType::kRejected, Side::kSell, RejectReason::kInvalidPrice, 5, 0, -10, 2),
      events[1]));
  EXPECT_EQ(0u, book.open_orders());
  EXPECT_EQ(std::nullopt, book.best_ask());
}

TEST(NaiveBookAddCancel, CancelRemovesOrderAndEmitsCanceled) {
  NaiveBook book;
  std::vector<Event> events;
  book.add_limit(1, Side::kBuy, 100, 10, events);
  book.add_limit(2, Side::kBuy, 100, 5, events);
  book.add_limit(3, Side::kBuy, 99, 7, events);
  events.clear();

  // Cancel one of two orders at the best level: level shrinks, best unchanged.
  EXPECT_TRUE(book.cancel(1, events));
  ASSERT_EQ(1u, events.size());
  EXPECT_TRUE(EventEq(
      MakeEvent(EventType::kCanceled, Side::kBuy, RejectReason::kNone, 10, 0, 100, 1), events[0]));
  EXPECT_FALSE(book.contains(1));
  EXPECT_EQ(5u, book.qty_at(Side::kBuy, 100));
  EXPECT_EQ(1u, book.orders_at(Side::kBuy, 100));
  EXPECT_EQ(OptPx{100}, book.best_bid());

  // Cancel the last order at the best level: level disappears, best moves.
  EXPECT_TRUE(book.cancel(2, events));
  EXPECT_EQ(0u, book.qty_at(Side::kBuy, 100));
  EXPECT_EQ(0u, book.orders_at(Side::kBuy, 100));
  EXPECT_EQ(OptPx{99}, book.best_bid());

  // Cancel the final order: side is empty again.
  EXPECT_TRUE(book.cancel(3, events));
  EXPECT_EQ(std::nullopt, book.best_bid());
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookAddCancel, CancelUnknownIdRejected) {
  NaiveBook book;
  std::vector<Event> events;
  EXPECT_FALSE(book.cancel(42, events));
  ASSERT_EQ(1u, events.size());
  // Side of a cancel-reject is unknowable and left zero (kBuy) by spec.
  EXPECT_TRUE(
      EventEq(MakeEvent(EventType::kRejected, Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 0, 42),
              events[0]));
}

TEST(NaiveBookAddCancel, DoubleCancelRejected) {
  NaiveBook book;
  std::vector<Event> events;
  book.add_limit(1, Side::kSell, 105, 5, events);
  EXPECT_TRUE(book.cancel(1, events));
  events.clear();
  EXPECT_FALSE(book.cancel(1, events));
  ASSERT_EQ(1u, events.size());
  EXPECT_TRUE(
      EventEq(MakeEvent(EventType::kRejected, Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 0, 1),
              events[0]));
}

// ---------------------------------------------------------------------------
// Matching semantics (task 12): these sequences ARE the spec the optimized
// engine must reproduce byte-for-byte.
// ---------------------------------------------------------------------------

TEST(NaiveBookMatching, CrossingLimitFullFillEmitsMakerThenTaker) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 10, events);
  events.clear();

  EXPECT_TRUE(book.add(2, Side::kBuy, OrderType::kLimit, 100, 10, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 10, 100, 2),
                           Traded(Side::kSell, 10, 0, 100, 1),  // maker first
                           Traded(Side::kBuy, 10, 0, 100, 2),   // then taker
                       });
  EXPECT_EQ(std::nullopt, book.best_bid());
  EXPECT_EQ(std::nullopt, book.best_ask());
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookMatching, PartialFillRemainderRests) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 5, events);
  events.clear();

  EXPECT_TRUE(book.add(2, Side::kBuy, OrderType::kLimit, 100, 8, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 8, 100, 2),
                           Traded(Side::kSell, 5, 0, 100, 1),
                           Traded(Side::kBuy, 5, 3, 100, 2),
                       });
  EXPECT_EQ(OptPx{100}, book.best_bid());
  EXPECT_EQ(3u, book.qty_at(Side::kBuy, 100));
  EXPECT_EQ(std::nullopt, book.best_ask());
  EXPECT_TRUE(book.contains(2));
  EXPECT_FALSE(book.contains(1));
}

TEST(NaiveBookMatching, AggressorPartiallyFillsRestingKeepsPriority) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 10, events);
  events.clear();

  EXPECT_TRUE(book.add(2, Side::kBuy, OrderType::kLimit, 100, 4, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 4, 100, 2),
                           Traded(Side::kSell, 4, 6, 100, 1),
                           Traded(Side::kBuy, 4, 0, 100, 2),
                       });
  EXPECT_EQ(6u, book.qty_at(Side::kSell, 100));
  EXPECT_EQ(1u, book.orders_at(Side::kSell, 100));
  EXPECT_TRUE(book.contains(1));   // still resting, reduced
  EXPECT_FALSE(book.contains(2));  // fully filled, never rested
  EXPECT_EQ(1u, book.open_orders());
}

TEST(NaiveBookMatching, SweepsLevelsBestFirstAtMakerPrices) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 103, 5, events);
  book.add(2, Side::kSell, OrderType::kLimit, 104, 5, events);
  events.clear();

  // Buy 12 @ 105: fills 5 @ 103, 5 @ 104 (price improvement), rests 2 @ 105.
  EXPECT_TRUE(book.add(3, Side::kBuy, OrderType::kLimit, 105, 12, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 12, 105, 3),
                           Traded(Side::kSell, 5, 0, 103, 1),
                           Traded(Side::kBuy, 5, 7, 103, 3),
                           Traded(Side::kSell, 5, 0, 104, 2),
                           Traded(Side::kBuy, 5, 2, 104, 3),
                       });
  EXPECT_EQ(std::nullopt, book.best_ask());
  EXPECT_EQ(OptPx{105}, book.best_bid());
  EXPECT_EQ(2u, book.qty_at(Side::kBuy, 105));
}

TEST(NaiveBookMatching, FifoWithinLevel) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 5, events);
  book.add(2, Side::kSell, OrderType::kLimit, 100, 5, events);
  events.clear();

  // Buy 7: id 1 (older) fills fully first, id 2 fills 2 and stays.
  EXPECT_TRUE(book.add(3, Side::kBuy, OrderType::kLimit, 100, 7, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 7, 100, 3),
                           Traded(Side::kSell, 5, 0, 100, 1),
                           Traded(Side::kBuy, 5, 2, 100, 3),
                           Traded(Side::kSell, 2, 3, 100, 2),
                           Traded(Side::kBuy, 2, 0, 100, 3),
                       });
  EXPECT_EQ(3u, book.qty_at(Side::kSell, 100));
  EXPECT_EQ(1u, book.orders_at(Side::kSell, 100));
  EXPECT_FALSE(book.contains(1));
  EXPECT_TRUE(book.contains(2));
}

TEST(NaiveBookMatching, SellAggressorSymmetry) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kBuy, OrderType::kLimit, 100, 5, events);
  book.add(2, Side::kBuy, OrderType::kLimit, 99, 5, events);
  events.clear();

  // Sell 8 @ 99: fills 5 @ 100 (improvement), then 3 @ 99; nothing rests.
  EXPECT_TRUE(book.add(3, Side::kSell, OrderType::kLimit, 99, 8, events));
  ExpectEvents(events, {
                           Accepted(Side::kSell, 8, 99, 3),
                           Traded(Side::kBuy, 5, 0, 100, 1),
                           Traded(Side::kSell, 5, 3, 100, 3),
                           Traded(Side::kBuy, 3, 2, 99, 2),
                           Traded(Side::kSell, 3, 0, 99, 3),
                       });
  EXPECT_EQ(OptPx{99}, book.best_bid());
  EXPECT_EQ(2u, book.qty_at(Side::kBuy, 99));
  EXPECT_FALSE(book.contains(3));
}

TEST(NaiveBookMatching, IocCancelsUnfilledRemainder) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 5, events);
  events.clear();

  EXPECT_TRUE(book.add(2, Side::kBuy, OrderType::kIoc, 100, 8, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 8, 100, 2), Traded(Side::kSell, 5, 0, 100, 1),
                           Traded(Side::kBuy, 5, 3, 100, 2),
                           Canceled(Side::kBuy, 3, 100, 2),  // remainder never rests
                       });
  EXPECT_EQ(std::nullopt, book.best_bid());
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookMatching, IocExactFillEmitsNoCancel) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 8, events);
  events.clear();

  EXPECT_TRUE(book.add(2, Side::kBuy, OrderType::kIoc, 100, 8, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 8, 100, 2),
                           Traded(Side::kSell, 8, 0, 100, 1),
                           Traded(Side::kBuy, 8, 0, 100, 2),
                       });
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookMatching, IocWithNoCrossableLiquidityCanceledInFull) {
  NaiveBook book;
  std::vector<Event> events;
  // Ask exists but does not cross the IOC's limit.
  book.add(1, Side::kSell, OrderType::kLimit, 105, 5, events);
  events.clear();

  EXPECT_TRUE(book.add(2, Side::kBuy, OrderType::kIoc, 100, 5, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 5, 100, 2),
                           Canceled(Side::kBuy, 5, 100, 2),
                       });
  EXPECT_EQ(5u, book.qty_at(Side::kSell, 105));  // book untouched
  EXPECT_EQ(1u, book.open_orders());
}

TEST(NaiveBookMatching, MarketSweepsAnyPriceAndNeverRests) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 103, 5, events);
  book.add(2, Side::kSell, OrderType::kLimit, 110, 5, events);
  events.clear();

  // Market buy 7: fills 5 @ 103 then 2 @ 110; market events carry price 0.
  EXPECT_TRUE(book.add(3, Side::kBuy, OrderType::kMarket, 0, 7, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 7, 0, 3),
                           Traded(Side::kSell, 5, 0, 103, 1),
                           Traded(Side::kBuy, 5, 2, 103, 3),
                           Traded(Side::kSell, 2, 3, 110, 2),
                           Traded(Side::kBuy, 2, 0, 110, 3),
                       });
  EXPECT_EQ(3u, book.qty_at(Side::kSell, 110));
  EXPECT_EQ(1u, book.open_orders());
  EXPECT_EQ(std::nullopt, book.best_bid());
}

TEST(NaiveBookMatching, MarketOnEmptyBookAcceptedThenCanceled) {
  NaiveBook book;
  std::vector<Event> events;
  // Market ignores the price argument entirely (even a garbage negative one);
  // its events carry price 0. Lack of liquidity is a cancel, not a reject.
  EXPECT_TRUE(book.add(1, Side::kSell, OrderType::kMarket, -7, 5, events));
  ExpectEvents(events, {
                           Accepted(Side::kSell, 5, 0, 1),
                           Canceled(Side::kSell, 5, 0, 1),
                       });
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookMatching, ValidationRejectsPerType) {
  NaiveBook book;
  std::vector<Event> events;
  // IOC and limit require a positive price; market does not. qty must be > 0.
  EXPECT_FALSE(book.add(1, Side::kBuy, OrderType::kIoc, 0, 5, events));
  EXPECT_FALSE(book.add(2, Side::kBuy, OrderType::kLimit, -1, 5, events));
  EXPECT_FALSE(book.add(3, Side::kSell, OrderType::kMarket, 0, 0, events));
  ExpectEvents(events, {
                           Rejected(Side::kBuy, RejectReason::kInvalidPrice, 5, 0, 1),
                           Rejected(Side::kBuy, RejectReason::kInvalidPrice, 5, -1, 2),
                           Rejected(Side::kSell, RejectReason::kInvalidQty, 0, 0, 3),
                       });
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookMatching, CancelAfterFullFillRejectedUnknown) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 5, events);
  book.add(2, Side::kBuy, OrderType::kLimit, 100, 5, events);  // fills id 1 fully
  events.clear();

  // The fill removed the maker from the book; canceling it is now unknown.
  EXPECT_FALSE(book.cancel(1, events));
  // The taker filled fully on entry and never rested; also unknown.
  EXPECT_FALSE(book.cancel(2, events));
  ExpectEvents(events, {
                           Rejected(Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 1),
                           Rejected(Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 2),
                       });
}

TEST(NaiveBookMatching, CancelAfterPartialFillReturnsOpenRemainder) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 10, events);
  book.add(2, Side::kBuy, OrderType::kLimit, 100, 4, events);  // id 1: 10 -> 6
  events.clear();

  // Canceled.qty is the OPEN quantity removed (6), not the original qty (10).
  EXPECT_TRUE(book.cancel(1, events));
  ExpectEvents(events, {Canceled(Side::kSell, 6, 100, 1)});
  EXPECT_EQ(0u, book.open_orders());
  EXPECT_EQ(std::nullopt, book.best_ask());
}

TEST(NaiveBookMatching, RepeatedPartialFillsKeepPriorityAndCountDown) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 20, events);

  const Qty maker_remaining_after[] = {15, 10, 5};
  for (int i = 0; i < 3; ++i) {
    events.clear();
    book.add(static_cast<OrderId>(2 + i), Side::kBuy, OrderType::kLimit, 100, 5, events);
    ExpectEvents(events, {
                             Accepted(Side::kBuy, 5, 100, static_cast<OrderId>(2 + i)),
                             Traded(Side::kSell, 5, maker_remaining_after[i], 100, 1),
                             Traded(Side::kBuy, 5, 0, 100, static_cast<OrderId>(2 + i)),
                         });
    EXPECT_EQ(maker_remaining_after[i], book.qty_at(Side::kSell, 100));
    EXPECT_TRUE(book.contains(1));  // still resting, still front
  }

  // Fourth aggressor takes the last 5; the maker finally leaves the book.
  events.clear();
  book.add(5, Side::kBuy, OrderType::kLimit, 100, 5, events);
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 5, 100, 5),
                           Traded(Side::kSell, 5, 0, 100, 1),
                           Traded(Side::kBuy, 5, 0, 100, 5),
                       });
  EXPECT_FALSE(book.contains(1));
  EXPECT_EQ(0u, book.open_orders());
}

TEST(NaiveBookMatching, DeepSweepAcrossLevelsIsFifoBestFirst) {
  NaiveBook book;
  std::vector<Event> events;
  // Two FIFO orders per level at 101 < 102 < 103, 5 qty each.
  book.add(1, Side::kSell, OrderType::kLimit, 101, 5, events);
  book.add(2, Side::kSell, OrderType::kLimit, 101, 5, events);
  book.add(3, Side::kSell, OrderType::kLimit, 102, 5, events);
  book.add(4, Side::kSell, OrderType::kLimit, 102, 5, events);
  book.add(5, Side::kSell, OrderType::kLimit, 103, 5, events);
  book.add(6, Side::kSell, OrderType::kLimit, 103, 5, events);
  events.clear();

  // Buy 27 @ 103 consumes five whole makers and 2 of the sixth.
  EXPECT_TRUE(book.add(7, Side::kBuy, OrderType::kLimit, 103, 27, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 27, 103, 7),
                           Traded(Side::kSell, 5, 0, 101, 1),
                           Traded(Side::kBuy, 5, 22, 101, 7),
                           Traded(Side::kSell, 5, 0, 101, 2),
                           Traded(Side::kBuy, 5, 17, 101, 7),
                           Traded(Side::kSell, 5, 0, 102, 3),
                           Traded(Side::kBuy, 5, 12, 102, 7),
                           Traded(Side::kSell, 5, 0, 102, 4),
                           Traded(Side::kBuy, 5, 7, 102, 7),
                           Traded(Side::kSell, 5, 0, 103, 5),
                           Traded(Side::kBuy, 5, 2, 103, 7),
                           Traded(Side::kSell, 2, 3, 103, 6),
                           Traded(Side::kBuy, 2, 0, 103, 7),
                       });
  EXPECT_EQ(OptPx{103}, book.best_ask());
  EXPECT_EQ(3u, book.qty_at(Side::kSell, 103));
  EXPECT_EQ(1u, book.open_orders());
  EXPECT_EQ(std::nullopt, book.best_bid());  // taker filled fully, rests nothing
}

TEST(NaiveBookMatching, SweepStopsAtFirstNonCrossingLevelAndRests) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 5, events);
  book.add(2, Side::kSell, OrderType::kLimit, 106, 5, events);
  events.clear();

  // Buy 8 @ 105: takes the 100 level, must NOT touch 106; remainder rests.
  EXPECT_TRUE(book.add(3, Side::kBuy, OrderType::kLimit, 105, 8, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 8, 105, 3),
                           Traded(Side::kSell, 5, 0, 100, 1),
                           Traded(Side::kBuy, 5, 3, 100, 3),
                       });
  // Book ends uncrossed: bid 105 < ask 106.
  EXPECT_EQ(OptPx{105}, book.best_bid());
  EXPECT_EQ(OptPx{106}, book.best_ask());
  EXPECT_EQ(3u, book.qty_at(Side::kBuy, 105));
  EXPECT_EQ(5u, book.qty_at(Side::kSell, 106));
}

TEST(NaiveBookMatching, IocSweepsMultipleLevelsThenCancels) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kSell, OrderType::kLimit, 100, 5, events);
  book.add(2, Side::kSell, OrderType::kLimit, 101, 5, events);
  book.add(3, Side::kSell, OrderType::kLimit, 103, 5, events);
  events.clear();

  // IOC buy 12 @ 101: fills both crossable levels, cancels the last 2.
  EXPECT_TRUE(book.add(4, Side::kBuy, OrderType::kIoc, 101, 12, events));
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 12, 101, 4),
                           Traded(Side::kSell, 5, 0, 100, 1),
                           Traded(Side::kBuy, 5, 7, 100, 4),
                           Traded(Side::kSell, 5, 0, 101, 2),
                           Traded(Side::kBuy, 5, 2, 101, 4),
                           Canceled(Side::kBuy, 2, 101, 4),
                       });
  EXPECT_EQ(OptPx{103}, book.best_ask());
  EXPECT_EQ(1u, book.open_orders());
  EXPECT_EQ(std::nullopt, book.best_bid());
}

TEST(NaiveBookMatching, MarketSellSymmetry) {
  NaiveBook book;
  std::vector<Event> events;
  book.add(1, Side::kBuy, OrderType::kLimit, 100, 5, events);
  book.add(2, Side::kBuy, OrderType::kLimit, 97, 5, events);
  events.clear();

  EXPECT_TRUE(book.add(3, Side::kSell, OrderType::kMarket, 0, 8, events));
  ExpectEvents(events, {
                           Accepted(Side::kSell, 8, 0, 3),
                           Traded(Side::kBuy, 5, 0, 100, 1),
                           Traded(Side::kSell, 5, 3, 100, 3),
                           Traded(Side::kBuy, 3, 2, 97, 2),
                           Traded(Side::kSell, 3, 0, 97, 3),
                       });
  EXPECT_EQ(OptPx{97}, book.best_bid());
  EXPECT_EQ(2u, book.qty_at(Side::kBuy, 97));
  EXPECT_EQ(1u, book.open_orders());
}

TEST(NaiveBookMatching, QtyConservationAcrossMixedSequence) {
  NaiveBook book;
  std::vector<Event> events;  // never cleared: whole-session stream
  book.add(1, Side::kSell, OrderType::kLimit, 100, 10, events);
  book.add(2, Side::kSell, OrderType::kLimit, 101, 10, events);
  book.add(3, Side::kBuy, OrderType::kLimit, 100, 4, events);  // id 1: 10 -> 6
  book.cancel(1, events);                                      // returns 6
  book.add(4, Side::kBuy, OrderType::kIoc, 101, 15, events);   // fills 10, cancels 5
  book.add(5, Side::kBuy, OrderType::kLimit, 99, 7, events);   // rests
  book.add(6, Side::kSell, OrderType::kMarket, 0, 3, events);  // id 5: 7 -> 4
  book.cancel(5, events);                                      // returns 4
  ASSERT_EQ(0u, book.open_orders());                           // book fully drained

  // Per order: accepted qty == its fills + canceled remainder (+ open, 0 here).
  // Summed over the whole stream: each Traded event is exactly one order's
  // fill, so sum(Accepted.qty) == sum(Traded.qty) + sum(Canceled.qty).
  Qty accepted = 0;
  Qty traded = 0;
  Qty canceled = 0;
  for (const Event& e : events) {
    switch (e.kind) {
      case EventType::kAccepted:
        accepted += e.qty;
        break;
      case EventType::kTraded:
        traded += e.qty;
        break;
      case EventType::kCanceled:
        canceled += e.qty;
        break;
      case EventType::kRejected:
        ADD_FAILURE() << "unexpected reject: " << Describe(e);
        break;
    }
  }
  EXPECT_EQ(49u, accepted);  // 10+10+4+15+7+3
  EXPECT_EQ(34u, traded);    // 17 per side
  EXPECT_EQ(15u, canceled);  // 6 (id 1) + 5 (id 4) + 4 (id 5)
  EXPECT_EQ(accepted, traded + canceled);
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(NaiveBookAddCancelDeathTest, DuplicateLiveIdAsserts) {
  NaiveBook book;
  std::vector<Event> events;
  book.add_limit(1, Side::kBuy, 100, 10, events);
  EXPECT_DEATH(book.add_limit(1, Side::kSell, 105, 5, events), "unique");
}
#endif

}  // namespace
}  // namespace lob
