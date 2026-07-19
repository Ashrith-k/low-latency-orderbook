#include "lob/workload_gen.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/types.h"

namespace lob {
namespace {

// Day 5 task 3: the workload generator. The load-bearing property is the
// shadow-engine design: every cancel in a generated stream must target an id
// that is live at that point of a fresh replay — proven here by replaying
// with zero unknown-order rejects and full end-state parity.

WorkloadConfig TestConfig(std::uint64_t seed = 1) {
  WorkloadConfig cfg;
  cfg.engine.anchor_price = 10'000;
  cfg.engine.pool_capacity = std::uint32_t{1} << 17;  // no exhaustion at test scale
  cfg.seed = seed;
  return cfg;
}

TEST(WorkloadGenerator, SameSeedProducesIdenticalStream) {
  WorkloadGenerator a(TestConfig(7));
  WorkloadGenerator b(TestConfig(7));
  for (int i = 0; i < 20'000; ++i) {
    const Command ca = a.next();
    const Command cb = b.next();
    ASSERT_EQ(std::memcmp(&ca, &cb, sizeof(Command)), 0) << "streams diverge at command " << i;
  }
}

TEST(WorkloadGenerator, DifferentSeedProducesDifferentStream) {
  const std::vector<Command> a = generate_workload(TestConfig(1), 1'000);
  const std::vector<Command> b = generate_workload(TestConfig(2), 1'000);
  EXPECT_NE(std::memcmp(a.data(), b.data(), a.size() * sizeof(Command)), 0)
      << "1000 commands from different seeds should not be byte-identical";
}

TEST(WorkloadGenerator, GenerateWorkloadHelperMatchesManualLoop) {
  WorkloadGenerator gen(TestConfig(3));
  std::vector<Command> manual;
  manual.reserve(5'000);
  for (int i = 0; i < 5'000; ++i) {
    manual.push_back(gen.next());
  }
  const std::vector<Command> helper = generate_workload(TestConfig(3), 5'000);
  ASSERT_EQ(helper.size(), manual.size());
  EXPECT_EQ(std::memcmp(helper.data(), manual.data(), manual.size() * sizeof(Command)), 0);
}

TEST(WorkloadGenerator, OpMixTracksConfiguredWeights) {
  constexpr std::size_t kOps = 100'000;
  WorkloadGenerator gen(TestConfig(11));
  for (std::size_t i = 0; i < kOps; ++i) {
    gen.next();
  }
  const WorkloadCounts& c = gen.counts();
  EXPECT_EQ(c.limits + c.markets + c.iocs + c.cancels, kOps);

  // Defaults: limit 60 / market 5 / IOC 15 / cancel 20 (of 100). Sampling
  // noise at 100k is ~±0.5%; degraded cancels inflate limits slightly, so
  // give every bound comfortable slack and pin degradation as rare.
  const auto frac = [](std::uint64_t n) {
    return static_cast<double>(n) / static_cast<double>(kOps);
  };
  EXPECT_NEAR(frac(c.markets), 0.05, 0.02);
  EXPECT_NEAR(frac(c.iocs), 0.15, 0.02);
  EXPECT_NEAR(frac(c.cancels), 0.20, 0.02);
  EXPECT_NEAR(frac(c.limits), 0.60, 0.03);
  EXPECT_LT(frac(c.degraded_cancels), 0.01);
}

TEST(WorkloadGenerator, CommandsRespectConfiguredBounds) {
  const WorkloadConfig cfg = TestConfig(13);
  const std::vector<Command> cmds = generate_workload(cfg, 50'000);

  const PriceTicks max_price =
      cfg.engine.anchor_price + PriceTicks{cfg.walk_radius} + PriceTicks{cfg.max_price_offset};
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const Command& cmd = cmds[i];
    ASSERT_EQ(cmd.pad0, 0u) << "command " << i;
    if (cmd.kind == CommandType::kNew) {
      ASSERT_GE(cmd.qty, cfg.min_qty) << "command " << i;
      ASSERT_LE(cmd.qty, cfg.max_qty) << "command " << i;
      ASSERT_EQ(cmd.order_id, kInvalidOrderId) << "command " << i;
      if (cmd.type == OrderType::kMarket) {
        ASSERT_EQ(cmd.price_ticks, 0) << "market price must be 0 for deterministic bytes";
      } else {
        ASSERT_GE(cmd.price_ticks, 1) << "command " << i;
        ASSERT_LE(cmd.price_ticks, max_price) << "command " << i;
      }
    } else {
      ASSERT_EQ(cmd.kind, CommandType::kCancel);
      ASSERT_NE(cmd.order_id, kInvalidOrderId) << "command " << i;
      ASSERT_EQ(cmd.qty, 0u) << "command " << i;
      ASSERT_EQ(cmd.price_ticks, 0) << "command " << i;
    }
  }
}

TEST(WorkloadGenerator, ReplayIsValidAndMatchesShadowBook) {
  constexpr std::size_t kOps = 100'000;
  const WorkloadConfig cfg = TestConfig(42);
  WorkloadGenerator gen(cfg);
  std::vector<Command> cmds;
  cmds.reserve(kOps);
  for (std::size_t i = 0; i < kOps; ++i) {
    cmds.push_back(gen.next());
  }

  // A fresh engine with the SAME EngineConfig must mint the same ids.
  MatchingEngine engine(cfg.engine);
  std::uint64_t unknown = 0;
  std::uint64_t invalid = 0;
  std::uint64_t exhausted = 0;
  std::uint64_t traded = 0;
  for (const Command& cmd : cmds) {
    engine.process(cmd, [&](const Event& e) {
      if (e.kind == EventType::kTraded) {
        ++traded;
      } else if (e.kind == EventType::kRejected) {
        switch (e.reason) {
          case RejectReason::kUnknownOrder:
            ++unknown;
            break;
          case RejectReason::kInvalidQty:
          case RejectReason::kInvalidPrice:
            ++invalid;
            break;
          case RejectReason::kPoolExhausted:
            ++exhausted;
            break;
          case RejectReason::kNone:
            break;
        }
      }
    });
  }

  // The shadow-engine guarantee: every cancel targeted a then-live id, and
  // the generator never emits an invalid add.
  EXPECT_EQ(unknown, 0u);
  EXPECT_EQ(invalid, 0u);
  EXPECT_EQ(exhausted, 0u);
  // The mix must actually exercise matching (Traded come in pairs).
  EXPECT_GT(traded, 2'000u);
  EXPECT_EQ(traded % 2, 0u);

  // End-state parity with the generator's shadow book, and both consistent.
  EXPECT_EQ(engine.book().best_bid(), gen.shadow_book().best_bid());
  EXPECT_EQ(engine.book().best_ask(), gen.shadow_book().best_ask());
  EXPECT_EQ(engine.book().open_orders(), gen.shadow_book().open_orders());
  EXPECT_EQ(gen.live_orders(), engine.book().open_orders());
  engine.book().check_invariants();
}

TEST(WorkloadGenerator, CancelOnlyMixDegradesGracefully) {
  WorkloadConfig cfg = TestConfig(5);
  cfg.limit_weight = 0;
  cfg.market_weight = 0;
  cfg.ioc_weight = 0;
  cfg.cancel_weight = 1;

  WorkloadGenerator gen(cfg);
  MatchingEngine engine(cfg.engine);
  std::uint64_t unknown = 0;
  for (int i = 0; i < 1'000; ++i) {
    engine.process(gen.next(), [&](const Event& e) {
      unknown +=
          (e.kind == EventType::kRejected && e.reason == RejectReason::kUnknownOrder) ? 1 : 0;
    });
  }

  const WorkloadCounts& c = gen.counts();
  EXPECT_EQ(unknown, 0u);
  EXPECT_EQ(c.limits + c.cancels, 1'000u);
  EXPECT_EQ(c.degraded_cancels, c.limits) << "every limit must come from a degraded cancel";
  EXPECT_GT(c.cancels, 0u);
  EXPECT_EQ(c.markets + c.iocs, 0u);
}

}  // namespace
}  // namespace lob
