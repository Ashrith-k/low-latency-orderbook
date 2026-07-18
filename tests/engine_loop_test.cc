#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/spsc_queue.h"
#include "lob/types.h"

namespace lob {
namespace {

// Day 4 task 4: MatchingEngine::run — the batched ring-consuming loop. All
// tests here are single-threaded and deterministic: commands are pushed, stop
// is set per the shutdown contract, and run() is called on this thread. The
// two-thread pipeline smoke test is Day 4 task 6.

constexpr PriceTicks kAnchor = 10'000;

EngineConfig SmallConfig() {
  EngineConfig cfg;
  cfg.anchor_price = kAnchor;
  cfg.band_radius = 64;
  cfg.pool_capacity = 4096;
  return cfg;
}

Command NewCmd(Side side, OrderType type, PriceTicks price, Qty qty) {
  Command cmd{};
  cmd.kind = CommandType::kNew;
  cmd.side = side;
  cmd.type = type;
  cmd.qty = qty;
  cmd.price_ticks = price;
  return cmd;
}

Command CancelCmd(OrderId id) {
  Command cmd{};
  cmd.kind = CommandType::kCancel;
  cmd.order_id = id;
  return cmd;
}

struct Script {
  std::vector<Command> commands;
  std::vector<Event> expected;  // the scout engine's event stream
  // Scout end state, for parity checks on the loop-driven engine.
  std::optional<PriceTicks> best_bid;
  std::optional<PriceTicks> best_ask;
  std::uint32_t open_orders = 0;
};

// Builds a deterministic ~1000-command script — resting and crossing limits
// on both sides, IOC/market takers, a sprinkle of invalid adds, cancels of
// previously-minted ids (some already dead: reject coverage) — by running a
// scout engine while choosing ops. Engine behavior is fully deterministic
// (ids come from the pool in allocation order), so a twin engine fed the
// same commands must reproduce the scout's event stream byte for byte.
Script BuildScript(std::size_t op_count) {
  Script script;
  MatchingEngine scout(SmallConfig());
  std::mt19937_64 rng(0xD1E5E1u);  // fixed seed: determinism is the point
  std::vector<OrderId> minted;
  auto sink = [&script, &minted](const Event& e) {
    script.expected.push_back(e);
    if (e.kind == EventType::kAccepted && e.order_id != kInvalidOrderId) {
      minted.push_back(e.order_id);
    }
  };
  for (std::size_t i = 0; i < op_count; ++i) {
    const std::uint64_t roll = rng() % 100;
    Command cmd{};
    if (roll < 30 && !minted.empty()) {
      cmd = CancelCmd(minted[rng() % minted.size()]);
    } else if (roll < 32) {
      cmd = NewCmd(Side::kBuy, OrderType::kLimit, kAnchor, 0);  // invalid qty
    } else {
      const Side side = (rng() % 2 == 0) ? Side::kBuy : Side::kSell;
      const std::uint64_t type_roll = rng() % 10;
      const OrderType type = type_roll < 8   ? OrderType::kLimit
                             : type_roll < 9 ? OrderType::kIoc
                                             : OrderType::kMarket;
      const auto price =
          static_cast<PriceTicks>(kAnchor - 10 + static_cast<PriceTicks>(rng() % 21));
      const auto qty = static_cast<Qty>(1 + rng() % 100);
      cmd = NewCmd(side, type, price, qty);
    }
    scout.process(cmd, sink);
    script.commands.push_back(cmd);
  }
  script.best_bid = scout.book().best_bid();
  script.best_ask = scout.book().best_ask();
  script.open_orders = scout.book().open_orders();
  return script;
}

TEST(EngineLoop, ReproducesDirectProcessingAndDrainsRing) {
  const Script script = BuildScript(1000);
  // Multi-batch coverage: the loop must cross several full batches plus a
  // partial remainder.
  ASSERT_GT(script.commands.size(), 3 * kEngineBatchSize);
  ASSERT_NE(script.commands.size() % kEngineBatchSize, 0u);

  SPSCQueue<Command> ring(2048);  // holds the whole script up front
  for (const Command& cmd : script.commands) {
    ASSERT_TRUE(ring.try_push(cmd));
  }
  std::atomic<bool> stop{false};
  // release: the shutdown contract — stop is set only after the final push.
  stop.store(true, std::memory_order_release);

  MatchingEngine engine(SmallConfig());
  std::vector<Event> got;
  const std::uint64_t processed =
      engine.run(ring, stop, [&got](const Event& e) { got.push_back(e); });

  EXPECT_EQ(processed, script.commands.size());
  EXPECT_TRUE(ring.empty());
  ASSERT_EQ(got.size(), script.expected.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    ASSERT_EQ(std::memcmp(&got[i], &script.expected[i], sizeof(Event)), 0)
        << "event #" << i << " diverges (kind " << to_cstr(got[i].kind) << " vs "
        << to_cstr(script.expected[i].kind) << ")";
  }
  // End-state parity with the scout.
  EXPECT_EQ(engine.book().best_bid(), script.best_bid);
  EXPECT_EQ(engine.book().best_ask(), script.best_ask);
  EXPECT_EQ(engine.book().open_orders(), script.open_orders);
  engine.book().check_invariants();
}

TEST(EngineLoop, EmptyRingWithStopSetReturnsZero) {
  SPSCQueue<Command> ring(8);
  std::atomic<bool> stop{true};
  MatchingEngine engine(SmallConfig());
  std::size_t events = 0;
  const std::uint64_t processed = engine.run(ring, stop, [&events](const Event&) { ++events; });
  EXPECT_EQ(processed, 0u);
  EXPECT_EQ(events, 0u);
}

TEST(EngineLoop, DrainsEverythingPushedBeforeStop) {
  SPSCQueue<Command> ring(8);
  ASSERT_TRUE(ring.try_push(NewCmd(Side::kBuy, OrderType::kLimit, kAnchor - 1, 10)));
  ASSERT_TRUE(ring.try_push(NewCmd(Side::kSell, OrderType::kLimit, kAnchor + 1, 10)));
  ASSERT_TRUE(ring.try_push(NewCmd(Side::kBuy, OrderType::kLimit, kAnchor + 1, 4)));  // crosses
  std::atomic<bool> stop{false};
  stop.store(true, std::memory_order_release);  // per the shutdown contract

  MatchingEngine engine(SmallConfig());
  std::vector<Event> got;
  const std::uint64_t processed =
      engine.run(ring, stop, [&got](const Event& e) { got.push_back(e); });

  EXPECT_EQ(processed, 3u);
  EXPECT_TRUE(ring.empty());
  // Accepted ×3 plus one maker/taker Traded pair for the crossing buy.
  ASSERT_EQ(got.size(), 5u);
  EXPECT_EQ(got[0].kind, EventType::kAccepted);
  EXPECT_EQ(got[1].kind, EventType::kAccepted);
  EXPECT_EQ(got[2].kind, EventType::kAccepted);
  EXPECT_EQ(got[3].kind, EventType::kTraded);
  EXPECT_EQ(got[4].kind, EventType::kTraded);
  EXPECT_EQ(engine.book().open_orders(), 2u);  // resting bid + partially filled ask
}

}  // namespace
}  // namespace lob
