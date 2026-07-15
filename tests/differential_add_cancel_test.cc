#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/naive_book.h"
#include "lob/order_book.h"
#include "lob/types.h"

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
// Deliberately small band: the price walk regularly brushes the edges and the
// 1% excursions land well outside, so the overflow map sees organic traffic.
constexpr std::uint32_t kRadius = 512;

// Id handed to NaiveBook when OrderBook rejected the add and minted nothing.
// Never inserted (naive rejects the same command), so it never collides.
constexpr OrderId kDummyId = make_order_id(0x7FFFFFFF, 0x7FFFFFFF);

// Drives OrderBook and NaiveBook with one seeded random add/cancel stream and
// compares observable state after every op (DESIGN.md §10.2). Matching does
// not exist yet, so crossing prices rest in both books by construction.
class DifferentialHarness {
 public:
  DifferentialHarness(std::uint64_t seed, int steps)
      : seed_(seed),
        steps_(steps),
        sweep_interval_(std::max(steps / 16, 4096)),
        rng_(seed),
        fast_(kAnchor, kRadius, /*pool_capacity=*/static_cast<std::uint32_t>(steps)) {}

  void Run() {
    for (step_ = 0; step_ < steps_; ++step_) {
      const int roll = Uniform(0, 99);
      if (live_.empty() || roll < 55) {
        DoAdd(/*invalid=*/roll < 5);
      } else {
        DoCancel(roll);
      }
      if (testing::Test::HasFatalFailure()) {
        return;  // first divergence stops the run; Ctx() names seed and step
      }
      // Sweep cadence scales with run length so 1M-op runs stay bounded even
      // as the touched-price set grows into the thousands (task 9).
      if (step_ % sweep_interval_ == 0) {
        SweepTouchedPrices();
      }
    }
    SweepTouchedPrices();
  }

 private:
  void DoAdd(bool invalid) {
    const Side side = Uniform(0, 1) == 0 ? Side::kBuy : Side::kSell;
    PriceTicks price = WalkPrice();
    Qty qty = static_cast<Qty>(Uniform(1, 100));
    if (invalid) {
      if (Uniform(0, 1) == 0) {
        qty = 0;
      } else {
        price = -Uniform(0, 5);
      }
    }
    last_op_ = std::string("add ") + to_cstr(side) + " px=" + std::to_string(price) +
               " qty=" + std::to_string(qty);

    const OrderId id = fast_.add_limit(side, price, qty);
    events_.clear();
    const bool naive_ok =
        naive_.add_limit(id != kInvalidOrderId ? id : kDummyId, side, price, qty, events_);

    ASSERT_EQ(naive_ok, id != kInvalidOrderId) << Ctx();
    if (id != kInvalidOrderId) {
      ASSERT_TRUE(fast_.contains(id)) << Ctx();
      live_.push_back(id);
      price_of_[id] = price;
      touched_.insert(price);
    }
    ComparePoint(price);
  }

  void DoCancel(int roll) {
    OrderId id;
    std::size_t live_slot = 0;
    bool expect_live;
    if (roll < 85 || (dead_.empty() && roll < 95)) {
      live_slot = static_cast<std::size_t>(Uniform(0, static_cast<int>(live_.size()) - 1));
      id = live_[live_slot];
      expect_live = true;
    } else if (roll < 95) {
      id = dead_[static_cast<std::size_t>(Uniform(0, static_cast<int>(dead_.size()) - 1))];
      expect_live = false;
    } else {
      id = make_order_id(static_cast<std::uint32_t>(rng_()), static_cast<std::uint32_t>(rng_()));
      expect_live = false;
    }
    last_op_ = "cancel id=" + std::to_string(id);
    // Aim the per-op comparison at the level the cancel touches (stale and
    // forged ids touch nothing; any price will do).
    const auto price_it = price_of_.find(id);
    const PriceTicks price = price_it != price_of_.end() ? price_it->second : kAnchor;

    const bool fast_ok = fast_.cancel(id);
    events_.clear();
    const bool naive_ok = naive_.cancel(id, events_);

    ASSERT_EQ(fast_ok, naive_ok) << Ctx();
    ASSERT_EQ(fast_ok, expect_live) << Ctx();
    if (fast_ok) {
      // O(1) swap-remove: std::erase's linear scan made 1M-op runs quadratic.
      live_[live_slot] = live_.back();
      live_.pop_back();
      dead_.push_back(id);
      price_of_.erase(id);
      ASSERT_FALSE(fast_.contains(id)) << Ctx();
    }
    ComparePoint(price);
  }

