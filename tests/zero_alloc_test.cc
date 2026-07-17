#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/types.h"

// Day 3 task 8 (DESIGN.md §10.5): the global operator-new counter must not
// move during steady-state in-band processing. Replacing operator new is a
// binary-wide property, so this test owns its own executable
// (lob_zero_alloc_test) instead of joining lob_tests.
//
// The guarantee under test is exactly DESIGN.md's: zero heap allocation on
// the hot path AFTER startup, with the out-of-band overflow map as the one
// blessed exception — pinned down by its own test below.

namespace {

// Relaxed: the test is single-threaded; the counter needs atomicity against
// nothing, ordering against nothing — it exists to survive any future
// multi-threaded reuse without UB.
std::atomic<std::uint64_t> g_new_calls{0};

std::uint64_t AllocCount() { return g_new_calls.load(std::memory_order_relaxed); }

void* CountedAlloc(std::size_t size) {
  g_new_calls.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* CountedAllocNoThrow(std::size_t size) noexcept {
  g_new_calls.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size == 0 ? 1 : size);
}

void* CountedAlignedAllocNoThrow(std::size_t size, std::size_t align) noexcept {
  g_new_calls.fetch_add(1, std::memory_order_relaxed);
  if (align < sizeof(void*)) {
    align = sizeof(void*);
  }
  void* p = nullptr;
  return posix_memalign(&p, align, size == 0 ? 1 : size) == 0 ? p : nullptr;
}

void* CountedAlignedAlloc(std::size_t size, std::size_t align) {
  if (void* p = CountedAlignedAllocNoThrow(size, align)) {
    return p;
  }
  throw std::bad_alloc();
}

}  // namespace

// Replacement set: plain/array x unaligned/aligned x throwing/nothrow. The
// set must be COMPLETE: the aligned forms because the pool is a std::vector
// of alignas(64) Orders (the default aligned new does not forward through
// the plain one), and the nothrow forms because libstdc++ temporary buffers
// (e.g. under gtest's stable_sort) use them — leaving those to ASan's own
// interceptor while our delete calls free() trips alloc-dealloc-mismatch.
void* operator new(std::size_t size) { return CountedAlloc(size); }
void* operator new[](std::size_t size) { return CountedAlloc(size); }
void* operator new(std::size_t size, std::align_val_t align) {
  return CountedAlignedAlloc(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return CountedAlignedAlloc(size, static_cast<std::size_t>(align));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return CountedAllocNoThrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return CountedAllocNoThrow(size);
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return CountedAlignedAllocNoThrow(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return CountedAlignedAllocNoThrow(size, static_cast<std::size_t>(align));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace lob {
namespace {

constexpr PriceTicks kAnchor = 10'000;
constexpr std::uint32_t kRadius = 256;

// A counter that never counts is a test that never fails: prove the
// replacement is wired, for both the plain and the aligned path.
TEST(ZeroAlloc, CounterSeesOrdinaryAllocations) {
  const std::uint64_t before = AllocCount();
  auto* p = new int(7);
  EXPECT_GT(AllocCount(), before);
  delete p;

  const std::uint64_t before_aligned = AllocCount();
  std::vector<Order> aligned(4);  // alignas(64) element: aligned operator new
  EXPECT_GT(AllocCount(), before_aligned);
  EXPECT_EQ(aligned.size(), 4u);
}

TEST(ZeroAlloc, EngineConstructionAllocatesUpFront) {
  const std::uint64_t before = AllocCount();
  MatchingEngine engine({.anchor_price = kAnchor, .band_radius = kRadius, .pool_capacity = 1024});
  EXPECT_GT(AllocCount(), before);  // pool slab + band arrays, by design
  EXPECT_EQ(engine.book().open_orders(), 0u);
}

// The centerpiece: after startup and warmup, a 100k-op window of in-band
// limit/IOC/market/cancel churn — fills, rests, rejects, event emission —
// must not allocate once. The test side is allocation-free too: fixed id
// ring, no vectors, no gtest macros inside the window.
TEST(ZeroAlloc, SteadyStateProcessingDoesNotAllocate) {
  MatchingEngine engine(
      {.anchor_price = kAnchor, .band_radius = kRadius, .pool_capacity = 1u << 17});
  std::mt19937_64 rng(0xA110Cull);
  std::uniform_int_distribution<int> percent(0, 99);
  std::uniform_int_distribution<PriceTicks> px(kAnchor - 200, kAnchor + 200);  // strictly in-band
  std::uniform_int_distribution<Qty> qty(1, 64);

  std::array<OrderId, 1024> ring{};
  std::size_t ring_pos = 0;
  std::uint64_t event_qty = 0;  // consumes event fields so nothing folds away
  auto sink = [&](const Event& e) { event_qty += e.qty; };

  const auto one_op = [&] {
    const int roll = percent(rng);
    if (roll < 60) {
      const Side side = roll % 2 == 0 ? Side::kBuy : Side::kSell;
      const OrderType type = roll < 42   ? OrderType::kLimit
                             : roll < 51 ? OrderType::kIoc
                                         : OrderType::kMarket;
      const OrderId id = engine.submit(side, type, px(rng), qty(rng), sink);
      ring[ring_pos++ % ring.size()] = id;
    } else {
      engine.cancel(ring[static_cast<std::size_t>(roll) % ring.size()], sink);
    }
  };

  for (int i = 0; i < 10'000; ++i) {
    one_op();  // warmup: reach steady state before measuring
  }

  const std::uint64_t before = AllocCount();
  for (int i = 0; i < 100'000; ++i) {
    one_op();
  }
  const std::uint64_t after = AllocCount();

  EXPECT_EQ(before, after) << "hot path allocated " << (after - before) << " time(s)";
  EXPECT_GT(event_qty, 0u);  // the window really processed fills/cancels
  engine.book().check_invariants();
}

// The boundary of the guarantee, pinned honestly: resting an out-of-band
// order inserts into the overflow std::map — the DESIGN §4.3 blessed
// rare-path allocation. This also proves the steady-state test would catch
// a real hot-path allocation.
TEST(ZeroAlloc, OutOfBandRestingAllocatesByDesign) {
  MatchingEngine engine({.anchor_price = kAnchor, .band_radius = kRadius, .pool_capacity = 1024});
  const std::uint64_t before = AllocCount();
  const OrderId id = engine.submit(Side::kBuy, OrderType::kLimit, kAnchor + kRadius + 100, 5);
  EXPECT_GT(AllocCount(), before);
  EXPECT_TRUE(engine.book().contains(id));
}

}  // namespace
}  // namespace lob
