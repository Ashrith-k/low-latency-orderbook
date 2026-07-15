#ifndef LOB_PRICE_LADDER_H_
#define LOB_PRICE_LADDER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "lob/order_pool.h"
#include "lob/types.h"

namespace lob {

// ---------------------------------------------------------------------------
// PriceLevel: one price point on one side of the book — a FIFO of resting
// orders as an intrusive doubly-linked list of pool indices (DESIGN.md §4.3),
// plus the aggregates the ladder and market-data queries need. The level does
// not know its own slot in the ladder, so Order::level_idx is written by the
// book layer, never here.
// ---------------------------------------------------------------------------

struct PriceLevel {
  // Front of the FIFO: the order with time priority at this price.
  std::int32_t head_idx = kNullIdx;
  std::int32_t tail_idx = kNullIdx;
  std::uint32_t order_count = 0;
  // Explicit padding; keep zero.
  std::uint32_t pad0 = 0;
  // Sum of `remaining` (not original qty) across resting orders — the same
  // quantity NaiveBook::qty_at reports, so the differential harness can
  // compare levels directly. 64-bit: many u32 remainings can top 32 bits.
  std::uint64_t total_qty = 0;

  bool empty() const noexcept { return head_idx == kNullIdx; }

  // Appends a freshly-allocated (or freshly-unlinked) order at the back of
  // the FIFO. Precondition: the order's links are kNullIdx.
  void push_back(OrderPool& pool, std::uint32_t order_idx) noexcept {
    Order& order = pool[order_idx];
    assert(order.prev_idx == kNullIdx && order.next_idx == kNullIdx && "order already linked");
    order.prev_idx = tail_idx;
    if (tail_idx != kNullIdx) {
      pool[static_cast<std::uint32_t>(tail_idx)].next_idx = static_cast<std::int32_t>(order_idx);
    } else {
      head_idx = static_cast<std::int32_t>(order_idx);
    }
    tail_idx = static_cast<std::int32_t>(order_idx);
    ++order_count;
    total_qty += order.remaining;
  }

  // Removes an order from anywhere in the FIFO (cancel from the middle, fill
  // from the front) and resets its links so the slot can be relinked or
  // freed. Precondition: the order is linked into *this* level.
  void unlink(OrderPool& pool, std::uint32_t order_idx) noexcept {
    Order& order = pool[order_idx];
    assert(order_count > 0 && total_qty >= order.remaining);
    // Membership sanity: an order with no prev must be our head, one with no
    // next must be our tail. Catches unlinking from the wrong level.
    assert(order.prev_idx != kNullIdx || head_idx == static_cast<std::int32_t>(order_idx));
    assert(order.next_idx != kNullIdx || tail_idx == static_cast<std::int32_t>(order_idx));
    if (order.prev_idx != kNullIdx) {
      pool[static_cast<std::uint32_t>(order.prev_idx)].next_idx = order.next_idx;
    } else {
      head_idx = order.next_idx;
    }
    if (order.next_idx != kNullIdx) {
      pool[static_cast<std::uint32_t>(order.next_idx)].prev_idx = order.prev_idx;
    } else {
      tail_idx = order.prev_idx;
    }
    order.prev_idx = kNullIdx;
    order.next_idx = kNullIdx;
    --order_count;
    total_qty -= order.remaining;
  }
};

static_assert(sizeof(PriceLevel) == 24);
static_assert(alignof(PriceLevel) == 8);
static_assert(std::is_trivially_copyable_v<PriceLevel>);
static_assert(std::is_standard_layout_v<PriceLevel>);
static_assert(std::has_unique_object_representations_v<PriceLevel>);
static_assert(offsetof(PriceLevel, head_idx) == 0);
static_assert(offsetof(PriceLevel, tail_idx) == 4);
static_assert(offsetof(PriceLevel, order_count) == 8);
static_assert(offsetof(PriceLevel, total_qty) == 16);

// Default banded-array radius: ±4096 ticks around the anchor (DESIGN.md §4.3).
inline constexpr std::uint32_t kDefaultBandRadius = 4096;

// ---------------------------------------------------------------------------
// PriceLadder: one side of the book as a contiguous array of PriceLevels
// indexed by tick offset within [anchor - radius, anchor + radius], plus a
// best-price cursor. Level lookup is a subtract and one indexed load — the
// cache-behavior argument against std::map (DESIGN.md §4.3). Out-of-band
// prices are the caller's problem until the overflow region lands (task 5);
// in_band() is the routing query.
//
// push_order/remove_order deliberately wrap the PriceLevel ops: the best
// cursor and level contents are updated in one place and cannot drift apart.
// The ladder also owns Order::level_idx (the level's slot is its offset).
// ---------------------------------------------------------------------------

class PriceLadder {
 public:
  // Startup-only; the one place this class allocates.
  explicit PriceLadder(Side side, PriceTicks anchor_price,
                       std::uint32_t band_radius = kDefaultBandRadius)
      : side_(side), low_price_(anchor_price - band_radius), levels_(2 * band_radius + 1) {
    assert(anchor_price > 0);
    assert(band_radius > 0 && band_radius <= (1u << 24));
  }

