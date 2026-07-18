#include <benchmark/benchmark.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "lob/spsc_queue.h"

// Day 4 task 3: the false-sharing before/after numbers (README material).
// lob::SPSCQueue is the "after". The two bench-local queues below are
// deliberately pessimized "before" stages of the exact same release/acquire
// protocol (orderings mirror spsc_queue.h; see the rationale comments there),
// so each delta isolates one design choice:
//   unpadded_uncached -> unpadded_cached : what cached peer indices buy
//   unpadded_cached   -> padded_cached   : what alignas(64) padding buys
namespace {

// Same algorithm as lob::SPSCQueue — cached peer indices included — but all
// four index fields sit packed on one cache line: every producer store to
// tail_ invalidates the line holding the consumer's head_ and tail_cache_,
// and vice versa. Pure false sharing, no protocol difference.
template <typename T>
class UnpaddedCachedSpsc {
 public:
  explicit UnpaddedCachedSpsc(std::size_t capacity) : slots_(capacity), mask_(capacity - 1) {
    assert(capacity != 0 && (capacity & (capacity - 1)) == 0);
  }

  bool try_push(const T& value) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);  // own index
    if (tail - head_cache_ == slots_.size()) {
      head_cache_ = head_.load(std::memory_order_acquire);  // pairs with pop's release
      if (tail - head_cache_ == slots_.size()) {
        return false;
      }
    }
    slots_[tail & mask_] = value;
    tail_.store(tail + 1, std::memory_order_release);  // publishes the slot write
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);  // own index
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);  // pairs with push's release
      if (head == tail_cache_) {
        return false;
      }
    }
    out = slots_[head & mask_];
    head_.store(head + 1, std::memory_order_release);  // hands the slot back
    return true;
  }

 private:
  std::vector<T> slots_;
  std::uint64_t mask_;
  // Deliberately unpadded: both sides' hot fields share cache lines.
  std::atomic<std::uint64_t> tail_{0};
  std::uint64_t head_cache_{0};
  std::atomic<std::uint64_t> head_{0};
  std::uint64_t tail_cache_{0};
};

// Additionally drops the cached peer index: every push and every pop loads
// the peer's atomic, so the unpadded index lines ping-pong on every single
// operation. The naive textbook ring.
template <typename T>
class UnpaddedUncachedSpsc {
 public:
  explicit UnpaddedUncachedSpsc(std::size_t capacity) : slots_(capacity), mask_(capacity - 1) {
    assert(capacity != 0 && (capacity & (capacity - 1)) == 0);
  }

  bool try_push(const T& value) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);  // own index
    // Peer load on every call — pairs with pop's release store of head_.
    if (tail - head_.load(std::memory_order_acquire) == slots_.size()) {
      return false;
    }
    slots_[tail & mask_] = value;
    tail_.store(tail + 1, std::memory_order_release);  // publishes the slot write
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);  // own index
    // Peer load on every call — pairs with push's release store of tail_.
    if (head == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    out = slots_[head & mask_];
    head_.store(head + 1, std::memory_order_release);  // hands the slot back
    return true;
  }

 private:
  std::vector<T> slots_;
  std::uint64_t mask_;
  std::atomic<std::uint64_t> tail_{0};
  std::atomic<std::uint64_t> head_{0};
};

// Producer runs in the timed loop pushing kBatch items per iteration; the
// consumer thread spin-drains for the whole run. items_per_second (real time)
// is end-to-end transfer throughput. Busy spins, no yields: contention on the
// index cache lines is exactly what is being measured.
template <typename Queue>
void BM_SpscTransfer(benchmark::State& state) {
  constexpr std::size_t kBatch = 1 << 16;
  Queue q(1024);
  std::atomic<bool> stop{false};
  std::thread consumer([&q, &stop] {
    std::uint64_t v = 0;
    // relaxed: plain quit flag, no data published through it; queue contents
    // are synchronized by the ring's own release/acquire pairs.
    while (!stop.load(std::memory_order_relaxed)) {
      while (q.try_pop(v)) {
        benchmark::DoNotOptimize(v);
      }
    }
    while (q.try_pop(v)) {
    }
  });
  for (auto _ : state) {
    for (std::uint64_t i = 0; i < kBatch; ++i) {
      while (!q.try_push(i)) {
      }
    }
  }
  stop.store(true, std::memory_order_relaxed);  // quit flag; see above
  consumer.join();
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(kBatch));
}

}  // namespace

BENCHMARK_TEMPLATE(BM_SpscTransfer, UnpaddedUncachedSpsc<std::uint64_t>)
    ->Name("SpscTransfer/unpadded_uncached")
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscTransfer, UnpaddedCachedSpsc<std::uint64_t>)
    ->Name("SpscTransfer/unpadded_cached")
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscTransfer, lob::SPSCQueue<std::uint64_t>)
    ->Name("SpscTransfer/padded_cached")
    ->UseRealTime();
