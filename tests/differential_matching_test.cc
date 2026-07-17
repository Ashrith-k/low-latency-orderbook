#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "event_test_util.h"
#include "lob/naive_book.h"
#include "lob/order_book.h"
#include "lob/types.h"

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
// Deliberately small band: the price walk regularly brushes the edges and the
// 1% excursions land well outside, so matching sweeps through overflow levels.
constexpr std::uint32_t kRadius = 512;

// Id handed to NaiveBook when OrderBook rejected the add and minted nothing.
// Never inserted (naive rejects the same command), so it never collides.
constexpr OrderId kDummyId = make_order_id(0x7FFFFFFF, 0x7FFFFFFF);

// Day 3 task 5 (DESIGN.md §10.2, the centerpiece): drives OrderBook and
// NaiveBook with one seeded random stream of full matching commands — limit,
// IOC, market, cancels — and after every op compares return values, byte-
// exact event streams, contains(), and observable book state. The matching
// book is uncrossed by construction, so periodic sweeps also run the full
// check_invariants() walk (task 4). Unlike the Day-2 add/cancel harness,
// which keeps driving the rest-only add_limit primitive, this one exercises
// the real engine path.
class MatchingDifferentialHarness {
 public:
  MatchingDifferentialHarness(std::uint64_t seed, int steps)
      : seed_(seed),
        steps_(steps),
        sweep_interval_(std::max(steps / 16, 4096)),
        rng_(seed),
        fast_(kAnchor, kRadius, /*pool_capacity=*/static_cast<std::uint32_t>(steps)) {}

  void Run() {
    for (step_ = 0; step_ < steps_; ++step_) {
      const int roll = Uniform(0, 99);
      if (candidates_.empty() || roll < 60) {
        DoAdd();
      } else {
        DoCancel(roll);
      }
      if (testing::Test::HasFatalFailure()) {
        return;  // first divergence stops the run; Ctx() names seed and step
      }
      if (step_ % sweep_interval_ == 0) {
        SweepTouchedPrices();
        fast_.check_invariants();
      }
    }
    SweepTouchedPrices();
    fast_.check_invariants();
  }

 private:
  void DoAdd() {
    const Side side = Uniform(0, 1) == 0 ? Side::kBuy : Side::kSell;
    const int type_roll = Uniform(0, 99);
    const OrderType type = type_roll < 70   ? OrderType::kLimit
                           : type_roll < 85 ? OrderType::kIoc
                                            : OrderType::kMarket;
    PriceTicks price = WalkPrice();
    Qty qty = static_cast<Qty>(Uniform(1, 100));
    if (Uniform(0, 99) < 5) {  // invalid adds; for market a bad price is legal
      if (Uniform(0, 1) == 0) {
        qty = 0;
      } else {
        price = -Uniform(0, 5);
      }
    } else if (type == OrderType::kMarket && Uniform(0, 4) == 0) {
      price = -Uniform(0, 5);  // market ignores price: still accepted
    }
    last_op_ = std::string("add ") + to_cstr(side) + " " + to_cstr(type) +
               " px=" + std::to_string(price) + " qty=" + std::to_string(qty);

    fast_events_.clear();
    naive_events_.clear();
    const OrderId id =
        fast_.add(side, type, price, qty, [this](const Event& e) { fast_events_.push_back(e); });
    const bool naive_ok =
        naive_.add(id != kInvalidOrderId ? id : kDummyId, side, type, price, qty, naive_events_);
    ASSERT_EQ(naive_ok, id != kInvalidOrderId) << Ctx();
    if (id == kInvalidOrderId) {
      // The book mints no id on a reject and emits kInvalidOrderId (types.h);
      // naive echoes its caller-supplied id. Normalize the naive side only.
      for (Event& e : naive_events_) {
        if (e.order_id == kDummyId) {
          e.order_id = kInvalidOrderId;
        }
      }
    }
    CompareEvents();
    if (id != kInvalidOrderId) {
      ASSERT_EQ(fast_.contains(id), naive_.contains(id)) << Ctx();
      candidates_.push_back(id);
      if (price > 0) {
        touched_.insert(price);
      }
    }
    CompareAroundTouch(price > 0 ? price : kAnchor);
  }

