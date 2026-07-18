#ifndef LOB_BENCH_SCRIPT_RNG_H_
#define LOB_BENCH_SCRIPT_RNG_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.h"

// Shared script-generation primitives for the bench/ suite (bench-local tier,
// mirroring tests/script_test_util.h): deterministic randomness and the
// harmonic offset weighting, kept in one place so every bench draws from
// provably identical distributions and their numbers stay comparable.
namespace lob::benchutil {

// splitmix64, same constants as workload_gen.h: deterministic scripts with
// no <random> (whose distribution streams are stdlib-specific).
class ScriptRng {
 public:
  explicit ScriptRng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t NextU64() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Modulo bias ~n/2^64: unmeasurable at these ranges (workload_gen.h).
  std::uint64_t Bounded(std::uint64_t n) noexcept {
    assert(n > 0);
    return NextU64() % n;
  }

 private:
  std::uint64_t state_;
};

// Harmonic ("Zipf-ish", s = 1) CDF over offsets 0..max_offset — the exact
// weighting WorkloadGenerator uses: weight(k) ∝ 1/(k+1), floored at 1.
inline std::vector<std::uint64_t> MakeZipfCdf(std::uint32_t max_offset) {
  constexpr std::uint64_t kZipfScale = std::uint64_t{1} << 20;
  std::vector<std::uint64_t> cdf;
  cdf.reserve(std::size_t{max_offset} + 1);
  std::uint64_t acc = 0;
  for (std::uint64_t k = 0; k <= max_offset; ++k) {
    acc += kZipfScale / (k + 1) + 1;
    cdf.push_back(acc);
  }
  return cdf;
}

inline PriceTicks SampleOffset(ScriptRng& rng, const std::vector<std::uint64_t>& cdf) {
  const std::uint64_t r = rng.Bounded(cdf.back());
  return static_cast<PriceTicks>(std::upper_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
}

}  // namespace lob::benchutil

#endif  // LOB_BENCH_SCRIPT_RNG_H_
