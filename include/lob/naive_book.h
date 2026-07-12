#ifndef LOB_NAIVE_BOOK_H_
#define LOB_NAIVE_BOOK_H_

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
// Matching (limit/market/IOC sweeps, partial fills) lands in the next task;
// until then add_limit() rests every accepted order unconditionally, crossing
// or not.
class NaiveBook {
 public:
  // Rests a limit order. `id` is caller-supplied, unique and nonzero — the
  // matching engine mints ids and the differential harness feeds the same id
  // to both books; standalone tests use a counter. Reusing a live id is a
  // precondition violation (debug assert).
  // Emits exactly one event: Accepted (order rests) or Rejected (no state
  // change). Returns true iff accepted.
  bool add_limit(OrderId id, Side side, PriceTicks price, Qty qty, std::vector<Event>& events) {
    assert(id != kInvalidOrderId);
    assert(!contains(id) && "order ids must be unique");
    if (qty == 0) {
      events.push_back(MakeRejected(id, side, RejectReason::kInvalidQty, price, qty));
      return false;
    }
    if (price <= 0) {
      events.push_back(MakeRejected(id, side, RejectReason::kInvalidPrice, price, qty));
      return false;
    }
    Level& level = (side == Side::kBuy) ? bids_[price] : asks_[price];
    level.push_back(NaiveOrder{id, price, qty, qty, side});
    index_.emplace(id, Locator{side, price, std::prev(level.end())});
    events.push_back(Event{.kind = EventType::kAccepted,
                           .side = side,
                           .reason = RejectReason::kNone,
                           .pad0 = 0,
                           .qty = qty,
                           .remaining = qty,
                           .pad1 = 0,
                           .price_ticks = price,
                           .order_id = id});
    return true;
  }

  // Cancels an open order. Emits exactly one event: Canceled (qty = open qty
  // removed from the book, remaining = 0) or Rejected(kUnknownOrder) — ids
  // that never existed or were already canceled (later: fully filled). The
  // side of a cancel-reject is unknowable; it is left zero (kBuy) by spec.
  // Returns true iff canceled.
  bool cancel(OrderId id, std::vector<Event>& events) {
    auto idx = index_.find(id);
    if (idx == index_.end()) {
      events.push_back(MakeRejected(id, Side::kBuy, RejectReason::kUnknownOrder, 0, 0));
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
    events.push_back(Event{.kind = EventType::kCanceled,
                           .side = loc.side,
                           .reason = RejectReason::kNone,
                           .pad0 = 0,
                           .qty = open,
                           .remaining = 0,
                           .pad1 = 0,
                           .price_ticks = loc.price,
                           .order_id = id});
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

  static Event MakeRejected(OrderId id, Side side, RejectReason reason, PriceTicks price, Qty qty) {
    return Event{.kind = EventType::kRejected,
                 .side = side,
                 .reason = reason,
                 .pad0 = 0,
                 .qty = qty,
                 .remaining = 0,
                 .pad1 = 0,
                 .price_ticks = price,
                 .order_id = id};
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
