#ifndef LOB_TESTS_SCRIPT_TEST_UTIL_H_
#define LOB_TESTS_SCRIPT_TEST_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/types.h"

// Shared deterministic command-script builder. Used by the engine-loop tests
// (Day 4 task 4) and the pipeline smoke/stress tests (tasks 6–7): one scout
// engine defines the expected event stream, and any pipeline fed the same
// commands must reproduce it byte for byte.

namespace lob::testutil {

inline constexpr PriceTicks kScriptAnchor = 10'000;

// Band and anchor fixed; pool capacity is the knob callers size to taste
// (small enough to exercise kPoolExhausted, or large enough to avoid it).
inline EngineConfig ScriptEngineConfig(std::uint32_t pool_capacity = 4096) {
  EngineConfig cfg;
  cfg.anchor_price = kScriptAnchor;
  cfg.band_radius = 64;
  cfg.pool_capacity = pool_capacity;
  return cfg;
}

inline Command NewCmd(Side side, OrderType type, PriceTicks price, Qty qty) {
  Command cmd{};
  cmd.kind = CommandType::kNew;
  cmd.side = side;
  cmd.type = type;
  cmd.qty = qty;
  cmd.price_ticks = price;
  return cmd;
}

inline Command CancelCmd(OrderId id) {
  Command cmd{};
  cmd.kind = CommandType::kCancel;
  cmd.order_id = id;
  return cmd;
}

struct Script {
  std::vector<Command> commands;
  std::vector<Event> expected;  // the scout engine's event stream
  // Scout end state, for parity checks on the engine that replays the script.
  std::optional<PriceTicks> best_bid;
  std::optional<PriceTicks> best_ask;
  std::uint32_t open_orders = 0;
};

// Builds a deterministic command script — resting and crossing limits on
// both sides, IOC/market takers, a sprinkle of invalid adds, cancels of
// previously-minted ids (some already dead: reject coverage) — by running a
// scout engine (configured with `cfg`) while choosing ops. Engine behavior
// is fully deterministic (ids come from the pool in allocation order), so a
// twin engine fed the same commands must reproduce the scout's event stream
// byte for byte. The replaying engine must use the same `cfg`.
inline Script BuildScript(std::size_t op_count, const EngineConfig& cfg = ScriptEngineConfig()) {
  Script script;
  MatchingEngine scout(cfg);
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
      cmd = NewCmd(Side::kBuy, OrderType::kLimit, kScriptAnchor, 0);  // invalid qty
    } else {
      const Side side = (rng() % 2 == 0) ? Side::kBuy : Side::kSell;
      const std::uint64_t type_roll = rng() % 10;
      const OrderType type = type_roll < 8   ? OrderType::kLimit
                             : type_roll < 9 ? OrderType::kIoc
                                             : OrderType::kMarket;
      const auto price =
          static_cast<PriceTicks>(kScriptAnchor - 10 + static_cast<PriceTicks>(rng() % 21));
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

}  // namespace lob::testutil

#endif  // LOB_TESTS_SCRIPT_TEST_UTIL_H_
