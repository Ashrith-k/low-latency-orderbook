#ifndef LOB_WORKLOAD_GEN_H_
#define LOB_WORKLOAD_GEN_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/types.h"

namespace lob {

// ---------------------------------------------------------------------------
// WorkloadGenerator (DESIGN §8's workload generator; Day 5 task 3): produces
// deterministic Command streams for benchmarks and replay files. Tool tier —
// like NaiveBook it allocates freely and is never on the hot path; the
// reusable core lives here in include/lob/ so tests and benches can drive it
// (the tools/ CLI wrapper arrives with the replay tasks).
//
// Cancels are the interesting part: a pregenerated stream must carry concrete
// order ids, but ids are minted by the engine at process time. The generator
// therefore runs a private *shadow* MatchingEngine: every emitted command is
// applied to it, and its event stream maintains the set of live ids that
// future cancels draw from. Because the engine is deterministic, replaying
// the identical stream through a fresh engine built with the SAME
// EngineConfig mints identical ids — so every generated cancel is valid at
// replay time (pinned by tests).
//
// Randomness is in-house (splitmix64 + integer-weight sampling) instead of
// <random> distributions, whose output is stdlib-specific: a replay file must
// be byte-identical no matter which toolchain generated it.
// ---------------------------------------------------------------------------

struct WorkloadCounts {
  std::uint64_t limits = 0;
  std::uint64_t markets = 0;
  std::uint64_t iocs = 0;
  std::uint64_t cancels = 0;
  // Cancel drawn with no live order to target: a limit is emitted instead
  // (already included in `limits`).
  std::uint64_t degraded_cancels = 0;
};

struct WorkloadConfig {
  // The replaying engine MUST be constructed with exactly this config —
  // anchor/band/pool all shape id minting and matching. The replay file
  // header (Day 5 task 4) carries it for that reason.
  EngineConfig engine;

  std::uint64_t seed = 1;

  // Op mix in relative weights (need not sum to anything in particular).
  std::uint32_t limit_weight = 60;
  std::uint32_t market_weight = 5;
  std::uint32_t ioc_weight = 15;
  std::uint32_t cancel_weight = 20;

  // New-order qty: uniform in [min_qty, max_qty]; both >= 1.
  Qty min_qty = 1;
  Qty max_qty = 100;

  // Prices: order price = mid ± offset, offset in [0, max_price_offset]
  // drawn Zipf-ish (weight ∝ 1/(offset+1)), so quotes cluster at the touch
  // with a heavy-ish tail. 1-in-cross_one_in adds take the offset *through*
  // the mid (aggressive) so the stream actually produces fills.
  std::uint32_t max_price_offset = 256;
  std::uint32_t cross_one_in = 4;

  // Mid random walk: 1-in-walk_one_in ops step the mid by ±[1, max_walk_step]
  // ticks, clamped to [anchor − walk_radius, anchor + walk_radius] (and > 0).
  // The default radius equals the ladder band, so out-of-band prices are the
  // same rare excursion the differential harness exercises.
  std::uint32_t walk_one_in = 4;
  std::uint32_t max_walk_step = 2;
  std::uint32_t walk_radius = kDefaultBandRadius;
};

// Task 4 serializes the config into the replay file header verbatim.
static_assert(std::is_trivially_copyable_v<WorkloadConfig>);

class WorkloadGenerator {
 public:
  explicit WorkloadGenerator(const WorkloadConfig& cfg)
      : cfg_(cfg), rng_state_(cfg.seed), mid_(cfg.engine.anchor_price), engine_(cfg.engine) {
    assert(cfg.engine.anchor_price > 0);
    assert(cfg.min_qty >= 1 && cfg.max_qty >= cfg.min_qty);
    assert(cfg.cross_one_in >= 1 && cfg.walk_one_in >= 1 && cfg.max_walk_step >= 1);
    total_weight_ =
        std::uint64_t{cfg.limit_weight} + cfg.market_weight + cfg.ioc_weight + cfg.cancel_weight;
    assert(total_weight_ > 0 && "op mix must have at least one nonzero weight");

    // Integer harmonic ("Zipf-ish", s = 1) CDF over offsets 0..max_price_offset:
    // weight(k) = kZipfScale / (k + 1), floored at 1 so the tail never vanishes.
    // Integer weights keep sampling bit-exact across compilers.
    zipf_cdf_.reserve(std::size_t{cfg.max_price_offset} + 1);
    std::uint64_t acc = 0;
    for (std::uint64_t k = 0; k <= cfg.max_price_offset; ++k) {
      acc += kZipfScale / (k + 1) + 1;
      zipf_cdf_.push_back(acc);
    }

    walk_lo_ = std::max<PriceTicks>(1, cfg.engine.anchor_price - PriceTicks{cfg.walk_radius});
    walk_hi_ = cfg.engine.anchor_price + PriceTicks{cfg.walk_radius};
  }

  // The next command of the stream. Deterministic: config (seed included)
  // fully determines the sequence.
  Command next() {
    MaybeStepMid();
    std::uint64_t r = BoundedRand(total_weight_);
    Command cmd{};
    if (r < cfg_.cancel_weight) {
      if (!live_ids_.empty()) {
        cmd = MakeCancel();
        ++counts_.cancels;
      } else {
        // Nothing to cancel yet (empty book): degrade to a limit so the mix
        // stays sensible instead of stalling — counted, and pinned by tests.
        cmd = MakeAdd(OrderType::kLimit);
        ++counts_.limits;
        ++counts_.degraded_cancels;
      }
    } else {
      r -= cfg_.cancel_weight;
      if (r < cfg_.limit_weight) {
        cmd = MakeAdd(OrderType::kLimit);
        ++counts_.limits;
      } else if (r < std::uint64_t{cfg_.limit_weight} + cfg_.market_weight) {
        cmd = MakeAdd(OrderType::kMarket);
        ++counts_.markets;
      } else {
        cmd = MakeAdd(OrderType::kIoc);
        ++counts_.iocs;
      }
    }
    Apply(cmd);
    return cmd;
  }

