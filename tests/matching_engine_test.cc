#include "lob/matching_engine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "event_test_util.h"
#include "lob/types.h"

namespace lob {
namespace {

using namespace testutil;  // NOLINT(google-build-using-namespace): test-local

constexpr PriceTicks kAnchor = 10'000;

MatchingEngine MakeEngine(std::uint32_t pool_capacity = 64) {
  return MatchingEngine(
      {.anchor_price = kAnchor, .band_radius = 16, .pool_capacity = pool_capacity});
}

Command NewCmd(Side side, OrderType type, PriceTicks px, Qty qty, OrderId ignored_id = 0) {
  return Command{.kind = CommandType::kNew,
                 .side = side,
                 .type = type,
                 .pad0 = 0,
                 .qty = qty,
                 .price_ticks = px,
                 .order_id = ignored_id};
}

Command CancelCmd(OrderId id) {
  return Command{.kind = CommandType::kCancel,
                 .side = Side::kBuy,
                 .type = OrderType::kLimit,
                 .pad0 = 0,
                 .qty = 0,
                 .price_ticks = 0,
                 .order_id = id};
}

TEST(EngineFacade, NewCommandRestsAndEmitsAccepted) {
  MatchingEngine engine = MakeEngine();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  engine.process(NewCmd(Side::kBuy, OrderType::kLimit, kAnchor - 1, 10), sink);

  ASSERT_EQ(events.size(), 1u);
  const OrderId id = events[0].order_id;
  ExpectEvents(events, {Accepted(Side::kBuy, 10, kAnchor - 1, id)});
  EXPECT_TRUE(engine.book().contains(id));
  EXPECT_EQ(engine.book().best_bid(), kAnchor - 1);
  EXPECT_EQ(engine.book().open_orders(), 1u);
}

// The engine mints ids; whatever the producer left in cmd.order_id must not
// leak into the assigned id (types.h: kNew ignores order_id).
TEST(EngineFacade, NewCommandIgnoresCallerOrderId) {
  MatchingEngine engine = MakeEngine();
  const OrderId garbage = make_order_id(0xDEAD, 0xBEEF);
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  engine.process(NewCmd(Side::kSell, OrderType::kLimit, kAnchor + 1, 5, garbage), sink);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_NE(events[0].order_id, garbage);
  EXPECT_NE(events[0].order_id, kInvalidOrderId);
  EXPECT_TRUE(engine.book().contains(events[0].order_id));
}

TEST(EngineFacade, CrossingCommandsMatchAndEmitTrades) {
  MatchingEngine engine = MakeEngine();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  engine.process(NewCmd(Side::kSell, OrderType::kLimit, kAnchor + 1, 10), sink);
  const OrderId maker = events[0].order_id;
  events.clear();

  engine.process(NewCmd(Side::kBuy, OrderType::kLimit, kAnchor + 2, 15), sink);

  ASSERT_GE(events.size(), 1u);
  const OrderId taker = events[0].order_id;
  ExpectEvents(events, {
                           Accepted(Side::kBuy, 15, kAnchor + 2, taker),
                           Traded(Side::kSell, 10, 0, kAnchor + 1, maker),
                           Traded(Side::kBuy, 10, 5, kAnchor + 1, taker),
                       });
  EXPECT_EQ(engine.book().qty_at(Side::kBuy, kAnchor + 2), 5u);
  EXPECT_EQ(engine.book().best_ask(), std::nullopt);
}

// A cancel command targets order_id alone; garbage in the new-order fields
// must not matter.
TEST(EngineFacade, CancelCommandUsesOnlyTheId) {
  MatchingEngine engine = MakeEngine();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };
  engine.process(NewCmd(Side::kSell, OrderType::kLimit, kAnchor + 3, 7), sink);
  const OrderId id = events[0].order_id;
  events.clear();

  Command cmd = CancelCmd(id);
  cmd.side = Side::kSell;  // all garbage as far as cancel is concerned
  cmd.type = OrderType::kMarket;
  cmd.qty = 999;
  cmd.price_ticks = -42;
  engine.process(cmd, sink);

