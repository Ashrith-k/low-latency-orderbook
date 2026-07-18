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
