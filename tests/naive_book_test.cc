#include "lob/naive_book.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/types.h"

namespace lob {
namespace {

using OptPx = std::optional<PriceTicks>;

Event MakeEvent(EventType kind, Side side, RejectReason reason, Qty qty, Qty remaining,
                PriceTicks price, OrderId id) {
  return Event{.kind = kind,
               .side = side,
               .reason = reason,
               .pad0 = 0,
               .qty = qty,
               .remaining = remaining,
               .pad1 = 0,
               .price_ticks = price,
               .order_id = id};
}

std::string Describe(const Event& e) {
  std::ostringstream os;
  os << to_cstr(e.kind) << "{side=" << to_cstr(e.side) << " reason=" << to_cstr(e.reason)
     << " qty=" << e.qty << " remaining=" << e.remaining << " px=" << e.price_ticks
     << " id=" << e.order_id << "}";
  return os.str();
}

// Whole-struct comparison is legal: Event has unique object representations
// (asserted in types.h), so equal bytes <=> equal fields.
testing::AssertionResult EventEq(const Event& want, const Event& got) {
  if (std::memcmp(&want, &got, sizeof(Event)) == 0) return testing::AssertionSuccess();
  return testing::AssertionFailure()
         << "\n  want: " << Describe(want) << "\n  got:  " << Describe(got);
}

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
