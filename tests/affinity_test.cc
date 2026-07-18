#include "lob/affinity.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace lob {
namespace {

// The pinning API is best-effort and container-aware, so these tests derive
// the target CPUs from the thread's *allowed* set (sched_getaffinity) rather
// than assuming 0..hardware_concurrency-1 — a cgroup/cpuset-restricted
// environment (Codespaces, CI runners) passes honestly. Each test restores
// the original mask so a manual full-binary run leaves later tests unpinned.

#if defined(__linux__)

std::vector<int> AllowedCpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  EXPECT_EQ(sched_getaffinity(0, sizeof(set), &set), 0);
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
}

// RAII: capture the calling thread's affinity mask now, restore on scope exit.
class AffinityRestorer {
 public:
  AffinityRestorer() { sched_getaffinity(0, sizeof(mask_), &mask_); }
  ~AffinityRestorer() { sched_setaffinity(0, sizeof(mask_), &mask_); }

 private:
  cpu_set_t mask_{};
};

TEST(Affinity, PinCurrentThreadToEachAllowedCpu) {
  const AffinityRestorer restore;
  const std::vector<int> cpus = AllowedCpus();
  ASSERT_FALSE(cpus.empty());
  for (const int cpu : cpus) {
    EXPECT_TRUE(pin_current_thread_to_cpu(cpu)) << "cpu " << cpu;
    // pthread_setaffinity_np migrates synchronously: we are on `cpu` already.
    EXPECT_EQ(current_cpu(), cpu);
  }
}

TEST(Affinity, PinSpawnedThreadViaNativeHandle) {
  const std::vector<int> cpus = AllowedCpus();
  ASSERT_FALSE(cpus.empty());
  const int target = cpus.back();

  std::atomic<bool> go{false};
  std::atomic<int> seen{-1};
  std::thread worker([&go, &seen] {
    while (!go.load(std::memory_order_acquire)) {  // pairs with the release below:
      std::this_thread::yield();                   // pin completes before we sample
    }
    seen.store(current_cpu(), std::memory_order_relaxed);  // read after join
  });
  EXPECT_TRUE(pin_thread_to_cpu(worker.native_handle(), target));
  go.store(true, std::memory_order_release);
  worker.join();
  EXPECT_EQ(seen.load(std::memory_order_relaxed), target);
}

TEST(Affinity, RejectsInvalidCpus) {
  const AffinityRestorer restore;
  EXPECT_FALSE(pin_current_thread_to_cpu(-1));
  EXPECT_FALSE(pin_current_thread_to_cpu(CPU_SETSIZE));  // out of representable range
  // Representable but (on any sane test box) nonexistent: kernel EINVAL path.
  const std::vector<int> cpus = AllowedCpus();
  if (std::find(cpus.begin(), cpus.end(), CPU_SETSIZE - 1) == cpus.end()) {
    EXPECT_FALSE(pin_current_thread_to_cpu(CPU_SETSIZE - 1));
  }
}

TEST(Affinity, CurrentCpuIsWithinAllowedSet) {
  const std::vector<int> cpus = AllowedCpus();
  const int cpu = current_cpu();
  EXPECT_NE(cpu, -1);
  EXPECT_NE(std::find(cpus.begin(), cpus.end(), cpu), cpus.end());
}

#endif  // defined(__linux__)

}  // namespace
}  // namespace lob
