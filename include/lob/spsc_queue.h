#ifndef LOB_SPSC_QUEUE_H_
#define LOB_SPSC_QUEUE_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// SPSCQueue<T>: lock-free single-producer/single-consumer ring (DESIGN.md
// §4.4). Exactly one thread calls try_push and exactly one calls try_pop; the
// only synchronization between them is the tail_/head_ release/acquire pairs.
//
// Capacity is a power of two so masking replaces modulo. Indices are
// monotonic uint64 masked on access: all `capacity` slots are usable, and
// full (tail - head == capacity) vs empty (tail == head) are unambiguous
// without sacrificing a slot. The indices never wrap in practice — 2^64 ops
// at 1G ops/s is ~585 years.
//
// Cache layout: each side's own index and its cached copy of the peer's index
// share one alignas(64) block, so a steady-state push or pop writes no line
// the peer polls; the peer's atomic is (re)loaded only when the cached value
// says full/empty.
// ---------------------------------------------------------------------------

template <typename T>
class SPSCQueue {
  // Slots move by plain copy assignment with no cleverness; Command (24 B)
  // and Event (32 B) are the intended cargo.
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // Startup-only; the one place this class allocates. Capacity must be a
  // nonzero power of two so index masking replaces modulo.
  explicit SPSCQueue(std::size_t capacity) : slots_(capacity), mask_(capacity - 1) {
    assert(capacity != 0 && (capacity & (capacity - 1)) == 0 &&
           "capacity must be a nonzero power of two");
  }

  // Producer thread only. Returns false when the ring is full.
  bool try_push(const T& value) noexcept {
    // relaxed: tail_ is written only by this (producer) thread, so its own
    // last store is always visible without ordering.
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == slots_.size()) {
      // acquire: pairs with the consumer's release store of head_, so the
      // consumer's read of a reclaimed slot happens-before we overwrite it.
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == slots_.size()) {
        // Truly full (fresh head): the high-water mark is exactly capacity.
        update_high_water(slots_.size());
        return false;
      }
    }
    slots_[tail & mask_] = value;
    // Occupancy after this push, as the producer sees it (DESIGN §6 ring
    // high-water counter). head_cache_ may be stale, so this is an upper
    // bound on the true occupancy — and exact whenever the ring actually
    // fills, because the full check above always refreshes first.
    update_high_water(tail + 1 - head_cache_);
    // release: publishes the slot write above; the consumer's acquire load of
    // tail_ sees the data before the index that exposes it.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Returns false when the ring is empty.
  bool try_pop(T& out) noexcept {
    // relaxed: head_ is written only by this (consumer) thread, so its own
    // last store is always visible without ordering.
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      // acquire: pairs with the producer's release store of tail_, so the
      // producer's slot write happens-before our read of it below.
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return false;
      }
    }
    out = slots_[head & mask_];
    // release: hands the slot back; our read above happens-before the
    // producer's acquire sees the free space and overwrites it.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Pops up to `max_items` (> 0) into out[0..n),
  // preserving FIFO order; returns n, 0 when empty. At most one acquire
  // refresh and exactly one release store cover the whole batch — the engine
  // loop's amortization of ring synchronization (DESIGN.md §4.4).
  std::size_t try_pop_batch(T* out, std::size_t max_items) noexcept {
    assert(max_items > 0);
    // relaxed: head_ is written only by this (consumer) thread, so its own
    // last store is always visible without ordering.
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    std::uint64_t available = tail_cache_ - head;
    if (available == 0) {
      // acquire: pairs with the producer's release store of tail_ (the same
      // edge as try_pop), so every slot read below is fully written.
      tail_cache_ = tail_.load(std::memory_order_acquire);
      available = tail_cache_ - head;
      if (available == 0) {
        return 0;
      }
    }
    // Opportunistic batch: take what the cache already proves is available
    // rather than paying an extra acquire load to look for more.
    const std::uint64_t n =
        available < static_cast<std::uint64_t>(max_items) ? available : max_items;
    for (std::uint64_t i = 0; i < n; ++i) {
      out[i] = slots_[(head + i) & mask_];
    }
    // release: hands all n slots back at once; the producer's acquire sees
    // the free space only after every read above is done.
    head_.store(head + n, std::memory_order_release);
    return static_cast<std::size_t>(n);
  }

  std::size_t capacity() const noexcept { return slots_.size(); }

  // Diagnostic snapshot: exact only when both threads are quiescent (each
  // load is relaxed — no data is read on the strength of the result, so no
  // ordering is required).
  std::size_t size() const noexcept {
    return static_cast<std::size_t>(tail_.load(std::memory_order_relaxed) -
                                    head_.load(std::memory_order_relaxed));
  }

  // Diagnostic like size(): exact only at quiescence.
  bool empty() const noexcept { return size() == 0; }

  // Diagnostic like size(): the highest post-push occupancy the producer has
  // observed. Upper bound on true peak occupancy (see try_push); exact at
  // capacity when the ring ever filled. Monotone; exact only at quiescence.
  std::size_t high_water() const noexcept {
    // relaxed: a monotonic counter read for reporting; no data is read on
    // the strength of the value, so no ordering is required.
    return static_cast<std::size_t>(high_water_.load(std::memory_order_relaxed));
  }

 private:
  void update_high_water(std::uint64_t occupancy) noexcept {
    // relaxed both ways: written only by the producer (single writer), and
    // readers need a number, not ordering.
    if (occupancy > high_water_.load(std::memory_order_relaxed)) {
      high_water_.store(occupancy, std::memory_order_relaxed);
    }
  }

  // Shared, immutable after construction: read by both sides, written by
  // neither, so this line stays shared in both caches and cannot false-share.
  std::vector<T> slots_;
  std::uint64_t mask_;

  // Producer-owned line: tail_ plus the producer's stale-but-safe view of
  // head_ (stale only ever under-reports free space, never over-reports),
  // plus the occupancy high-water mark (producer-written, diagnostic-read).
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t head_cache_{0};
  std::atomic<std::uint64_t> high_water_{0};

  // Consumer-owned line: head_ plus the consumer's stale-but-safe view of
  // tail_ (stale only ever under-reports occupancy, never over-reports).
  alignas(64) std::atomic<std::uint64_t> head_{0};
  std::uint64_t tail_cache_{0};
};

// The alignas(64) blocks force whole-object alignment and size rounding, so
// the shared, producer, and consumer regions occupy disjoint cache lines.
static_assert(alignof(SPSCQueue<std::uint64_t>) == 64);
static_assert(sizeof(SPSCQueue<std::uint64_t>) % 64 == 0);
static_assert(sizeof(SPSCQueue<std::uint64_t>) >= 3 * 64);

}  // namespace lob

#endif  // LOB_SPSC_QUEUE_H_