  void ComparePoint(PriceTicks price) {
    ASSERT_EQ(fast_.best_bid(), naive_.best_bid()) << Ctx();
    ASSERT_EQ(fast_.best_ask(), naive_.best_ask()) << Ctx();
    ASSERT_EQ(static_cast<std::size_t>(fast_.open_orders()), naive_.open_orders()) << Ctx();
    for (const Side side : {Side::kBuy, Side::kSell}) {
      ASSERT_EQ(fast_.qty_at(side, price), static_cast<std::uint64_t>(naive_.qty_at(side, price)))
          << Ctx() << " qty_at " << to_cstr(side) << " px=" << price;
      ASSERT_EQ(static_cast<std::size_t>(fast_.orders_at(side, price)),
                naive_.orders_at(side, price))
          << Ctx() << " orders_at " << to_cstr(side) << " px=" << price;
    }
  }

  void SweepTouchedPrices() {
    for (const PriceTicks price : touched_) {
      ComparePoint(price);
      if (testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }

  // ±3 random walk around the anchor, clamped positive, with 1% deep
  // out-of-band excursions to exercise the overflow region.
  PriceTicks WalkPrice() {
    mid_ += Uniform(-3, 3);
    if (mid_ < 100) {
      mid_ = 100;
    }
    if (Uniform(0, 99) == 0) {
      const PriceTicks jump = kRadius + Uniform(1, 400);
      const PriceTicks price = Uniform(0, 1) == 0 ? kAnchor + jump : kAnchor - jump;
      return price > 0 ? price : 1;
    }
    return mid_ + Uniform(-5, 5);
  }

  int Uniform(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }

  std::string Ctx() const {
    return "seed=" + std::to_string(seed_) + " step=" + std::to_string(step_) + " op[" + last_op_ +
           "]";
  }

  std::uint64_t seed_;
  int steps_;
  int sweep_interval_;
  int step_ = 0;
  std::mt19937_64 rng_;
  OrderBook fast_;
  NaiveBook naive_;
  std::vector<Event> events_;
  std::vector<OrderId> live_;
  std::vector<OrderId> dead_;
  std::unordered_map<OrderId, PriceTicks> price_of_;
  std::set<PriceTicks> touched_;
  PriceTicks mid_ = kAnchor;
  std::string last_op_;
};

void RunSeed(std::uint64_t seed, int steps) {
  SCOPED_TRACE(testing::Message() << "seed=" << seed);
  DifferentialHarness harness(seed, steps);
  harness.Run();
}

TEST(DifferentialAddCancel, FourSeedsTwentyFiveThousandOpsEach) {
  for (const std::uint64_t seed : {0xA11CEull, 0xB0Bull, 0xC0FFEEull, 0xD00Dull}) {
    RunSeed(seed, 25'000);
    if (testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

// Task 9: the scale run — 1M ops of add/cancel churn against the oracle.
// Exercises deep slot reuse (hundreds of generations per slot), a wide price
// walk, and sustained overflow traffic. Must also be ASan/UBSan clean.
TEST(DifferentialAddCancel, MillionOpConvergence) { RunSeed(0x5EEDull, 1'000'000); }

// Deterministic reproduction of a failing seed: LOB_DIFF_SEED=0x... ctest ...
TEST(DifferentialAddCancel, EnvSeedReplay) {
  const char* env = std::getenv("LOB_DIFF_SEED");
  if (env == nullptr) {
    GTEST_SKIP() << "set LOB_DIFF_SEED to replay a seed";
  }
  RunSeed(std::strtoull(env, nullptr, 0), 25'000);
}

}  // namespace
}  // namespace lob