  void DoCancel(int roll) {
    OrderId id;
    std::size_t slot = 0;
    bool from_candidates = false;
    if (roll < 85 || dead_.empty()) {
      // Matching may have filled the pick long ago — that is the point:
      // cancel-after-fill must reject identically in both books.
      slot = static_cast<std::size_t>(Uniform(0, static_cast<int>(candidates_.size()) - 1));
      id = candidates_[slot];
      from_candidates = true;
    } else if (roll < 95) {
      id = dead_[static_cast<std::size_t>(Uniform(0, static_cast<int>(dead_.size()) - 1))];
    } else {
      id = make_order_id(static_cast<std::uint32_t>(rng_()), static_cast<std::uint32_t>(rng_()));
    }
    last_op_ = "cancel id=" + std::to_string(id);

    fast_events_.clear();
    naive_events_.clear();
    const bool fast_ok = fast_.cancel(id, [this](const Event& e) { fast_events_.push_back(e); });
    const bool naive_ok = naive_.cancel(id, naive_events_);
    ASSERT_EQ(fast_ok, naive_ok) << Ctx();
    CompareEvents();
    ASSERT_FALSE(fast_.contains(id)) << Ctx();
    ASSERT_FALSE(naive_.contains(id)) << Ctx();
    if (from_candidates) {
      // Canceled or already dead either way; O(1) swap-remove keeps runs linear.
      candidates_[slot] = candidates_.back();
      candidates_.pop_back();
      dead_.push_back(id);
    }
    const PriceTicks touch =
        (fast_ok && !fast_events_.empty()) ? fast_events_.front().price_ticks : kAnchor;
    CompareAroundTouch(touch);
  }

  void CompareEvents() {
    ASSERT_EQ(fast_events_.size(), naive_events_.size()) << Ctx() << DumpStreams();
    for (std::size_t i = 0; i < fast_events_.size(); ++i) {
      ASSERT_TRUE(testutil::EventEq(naive_events_[i], fast_events_[i])) << Ctx() << " event " << i;
    }
  }

  // State parity at the op's own price plus both post-op bests — the levels
  // matching just acted on.
  void CompareAroundTouch(PriceTicks price) {
    ComparePoint(price);
    if (const auto bid = naive_.best_bid()) ComparePoint(*bid);
    if (const auto ask = naive_.best_ask()) ComparePoint(*ask);
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
  // out-of-band excursions. Buys and sells share the walk, so crossing —
  // hence matching — happens constantly.
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

  std::string DumpStreams() const {
    std::string out = "\n fast:";
    for (const Event& e : fast_events_) out += "\n  " + testutil::Describe(e);
    out += "\n naive:";
    for (const Event& e : naive_events_) out += "\n  " + testutil::Describe(e);
    return out;
  }

  std::uint64_t seed_;
  int steps_;
  int sweep_interval_;
  int step_ = 0;
  std::mt19937_64 rng_;
  OrderBook fast_;
  NaiveBook naive_;
  std::vector<Event> fast_events_;
  std::vector<Event> naive_events_;
  // Minted ids that were once live; matching silently kills makers, so
  // entries may be long dead — exactly the cancel targets we want.
  std::vector<OrderId> candidates_;
  std::vector<OrderId> dead_;
  std::set<PriceTicks> touched_;
  PriceTicks mid_ = kAnchor;
  std::string last_op_;
};

void RunSeed(std::uint64_t seed, int steps) {
  SCOPED_TRACE(testing::Message() << "seed=" << seed);
  MatchingDifferentialHarness harness(seed, steps);
  harness.Run();
}

TEST(DifferentialMatching, FourSeedsTwentyFiveThousandOpsEach) {
  for (const std::uint64_t seed : {0x1AB0Full, 0x2B0Bull, 0x3CADull, 0x4DEEDull}) {
    RunSeed(seed, 25'000);
    if (testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

// The scale run: 1M ops of full matching churn against the oracle, including
// sustained sweeps, cancel-after-fill, and overflow traffic. ASan/UBSan clean.
TEST(DifferentialMatching, MillionOpConvergence) { RunSeed(0x5EED2ull, 1'000'000); }

// Deterministic reproduction of a failing seed: LOB_DIFF_MATCH_SEED=0x... ctest
TEST(DifferentialMatching, EnvSeedReplay) {
  const char* env = std::getenv("LOB_DIFF_MATCH_SEED");
  if (env == nullptr) {
    GTEST_SKIP() << "set LOB_DIFF_MATCH_SEED to replay a seed";
  }
  RunSeed(std::strtoull(env, nullptr, 0), 25'000);
}

}  // namespace
}  // namespace lob
