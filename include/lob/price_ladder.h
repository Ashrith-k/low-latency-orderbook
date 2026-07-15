#ifndef LOB_PRICE_LADDER_H_
#define LOB_PRICE_LADDER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

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

}  // namespace lob

#endif  // LOB_PRICE_LADDER_H_
