# ---------------------------------------------------------------------------
# GoogleTest via FetchContent (pinned; no system dependency). gmock and install
# rules are disabled — the suite only needs gtest.
# ---------------------------------------------------------------------------
include(FetchContent)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # no-op off Windows; harmless

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googletest)

# First-party targets build under -Werror (via lob_warnings); GoogleTest headers
# pulled into our TUs must not be held to that bar, so mark its include dirs as
# SYSTEM for consumers. (Portable to CMake 3.22; the FetchContent SYSTEM keyword
# would need 3.25.)
foreach(_gtest_target gtest gtest_main)
  if(TARGET ${_gtest_target})
    get_target_property(_inc ${_gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${_gtest_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Test executable. New test sources are appended here as modules land.
# ---------------------------------------------------------------------------
add_executable(lob_tests
  affinity_test.cc
  differential_add_cancel_test.cc
  differential_matching_test.cc
  edge_cases_test.cc
  engine_loop_test.cc
  invariants_test.cc
  matching_engine_test.cc
  naive_book_test.cc
  order_book_test.cc
  order_pool_test.cc
  price_ladder_test.cc
  price_level_test.cc
  smoke_test.cc
  spsc_queue_test.cc
  types_test.cc)
target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)

# The zero-alloc test replaces global operator new to count allocations — a
# binary-wide property — so it owns its own executable instead of joining
# lob_tests. Not built under TSan: libclang_rt.tsan_cxx strongly defines all
# new/delete operators itself (link collision), and a zero-allocation claim
# is single-threaded anyway.
string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize=thread" _lob_tsan_flag_pos)
if(_lob_tsan_flag_pos EQUAL -1)
  add_executable(lob_zero_alloc_test zero_alloc_test.cc)
  target_link_libraries(lob_zero_alloc_test PRIVATE lob lob_warnings GTest::gtest_main)
endif()

# Two-thread stress tests live in their own binary, labeled `concurrency`: the
# TSan CI job runs exactly this label — the single-threaded 1M-op differential
# suites would be minutes-slow under TSan while exercising no synchronization.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_executable(lob_spsc_stress_test spsc_queue_stress_test.cc)
target_link_libraries(lob_spsc_stress_test PRIVATE lob lob_warnings GTest::gtest_main
                                                   Threads::Threads)

include(GoogleTest)
gtest_discover_tests(lob_tests)
if(TARGET lob_zero_alloc_test)
  gtest_discover_tests(lob_zero_alloc_test)
endif()
gtest_discover_tests(lob_spsc_stress_test PROPERTIES LABELS concurrency)
