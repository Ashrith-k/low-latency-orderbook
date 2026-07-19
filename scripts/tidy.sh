#!/usr/bin/env bash
#
# Run clang-tidy (check set: the repo-root .clang-tidy) over every first-party
# translation unit, using the compile database of a configured CMake preset
# (CMAKE_EXPORT_COMPILE_COMMANDS is on by default; headers are covered via the
# config's HeaderFilterRegex as the TUs include them).
#
#   ./scripts/tidy.sh          # analyze; any warning is an error (CI gate)
#   ./scripts/tidy.sh --fix    # apply suggested fixes in place, then rerun
#                              # without --fix and ./scripts/format.sh
#
# TIDY_PRESET=release ./scripts/tidy.sh  # analyze under another preset's flags
# Override binaries with CLANG_TIDY / RUN_CLANG_TIDY (to pin versions like
# CLANG_FORMAT in format.sh).
set -euo pipefail

CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-run-clang-tidy}"
TIDY_PRESET="${TIDY_PRESET:-debug}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/${TIDY_PRESET}"
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
  echo "tidy.sh: $BUILD_DIR/compile_commands.json not found — run: cmake --preset $TIDY_PRESET" >&2
  exit 2
fi

"$CLANG_TIDY" --version

# First-party TUs only: the anchored regex keeps vendored FetchContent sources
# (build/_deps) out even though they are in the compile database.
TU_REGEX="^$REPO_ROOT/(tests|bench|tools)/"

if [[ "${1:-}" == "--fix" ]]; then
  "$RUN_CLANG_TIDY" -clang-tidy-binary "$CLANG_TIDY" -p "$BUILD_DIR" -quiet -fix "$TU_REGEX"
  echo "tidy.sh: fixes applied — rerun without --fix to verify, then ./scripts/format.sh."
else
  # Enforcement lives here, not in .clang-tidy (WarningsAsErrors is empty
  # there by design): the config defines the checks, the caller the severity.
  "$RUN_CLANG_TIDY" -clang-tidy-binary "$CLANG_TIDY" -p "$BUILD_DIR" -quiet \
    -warnings-as-errors='*' "$TU_REGEX"
  echo "tidy.sh: clean."
fi
