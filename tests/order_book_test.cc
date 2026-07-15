#include "lob/order_book.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <unordered_set>

#include "lob/types.h"

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
constexpr std::uint32_t kRadius = 16;

OrderBook MakeBook(std::uint32_t pool_capacity = 32) {
  return OrderBook(kAnchor, kRadius, pool_capacity);
}

TEST(OrderBookAdd, EmptyBookHasNoBestAndNoOrders) {
  const OrderBook book = MakeBook();
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.open_orders(), 0u);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor), 0u);
  EXPECT_EQ(book.orders_at(Side::kSell, kAnchor), 0u);
}

TEST(OrderBookAdd, RestsBid) {
  OrderBook book = MakeBook();
  const OrderId id = book.add_limit(Side::kBuy, kAnchor - 1, 10);

  ASSERT_NE(id, kInvalidOrderId);
  EXPECT_TRUE(book.contains(id));
  EXPECT_EQ(book.best_bid(), kAnchor - 1);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 1), 10u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor - 1), 1u);
  EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBookAdd, RestsAsk) {
  OrderBook book = MakeBook();
  const OrderId id = book.add_limit(Side::kSell, kAnchor + 1, 7);

  ASSERT_NE(id, kInvalidOrderId);
  EXPECT_EQ(book.best_ask(), kAnchor + 1);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.qty_at(Side::kSell, kAnchor + 1), 7u);
}

TEST(OrderBookAdd, RejectsZeroQty) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add_limit(Side::kBuy, kAnchor, 0), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBookAdd, RejectsNonPositivePrice) {
  OrderBook book = MakeBook();
  EXPECT_EQ(book.add_limit(Side::kSell, 0, 5), kInvalidOrderId);
  EXPECT_EQ(book.add_limit(Side::kSell, -3, 5), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBookAdd, PoolExhaustionRejectsAndLeavesBookIntact) {
  OrderBook book = MakeBook(/*pool_capacity=*/2);
  const OrderId a = book.add_limit(Side::kBuy, kAnchor - 1, 1);
  const OrderId b = book.add_limit(Side::kSell, kAnchor + 1, 2);
  ASSERT_NE(a, kInvalidOrderId);
  ASSERT_NE(b, kInvalidOrderId);

  EXPECT_EQ(book.add_limit(Side::kBuy, kAnchor - 2, 3), kInvalidOrderId);
  EXPECT_EQ(book.open_orders(), 2u);
  EXPECT_EQ(book.best_bid(), kAnchor - 1);
  EXPECT_EQ(book.best_ask(), kAnchor + 1);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor - 2), 0u);
}

TEST(OrderBookAdd, BestTracksBetterPrices) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor - 5, 1);
  book.add_limit(Side::kBuy, kAnchor - 2, 1);  // better bid
  book.add_limit(Side::kBuy, kAnchor - 8, 1);  // worse bid
  EXPECT_EQ(book.best_bid(), kAnchor - 2);

  book.add_limit(Side::kSell, kAnchor + 5, 1);
  book.add_limit(Side::kSell, kAnchor + 2, 1);  // better ask
  book.add_limit(Side::kSell, kAnchor + 8, 1);  // worse ask
  EXPECT_EQ(book.best_ask(), kAnchor + 2);
}

TEST(OrderBookAdd, SamePriceAggregates) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor, 10);
  book.add_limit(Side::kBuy, kAnchor, 20);
  book.add_limit(Side::kBuy, kAnchor, 30);
  EXPECT_EQ(book.qty_at(Side::kBuy, kAnchor), 60u);
  EXPECT_EQ(book.orders_at(Side::kBuy, kAnchor), 3u);
  EXPECT_EQ(book.open_orders(), 3u);
}

// Pre-matching semantics: crossing prices rest, exactly like
// NaiveBook::add_limit — the add/cancel differential harness depends on it.
TEST(OrderBookAdd, CrossedPricesRestWithoutMatching) {
  OrderBook book = MakeBook();
  book.add_limit(Side::kBuy, kAnchor + 5, 10);
  book.add_limit(Side::kSell, kAnchor - 5, 10);

  EXPECT_EQ(book.best_bid(), kAnchor + 5);
  EXPECT_EQ(book.best_ask(), kAnchor - 5);
  EXPECT_EQ(book.open_orders(), 2u);
}

TEST(OrderBookAdd, OutOfBandPricesRestViaOverflow) {
  OrderBook book = MakeBook();
  const PriceTicks deep_bid = kAnchor - kRadius - 200;
  const PriceTicks high_bid = kAnchor + kRadius + 200;
  book.add_limit(Side::kBuy, deep_bid, 4);
  const OrderId top = book.add_limit(Side::kBuy, high_bid, 6);

  ASSERT_NE(top, kInvalidOrderId);
  EXPECT_EQ(book.qty_at(Side::kBuy, deep_bid), 4u);
  EXPECT_EQ(book.qty_at(Side::kBuy, high_bid), 6u);
  EXPECT_EQ(book.best_bid(), high_bid);  // better-side overflow wins
}

TEST(OrderBookAdd, MintedIdsAreUnique) {
  OrderBook book = MakeBook();
  std::unordered_set<OrderId> ids;
  for (int i = 0; i < 20; ++i) {
    const OrderId id = book.add_limit(Side::kBuy, kAnchor - (i % 4), 1);
    ASSERT_NE(id, kInvalidOrderId);
    EXPECT_TRUE(ids.insert(id).second);
  }
}

}  // namespace
}  // namespace lob
