#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "lob/order_book.h"
#include "lob/types.h"

// Day 3 task 4: OrderBook::check_invariants() — the full structural walk
// (FIFO links, level aggregates, best cursors, pool census, uncrossed book).
// A passing test here means no assert fired during the walk; the death test
// proves the walk actually detects a broken invariant.

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
constexpr std::uint32_t kRadius = 16;

OrderBook MakeBook(std::uint32_t pool_capacity = 4096) {
  return OrderBook(kAnchor, kRadius, pool_capacity);
}

TEST(Invariants, EmptyBookPasses) {
  const OrderBook book = MakeBook();
  book.check_invariants();
}

// Every structural path in one scripted lifecycle, checked after each op:
// band and overflow resting, FIFO buildup, best-first sweeps with partial
// fills, side-flipping remainders, market/IOC, and cancels at head, middle,
// tail, and overflow.
TEST(Invariants, HoldAfterEveryScriptedOp) {
  OrderBook book = MakeBook();
  std::vector<OrderId> live;
  auto add = [&](Side s, OrderType t, PriceTicks px, Qty q) {
    const OrderId id = book.add(s, t, px, q);
    book.check_invariants();
    if (book.contains(id)) live.push_back(id);
    return id;
  };
  auto cancel = [&](OrderId id) {
    book.cancel(id);
    book.check_invariants();
  };

  add(Side::kBuy, OrderType::kLimit, kAnchor - 2, 10);
  add(Side::kBuy, OrderType::kLimit, kAnchor - 2, 7);  // FIFO sibling
  add(Side::kBuy, OrderType::kLimit, kAnchor - 5, 3);
  add(Side::kBuy, OrderType::kLimit, kAnchor - kRadius, 4);        // band edge
  add(Side::kBuy, OrderType::kLimit, kAnchor - kRadius - 500, 6);  // overflow
  add(Side::kSell, OrderType::kLimit, kAnchor + 1, 5);
  add(Side::kSell, OrderType::kLimit, kAnchor + 1, 9);
  add(Side::kSell, OrderType::kLimit, kAnchor + kRadius + 500, 2);  // overflow

  add(Side::kBuy, OrderType::kLimit, kAnchor + 1, 7);    // sweep: full + partial fill
  add(Side::kSell, OrderType::kLimit, kAnchor - 2, 20);  // sweep bids, remainder flips side
  add(Side::kBuy, OrderType::kMarket, 0, 4);
  add(Side::kBuy, OrderType::kIoc, kAnchor - 2, 50);  // drains level, remainder canceled

  // Cancel middle, head, tail, overflow, then everything left.
  ASSERT_GE(live.size(), 4u);
  cancel(live[live.size() / 2]);
  cancel(live.front());
  cancel(live.back());
  for (const OrderId id : live) cancel(id);  // stale ids exercise the reject path
  EXPECT_EQ(book.open_orders(), 0u);
  book.check_invariants();
}

TEST(Invariants, HoldThroughRandomizedChurn) {
  constexpr std::uint32_t kOps = 10'000;
  constexpr std::uint32_t kCheckEvery = 64;
  OrderBook book = MakeBook(/*pool_capacity=*/kOps);
  std::mt19937 rng(0xC0FFEEu);
  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<PriceTicks> px_dist(kAnchor - kRadius - 4, kAnchor + kRadius + 4);
  std::uniform_int_distribution<Qty> qty_dist(1, 40);
  std::vector<OrderId> live;

  for (std::uint32_t i = 0; i < kOps; ++i) {
    const int roll = op_dist(rng);
    if (roll < 60 || live.empty()) {
      const Side side = (roll % 2 == 0) ? Side::kBuy : Side::kSell;
      const OrderType type = roll < 40   ? OrderType::kLimit
                             : roll < 50 ? OrderType::kIoc
                                         : OrderType::kMarket;
      const OrderId id = book.add(side, type, px_dist(rng), qty_dist(rng));
      if (id != kInvalidOrderId && book.contains(id)) live.push_back(id);
    } else {
      // Cancel a random tracked id; matching may have filled it already, so
      // this also exercises the unknown-order reject path.
      std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
      const std::size_t at = pick(rng);
      book.cancel(live[at]);
      live[at] = live.back();
      live.pop_back();
    }
    if (i % kCheckEvery == 0) book.check_invariants();
  }
  book.check_invariants();
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
// The checker must actually detect violations: add_limit (the Day-2
// rest-without-matching primitive) can build a crossed book, which
// check_invariants treats as broken.
TEST(InvariantsDeathTest, DetectsCrossedBook) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor + 5, 10);
  book.add_limit(Side::kSell, kAnchor - 5, 10);
  EXPECT_DEATH(book.check_invariants(), "crossed");
}
#endif

}  // namespace
}  // namespace lob
