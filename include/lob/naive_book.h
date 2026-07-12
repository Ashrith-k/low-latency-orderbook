#ifndef LOB_NAIVE_BOOK_H_
#define LOB_NAIVE_BOOK_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.h"

namespace lob {

// Reference limit order book: std::map price levels holding std::list FIFO
// queues. Deliberately boring — this is the correctness oracle the optimized
// OrderBook is differentially tested against, and the benchmark baseline
// (DESIGN.md §4.3, §10). Test/bench only: never on the hot path, so it
// allocates freely and favors obviousness over speed.
//
// Matching semantics (the executable spec — the optimized engine must
// reproduce these event streams byte-for-byte):
//   * Price-time priority: an aggressive order sweeps opposing levels best
//     price first, FIFO within a level; partial fills supported.
//   * Every fill executes at the RESTING (maker) order's price — price
//     improvement goes to the aggressor — and emits two Traded events:
//     maker first, then taker, each carrying that order's own side/remaining.
//   * Accepted is emitted first, before any fills, echoing the order as
//     submitted. Market orders ignore the price argument and carry price 0 in
//     their Accepted/Canceled events.
//   * Limit: the unfilled remainder rests. IOC: the remainder is Canceled.
//     Market: sweeps at any price, never rests; the remainder is Canceled —
//     lack of liquidity is not a reject.
class NaiveBook {
 public:
  // Submits an order with full matching semantics (see class comment).
  // `id` is caller-supplied, unique and nonzero — the matching engine mints
  // ids and the differential harness feeds the same id to both books;
  // standalone tests use a counter. Reusing a live id is a precondition
  // violation (debug assert).
  // Emits Accepted, then zero or more maker/taker Traded pairs, then — for an
  // IOC/market remainder — Canceled; or emits exactly one Rejected
  // (kInvalidQty, or kInvalidPrice for limit/IOC price <= 0; market ignores
  // price) with no state change. Returns true iff accepted.
  bool add(OrderId id, Side side, OrderType type, PriceTicks price, Qty qty,
           std::vector<Event>& events) {
    assert(id != kInvalidOrderId);
    assert(!contains(id) && "order ids must be unique");
    if (qty == 0) {
      events.push_back(
          MakeEvent(EventType::kRejected, side, RejectReason::kInvalidQty, qty, 0, price, id));
      return false;
    }
    const bool is_market = (type == OrderType::kMarket);
    if (!is_market && price <= 0) {
      events.push_back(
          MakeEvent(EventType::kRejected, side, RejectReason::kInvalidPrice, qty, 0, price, id));
      return false;
    }
    const PriceTicks order_px = is_market ? 0 : price;
    events.push_back(
        MakeEvent(EventType::kAccepted, side, RejectReason::kNone, qty, qty, order_px, id));

    Qty remaining = qty;
    if (side == Side::kBuy) {
      remaining = Match(asks_, id, side, remaining, events,
                        [&](PriceTicks level_px) { return is_market || level_px <= price; });
    } else {
      remaining = Match(bids_, id, side, remaining, events,
                        [&](PriceTicks level_px) { return is_market || level_px >= price; });
    }

    if (remaining > 0) {
      if (type == OrderType::kLimit) {
        Rest(id, side, price, qty, remaining);
      } else {
        events.push_back(
            MakeEvent(EventType::kCanceled, side, RejectReason::kNone, remaining, 0, order_px, id));
      }
    }
    return true;
  }

  // Resting primitive: validates and rests a limit order WITHOUT matching,
  // even if it crosses. Structural counterpart of OrderBook::add_limit for the
  // Day-2 add/cancel differential (the optimized book learns to match on
  // Day 3); add() above is the full matching entry point. Emits exactly one
  // event: Accepted (order rests) or Rejected (no state change). Returns true
  // iff accepted.
  bool add_limit(OrderId id, Side side, PriceTicks price, Qty qty, std::vector<Event>& events) {
    assert(id != kInvalidOrderId);
    assert(!contains(id) && "order ids must be unique");
    if (qty == 0) {
      events.push_back(
          MakeEvent(EventType::kRejected, side, RejectReason::kInvalidQty, qty, 0, price, id));
      return false;
    }
    if (price <= 0) {
      events.push_back(
          MakeEvent(EventType::kRejected, side, RejectReason::kInvalidPrice, qty, 0, price, id));
      return false;
    }
    Rest(id, side, price, qty, qty);
    events.push_back(
        MakeEvent(EventType::kAccepted, side, RejectReason::kNone, qty, qty, price, id));
    return true;
  }

