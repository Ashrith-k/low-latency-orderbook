#ifndef LOB_LATENCY_H_
#define LOB_LATENCY_H_

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace lob {

// ---------------------------------------------------------------------------
// Latency instrumentation (DESIGN §6): rdtsc-based timestamps calibrated
// against steady_clock, a log2-bucketed histogram, and integer percentile
// extraction. No allocation, no locks, no floating point — record() is O(1)
// on the hot path; everything expensive (calibration, conversion, report)
// happens off it.
// ---------------------------------------------------------------------------

// Raw cycle-counter read. Plain rdtsc, deliberately without a serializing
// fence: the few-ns out-of-order skew is noise at the op granularity we
// measure, and a fence would perturb the very pipeline being timed. Assumes
// invariant TSC (every x86 since Nehalem; calibration sanity-checks it).
// Non-x86 builds fall back to steady_clock nanos, keeping the header
// portable the way affinity.h is.
inline std::uint64_t rdtsc_now() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// ---------------------------------------------------------------------------
// TscCalibration: ticks/second measured against steady_clock, startup-only.
// ---------------------------------------------------------------------------

struct TscCalibration {
  std::uint64_t ticks_per_second = 1'000'000'000;  // identity default: 1 tick == 1 ns

  // Spin-measures the TSC rate over `window` (spinning rather than sleeping
  // keeps scheduler noise out of the two endpoint pairs). Degenerate
  // readings — non-monotonic TSC, zero elapsed — fall back to the identity
  // rate instead of dividing by zero. Window is capped so the tick product
  // below cannot overflow (5 GHz × 1 s × 1e9 < 2^63).
  static TscCalibration measure(std::chrono::milliseconds window = std::chrono::milliseconds{20}) {
    assert(window.count() > 0 && window.count() <= 1'000);
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = rdtsc_now();
    const auto deadline = t0 + window;
    auto t1 = t0;
    do {
      t1 = std::chrono::steady_clock::now();
    } while (t1 < deadline);
    const std::uint64_t c1 = rdtsc_now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    TscCalibration cal;
    if (c1 > c0 && ns > 0) {
      cal.ticks_per_second = (c1 - c0) * 1'000'000'000 / static_cast<std::uint64_t>(ns);
    }
    if (cal.ticks_per_second == 0) {
      cal.ticks_per_second = 1'000'000'000;
    }
    return cal;
  }

  // Exact split math: sec * 1e9 never overflows for any plausible tick
  // count, and rem < ticks_per_second (< 2^34 for real clocks) keeps the
  // second product under 2^64.
  [[nodiscard]] std::uint64_t ticks_to_nanos(std::uint64_t ticks) const noexcept {
    const std::uint64_t sec = ticks / ticks_per_second;
    const std::uint64_t rem = ticks % ticks_per_second;
    return sec * 1'000'000'000 + rem * 1'000'000'000 / ticks_per_second;
  }
};

// ---------------------------------------------------------------------------
// LatencyRecorder: 65 counters indexed by bit_width(ticks) — bucket 0 holds
// exact zeros, bucket b holds [2^(b-1), 2^b). Percentiles use ceil-rank
// with linear interpolation inside the bucket, clamped to the observed
// [min, max]. Honesty note: worst-case error is one bucket span (< 2×);
// interpolation lands uniform-ish data within a few percent, and min/max
// are always exact. That resolution buys an allocation-free, lock-free,
// O(1)-record histogram (DESIGN §6). Single-writer: one thread records;
// merge() combines per-thread recorders at report time.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kLatencyBuckets = 65;

// All fields in ticks (or nanos after to_nanos); count is a count.
struct LatencySummary {
  std::uint64_t count;
  std::uint64_t min_ticks;
  std::uint64_t max_ticks;
  std::uint64_t p50;
  std::uint64_t p90;
  std::uint64_t p99;
  std::uint64_t p999;

  [[nodiscard]] LatencySummary to_nanos(const TscCalibration& cal) const noexcept {
    return LatencySummary{count,
                          cal.ticks_to_nanos(min_ticks),
                          cal.ticks_to_nanos(max_ticks),
                          cal.ticks_to_nanos(p50),
                          cal.ticks_to_nanos(p90),
                          cal.ticks_to_nanos(p99),
                          cal.ticks_to_nanos(p999)};
  }
};

static_assert(sizeof(LatencySummary) == 56);
static_assert(std::is_trivially_copyable_v<LatencySummary>);
static_assert(std::has_unique_object_representations_v<LatencySummary>);

class LatencyRecorder {
 public:
  // Hot path: one increment, two compares, no branches beyond them.
  void record(std::uint64_t ticks) noexcept {
    ++counts_[static_cast<std::size_t>(std::bit_width(ticks))];
    ++count_;
    min_ = ticks < min_ ? ticks : min_;
    max_ = ticks > max_ ? ticks : max_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  // Exact observed extremes (not bucket bounds); both 0 on an empty recorder.
  [[nodiscard]] std::uint64_t min_ticks() const noexcept { return count_ == 0 ? 0 : min_; }
  [[nodiscard]] std::uint64_t max_ticks() const noexcept { return max_; }

  // The num/den-th percentile in ticks (e.g. 50/100, 999/1000). Integer
  // ceil-rank; 0 on an empty recorder; num == den is exactly max.
  [[nodiscard]] std::uint64_t percentile_ticks(std::uint64_t num,
                                               std::uint64_t den) const noexcept {
    assert(den > 0 && num <= den);
    if (count_ == 0) {
      return 0;
    }
    if (num == den) {
      return max_;
    }
    std::uint64_t rank = (count_ * num + den - 1) / den;  // ceil
    rank = rank == 0 ? 1 : rank;
    std::uint64_t cum = 0;
    for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
      if (cum + counts_[b] >= rank) {
        return clamp_to_observed(interpolate(b, rank - cum - 1, counts_[b]));
      }
      cum += counts_[b];
    }
    return max_;  // unreachable: cum totals count_ and rank <= count_
  }

  [[nodiscard]] LatencySummary summary() const noexcept {
    return LatencySummary{count_,
                          min_ticks(),
                          max_ticks(),
                          percentile_ticks(50, 100),
                          percentile_ticks(90, 100),
                          percentile_ticks(99, 100),
                          percentile_ticks(999, 1'000)};
  }

  // Report-time combination of per-thread recorders (same bucket layout by
  // construction).
  void merge(const LatencyRecorder& other) noexcept {
    for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
      counts_[b] += other.counts_[b];
    }
    count_ += other.count_;
    min_ = other.min_ < min_ ? other.min_ : min_;
    max_ = other.max_ > max_ ? other.max_ : max_;
  }

  // Warmup discard (DESIGN §11: warm up, reset, then measure).
  void reset() noexcept { *this = LatencyRecorder{}; }

 private:
  static constexpr std::uint64_t bucket_lo(std::size_t b) noexcept {
    return b == 0 ? 0 : std::uint64_t{1} << (b - 1);
  }

  static constexpr std::uint64_t bucket_hi(std::size_t b) noexcept {
    if (b == 0) {
      return 0;
    }
    if (b == kLatencyBuckets - 1) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return (std::uint64_t{1} << b) - 1;
  }

  // Linear interpolation inside bucket b at 0-based in-bucket rank. Split
  // multiplication keeps everything in u64: q * in_rank <= span, and
  // (span % bucket_count) * in_rank < bucket_count * count_ — far below 2^64
  // for any real run.
  static std::uint64_t interpolate(std::size_t b, std::uint64_t in_rank,
                                   std::uint64_t bucket_count) noexcept {
    const std::uint64_t lo = bucket_lo(b);
    const std::uint64_t span = bucket_hi(b) - lo;
    const std::uint64_t q = span / bucket_count;
    const std::uint64_t r = span % bucket_count;
    return lo + q * in_rank + r * in_rank / bucket_count;
  }

  // Exactness where it matters: degenerate distributions report the exact
  // value, and no percentile can leave the observed range.
  [[nodiscard]] std::uint64_t clamp_to_observed(std::uint64_t v) const noexcept {
    v = v < min_ ? min_ : v;
    return v > max_ ? max_ : v;
  }

  std::array<std::uint64_t, kLatencyBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

static_assert(std::is_trivially_copyable_v<LatencyRecorder>);

}  // namespace lob

#endif  // LOB_LATENCY_H_
