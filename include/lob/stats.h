#ifndef LOB_STATS_H_
#define LOB_STATS_H_

#include <cstdint>
#include <type_traits>

#include "lob/types.h"

namespace lob {

// ---------------------------------------------------------------------------
// EngineStats (DESIGN §6 counters): orders in, event outcomes out. Owned and
// written by the engine thread only — plain increments, no atomics on the
// hot path — and read at quiescence (after run() returns, or from the engine
// thread itself), the same single-writer convention as AsyncLogger's
// records_written_. Ring occupancy high-water marks live on the rings
// themselves (SPSCQueue::high_water); latency percentiles live in
// LatencyRecorder — this struct is pure counting.
//
// All fields are u64 with no padding (asserted), so tests may memcmp two
// stats blocks.
// ---------------------------------------------------------------------------

struct EngineStats {
  // Orders in.
  std::uint64_t news = 0;             // kNew commands / submit() calls
  std::uint64_t cancel_requests = 0;  // kCancel commands / cancel() calls

  // Events out.
  std::uint64_t events = 0;  // every event emitted, of any kind
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;                // all reasons
  std::uint64_t rejected_unknown_order = 0;  // cancel misses
  std::uint64_t rejected_invalid = 0;        // qty/price validation failures
  std::uint64_t rejected_pool_exhausted = 0;
  std::uint64_t traded_events = 0;          // two per fill (maker + taker)
  std::uint64_t traded_qty_both_sides = 0;  // sums both sides' fill qtys (2x real volume)
  std::uint64_t canceled = 0;               // cancels AND market/IOC remainders

  // Update surface (engine thread only).
  void on_new() noexcept { ++news; }
  void on_cancel_request() noexcept { ++cancel_requests; }

  void on_event(const Event& e) noexcept {
    ++events;
    switch (e.kind) {
      case EventType::kAccepted:
        ++accepted;
        return;
      case EventType::kRejected:
        ++rejected;
        switch (e.reason) {
          case RejectReason::kUnknownOrder:
            ++rejected_unknown_order;
            return;
          case RejectReason::kInvalidQty:
          case RejectReason::kInvalidPrice:
            ++rejected_invalid;
            return;
          case RejectReason::kPoolExhausted:
            ++rejected_pool_exhausted;
            return;
          case RejectReason::kNone:
            return;
        }
        return;
      case EventType::kTraded:
        ++traded_events;
        traded_qty_both_sides += e.qty;
        return;
      case EventType::kCanceled:
        ++canceled;
        return;
    }
  }

  // Derived views (identities pinned by tests: events == accepted + rejected
  // + traded_events + canceled; news == accepted + rejected_invalid +
  // rejected_pool_exhausted).
  std::uint64_t commands() const noexcept { return news + cancel_requests; }
  std::uint64_t fills() const noexcept { return traded_events / 2; }
  std::uint64_t traded_qty() const noexcept { return traded_qty_both_sides / 2; }

  // Warmup discard (DESIGN §11), alongside LatencyRecorder::reset().
  void reset() noexcept { *this = EngineStats{}; }
};

static_assert(sizeof(EngineStats) == 88);
static_assert(alignof(EngineStats) == 8);
static_assert(std::is_trivially_copyable_v<EngineStats>);
static_assert(std::has_unique_object_representations_v<EngineStats>);

}  // namespace lob

#endif  // LOB_STATS_H_