  bool in_band(PriceTicks price) const noexcept {
    return price >= low_price_ && price < low_price_ + static_cast<PriceTicks>(levels_.size());
  }

  // Direct level access for queries and the differential harness.
  // Precondition: in_band(price).
  const PriceLevel& level_at(PriceTicks price) const noexcept {
    assert(in_band(price));
    return levels_[static_cast<std::size_t>(price - low_price_)];
  }

  // Links an order (fields already filled, links null) into its price level
  // and pulls the best cursor toward it if it improves the side. Writes
  // Order::level_idx. Precondition: in_band(order.price_ticks).
  void push_order(OrderPool& pool, std::uint32_t order_idx) noexcept {
    Order& order = pool[order_idx];
    assert(in_band(order.price_ticks) && "out-of-band price");
    const auto offset = static_cast<std::int32_t>(order.price_ticks - low_price_);
    levels_[static_cast<std::size_t>(offset)].push_back(pool, order_idx);
    order.level_idx = offset;
    if (best_offset_ == kNullIdx || IsBetter(offset, best_offset_)) {
      best_offset_ = offset;
    }
  }

  // Unlinks an order from its level (located via Order::level_idx, no price
  // recompute) and resets level_idx. If that empties the best level, the
  // cursor scans toward worse prices: worst-case O(band), but the next
  // non-empty level is typically a few ticks away and the walk is sequential
  // loads over a contiguous array — the trade we chose against a tree.
  void remove_order(OrderPool& pool, std::uint32_t order_idx) noexcept {
    Order& order = pool[order_idx];
    const std::int32_t offset = order.level_idx;
    assert(offset != kNullIdx && "order not in a ladder level");
    assert(offset == static_cast<std::int32_t>(order.price_ticks - low_price_));
    PriceLevel& level = levels_[static_cast<std::size_t>(offset)];
    level.unlink(pool, order_idx);
    order.level_idx = kNullIdx;
    if (level.empty() && offset == best_offset_) {
      AdvanceBest();
    }
  }

  bool empty() const noexcept { return best_offset_ == kNullIdx; }

  // Precondition for both: !empty().
  PriceTicks best_price() const noexcept {
    assert(!empty());
    return low_price_ + best_offset_;
  }
  PriceLevel& best_level() noexcept {
    assert(!empty());
    return levels_[static_cast<std::size_t>(best_offset_)];
  }

  Side side() const noexcept { return side_; }

 private:
  // "Better" toward the front of the book: higher for bids, lower for asks.
  bool IsBetter(std::int32_t lhs, std::int32_t rhs) const noexcept {
    return side_ == Side::kBuy ? lhs > rhs : lhs < rhs;
  }

  // Steps the cursor from the just-emptied best level toward worse prices
  // until a non-empty level or the band edge (then the side is empty).
  void AdvanceBest() noexcept {
    const std::int32_t step = side_ == Side::kBuy ? -1 : 1;
    const std::int32_t end = side_ == Side::kBuy ? -1 : static_cast<std::int32_t>(levels_.size());
    for (std::int32_t off = best_offset_ + step; off != end; off += step) {
      if (!levels_[static_cast<std::size_t>(off)].empty()) {
        best_offset_ = off;
        return;
      }
    }
    best_offset_ = kNullIdx;
  }

  Side side_;
  PriceTicks low_price_;  // price of levels_[0]
  std::int32_t best_offset_ = kNullIdx;
  std::vector<PriceLevel> levels_;
};

}  // namespace lob

#endif  // LOB_PRICE_LADDER_H_
