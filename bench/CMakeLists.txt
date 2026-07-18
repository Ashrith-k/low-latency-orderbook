# ---------------------------------------------------------------------------
# Google Benchmark via FetchContent (pinned; no system dependency). Its own
# tests/install and internal -Werror are disabled — we only need the library.
# ---------------------------------------------------------------------------
include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.9.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(benchmark)

# Exempt Google Benchmark headers from our -Werror (same rationale as tests/).
foreach(_bench_target benchmark benchmark_main)
  if(TARGET ${_bench_target})
    get_target_property(_inc ${_bench_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${_bench_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Benchmark executable. New benchmark sources are appended here on Day 6.
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_executable(lob_bench
  book_micro_bench.cc
  false_sharing_bench.cc
  matching_bench.cc)
target_link_libraries(lob_bench PRIVATE lob lob_warnings benchmark::benchmark_main
                                        Threads::Threads)

# ---------------------------------------------------------------------------
# End-to-end pipeline benchmark (Day 6 task 3): a standalone harness, not a
# Google Benchmark target — it reports per-op-type latency percentiles from
# a threaded pipeline, which doesn't fit GB's per-iteration timing model.
# Logic lives in e2e_bench_lib.h (thin-main pattern like tools/lob_replay);
# it reuses the replay CLI's parse/validate helpers, hence the tools include.
# ---------------------------------------------------------------------------
add_executable(lob_e2e_bench lob_e2e_bench.cc)
target_link_libraries(lob_e2e_bench PRIVATE lob lob_warnings Threads::Threads)
target_include_directories(lob_e2e_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools)

# One smoke test of the real binary — the gtest suite covers run_e2e_cli()'s
# logic; this proves the executable links and runs.
if(LOB_BUILD_TESTS)
  add_test(NAME lob_e2e_bench_help_smoke COMMAND lob_e2e_bench --help)
endif()