  ExpectEvents(events, {Canceled(Side::kSell, 7, kAnchor + 3, id)});
  EXPECT_FALSE(engine.book().contains(id));
  EXPECT_EQ(engine.book().open_orders(), 0u);
}

TEST(EngineFacade, RejectsFlowThroughProcess) {
  MatchingEngine engine = MakeEngine();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  engine.process(NewCmd(Side::kBuy, OrderType::kLimit, kAnchor, 0), sink);
  engine.process(CancelCmd(make_order_id(7, 7)), sink);

  ExpectEvents(events,
               {
                   Rejected(Side::kBuy, RejectReason::kInvalidQty, 0, kAnchor, kInvalidOrderId),
                   Rejected(Side::kBuy, RejectReason::kUnknownOrder, 0, 0, make_order_id(7, 7)),
               });
  EXPECT_EQ(engine.book().open_orders(), 0u);
}

// DESIGN §7 direct-call API: same semantics as the command path, plus the
// return value.
TEST(EngineFacade, SubmitAndCancelConvenienceMirrorProcess) {
  MatchingEngine engine = MakeEngine();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  const OrderId id = engine.submit(Side::kBuy, OrderType::kLimit, kAnchor - 2, 8, sink);
  ASSERT_NE(id, kInvalidOrderId);
  ExpectEvents(events, {Accepted(Side::kBuy, 8, kAnchor - 2, id)});

  events.clear();
  EXPECT_TRUE(engine.cancel(id, sink));
  ExpectEvents(events, {Canceled(Side::kBuy, 8, kAnchor - 2, id)});

  // Event-less overloads work too.
  const OrderId quiet = engine.submit(Side::kSell, OrderType::kLimit, kAnchor + 2, 3);
  ASSERT_NE(quiet, kInvalidOrderId);
  EXPECT_TRUE(engine.cancel(quiet));
  EXPECT_EQ(engine.book().open_orders(), 0u);
}

// A short command stream through the facade produces one combined event
// stream, exactly as the book-level spec dictates.
TEST(EngineFacade, ScriptedCommandStream) {
  MatchingEngine engine = MakeEngine();
  std::vector<Event> events;
  auto sink = [&](const Event& e) { events.push_back(e); };

  engine.process(NewCmd(Side::kBuy, OrderType::kLimit, kAnchor - 1, 6), sink);
  const OrderId bid = events.back().order_id;
  engine.process(NewCmd(Side::kSell, OrderType::kIoc, kAnchor - 1, 10), sink);
  const OrderId ioc = events[1].order_id;  // the IOC's own Accepted
  engine.process(CancelCmd(bid), sink);    // already fully filled: reject

  ExpectEvents(events, {
                           Accepted(Side::kBuy, 6, kAnchor - 1, bid),
                           Accepted(Side::kSell, 10, kAnchor - 1, ioc),
                           Traded(Side::kBuy, 6, 0, kAnchor - 1, bid),
                           Traded(Side::kSell, 6, 4, kAnchor - 1, ioc),
                           Canceled(Side::kSell, 4, kAnchor - 1, ioc),
                           Rejected(Side::kBuy, RejectReason::kUnknownOrder, 0, 0, bid),
                       });
  EXPECT_EQ(engine.book().open_orders(), 0u);
  engine.book().check_invariants();
}

// A corrupted command kind asserts in debug and is a no-op in release (no
// events, no state change) — the Day-5 replay/wire readers validate kinds
// before submitting.
#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(EngineFacadeDeathTest, UnknownCommandKindAsserts) {
  MatchingEngine engine = MakeEngine();
  Command cmd = NewCmd(Side::kBuy, OrderType::kLimit, kAnchor, 1);
  cmd.kind = static_cast<CommandType>(0x7F);
  EXPECT_DEATH(engine.process(cmd, [](const Event&) {}), "unknown CommandType");
}
#else
TEST(EngineFacade, UnknownCommandKindIsNoOpInRelease) {
  MatchingEngine engine = MakeEngine();
  engine.submit(Side::kBuy, OrderType::kLimit, kAnchor - 1, 5);
  Command cmd = NewCmd(Side::kBuy, OrderType::kLimit, kAnchor, 1);
  cmd.kind = static_cast<CommandType>(0x7F);
  std::vector<Event> events;
  engine.process(cmd, [&](const Event& e) { events.push_back(e); });
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(engine.book().open_orders(), 1u);
  EXPECT_EQ(engine.book().best_bid(), kAnchor - 1);
}
#endif

}  // namespace
}  // namespace lob
