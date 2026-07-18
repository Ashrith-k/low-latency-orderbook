#include <benchmark/benchmark.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/naive_book.h"
#include "lob/order_book.h"
#include "lob/price_ladder.h"
#include "lob/types.h"
#include "script_rng.h"

// Day 6 task 1: the DESIGN §4.3 comparison — add/cancel on NaiveBook
// (std::map + std::list baseline) vs OrderBook (banded-array ladder), driven
// by identical pregenerated op scripts.
//
// Scenarios, each over book depth = state.range(0) resting orders:
//   BookAddRest:         fill an empty book with depth passive limits (timed).
//   BookSteadyAddCancel: prefill depth orders (untimed), then a timed 64Ki-op
//                        50/50 add/cancel window that holds depth roughly
//                        constant — the steady-state maintenance cost.
// items_per_second == book ops per second; that ratio is the resume number.
//
// Scripts are pregenerated with bench-local splitmix64 plus the project's
// harmonic "Zipf-ish" offset weighting (workload_gen.h rationale): quotes
// cluster at the touch, buys strictly below / sells strictly above a fixed
// mid, so nothing ever crosses and the rest-only add_limit path runs on a
// realistically shaped, uncrossed book. WorkloadGenerator itself cannot be
// reused here: its cancel synthesis runs a full-matching shadow engine, and
// those ids are only valid on a full-matching replay (task 2's territory).
//
// Cancels name their victim by *position* in the live-order set, decided at
// script time from the deterministic live-count evolution; each replayer
// keeps its own id vector under the same swap-remove discipline. Both books
// therefore execute structurally identical sequences — same prices, same
// quantities, same victim choices — and every cancel hits a live order (the
// real removal path, never the reject path).
//
// Honesty (DESIGN §11): Codespaces numbers are indicative — 2 vCPUs that are
// SMT siblings of one physical core, unpinned, in a VM. Any number that
// leaves this file travels with CPU model, kernel, compiler, and flags;
// README-grade numbers come from the release preset, bare metal if possible.
namespace {

using lob::benchutil::MakeZipfCdf;
using lob::benchutil::SampleOffset;
using lob::benchutil::ScriptRng;

constexpr lob::PriceTicks kMid = 100'000;  // fixed mid; anchor of the banded ladder
constexpr std::uint32_t kMaxOffset = 256;  // quotes span mid ± [1, 257] — ~257 levels/side
constexpr lob::Qty kMinQty = 1;
constexpr lob::Qty kMaxQty = 100;
constexpr std::uint64_t kSeed = 20260718;
constexpr std::size_t kMixedOps = std::size_t{1} << 16;  // steady-state timed window

// One scripted book op, decided before any book exists. A cancel carries a
// position into the replayer's live-order vector, never an id — the two
// books mint different ids for the same add.
struct Op {
  lob::PriceTicks price = 0;  // add only
  lob::Qty qty = 0;           // add only
  std::uint32_t victim = 0;   // cancel only: index into the live set
  lob::Side side = lob::Side::kBuy;
  bool is_add = false;
};

Op MakeAdd(ScriptRng& rng, const std::vector<std::uint64_t>& cdf) {
  Op op;
  op.is_add = true;
  op.side = rng.Bounded(2) == 0 ? lob::Side::kBuy : lob::Side::kSell;
  op.qty = static_cast<lob::Qty>(kMinQty + rng.Bounded(std::uint64_t{kMaxQty} - kMinQty + 1));
  // Buys strictly below the mid, sells strictly above: scripts never cross,
  // so the rest-only add_limit path runs on an uncrossed book.
  const lob::PriceTicks offset = SampleOffset(rng, cdf);
  op.price = op.side == lob::Side::kBuy ? kMid - 1 - offset : kMid + 1 + offset;
  return op;
}

std::vector<Op> MakeAddScript(std::size_t n, ScriptRng& rng,
                              const std::vector<std::uint64_t>& cdf) {
  std::vector<Op> script;
  script.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    script.push_back(MakeAdd(rng, cdf));
  }
  return script;
}

// A 50/50 add/cancel stream starting from live_count resting orders. Victim
// positions are drawn against the deterministic live-count evolution the
// replayer reproduces (add: push_back; cancel: swap-remove at victim).
std::vector<Op> MakeMixedScript(std::size_t live_count, std::size_t ops, ScriptRng& rng,
                                const std::vector<std::uint64_t>& cdf) {
  std::vector<Op> script;
  script.reserve(ops);
  for (std::size_t i = 0; i < ops; ++i) {
    if (live_count > 0 && rng.Bounded(2) == 0) {
      Op op;
      op.victim = static_cast<std::uint32_t>(rng.Bounded(live_count));
      script.push_back(op);
      --live_count;
    } else {
      script.push_back(MakeAdd(rng, cdf));
      ++live_count;
    }
  }
  return script;
}

