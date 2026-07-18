#include "lob/stats.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/spsc_queue.h"
#include "lob/types.h"

namespace lob {
namespace {

// Day 5 task 7: EngineStats and its wiring through MatchingEngine. The two
// engine entry paths (Command dispatch and direct calls) must count
// identically, and the counters must reconcile exactly against a hand-built
// script with a known outcome.

EngineConfig SmallConfig(std::uint32_t pool_capacity = 64) {
  EngineConfig cfg;
  cfg.anchor_price = 1'000;
  cfg.band_radius = 128;
  cfg.pool_capacity = pool_capacity;
  return cfg;
}

TEST(EngineStats, StartsAtZero) {
  const EngineStats s;
  EXPECT_EQ(s.commands(), 0u);
  EXPECT_EQ(s.events, 0u);
  EXPECT_EQ(s.fills(), 0u);
  EXPECT_EQ(s.traded_qty(), 0u);
}

TEST(EngineStats, OnEventCountsEveryKindAndReason) {
  EngineStats s;
  s.on_event(Event{EventType::kAccepted, Side::kBuy, RejectReason::kNone, 0, 5, 5, 0, 100, 1});
  s.on_event(
      Event{EventType::kRejected, Side::kBuy, RejectReason::kInvalidQty, 0, 0, 0, 0, 100, 0});
  s.on_event(
      Event{EventType::kRejected, Side::kBuy, RejectReason::kInvalidPrice, 0, 5, 0, 0, -1, 0});
  s.on_event(
      Event{EventType::kRejected, Side::kBuy, RejectReason::kPoolExhausted, 0, 5, 0, 0, 100, 0});
  s.on_event(
      Event{EventType::kRejected, Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 0, 0, 0, 99});
  s.on_event(Event{EventType::kTraded, Side::kSell, RejectReason::kNone, 0, 3, 0, 0, 100, 1});
  s.on_event(Event{EventType::kTraded, Side::kBuy, RejectReason::kNone, 0, 3, 2, 0, 100, 2});
  s.on_event(Event{EventType::kCanceled, Side::kBuy, RejectReason::kNone, 0, 2, 2, 0, 100, 2});

  EXPECT_EQ(s.events, 8u);
  EXPECT_EQ(s.accepted, 1u);
  EXPECT_EQ(s.rejected, 4u);
  EXPECT_EQ(s.rejected_invalid, 2u);
  EXPECT_EQ(s.rejected_pool_exhausted, 1u);
  EXPECT_EQ(s.rejected_unknown_order, 1u);
  EXPECT_EQ(s.traded_events, 2u);
  EXPECT_EQ(s.fills(), 1u);
  EXPECT_EQ(s.traded_qty_both_sides, 6u);
  EXPECT_EQ(s.traded_qty(), 3u);
  EXPECT_EQ(s.canceled, 1u);
}

// The hand-built script with a fully known outcome. Ops:
//   1. buy limit 10 @ 990        -> Accepted, rests
//   2. sell limit 4 @ 990        -> Accepted + 2 Traded (fill qty 4)
//   3. sell IOC 9 @ 990          -> Accepted + 2 Traded (fill qty 6) + Canceled (remainder 3)
//   4. buy limit qty 0 (invalid) -> Rejected kInvalidQty
//   5. sell market 5             -> Accepted + Canceled: order 1 is fully
//      filled by now (4 + 6 = 10), so the book is empty and nothing trades
//   6. buy limit 7 @ 995         -> Accepted, rests
//   7. cancel (6)                -> Canceled
//   8. cancel bogus id           -> Rejected kUnknownOrder
// Totals: news 6, cancel_requests 2, accepted 5, rejected 2 (1 invalid,
// 1 unknown), traded_events 4 (2 fills, qty 10), canceled 3, events 14.
template <typename SubmitFn, typename CancelFn>
void DriveScript(SubmitFn&& submit, CancelFn&& cancel) {
  const OrderId resting = submit(Side::kBuy, OrderType::kLimit, 990, 10);
  submit(Side::kSell, OrderType::kLimit, 990, 4);
  submit(Side::kSell, OrderType::kIoc, 990, 9);
  submit(Side::kBuy, OrderType::kLimit, 990, 0);  // invalid qty
  submit(Side::kSell, OrderType::kMarket, 0, 5);  // empty book: accepted, canceled
  const OrderId to_cancel = submit(Side::kBuy, OrderType::kLimit, 995, 7);
  cancel(to_cancel);
  cancel(resting + (std::uint64_t{1} << 32));  // forged generation: unknown
}

void ExpectScriptStats(const EngineStats& s) {
  EXPECT_EQ(s.news, 6u);
  EXPECT_EQ(s.cancel_requests, 2u);
  EXPECT_EQ(s.commands(), 8u);
  EXPECT_EQ(s.accepted, 5u);
  EXPECT_EQ(s.rejected, 2u);
  EXPECT_EQ(s.rejected_invalid, 1u);
  EXPECT_EQ(s.rejected_unknown_order, 1u);
  EXPECT_EQ(s.rejected_pool_exhausted, 0u);
  EXPECT_EQ(s.traded_events, 4u);
  EXPECT_EQ(s.fills(), 2u);
  EXPECT_EQ(s.traded_qty(), 10u);
  EXPECT_EQ(s.canceled, 3u);
  EXPECT_EQ(s.events, 14u);
  // The reconciliation identities every run must satisfy.
  EXPECT_EQ(s.events, s.accepted + s.rejected + s.traded_events + s.canceled);
  EXPECT_EQ(s.news, s.accepted + s.rejected_invalid + s.rejected_pool_exhausted);
}

TEST(MatchingEngineStats, DirectCallsCountScriptExactly) {
  MatchingEngine engine(SmallConfig());
  DriveScript([&engine](Side side, OrderType type, PriceTicks price,
                        Qty qty) { return engine.submit(side, type, price, qty); },
              [&engine](OrderId id) { engine.cancel(id); });
  ExpectScriptStats(engine.stats());
}

TEST(MatchingEngineStats, ProcessPathCountsIdentically) {
  MatchingEngine direct(SmallConfig());
  DriveScript([&direct](Side side, OrderType type, PriceTicks price,
                        Qty qty) { return direct.submit(side, type, price, qty); },
              [&direct](OrderId id) { direct.cancel(id); });

  MatchingEngine via_process(SmallConfig());
  DriveScript(
      [&via_process](Side side, OrderType type, PriceTicks price, Qty qty) {
        // Recover the minted id from the Accepted event so cancels can
        // target it, exactly like a ring-driven caller would.
        OrderId minted = kInvalidOrderId;
        const Command cmd{CommandType::kNew, side, type, 0, qty, price, kInvalidOrderId};
        via_process.process(cmd, [&minted](const Event& e) {
          if (e.kind == EventType::kAccepted) {
            minted = e.order_id;
          }
        });
        return minted;
      },
      [&via_process](OrderId id) {
        const Command cmd{CommandType::kCancel, Side::kBuy, OrderType::kLimit, 0, 0, 0, id};
        via_process.process(cmd, [](const Event&) {});
      });

  ExpectScriptStats(via_process.stats());
  EXPECT_EQ(std::memcmp(&direct.stats(), &via_process.stats(), sizeof(EngineStats)), 0)
      << "the two entry paths must count identically";
}

TEST(MatchingEngineStats, PoolExhaustionIsCounted) {
  MatchingEngine engine(SmallConfig(/*pool_capacity=*/2));
  EXPECT_NE(engine.submit(Side::kBuy, OrderType::kLimit, 990, 1), kInvalidOrderId);
  EXPECT_NE(engine.submit(Side::kBuy, OrderType::kLimit, 991, 1), kInvalidOrderId);
  EXPECT_EQ(engine.submit(Side::kBuy, OrderType::kLimit, 992, 1), kInvalidOrderId);
  const EngineStats& s = engine.stats();
  EXPECT_EQ(s.news, 3u);
  EXPECT_EQ(s.accepted, 2u);
  EXPECT_EQ(s.rejected_pool_exhausted, 1u);
  EXPECT_EQ(s.news, s.accepted + s.rejected_invalid + s.rejected_pool_exhausted);
}

TEST(MatchingEngineStats, RunLoopAccumulates) {
  // Single-threaded run(): preload the ring, set stop, then drain. Stats
  // must cover every ring-delivered command.
  MatchingEngine engine(SmallConfig());
  SPSCQueue<Command> ring(64);
  std::atomic<bool> stop{false};
  const std::vector<Command> cmds = {
      Command{CommandType::kNew, Side::kBuy, OrderType::kLimit, 0, 5, 995, kInvalidOrderId},
      Command{CommandType::kNew, Side::kSell, OrderType::kLimit, 0, 5, 995, kInvalidOrderId},
      Command{CommandType::kCancel, Side::kBuy, OrderType::kLimit, 0, 0, 0, 12345},
  };
  for (const Command& cmd : cmds) {
    ASSERT_TRUE(ring.try_push(cmd));
  }
  stop.store(true, std::memory_order_release);
  std::uint64_t events_seen = 0;
  const std::uint64_t processed =
      engine.run(ring, stop, [&events_seen](const Event&) { ++events_seen; });

  EXPECT_EQ(processed, cmds.size());
  const EngineStats& s = engine.stats();
  EXPECT_EQ(s.commands(), cmds.size());
  EXPECT_EQ(s.news, 2u);
  EXPECT_EQ(s.cancel_requests, 1u);
  EXPECT_EQ(s.events, events_seen) << "stats and the sink must see the same stream";
  EXPECT_EQ(s.accepted, 2u);
  EXPECT_EQ(s.traded_events, 2u);
  EXPECT_EQ(s.rejected_unknown_order, 1u);
  EXPECT_EQ(ring.high_water(), cmds.size()) << "preloading 3 sets the ring's high-water to 3";
}

TEST(MatchingEngineStats, ResetClearsCountersNotBook) {
  MatchingEngine engine(SmallConfig());
  engine.submit(Side::kBuy, OrderType::kLimit, 990, 10);
  engine.submit(Side::kSell, OrderType::kLimit, 1'010, 10);
  ASSERT_EQ(engine.stats().accepted, 2u);

  engine.reset_stats();
  EXPECT_EQ(engine.stats().commands(), 0u);
  EXPECT_EQ(engine.stats().events, 0u);
  EXPECT_EQ(engine.book().open_orders(), 2u) << "reset_stats must not touch the book";

  engine.submit(Side::kBuy, OrderType::kLimit, 991, 1);
  EXPECT_EQ(engine.stats().news, 1u);
}

}  // namespace
}  // namespace lob
