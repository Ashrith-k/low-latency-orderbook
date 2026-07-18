#include "lob/latency.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lob {
namespace {

// Day 5 task 6: LatencyRecorder. The histogram math is tested with synthetic
// tick values (fully deterministic); rdtsc and calibration get sanity checks
// that hold on any machine, VM or bare metal.

// ---------------------------------------------------------------------------
// Histogram + percentiles.
// ---------------------------------------------------------------------------

TEST(LatencyRecorder, EmptyReportsZeros) {
  const LatencyRecorder rec;
  EXPECT_EQ(rec.count(), 0u);
  EXPECT_EQ(rec.min_ticks(), 0u);
  EXPECT_EQ(rec.max_ticks(), 0u);
  EXPECT_EQ(rec.percentile_ticks(50, 100), 0u);
  const LatencySummary s = rec.summary();
  EXPECT_EQ(s.count, 0u);
  EXPECT_EQ(s.p999, 0u);
}

TEST(LatencyRecorder, SingleRepeatedValueIsExactEverywhere) {
  LatencyRecorder rec;
  for (int i = 0; i < 100; ++i) {
    rec.record(42);
  }
  EXPECT_EQ(rec.count(), 100u);
  EXPECT_EQ(rec.min_ticks(), 42u);
  EXPECT_EQ(rec.max_ticks(), 42u);
  // The bucket for 42 spans [32, 63], but clamping to observed min/max makes
  // every percentile exact for a degenerate distribution.
  EXPECT_EQ(rec.percentile_ticks(1, 100), 42u);
  EXPECT_EQ(rec.percentile_ticks(50, 100), 42u);
  EXPECT_EQ(rec.percentile_ticks(999, 1000), 42u);
  EXPECT_EQ(rec.percentile_ticks(100, 100), 42u);
}

TEST(LatencyRecorder, UniformDataInterpolatesAccurately) {
  LatencyRecorder rec;
  for (std::uint64_t v = 1; v <= 1'000; ++v) {
    rec.record(v);
  }
  EXPECT_EQ(rec.count(), 1'000u);
  EXPECT_EQ(rec.min_ticks(), 1u);
  EXPECT_EQ(rec.max_ticks(), 1'000u);
  // In-bucket interpolation puts uniform data within a few percent of truth
  // (true p50 = 500, p90 = 900, p99 = 990); the log2 bucket alone would only
  // promise factor-2. Windows here are ±10%.
  const std::uint64_t p50 = rec.percentile_ticks(50, 100);
  const std::uint64_t p90 = rec.percentile_ticks(90, 100);
  const std::uint64_t p99 = rec.percentile_ticks(99, 100);
  EXPECT_GE(p50, 450u);
  EXPECT_LE(p50, 550u);
  EXPECT_GE(p90, 810u);
  EXPECT_LE(p90, 990u);
  EXPECT_GE(p99, 891u);
  EXPECT_LE(p99, 1'000u);
}

TEST(LatencyRecorder, PercentilesAreMonotonic) {
  LatencyRecorder rec;
  // Mixed magnitudes: a fast mode, a slow mode, a few extreme outliers.
  for (std::uint64_t i = 0; i < 900; ++i) {
    rec.record(100 + i % 50);
  }
  for (std::uint64_t i = 0; i < 95; ++i) {
    rec.record(10'000 + i * 17);
  }
  for (std::uint64_t i = 0; i < 5; ++i) {
    rec.record(1'000'000 + i);
  }
  const LatencySummary s = rec.summary();
  EXPECT_LE(s.min_ticks, s.p50);
  EXPECT_LE(s.p50, s.p90);
  EXPECT_LE(s.p90, s.p99);
  EXPECT_LE(s.p99, s.p999);
  EXPECT_LE(s.p999, s.max_ticks);
  // 90% of samples are < 150, so p50 must sit in the fast mode; the five
  // outliers own only the top 0.5%, so p99 must not reach them.
  EXPECT_LT(s.p50, 150u);
  EXPECT_LT(s.p99, 1'000'000u);
  EXPECT_EQ(s.max_ticks, 1'000'004u);
}

TEST(LatencyRecorder, HandlesZeroAndHugeValues) {
  LatencyRecorder rec;
  rec.record(0);
  rec.record(0);
  rec.record(std::uint64_t{1} << 63);  // top bucket (bit_width 64)
  EXPECT_EQ(rec.count(), 3u);
  EXPECT_EQ(rec.min_ticks(), 0u);
  EXPECT_EQ(rec.max_ticks(), std::uint64_t{1} << 63);
  EXPECT_EQ(rec.percentile_ticks(50, 100), 0u);
  // The top bucket spans up to 2^64-1; interpolation must not overflow and
  // clamping pins the result to the observed max.
  EXPECT_EQ(rec.percentile_ticks(100, 100), std::uint64_t{1} << 63);
  EXPECT_LE(rec.percentile_ticks(999, 1000), std::uint64_t{1} << 63);
}

TEST(LatencyRecorder, MergeMatchesCombinedRecording) {
  LatencyRecorder a;
  LatencyRecorder b;
  LatencyRecorder combined;
  for (std::uint64_t v = 1; v <= 500; ++v) {
    a.record(v * 3);
    combined.record(v * 3);
  }
  for (std::uint64_t v = 1; v <= 300; ++v) {
    b.record(v * 7 + 1'000);
    combined.record(v * 7 + 1'000);
  }
  a.merge(b);
  const LatencySummary got = a.summary();
  const LatencySummary want = combined.summary();
  EXPECT_EQ(got.count, want.count);
  EXPECT_EQ(got.min_ticks, want.min_ticks);
  EXPECT_EQ(got.max_ticks, want.max_ticks);
  EXPECT_EQ(got.p50, want.p50);
  EXPECT_EQ(got.p90, want.p90);
  EXPECT_EQ(got.p99, want.p99);
  EXPECT_EQ(got.p999, want.p999);
}

TEST(LatencyRecorder, ResetClears) {
  LatencyRecorder rec;
  rec.record(7);
  rec.record(1 << 20);
  rec.reset();
  EXPECT_EQ(rec.count(), 0u);
  EXPECT_EQ(rec.max_ticks(), 0u);
  EXPECT_EQ(rec.percentile_ticks(50, 100), 0u);
  rec.record(5);
  EXPECT_EQ(rec.summary().p50, 5u);
}

// ---------------------------------------------------------------------------
// Calibration + conversion.
// ---------------------------------------------------------------------------

TEST(TscCalibration, TicksToNanosIsExactAtKnownPoints) {
  const TscCalibration cal{1'000'000'000};  // identity: 1 tick == 1 ns
  EXPECT_EQ(cal.ticks_to_nanos(0), 0u);
  EXPECT_EQ(cal.ticks_to_nanos(1), 1u);
  EXPECT_EQ(cal.ticks_to_nanos(123'456'789), 123'456'789u);

  const TscCalibration cal3{3'000'000'000};                       // 3 GHz
  EXPECT_EQ(cal3.ticks_to_nanos(3'000'000'000), 1'000'000'000u);  // 1 s
  EXPECT_EQ(cal3.ticks_to_nanos(3'000), 1'000u);                  // 1 us
  EXPECT_EQ(cal3.ticks_to_nanos(3), 1u);
  // Split math survives multi-second tick counts that would overflow the
  // naive ticks * 1e9 product.
  EXPECT_EQ(cal3.ticks_to_nanos(std::uint64_t{3'000'000'000} * 100),
            std::uint64_t{1'000'000'000} * 100);
}

TEST(TscCalibration, MeasureReturnsPlausibleRate) {
  const TscCalibration cal = TscCalibration::measure(std::chrono::milliseconds{5});
  // Any real TSC (or the steady_clock fallback / identity fallback) lands in
  // [100 MHz, 100 GHz].
  EXPECT_GE(cal.ticks_per_second, 100'000'000u);
  EXPECT_LE(cal.ticks_per_second, 100'000'000'000u);
  // Round trip: one second's worth of ticks is one second of nanos.
  EXPECT_EQ(cal.ticks_to_nanos(cal.ticks_per_second), 1'000'000'000u);
}

TEST(TscCalibration, RdtscAdvancesAcrossASpin) {
  const std::uint64_t c0 = rdtsc_now();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
  }
  const std::uint64_t c1 = rdtsc_now();
  EXPECT_GT(c1, c0);
}

TEST(LatencySummary, ToNanosConvertsEverythingButCount) {
  LatencyRecorder rec;
  for (std::uint64_t v = 1; v <= 100; ++v) {
    rec.record(v * 3);  // ticks at "3 GHz" == v nanos
  }
  const TscCalibration cal3{3'000'000'000};
  const LatencySummary ticks = rec.summary();
  const LatencySummary ns = ticks.to_nanos(cal3);
  EXPECT_EQ(ns.count, ticks.count);
  EXPECT_EQ(ns.min_ticks, 1u);
  EXPECT_EQ(ns.max_ticks, 100u);
  EXPECT_EQ(ns.p50, cal3.ticks_to_nanos(ticks.p50));
  EXPECT_EQ(ns.p999, cal3.ticks_to_nanos(ticks.p999));
}

}  // namespace
}  // namespace lob
