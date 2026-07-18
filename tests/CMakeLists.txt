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
  differential_add_cancel_test.cc
  differential_matching_test.cc
  edge_cases_test.cc
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
# lob_tests.
add_executable(lob_zero_alloc_test zero_alloc_test.cc)
target_link_libraries(lob_zero_alloc_test PRIVATE lob lob_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)
gtest_discover_tests(lob_zero_alloc_test)
