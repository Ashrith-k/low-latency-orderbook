#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

// Day 4 task 6: the full DESIGN §3 pipeline — producer thread → command ring
// → engine thread (MatchingEngine::run) → event ring → drainer thread — with
// end-to-end reconciliation. Runs under the TSan CI job via the concurrency
// label (DESIGN §10.4 lists the pipeline smoke among the TSan tests).
//
// The key property: SPSC rings preserve FIFO order and the engine is
// deterministic, so thread timing may change *batching* but never order or
// content — the three-thread pipeline must reproduce the single-threaded
// scout's event stream byte for byte. Rings are deliberately small relative
// to the script, so the producer regularly finds the command ring full and
// the engine regularly finds the event ring full: both backpressure edges
// get real contention, not just the happy path.

TEST(PipelineSmoke, CountsReconcileAndStreamMatchesScout) {
  const EngineConfig cfg = ScriptEngineConfig(1u << 16);  // no pool exhaustion at this scale
  const Script script = BuildScript(50'000, cfg);

  SPSCQueue<Command> cmd_ring(256);
  SPSCQueue<Event> evt_ring(512);
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
      // Test-local lossless policy: spin until the drainer frees a slot, so
      // counts must reconcile exactly. The production backpressure policy
      // (count-and-drop, never block the engine) is the Day-5 logger's.
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
      // acquire: pairs with main's release store after the engine thread is
      // joined — once seen, the next pop attempt sees every pushed event.
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

  // Counts reconcile end to end: nothing lost, nothing invented.
  EXPECT_EQ(processed, script.commands.size());
  EXPECT_TRUE(cmd_ring.empty());
  EXPECT_TRUE(evt_ring.empty());
  ASSERT_EQ(got.size(), script.expected.size());

  // Readable reconciliation: every New command got exactly one verdict.
  std::size_t new_cmds = 0;
  for (const Command& cmd : script.commands) {
    new_cmds += cmd.kind == CommandType::kNew ? 1 : 0;
  }
  std::size_t accepted = 0;
  std::size_t new_rejects = 0;
  std::size_t traded = 0;
  for (const Event& e : got) {
    accepted += e.kind == EventType::kAccepted ? 1 : 0;
    traded += e.kind == EventType::kTraded ? 1 : 0;
    new_rejects +=
        (e.kind == EventType::kRejected && e.reason != RejectReason::kUnknownOrder) ? 1 : 0;
  }
  EXPECT_EQ(accepted + new_rejects, new_cmds);
  EXPECT_EQ(traded % 2, 0u) << "Traded events must come in maker/taker pairs";

  // The stream itself: byte-identical to the scout's.
  for (std::size_t i = 0; i < got.size(); ++i) {
    ASSERT_EQ(std::memcmp(&got[i], &script.expected[i], sizeof(Event)), 0)
        << "event #" << i << " diverges (kind " << to_cstr(got[i].kind) << " vs "
        << to_cstr(script.expected[i].kind) << ")";
  }

  // End-state parity with the scout, and the book is internally consistent.
  EXPECT_EQ(engine.book().best_bid(), script.best_bid);
  EXPECT_EQ(engine.book().best_ask(), script.best_ask);
  EXPECT_EQ(engine.book().open_orders(), script.open_orders);
  engine.book().check_invariants();
}

}  // namespace
}  // namespace lob
