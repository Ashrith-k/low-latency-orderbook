cmake_minimum_required(VERSION 3.22)

project(lob
  VERSION 0.1.0
  DESCRIPTION "Low-latency limit order book & matching engine"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global language settings: C++20, no compiler extensions (portable).
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Export compile_commands.json for clang-tidy / editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Warnings-as-errors, carried on an INTERFACE target so that vendored
# dependencies (GoogleTest, Google Benchmark) are NOT built with -Werror.
# Link this into first-party targets only.
# ---------------------------------------------------------------------------
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Werror)

# ---------------------------------------------------------------------------
# The library itself is header-mostly; expose include/ to consumers.
# Implementation sources under src/ are added as the project grows.
# ---------------------------------------------------------------------------
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(lob INTERFACE cxx_std_20)
