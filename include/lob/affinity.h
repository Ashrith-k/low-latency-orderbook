#ifndef LOB_AFFINITY_H_
#define LOB_AFFINITY_H_

#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lob {

// ---------------------------------------------------------------------------
// CPU pinning utility (DESIGN.md §11: benchmarks run the engine thread
// pinned — scheduler migrations wreck cache locality and smear the latency
// percentiles being measured). Best-effort by contract: every call reports
// failure instead of raising, because a refusal (container cpuset limits in
// Codespaces/CI, non-Linux hosts) must degrade a run to "unpinned and
// documented", never kill it.
// ---------------------------------------------------------------------------

// Pins the thread behind `handle` (std::thread::native_handle(), a pthread_t
// on Linux) to the single 0-based `cpu`. True on success; false on an invalid
// or disallowed cpu, or on non-Linux platforms.
inline bool pin_thread_to_cpu(std::thread::native_handle_type handle, int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(handle, sizeof(set), &set) == 0;
#else
  (void)handle;
  (void)cpu;
  return false;
#endif
}

// Same, for the calling thread. On success the caller is already executing
// on `cpu` when this returns (the kernel migrates synchronously).
inline bool pin_current_thread_to_cpu(int cpu) noexcept {
#if defined(__linux__)
  return pin_thread_to_cpu(pthread_self(), cpu);
#else
  (void)cpu;
  return false;
#endif
}

// CPU the calling thread is executing on right now (for logging/verifying a
// pin), or -1 where unsupported. Meaningful only while pinned: an unpinned
// thread may migrate the instant this returns.
inline int current_cpu() noexcept {
#if defined(__linux__)
  return sched_getcpu();
#else
  return -1;
#endif
}

}  // namespace lob

#endif  // LOB_AFFINITY_H_