// The two drivers give the books one replay surface over their Day-2
// structural primitives. The API asymmetry (NaiveBook wants caller-minted
// ids and an event vector; OrderBook mints ids, event-less add_limit/cancel)
// is the design under test, not an unfairness: each side pays exactly what
// its own interface costs.
class NaiveDriver {
 public:
  explicit NaiveDriver(std::size_t /*pool_capacity*/) {}

  lob::OrderId add(const Op& op) {
    events_.clear();  // capacity is retained; steady state re-uses the buffer
    const lob::OrderId id = next_id_++;
    book_.add_limit(id, op.side, op.price, op.qty, events_);
    return id;
  }

  bool cancel(lob::OrderId id) {
    events_.clear();
    return book_.cancel(id, events_);
  }

 private:
  lob::NaiveBook book_;
  std::vector<lob::Event> events_;
  lob::OrderId next_id_ = 1;
};

class BookDriver {
 public:
  explicit BookDriver(std::size_t pool_capacity)
      : book_(kMid, lob::kDefaultBandRadius, static_cast<std::uint32_t>(pool_capacity)) {}

  lob::OrderId add(const Op& op) { return book_.add_limit(op.side, op.price, op.qty); }
  bool cancel(lob::OrderId id) { return book_.cancel(id); }

 private:
  lob::OrderBook book_;
};

template <typename Driver>
void Replay(Driver& driver, const std::vector<Op>& script, std::vector<lob::OrderId>& live) {
  for (const Op& op : script) {
    if (op.is_add) {
      // Mutable locals: benchmark 1.9+ deprecates const-ref DoNotOptimize
      // (it can still permit unwanted optimizations).
      lob::OrderId id = driver.add(op);
      benchmark::DoNotOptimize(id);
      live.push_back(id);
    } else {
      const lob::OrderId id = live[op.victim];
      live[op.victim] = live.back();
      live.pop_back();
      bool ok = driver.cancel(id);
      benchmark::DoNotOptimize(ok);
      assert(ok && "scripted cancels always target a live order");
    }
  }
}

template <typename Driver>
void BM_BookAddRest(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  ScriptRng rng(kSeed);
  const std::vector<std::uint64_t> cdf = MakeZipfCdf(kMaxOffset);
  const std::vector<Op> script = MakeAddScript(depth, rng, cdf);
  std::vector<lob::OrderId> live;
  live.reserve(depth);
  std::optional<Driver> driver;
  for (auto _ : state) {
    state.PauseTiming();
    driver.emplace(depth);  // fresh book; the previous one is destroyed untimed
    live.clear();
    state.ResumeTiming();
    Replay(*driver, script, live);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(depth));
}

template <typename Driver>
void BM_BookSteadyAddCancel(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  ScriptRng rng(kSeed);
  const std::vector<std::uint64_t> cdf = MakeZipfCdf(kMaxOffset);
  const std::vector<Op> prefill = MakeAddScript(depth, rng, cdf);
  const std::vector<Op> mixed = MakeMixedScript(depth, kMixedOps, rng, cdf);
  std::vector<lob::OrderId> live;
  live.reserve(depth + kMixedOps);
  std::optional<Driver> driver;
  for (auto _ : state) {
    state.PauseTiming();
    driver.emplace(depth + kMixedOps);  // headroom for worst-case all-add drift
    live.clear();
    Replay(*driver, prefill, live);
    state.ResumeTiming();
    Replay(*driver, mixed, live);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(kMixedOps));
}

}  // namespace

BENCHMARK_TEMPLATE(BM_BookAddRest, NaiveDriver)
    ->Name("BookAddRest/naive")
    ->RangeMultiplier(8)
    ->Range(1024, 65536)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_BookAddRest, BookDriver)
    ->Name("BookAddRest/book")
    ->RangeMultiplier(8)
    ->Range(1024, 65536)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_BookSteadyAddCancel, NaiveDriver)
    ->Name("BookSteadyAddCancel/naive")
    ->RangeMultiplier(8)
    ->Range(1024, 65536)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_BookSteadyAddCancel, BookDriver)
    ->Name("BookSteadyAddCancel/book")
    ->RangeMultiplier(8)
    ->Range(1024, 65536)
    ->Unit(benchmark::kMillisecond);
