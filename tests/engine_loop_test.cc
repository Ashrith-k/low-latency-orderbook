#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/spsc_queue.h"
#include "lob/types.h"
#include "script_test_util.h"

namespace lob {
namespace {

using testutil::BuildScript;
using testutil::CancelCmd;
using testutil::kScriptAnchor;
using testutil::NewCmd;
using testutil::Script;
using testutil::ScriptEngineConfig;

// Day 4 task 4: MatchingEngine::run — the batched ring-consuming loop. All
// tests here are single-threaded and deterministic: commands are pushed, stop
// is set per the shutdown contract, and run() is called on this thread. The
// two-thread pipeline smoke test is Day 4 task 6 (pipeline_smoke_test.cc).

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

  MatchingEngine engine(ScriptEngineConfig());
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
  MatchingEngine engine(ScriptEngineConfig());
  std::size_t events = 0;
  const std::uint64_t processed = engine.run(ring, stop, [&events](const Event&) { ++events; });
  EXPECT_EQ(processed, 0u);
  EXPECT_EQ(events, 0u);
}

TEST(EngineLoop, DrainsEverythingPushedBeforeStop) {
  SPSCQueue<Command> ring(8);
  ASSERT_TRUE(ring.try_push(NewCmd(Side::kBuy, OrderType::kLimit, kScriptAnchor - 1, 10)));
  ASSERT_TRUE(ring.try_push(NewCmd(Side::kSell, OrderType::kLimit, kScriptAnchor + 1, 10)));
  // Crosses the resting ask.
  ASSERT_TRUE(ring.try_push(NewCmd(Side::kBuy, OrderType::kLimit, kScriptAnchor + 1, 4)));
  std::atomic<bool> stop{false};
  stop.store(true, std::memory_order_release);  // per the shutdown contract

  MatchingEngine engine(ScriptEngineConfig());
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
