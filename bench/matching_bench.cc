#include <benchmark/benchmark.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/naive_book.h"
#include "lob/order_book.h"
#include "lob/price_ladder.h"
#include "lob/types.h"
#include "script_rng.h"

// Day 6 task 2: matching throughput by book depth — the full matching path
// (OrderBook::add / NaiveBook::add) under crossing flow, NaiveBook vs
// OrderBook, driven by identical pregenerated scripts (task 1's discipline).
//
// Scenarios:
//   MatchSteady, over background depth = state.range(0) resting orders:
//     A Zipf-shaped background book (buys <= mid-1, sells >= mid+1) is
//     prefilled untimed and never touched again. The timed window replays
//     maker/taker pairs at the mid: a passive limit rests inside the spread,
//     then an opposite-side IOC of equal qty consumes exactly that order.
//     Every pair restores the book, so the loop is indefinitely sustainable
//     with no PauseTiming anywhere — depth enters only through the cost of
//     maintaining a big book around the matching (map size vs banded array).
//   MatchSweep, over sweep breadth K = state.range(0):
//     Each round rests K makers laddered one per level away from the mid,
//     then one IOC sized to consume all K — the taker crosses K price levels
//     and drains everything the round added. Sides alternate per round. Two
//     permanent floor orders (see BM_MatchSweep) keep the swept side from
//     ever being truly empty. This isolates the per-level walk: map
//     erase+rebalance per exhausted level vs the ladder's best-price cursor.
//   MatchSweepDrain: MatchSweep without the floors — every round drains its
//     side truly empty, pinning the ladder's empty-side transition cost (the
//     Day-6 task-5 optimization target; see that scenario's comment).
//
// items_per_second == commands/s; the fills/s counter is exact by
// construction (pairs, or rounds*K — note MatchSweep does K+1 commands per
// K fills). The naive side's per-pair/per-level map-node churn is its honest
// baseline cost, not a rigging; OrderBook uses the event-less add overload
// (the no-op sink inlines away — matching cost in isolation; evented
// pipeline cost belongs to the task-3 e2e bench).
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
constexpr std::uint32_t kMaxOffset = 256;  // background quotes span mid ± [1, 257]
constexpr lob::Qty kMinQty = 1;
constexpr lob::Qty kMaxQty = 100;
constexpr std::uint64_t kSeed = 20260718;
constexpr std::size_t kSteadyPairs = 8192;  // pairs per timed window (2 commands each)
constexpr std::size_t kSweepRounds = 4096;  // rounds per timed window (K+1 commands each)

// One scripted command of crossing flow. Matching needs no cancels and no
// victim bookkeeping — takers remove their own makers — so unlike task 1's
// script every op is an add and ids never appear.
struct Op {
  lob::PriceTicks price = 0;
  lob::Qty qty = 0;
  lob::Side side = lob::Side::kBuy;
  lob::OrderType type = lob::OrderType::kLimit;
};

lob::Qty RandQty(ScriptRng& rng) {
  return static_cast<lob::Qty>(kMinQty + rng.Bounded(std::uint64_t{kMaxQty} - kMinQty + 1));
}

// Maker/taker pair at the mid: the passive limit rests strictly inside the
// spread (background is at mid-1 / mid+1 or worse), the IOC consumes exactly
// it and stops — the background book is provably never touched.
std::vector<Op> MakeSteadyPairScript(std::size_t pairs, ScriptRng& rng) {
  std::vector<Op> script;
  script.reserve(2 * pairs);
  for (std::size_t i = 0; i < pairs; ++i) {
    const bool maker_buys = rng.Bounded(2) == 0;
    const lob::Qty qty = RandQty(rng);
    script.push_back(
        {kMid, qty, maker_buys ? lob::Side::kBuy : lob::Side::kSell, lob::OrderType::kLimit});
    script.push_back(
        {kMid, qty, maker_buys ? lob::Side::kSell : lob::Side::kBuy, lob::OrderType::kIoc});
  }
  return script;
}

