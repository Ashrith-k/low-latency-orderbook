#ifndef LOB_ORDER_POOL_H_
#define LOB_ORDER_POOL_H_

#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "lob/types.h"

namespace lob {

// Null value for intrusive pool-index links (DESIGN.md §4.1: pool indices, not
// pointers). Valid indices are non-negative and fit in 31 bits.
inline constexpr std::int32_t kNullIdx = -1;

// Default pool capacity (DESIGN.md §4.2: pre-sized at startup, default 1M).
inline constexpr std::uint32_t kDefaultPoolCapacity = 1u << 20;

// ---------------------------------------------------------------------------
// OrderId encoding: high 32 bits = slot generation, low 32 bits = pool index.
// Generations start at 1 and skip 0 on wrap, so no minted id ever equals
// kInvalidOrderId, and a slot's ids never repeat until 2^32 - 1 reuses.
// ---------------------------------------------------------------------------

constexpr OrderId make_order_id(std::uint32_t generation, std::uint32_t index) noexcept {
  return (static_cast<OrderId>(generation) << 32) | index;
}

constexpr std::uint32_t index_of(OrderId id) noexcept { return static_cast<std::uint32_t>(id); }

constexpr std::uint32_t generation_of(OrderId id) noexcept {
  return static_cast<std::uint32_t>(id >> 32);
}

// ---------------------------------------------------------------------------
// Order: one resting order, exactly one cache line. Skeleton for now — only
// the fields the pool itself uses; the full book layout and per-field offset
// asserts land with Day 2 task 2.
// ---------------------------------------------------------------------------

struct alignas(64) Order {
  // Full id while live; kInvalidOrderId while free (the pool's liveness signal).
  OrderId order_id;
  // Last generation minted for this slot. Survives free() so the next alloc
  // can bump it; only the pool reads or writes it.
  std::uint32_t generation;
  // Intrusive links: FIFO neighbors within a price level, owning level slot.
  // While an order is on the free list, next_idx threads that list instead.
  std::int32_t prev_idx;
  std::int32_t next_idx;
  std::int32_t level_idx;
};

static_assert(sizeof(Order) == 64);
static_assert(alignof(Order) == 64);
static_assert(std::is_trivially_copyable_v<Order>);
static_assert(std::is_standard_layout_v<Order>);

// ---------------------------------------------------------------------------
// OrderPool: fixed-capacity slab of Orders with an index free list threaded
// through next_idx. O(1) alloc/free/find, zero heap allocation after
// construction. Cancel lookup is a direct index off the id — no hash map
// (DESIGN.md §4.2).
// ---------------------------------------------------------------------------

class OrderPool {
 public:
  // Startup-only; the one place this class allocates. Capacity must fit in
  // 31 bits so indices are representable as non-negative int32 links.
  explicit OrderPool(std::uint32_t capacity = kDefaultPoolCapacity) : slots_(capacity) {
    assert(capacity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
    // Thread the free list 0 -> 1 -> ... -> capacity-1 so the first
    // allocations walk the slab sequentially.
    for (std::uint32_t i = 0; i < capacity; ++i) {
      slots_[i].next_idx = (i + 1 < capacity) ? static_cast<std::int32_t>(i + 1) : kNullIdx;
    }
    free_head_ = capacity > 0 ? 0 : kNullIdx;
  }

  // Pops the free-list head (LIFO after frees: the just-freed, cache-warm slot
  // is reused first). Returns kInvalidOrderId when exhausted — the engine maps
  // that to RejectReason::kPoolExhausted. Link fields come back reset to
  // kNullIdx; all other book fields are the caller's to fill.
  OrderId alloc() noexcept {
    if (free_head_ == kNullIdx) {
      return kInvalidOrderId;
    }
    const auto index = static_cast<std::uint32_t>(free_head_);
    Order& slot = slots_[index];
    free_head_ = slot.next_idx;
    // Bump the generation, skipping 0 on wrap: ids are never kInvalidOrderId.
    slot.generation =
        slot.generation == std::numeric_limits<std::uint32_t>::max() ? 1 : slot.generation + 1;
    slot.order_id = make_order_id(slot.generation, index);
    slot.prev_idx = kNullIdx;
    slot.next_idx = kNullIdx;
    slot.level_idx = kNullIdx;
    ++live_count_;
    return slot.order_id;
  }

  // Returns the freed slot to the free-list head. Precondition: `index` holds
  // a live order (double-free asserts in debug builds).
  void free(std::uint32_t index) noexcept {
    assert(index < slots_.size());
    Order& slot = slots_[index];
    assert(slot.order_id != kInvalidOrderId && "freeing a slot that is not live");
    slot.order_id = kInvalidOrderId;
    slot.next_idx = free_head_;
    free_head_ = static_cast<std::int32_t>(index);
    --live_count_;
  }

  // Cancel-path lookup: nullptr unless `id` names a currently-live order. A
  // freed slot's order_id is kInvalidOrderId and a reused slot's generation
  // differs, so stale and forged ids fail the single 64-bit compare. The one
  // value that compare cannot reject is kInvalidOrderId itself (a free slot's
  // stored order_id), hence the explicit guard.
  Order* find(OrderId id) noexcept {
    const std::uint32_t index = index_of(id);
    if (id == kInvalidOrderId || index >= slots_.size()) {
      return nullptr;
    }
    Order& slot = slots_[index];
    return slot.order_id == id ? &slot : nullptr;
  }

  const Order* find(OrderId id) const noexcept { return const_cast<OrderPool*>(this)->find(id); }

  // Unchecked-by-index slot access for book internals following intrusive
  // links (bounds asserted in debug builds only).
  Order& operator[](std::uint32_t index) noexcept {
    assert(index < slots_.size());
    return slots_[index];
  }

  const Order& operator[](std::uint32_t index) const noexcept {
    assert(index < slots_.size());
    return slots_[index];
  }

  std::uint32_t capacity() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
  std::uint32_t live_count() const noexcept { return live_count_; }

 private:
  std::vector<Order> slots_;
  std::int32_t free_head_ = kNullIdx;
  std::uint32_t live_count_ = 0;
};

}  // namespace lob

#endif  // LOB_ORDER_POOL_H_