  const WorkloadConfig& config() const noexcept { return cfg_; }
  const WorkloadCounts& counts() const noexcept { return counts_; }
  std::size_t live_orders() const noexcept { return live_ids_.size(); }

  // The shadow engine's book: the exact end state a faithful replay of the
  // emitted stream must reproduce (tests assert this parity).
  const OrderBook& shadow_book() const noexcept { return engine_.book(); }

 private:
  static constexpr std::uint64_t kZipfScale = std::uint64_t{1} << 20;

  // splitmix64 (public domain): tiny, full-period, and — unlike <random>
  // distributions — bit-exact on every toolchain.
  std::uint64_t NextU64() noexcept {
    std::uint64_t z = (rng_state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Uniform-ish in [0, n). Modulo bias is ~n/2^64 — unmeasurable at these
  // ranges and not worth a rejection loop in a workload tool.
  std::uint64_t BoundedRand(std::uint64_t n) noexcept {
    assert(n > 0);
    return NextU64() % n;
  }

  bool OneIn(std::uint32_t n) noexcept { return BoundedRand(n) == 0; }

  void MaybeStepMid() noexcept {
    if (!OneIn(cfg_.walk_one_in)) {
      return;
    }
    const auto step = static_cast<PriceTicks>(1 + BoundedRand(cfg_.max_walk_step));
    mid_ += OneIn(2) ? step : -step;
    mid_ = std::clamp(mid_, walk_lo_, walk_hi_);
  }

  // Zipf-ish offset in [0, max_price_offset]: first CDF entry above r.
  std::uint64_t SampleZipfOffset() noexcept {
    const std::uint64_t r = BoundedRand(zipf_cdf_.back());
    return static_cast<std::uint64_t>(std::upper_bound(zipf_cdf_.begin(), zipf_cdf_.end(), r) -
                                      zipf_cdf_.begin());
  }

  Command MakeAdd(OrderType type) noexcept {
    const Side side = BoundedRand(2) == 0 ? Side::kBuy : Side::kSell;
    const Qty qty = static_cast<Qty>(cfg_.min_qty +
                                     BoundedRand(std::uint64_t{cfg_.max_qty} - cfg_.min_qty + 1));
    PriceTicks price = 0;  // market ignores price; keep the bytes deterministic
    if (type != OrderType::kMarket) {
      const auto offset = static_cast<PriceTicks>(SampleZipfOffset());
      const bool aggressive = OneIn(cfg_.cross_one_in);
      // Passive quotes sit away from the touch (buy below mid, sell above);
      // aggressive ones take the same offset through the mid instead.
      const bool above_mid = (side == Side::kBuy) == aggressive;
      price = std::max<PriceTicks>(1, above_mid ? mid_ + offset : mid_ - offset);
    }
    return Command{CommandType::kNew, side, type, 0, qty, price, kInvalidOrderId};
  }

  Command MakeCancel() noexcept {
    const OrderId id = live_ids_[BoundedRand(live_ids_.size())];
    return Command{CommandType::kCancel, Side::kBuy, OrderType::kLimit, 0, 0, 0, id};
  }

  // Every emitted command flows through the shadow engine; its events are
  // the ground truth for which ids are live.
  void Apply(const Command& cmd) {
    engine_.process(cmd, [this](const Event& e) {
      switch (e.kind) {
        case EventType::kAccepted:
          AddLive(e.order_id);
          return;
        case EventType::kTraded:
          if (e.remaining == 0) {
            RemoveLive(e.order_id);
          }
          return;
        case EventType::kCanceled:
          RemoveLive(e.order_id);
          return;
        case EventType::kRejected:
          return;
      }
    });
  }

  void AddLive(OrderId id) {
    assert(live_pos_.find(id) == live_pos_.end() && "shadow tracking out of sync");
    live_pos_.emplace(id, live_ids_.size());
    live_ids_.push_back(id);
  }

  // O(1) swap-remove keyed by id; position map keeps the pick O(1) too.
  void RemoveLive(OrderId id) {
    const auto it = live_pos_.find(id);
    assert(it != live_pos_.end() && "shadow tracking out of sync");
    if (it == live_pos_.end()) {
      return;
    }
    const std::size_t pos = it->second;
    const OrderId last = live_ids_.back();
    live_ids_[pos] = last;
    live_pos_[last] = pos;
    live_ids_.pop_back();
    live_pos_.erase(id);
  }

  WorkloadConfig cfg_;
  std::uint64_t total_weight_ = 0;
  std::uint64_t rng_state_;
  PriceTicks mid_;
  PriceTicks walk_lo_ = 1;
  PriceTicks walk_hi_ = 1;
  std::vector<std::uint64_t> zipf_cdf_;
  MatchingEngine engine_;
  std::vector<OrderId> live_ids_;
  std::unordered_map<OrderId, std::size_t> live_pos_;
  WorkloadCounts counts_;
};

// Convenience for tests and the replay writer: the first n commands of the
// stream cfg defines.
inline std::vector<Command> generate_workload(const WorkloadConfig& cfg, std::size_t n) {
  WorkloadGenerator gen(cfg);
  std::vector<Command> cmds;
  cmds.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    cmds.push_back(gen.next());
  }
  return cmds;
}

}  // namespace lob

#endif  // LOB_WORKLOAD_GEN_H_