// Each round: K makers, one per level, laddered away from the mid on one
// side; then one IOC at the far end of the ladder, sized to the exact total,
// sweeping all K levels and leaving the book empty. Sides alternate.
std::vector<Op> MakeSweepScript(std::size_t rounds, std::uint32_t k, ScriptRng& rng) {
  std::vector<Op> script;
  script.reserve(rounds * (std::size_t{k} + 1));
  for (std::size_t r = 0; r < rounds; ++r) {
    const bool sell_round = r % 2 == 0;
    lob::Qty total = 0;
    for (std::uint32_t j = 0; j < k; ++j) {
      const lob::Qty qty = RandQty(rng);
      total += qty;
      const auto step = static_cast<lob::PriceTicks>(j);
      script.push_back({sell_round ? kMid + step : kMid - step, qty,
                        sell_round ? lob::Side::kSell : lob::Side::kBuy, lob::OrderType::kLimit});
    }
    const auto reach = static_cast<lob::PriceTicks>(k - 1);
    script.push_back({sell_round ? kMid + reach : kMid - reach, total,
                      sell_round ? lob::Side::kBuy : lob::Side::kSell, lob::OrderType::kIoc});
  }
  return script;
}

// One replay surface over the two books' full matching entry points. The API
// asymmetry (NaiveBook wants caller-minted ids and an event vector;
// OrderBook mints ids and offers an event-less overload) is the design under
// test: each side pays exactly what its own interface costs.
class NaiveDriver {
 public:
  explicit NaiveDriver(std::size_t /*pool_capacity*/) {}

  lob::OrderId add(const Op& op) {
    events_.clear();  // capacity is retained; steady state re-uses the buffer
    const lob::OrderId id = next_id_++;
    book_.add(id, op.side, op.type, op.price, op.qty, events_);
    return id;
  }

  std::size_t open_orders() const { return book_.open_orders(); }

 private:
  lob::NaiveBook book_;
  std::vector<lob::Event> events_;
  lob::OrderId next_id_ = 1;
};

class BookDriver {
 public:
  explicit BookDriver(std::size_t pool_capacity)
      : book_(kMid, lob::kDefaultBandRadius, static_cast<std::uint32_t>(pool_capacity)) {}

  lob::OrderId add(const Op& op) { return book_.add(op.side, op.type, op.price, op.qty); }

  std::size_t open_orders() const { return book_.open_orders(); }

 private:
  lob::OrderBook book_;
};

template <typename Driver>
void Replay(Driver& driver, const std::vector<Op>& script) {
  for (const Op& op : script) {
    // Mutable local: benchmark 1.9+ deprecates const-ref DoNotOptimize.
    lob::OrderId id = driver.add(op);
    benchmark::DoNotOptimize(id);
  }
}

template <typename Driver>
void BM_MatchSteady(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  ScriptRng rng(kSeed);
  const std::vector<std::uint64_t> cdf = MakeZipfCdf(kMaxOffset);
  // Background prefill (untimed: before the state loop). Passive shape as in
  // task 1: buys strictly below the mid, sells strictly above, Zipf-clustered
  // at the touch. +8 pool slots cover the transient pair (alloc-before-sweep
  // means maker and taker briefly coexist).
  Driver driver(depth + 8);
  for (std::size_t i = 0; i < depth; ++i) {
    const bool buy = rng.Bounded(2) == 0;
    const lob::PriceTicks offset = SampleOffset(rng, cdf);
    Op op;
    op.side = buy ? lob::Side::kBuy : lob::Side::kSell;
    op.price = buy ? kMid - 1 - offset : kMid + 1 + offset;
    op.qty = RandQty(rng);
    driver.add(op);
  }
  const std::vector<Op> pairs = MakeSteadyPairScript(kSteadyPairs, rng);
  for (auto _ : state) {
    Replay(driver, pairs);
    assert(driver.open_orders() == depth && "matched flow must leave the background intact");
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(pairs.size()));
  state.counters["fills/s"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * static_cast<double>(kSteadyPairs),
      benchmark::Counter::kIsRate);
}

