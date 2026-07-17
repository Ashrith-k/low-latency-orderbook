#ifndef LOB_ORDER_BOOK_H_
#define LOB_ORDER_BOOK_H_

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>

#include "lob/order_pool.h"
#include "lob/price_ladder.h"
#include "lob/types.h"

namespace lob {

// ---------------------------------------------------------------------------
// OrderBook: the optimized book — an OrderPool plus one PriceLadder per side
// (DESIGN.md §4). add() is the matching entry point (price-time priority,
// DESIGN.md §5); add_limit() is the rest-only primitive kept for the
// differential harness. The query surface deliberately mirrors NaiveBook so
// the harness can compare the two books call-for-call.
// ---------------------------------------------------------------------------

class OrderBook {
 public:
  // Startup-only; all allocation happens here.
  explicit OrderBook(PriceTicks anchor_price, std::uint32_t band_radius = kDefaultBandRadius,
                     std::uint32_t pool_capacity = kDefaultPoolCapacity)
      : pool_(pool_capacity),
        bids_(Side::kBuy, anchor_price, band_radius),
        asks_(Side::kSell, anchor_price, band_radius) {}

  // Full matching entry point (NaiveBook::add and naive_book_test.cc are the
  // executable spec): sweeps the opposite side best level first, FIFO within
  // a level, partial fills; every fill executes at the resting order's price.
  // A limit remainder rests at the limit price; a market/IOC remainder is
  // dropped (Canceled events land with Day 3 task 3). Market sweeps at any
  // price — the price argument is ignored — and lack of liquidity is
  // acceptance-then-cancel, not a reject.
  // Returns the minted id — an order that fills or cancels out never rests,
  // but its id is still real (and permanently stale) — or kInvalidOrderId on
  // validation failure (qty == 0 for all types; price <= 0 for limit/IOC
  // only — same checks, same order as NaiveBook) or pool exhaustion. The
  // slot is allocated before the sweep because even a never-rests taker
  // needs an id, so a full pool rejects up front regardless of what the
  // sweep would have done.
  OrderId add(Side side, OrderType type, PriceTicks price, Qty qty) noexcept {
    if (qty == 0) {
      return kInvalidOrderId;
    }
    const bool is_market = type == OrderType::kMarket;
    if (!is_market && price <= 0) {
      return kInvalidOrderId;
    }
    const OrderId id = pool_.alloc();
    if (id == kInvalidOrderId) {
      return kInvalidOrderId;
    }
    // A market order is a limit at an unbeatable price: sweeping with that
    // sentinel crosses every level, so Match needs no market branch. Safe —
    // Crosses only compares, never does arithmetic on the limit.
    const PriceTicks limit = !is_market           ? price
                             : side == Side::kBuy ? std::numeric_limits<PriceTicks>::max()
                                                  : std::numeric_limits<PriceTicks>::min();
    const Qty remaining = Match(side, limit, qty);
    const std::uint32_t index = index_of(id);
    if (remaining == 0 || type != OrderType::kLimit) {
      // Fully filled, or a market/IOC remainder: nothing rests.
      pool_.free(index);
      return id;
    }
    Order& order = pool_[index];
    order.price_ticks = price;
    order.qty = qty;
    order.remaining = remaining;
    order.side = side;
    order.type = type;
    order.flags = 0;
    ladder(side).push_order(pool_, index);
    return id;
  }

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

  // Cancels an open order: true iff it was resting and is now removed.
  // Unknown, stale (slot reused), and already-canceled ids return false and
  // leave the book untouched. The whole path is O(1) off the id itself —
  // find is a bounds check plus one compare, the level comes from
  // Order::level_idx, and the slot goes straight back to the free list
  // (DESIGN.md §4.2: no hash map on the hot path).
  bool cancel(OrderId id) noexcept {
    const Order* order = pool_.find(id);
    if (order == nullptr) {
      return false;
    }
    const std::uint32_t index = index_of(id);
    ladder(order->side).remove_order(pool_, index);
    pool_.free(index);
    return true;
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

  // A buy crosses levels priced at or below its limit; a sell at or above.
  static bool Crosses(Side taker_side, PriceTicks limit_price, PriceTicks level_price) noexcept {
    return taker_side == Side::kBuy ? level_price <= limit_price : level_price >= limit_price;
  }

  // Sweeps the side opposite `taker_side` while the taker has quantity left
  // and its limit crosses the best opposing level; fills hit the level head
  // (FIFO = time priority). A fully filled maker is remove_order'ed BEFORE
  // its remaining is touched — unlink debits total_qty by that remaining —
  // then its slot is recycled. A partially filled maker keeps its FIFO
  // place, so its remaining and the level aggregate are debited by hand
  // (unlink is the only level op that maintains total_qty implicitly).
  // Returns the taker's unfilled remainder.
  Qty Match(Side taker_side, PriceTicks limit_price, Qty qty) noexcept {
    PriceLadder& opposite = ladder(taker_side == Side::kBuy ? Side::kSell : Side::kBuy);
    Qty remaining = qty;
    while (remaining > 0 && !opposite.empty() &&
           Crosses(taker_side, limit_price, opposite.best_price())) {
      PriceLevel& level = opposite.best_level();
      assert(level.head_idx != kNullIdx && "best level of a non-empty side cannot be empty");
      const auto maker_idx = static_cast<std::uint32_t>(level.head_idx);
      Order& maker = pool_[maker_idx];
      const Qty fill = std::min(remaining, maker.remaining);
      remaining -= fill;
      if (fill == maker.remaining) {
        opposite.remove_order(pool_, maker_idx);
        pool_.free(maker_idx);
      } else {
        maker.remaining -= fill;
        level.total_qty -= fill;
      }
    }
    return remaining;
  }

  OrderPool pool_;
  PriceLadder bids_;
  PriceLadder asks_;
};

}  // namespace lob

#endif  // LOB_ORDER_BOOK_H_
