#ifndef LOB_ORDER_BOOK_H_
#define LOB_ORDER_BOOK_H_

#include <cstdint>
#include <optional>

#include "lob/order_pool.h"
#include "lob/price_ladder.h"
#include "lob/types.h"

namespace lob {

// ---------------------------------------------------------------------------
// OrderBook: the optimized book — an OrderPool plus one PriceLadder per side
// (DESIGN.md §4). This task rests limit orders only; matching lands Day 3.
// The query surface deliberately mirrors NaiveBook so the differential
// harness can compare the two books call-for-call.
// ---------------------------------------------------------------------------

class OrderBook {
 public:
  // Startup-only; all allocation happens here.
  explicit OrderBook(PriceTicks anchor_price, std::uint32_t band_radius = kDefaultBandRadius,
                     std::uint32_t pool_capacity = kDefaultPoolCapacity)
      : pool_(pool_capacity),
        bids_(Side::kBuy, anchor_price, band_radius),
        asks_(Side::kSell, anchor_price, band_radius) {}

  // Rests a limit order without matching (crossing prices rest too, exactly
  // like NaiveBook::add_limit — the Day-2 differential harness depends on
  // that). Returns the engine-assigned id, or kInvalidOrderId on validation
  // failure (qty == 0, price <= 0; same checks, same order as NaiveBook) or
  // pool exhaustion. Events land with Day 3 task 3.
  OrderId add_limit(Side side, PriceTicks price, Qty qty) noexcept {
    if (qty == 0 || price <= 0) {
      return kInvalidOrderId;
    }
    const OrderId id = pool_.alloc();
    if (id == kInvalidOrderId) {
      return kInvalidOrderId;
    }
    Order& order = pool_[index_of(id)];
    order.price_ticks = price;
    order.qty = qty;
    order.remaining = qty;
    order.side = side;
    order.type = OrderType::kLimit;
    order.flags = 0;
    ladder(side).push_order(pool_, index_of(id));
    return id;
  }

  std::optional<PriceTicks> best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.best_price();
  }

  std::optional<PriceTicks> best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.best_price();
  }

  // Total open quantity at a price level; 0 if the level does not exist.
  std::uint64_t qty_at(Side side, PriceTicks price) const noexcept {
    const PriceLevel* level = ladder(side).find_level(price);
    return level == nullptr ? 0 : level->total_qty;
  }

  // Number of open orders at a price level; 0 if the level does not exist.
  std::uint32_t orders_at(Side side, PriceTicks price) const noexcept {
    const PriceLevel* level = ladder(side).find_level(price);
    return level == nullptr ? 0 : level->order_count;
  }

  std::uint32_t open_orders() const noexcept { return pool_.live_count(); }

  bool contains(OrderId id) const noexcept { return pool_.find(id) != nullptr; }

 private:
  PriceLadder& ladder(Side side) noexcept { return side == Side::kBuy ? bids_ : asks_; }
  const PriceLadder& ladder(Side side) const noexcept { return side == Side::kBuy ? bids_ : asks_; }

  OrderPool pool_;
  PriceLadder bids_;
  PriceLadder asks_;
};

}  // namespace lob

#endif  // LOB_ORDER_BOOK_H_