// MatchSweepDrain: BM_MatchSweep without the floor orders — every round
// drains its side *truly* empty, hitting the ladder's empty-side transition
// on every drain. This is the measuring instrument for the Day-6 task-5
// O(1) empty-side cursor optimization (docs/optlog.md): before it, each
// drain paid an O(band_radius) cursor rescan (~0.9 µs at the default
// radius); after it, a counter check. Kept registered so the pathology can
// never silently return.
template <typename Driver>
void BM_MatchSweepDrain(benchmark::State& state) {
  const auto k = static_cast<std::uint32_t>(state.range(0));
  ScriptRng rng(kSeed);
  const std::vector<Op> script = MakeSweepScript(kSweepRounds, k, rng);
  Driver driver(std::size_t{k} + 8);
  for (auto _ : state) {
    Replay(driver, script);
    assert(driver.open_orders() == 0 && "every sweep round must drain the book empty");
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(script.size()));
  state.counters["fills/s"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * static_cast<double>(kSweepRounds * k),
      benchmark::Counter::kIsRate);
}

template <typename Driver>
void BM_MatchSweep(benchmark::State& state) {
  const auto k = static_cast<std::uint32_t>(state.range(0));
  ScriptRng rng(kSeed);
  const std::vector<Op> script = MakeSweepScript(kSweepRounds, k, rng);
  Driver driver(std::size_t{k} + 8);  // 2 floors + K makers + the transient taker
  // Two permanent floor orders bracket the ladder one tick beyond taker
  // reach, so a swept side never becomes *truly* empty and the post-drain
  // cursor step is O(1). Without them every round pays the banded ladder's
  // honest-but-pathological empty-side cost — the best cursor rescans
  // O(band_radius) levels to discover emptiness (measured ~0.9 µs/round at
  // the default radius, ~18x SLOWER than the map baseline at K=1). A side
  // oscillating empty<->non-empty at op frequency is outside the modeled
  // regime (DESIGN §4.3 assumes a populated book); the trade-off is recorded
  // here and the floors keep this bench on its subject: per-level sweep cost.
  driver.add({kMid + static_cast<lob::PriceTicks>(k), 1, lob::Side::kSell, lob::OrderType::kLimit});
  driver.add({kMid - static_cast<lob::PriceTicks>(k), 1, lob::Side::kBuy, lob::OrderType::kLimit});
  for (auto _ : state) {
    Replay(driver, script);
    assert(driver.open_orders() == 2 && "every sweep round must drain back to the two floors");
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(script.size()));
  state.counters["fills/s"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * static_cast<double>(kSweepRounds * k),
      benchmark::Counter::kIsRate);
}

}  // namespace

BENCHMARK_TEMPLATE(BM_MatchSteady, NaiveDriver)
    ->Name("MatchSteady/naive")
    ->RangeMultiplier(8)
    ->Range(1024, 65536)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MatchSteady, BookDriver)
    ->Name("MatchSteady/book")
    ->RangeMultiplier(8)
    ->Range(1024, 65536)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MatchSweep, NaiveDriver)
    ->Name("MatchSweep/naive")
    ->RangeMultiplier(4)
    ->Range(1, 64)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MatchSweep, BookDriver)
    ->Name("MatchSweep/book")
    ->RangeMultiplier(4)
    ->Range(1, 64)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MatchSweepDrain, NaiveDriver)
    ->Name("MatchSweepDrain/naive")
    ->RangeMultiplier(4)
    ->Range(1, 64)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MatchSweepDrain, BookDriver)
    ->Name("MatchSweepDrain/book")
    ->RangeMultiplier(4)
    ->Range(1, 64)
    ->Unit(benchmark::kMillisecond);
