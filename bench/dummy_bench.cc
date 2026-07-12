#include <benchmark/benchmark.h>

#include <cstdint>

// Dummy benchmark: proves the Google Benchmark wiring compiles, links, and runs
// under -Werror. Real microbenchmarks (book add/cancel, matching throughput)
// replace this on Day 6.
static void BM_ScalarAdd(benchmark::State& state) {
  std::int64_t acc = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(acc += 3);
  }
}
BENCHMARK(BM_ScalarAdd);
