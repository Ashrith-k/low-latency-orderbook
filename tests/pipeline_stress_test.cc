#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/spsc_queue.h"
#include "lob/types.h"
#include "script_test_util.h"

namespace lob {
namespace {

using testutil::BuildScript;
using testutil::Script;
using testutil::ScriptEngineConfig;

// Day 4 task 7: the long-running pipeline stress for the TSan CI job. Same
// three-thread topology as pipeline_smoke_test.cc, but 5× the ops and three
// sequential rounds with asymmetric ring sizes, so each round starves a
// different edge:
//   {64, 128}   both rings tiny — constant churn on every boundary
//   {1024, 64}  event ring starved — the engine blocks on its sink mid-batch
//   {64, 2048}  command ring starved — the engine spins hungry on pops
// Re-running rounds also re-exercises the raciest code — the stop/drain
// shutdown protocol — once per round on a fresh engine and rings.
//
// LOB_PIPELINE_STRESS_OPS overrides ops-per-round for longer local soaks
// (same env convention as LOB_DIFF_MATCH_SEED). Note the pool is fixed at
// 128k: a soak big enough to exhaust it just adds deterministic
// kPoolExhausted rejects, which the scout reproduces — coverage, not failure.

std::size_t OpsPerRound() {
  if (const char* env = std::getenv("LOB_PIPELINE_STRESS_OPS")) {
    const unsigned long long v = std::strtoull(env, nullptr, 0);
    if (v > 0) {
      return static_cast<std::size_t>(v);
    }
  }
  return 250'000;
}

struct RingSizes {
  std::size_t cmd;
  std::size_t evt;
};

TEST(PipelineStress, RepeatedRoundsUnderAsymmetricRingPressure) {
  const EngineConfig cfg = ScriptEngineConfig(1u << 17);
  const Script script = BuildScript(OpsPerRound(), cfg);

  constexpr std::array<RingSizes, 3> kRounds = {{{64, 128}, {1024, 64}, {64, 2048}}};
  for (std::size_t round = 0; round < 3; ++round) {
    SCOPED_TRACE("round " + std::to_string(round) + " (cmd ring " +
                 std::to_string(kRounds[round].cmd) + ", evt ring " +
                 std::to_string(kRounds[round].evt) + ")");

    SPSCQueue<Command> cmd_ring(kRounds[round].cmd);
    SPSCQueue<Event> evt_ring(kRounds[round].evt);
    std::atomic<bool> stop{false};
    std::atomic<bool> engine_done{false};
    MatchingEngine engine(cfg);
    std::uint64_t processed = 0;  // written by the engine thread, read after join
    std::vector<Event> got;       // written by the drainer thread, read after join
    got.reserve(script.expected.size());

    std::thread producer([&script, &cmd_ring, &stop] {
      for (const Command& cmd : script.commands) {
        while (!cmd_ring.try_push(cmd)) {
          std::this_thread::yield();
        }
      }
      // release: the run() shutdown contract — set only after the final push.
      stop.store(true, std::memory_order_release);
    });

    std::thread engine_thread([&engine, &cmd_ring, &evt_ring, &stop, &processed] {
      processed = engine.run(cmd_ring, stop, [&evt_ring](const Event& e) {
        // Test-local lossless policy (as in the smoke test): counts must
        // reconcile exactly; count-and-drop is the Day-5 logger's policy.
        while (!evt_ring.try_push(e)) {
        }
      });
    });

    std::thread drainer([&evt_ring, &engine_done, &got] {
      std::array<Event, 64> buf;
      bool done = false;
      for (;;) {
        const std::size_t n = evt_ring.try_pop_batch(buf.data(), buf.size());
        if (n > 0) {
          got.insert(got.end(), buf.data(), buf.data() + n);
          continue;
        }
        if (done) {
          return;  // ring seen empty after engine_done was observed: drained
        }
        // acquire: pairs with main's release store after the engine thread
        // is joined — once seen, the next pop sees every pushed event.
        done = engine_done.load(std::memory_order_acquire);
        if (!done) {
          std::this_thread::yield();
        }
      }
    });

    producer.join();
    engine_thread.join();
    // release: everything the engine pushed happened-before this store.
    engine_done.store(true, std::memory_order_release);
    drainer.join();

    // Full reconciliation, every round, on a fresh engine.
    EXPECT_EQ(processed, script.commands.size());
    EXPECT_TRUE(cmd_ring.empty());
    EXPECT_TRUE(evt_ring.empty());
    ASSERT_EQ(got.size(), script.expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
      ASSERT_EQ(std::memcmp(&got[i], &script.expected[i], sizeof(Event)), 0)
          << "event #" << i << " diverges (kind " << to_cstr(got[i].kind) << " vs "
          << to_cstr(script.expected[i].kind) << ")";
    }
    EXPECT_EQ(engine.book().best_bid(), script.best_bid);
    EXPECT_EQ(engine.book().best_ask(), script.best_ask);
    EXPECT_EQ(engine.book().open_orders(), script.open_orders);
    engine.book().check_invariants();
  }
}

}  // namespace
}  // namespace lob
