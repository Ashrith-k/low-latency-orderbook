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
  // canceled. Market sweeps at any price — the price argument is ignored,
  // and its events carry price 0 — and lack of liquidity is
  // acceptance-then-cancel, not a reject.
  // Events, in NaiveBook's exact order and shape: one Rejected (validation —
  // qty == 0 for all types, price <= 0 for limit/IOC only — or pool
  // exhaustion, which NaiveBook cannot express and is shaped by analogy), OR
  // Accepted, then a maker-then-taker Traded pair per fill, then Canceled
  // for a market/IOC remainder. `sink` is any callable taking const Event&;
  // it must not throw (add is noexcept — no exceptions on the hot path).
  // Add-rejects carry order_id = kInvalidOrderId: no id was minted
  // (types.h). Returns the minted id — an order that fills or cancels out
  // never rests, but its id is still real (and permanently stale) — or
  // kInvalidOrderId on reject. The slot is allocated before the sweep
  // because even a never-rests taker needs an id, so a full pool rejects up
  // front regardless of what the sweep would have done.
  template <typename EventSink>
  OrderId add(Side side, OrderType type, PriceTicks price, Qty qty, EventSink&& sink) noexcept {
    if (qty == 0) {
      sink(MakeEvent(EventType::kRejected, side, RejectReason::kInvalidQty, qty, 0, price,
                     kInvalidOrderId));
      return kInvalidOrderId;
    }
    const bool is_market = type == OrderType::kMarket;
    if (!is_market && price <= 0) {
      sink(MakeEvent(EventType::kRejected, side, RejectReason::kInvalidPrice, qty, 0, price,
                     kInvalidOrderId));
      return kInvalidOrderId;
    }
    const OrderId id = pool_.alloc();
    if (id == kInvalidOrderId) {
      sink(MakeEvent(EventType::kRejected, side, RejectReason::kPoolExhausted, qty, 0, price,
                     kInvalidOrderId));
      return kInvalidOrderId;
    }
    // Market events carry price 0 (NaiveBook: echoing a meaningless caller
    // price would make byte-exact differential comparison fragile).
    const PriceTicks order_px = is_market ? 0 : price;
    sink(MakeEvent(EventType::kAccepted, side, RejectReason::kNone, qty, qty, order_px, id));
    // A market order is a limit at an unbeatable price: sweeping with that
    // sentinel crosses every level, so Match needs no market branch. Safe —
    // Crosses only compares, never does arithmetic on the limit.
    const PriceTicks limit = !is_market           ? price
                             : side == Side::kBuy ? std::numeric_limits<PriceTicks>::max()
                                                  : std::numeric_limits<PriceTicks>::min();
    const Qty remaining = Match(side, limit, qty, id, sink);
    assert(remaining <= qty && "fill accounting underflow");
    const std::uint32_t index = index_of(id);
    if (remaining == 0 || type != OrderType::kLimit) {
      // Fully filled, or a market/IOC remainder: nothing rests.
      if (remaining > 0) {
        sink(
            MakeEvent(EventType::kCanceled, side, RejectReason::kNone, remaining, 0, order_px, id));
      }
      pool_.free(index);
      assert(uncrossed() && "matching left the book crossed");
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
    assert(uncrossed() && "matching left the book crossed");
    return id;
  }

  // Event-less convenience overload (structural tests, benchmarks).
  OrderId add(Side side, OrderType type, PriceTicks price, Qty qty) noexcept {
    return add(side, type, price, qty, [](const Event&) noexcept {});
  }

  // Rests a limit order without matching (crossing prices rest too, exactly
  // like NaiveBook::add_limit — the Day-2 differential harness depends on
  // that). Returns the engine-assigned id, or kInvalidOrderId on validation
  // failure (qty == 0, price <= 0; same checks, same order as NaiveBook) or
  // pool exhaustion. Deliberately event-less: it exists for structural
  // add/cancel comparison; add() is the event-emitting entry point.
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
  // Events, exactly like NaiveBook: Canceled (qty = open qty removed,
  // remaining = 0, the order's own side/price) or Rejected(kUnknownOrder)
  // echoing the target id, side zero (kBuy) by spec — the side of an
  // unknown id is unknowable.
  template <typename EventSink>
  bool cancel(OrderId id, EventSink&& sink) noexcept {
    const Order* order = pool_.find(id);
    if (order == nullptr) {
      sink(MakeEvent(EventType::kRejected, Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 0, id));
      return false;
    }
    const Side side = order->side;
    const PriceTicks price = order->price_ticks;
    const Qty open = order->remaining;
    const std::uint32_t index = index_of(id);
    ladder(side).remove_order(pool_, index);
    pool_.free(index);
    sink(MakeEvent(EventType::kCanceled, side, RejectReason::kNone, open, 0, price, id));
    return true;
  }

  // Event-less convenience overload (structural tests, benchmarks).
  bool cancel(OrderId id) noexcept {
    return cancel(id, [](const Event&) noexcept {});
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

  // True iff `id` names a currently-open order. Ids of filled/canceled orders
  // were real once but are stale now and return false (generation check).
  bool contains(OrderId id) const noexcept { return pool_.find(id) != nullptr; }

  // Full-book invariant verification (DESIGN.md §5, §10.3): both ladders'
  // structural checks (FIFO links, level aggregates, best cursors — see
  // PriceLadder::check_invariants), pool census == resting orders (no order
  // leaked or double-resting; matching never leaves an in-flight taker
  // live), and an uncrossed book. O(band + orders) — call from tests and
  // periodically from harnesses; the hot path asserts only the cheap subset
  // per op. Deliberately invalid for books crossed via add_limit (the Day-2
  // rest-without-matching primitive). Debug builds only; no-op under NDEBUG.
  void check_invariants() const noexcept {
#ifndef NDEBUG
    const PriceLadder::Census bids = bids_.check_invariants(pool_);
    const PriceLadder::Census asks = asks_.check_invariants(pool_);
    assert(bids.order_count + asks.order_count == pool_.live_count() &&
           "pool live_count != resting orders");
    assert(uncrossed() && "book is crossed");
#endif
  }

 private:
  PriceLadder& ladder(Side side) noexcept { return side == Side::kBuy ? bids_ : asks_; }
  const PriceLadder& ladder(Side side) const noexcept { return side == Side::kBuy ? bids_ : asks_; }

  // Matching must never leave the book crossed. add_limit can (by design),
  // which is why cancel() does not assert this: a cancel cannot cross a
  // book, and the Day-2 harness cancels on deliberately crossed ones.
  bool uncrossed() const noexcept {
    return bids_.empty() || asks_.empty() || bids_.best_price() < asks_.best_price();
  }

  // A buy crosses levels priced at or below its limit; a sell at or above.
  static bool Crosses(Side taker_side, PriceTicks limit_price, PriceTicks level_price) noexcept {
    return taker_side == Side::kBuy ? level_price <= limit_price : level_price >= limit_price;
  }

  // Sweeps the side opposite `taker_side` while the taker has quantity left
  // and its limit crosses the best opposing level; fills hit the level head
  // (FIFO = time priority). Each fill emits the maker-then-taker Traded pair
  // — both at the maker's price, each carrying that order's own post-fill
  // remaining — BEFORE the maker is unlinked, while its id/price are still
  // live. A fully filled maker is then remove_order'ed with its remaining
  // untouched — unlink debits total_qty by that remaining — and its slot
  // recycled. A partially filled maker keeps its FIFO place, so its
  // remaining and the level aggregate are debited by hand (unlink is the
  // only level op that maintains total_qty implicitly). Returns the taker's
  // unfilled remainder.
  template <typename EventSink>
  Qty Match(Side taker_side, PriceTicks limit_price, Qty qty, OrderId taker_id,
            EventSink&& sink) noexcept {
    PriceLadder& opposite = ladder(taker_side == Side::kBuy ? Side::kSell : Side::kBuy);
    Qty remaining = qty;
    while (remaining > 0 && !opposite.empty()) {
      // One best-price computation per fill: the price feeds both the cross
      // check and the level lookup (best_level() would recompute it).
      const PriceTicks level_px = opposite.best_price();
      if (!Crosses(taker_side, limit_price, level_px)) {
        break;
      }
      PriceLevel& level = opposite.level_at(level_px);
      assert(level.head_idx != kNullIdx && "best level of a non-empty side cannot be empty");
      const auto maker_idx = static_cast<std::uint32_t>(level.head_idx);
      Order& maker = pool_[maker_idx];
      const Qty fill = std::min(remaining, maker.remaining);
      assert(fill > 0 && "zero-qty fill: sweep would not progress");
      remaining -= fill;
      const Qty maker_left = maker.remaining - fill;
      sink(MakeEvent(EventType::kTraded, maker.side, RejectReason::kNone, fill, maker_left,
                     maker.price_ticks, maker.order_id));
      sink(MakeEvent(EventType::kTraded, taker_side, RejectReason::kNone, fill, remaining,
                     maker.price_ticks, taker_id));
      if (maker_left == 0) {
        opposite.remove_order(pool_, maker_idx);
        pool_.free(maker_idx);
      } else {
        maker.remaining = maker_left;
        level.total_qty -= fill;
      }
    }
    return remaining;
  }

  static Event MakeEvent(EventType kind, Side side, RejectReason reason, Qty qty, Qty remaining,
                         PriceTicks price, OrderId id) noexcept {
    return Event{.kind = kind,
                 .side = side,
                 .reason = reason,
                 .pad0 = 0,
                 .qty = qty,
                 .remaining = remaining,
                 .pad1 = 0,
                 .price_ticks = price,
                 .order_id = id};
  }

  OrderPool pool_;
  PriceLadder bids_;
  PriceLadder asks_;
};

}  // namespace lob

#endif  // LOB_ORDER_BOOK_H_