  // Cancels an open order. Emits exactly one event: Canceled (qty = open qty
  // removed from the book, remaining = 0) or Rejected(kUnknownOrder) — ids
  // that never existed, were already canceled, or fully filled. The side of a
  // cancel-reject is unknowable; it is left zero (kBuy) by spec.
  // Returns true iff canceled.
  bool cancel(OrderId id, std::vector<Event>& events) {
    auto idx = index_.find(id);
    if (idx == index_.end()) {
      events.push_back(
          MakeEvent(EventType::kRejected, Side::kBuy, RejectReason::kUnknownOrder, 0, 0, 0, id));
      return false;
    }
    const Locator loc = idx->second;
    const Qty open = loc.it->remaining;
    index_.erase(idx);
    if (loc.side == Side::kBuy) {
      EraseOrder(bids_, loc);
    } else {
      EraseOrder(asks_, loc);
    }
    events.push_back(
        MakeEvent(EventType::kCanceled, loc.side, RejectReason::kNone, open, 0, loc.price, id));
    return true;
  }

  std::optional<PriceTicks> best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
  }

  std::optional<PriceTicks> best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
  }

  // Total open quantity at a price level; 0 if the level does not exist.
  Qty qty_at(Side side, PriceTicks price) const {
    return (side == Side::kBuy) ? LevelQty(bids_, price) : LevelQty(asks_, price);
  }

  // Number of open orders at a price level; 0 if the level does not exist.
  std::size_t orders_at(Side side, PriceTicks price) const {
    return (side == Side::kBuy) ? LevelSize(bids_, price) : LevelSize(asks_, price);
  }

  std::size_t open_orders() const { return index_.size(); }

  bool contains(OrderId id) const { return index_.contains(id); }

 private:
  struct NaiveOrder {
    OrderId id;
    PriceTicks price;
    Qty qty;        // original quantity
    Qty remaining;  // open quantity still resting
    Side side;
  };

  using Level = std::list<NaiveOrder>;
  // std::list iterators stay valid across sibling insertions/erasures, so the
  // id index can hold them.
  struct Locator {
    Side side;
    PriceTicks price;
    Level::iterator it;
  };

  static Event MakeEvent(EventType kind, Side side, RejectReason reason, Qty qty, Qty remaining,
                         PriceTicks price, OrderId id) {
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

  void Rest(OrderId id, Side side, PriceTicks price, Qty original_qty, Qty remaining) {
    Level& level = (side == Side::kBuy) ? bids_[price] : asks_[price];
    level.push_back(NaiveOrder{id, price, original_qty, remaining, side});
    index_.emplace(id, Locator{side, price, std::prev(level.end())});
  }

  // Sweeps the opposite side while the aggressor has quantity left and
  // `crosses(best opposing level px)` holds; emits a maker-then-taker Traded
  // pair per fill. Fully filled makers are unlinked (their level too, when it
  // empties). Returns the aggressor's unfilled remainder.
  template <typename Map, typename CrossesFn>
  Qty Match(Map& opposite, OrderId taker_id, Side taker_side, Qty remaining,
            std::vector<Event>& events, CrossesFn crosses) {
    while (remaining > 0 && !opposite.empty() && crosses(opposite.begin()->first)) {
      auto level_it = opposite.begin();
      Level& level = level_it->second;
      NaiveOrder& maker = level.front();
      const Qty fill = std::min(remaining, maker.remaining);
      maker.remaining -= fill;
      remaining -= fill;
      // Both fills execute at the resting (maker) price.
      events.push_back(MakeEvent(EventType::kTraded, maker.side, RejectReason::kNone, fill,
                                 maker.remaining, maker.price, maker.id));
      events.push_back(MakeEvent(EventType::kTraded, taker_side, RejectReason::kNone, fill,
                                 remaining, maker.price, taker_id));
      if (maker.remaining == 0) {
        index_.erase(maker.id);
        level.pop_front();
        if (level.empty()) opposite.erase(level_it);
      }
    }
    return remaining;
  }

  template <typename Map>
  static void EraseOrder(Map& side_map, const Locator& loc) {
    auto level_it = side_map.find(loc.price);
    assert(level_it != side_map.end());
    level_it->second.erase(loc.it);
    if (level_it->second.empty()) side_map.erase(level_it);
  }

  template <typename Map>
  static Qty LevelQty(const Map& side_map, PriceTicks price) {
    auto it = side_map.find(price);
    if (it == side_map.end()) return 0;
    Qty total = 0;
    for (const NaiveOrder& order : it->second) total += order.remaining;
    return total;
  }

  template <typename Map>
  static std::size_t LevelSize(const Map& side_map, PriceTicks price) {
    auto it = side_map.find(price);
    return (it == side_map.end()) ? 0 : it->second.size();
  }

  std::map<PriceTicks, Level, std::greater<>> bids_;  // begin() = best (highest) bid
  std::map<PriceTicks, Level> asks_;                  // begin() = best (lowest) ask
  std::unordered_map<OrderId, Locator> index_;        // open orders only
};

}  // namespace lob

#endif  // LOB_NAIVE_BOOK_H_
