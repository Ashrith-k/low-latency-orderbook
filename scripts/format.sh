#!/usr/bin/env bash
#
# Format (or check) all first-party C++ sources with clang-format (Google style,
# 100 columns; see the repo-root .clang-format).
#
#   ./scripts/format.sh          # rewrite files in place
#   ./scripts/format.sh --check  # verify only; non-zero exit on drift (CI gate)
#
# Override the binary with CLANG_FORMAT=... (e.g. to pin a specific version so
# local formatting matches CI).
set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# First-party source roots only — never touch build/ or vendored dependencies.
CANDIDATE_DIRS=(include src tests bench tools)
DIRS=()
for d in "${CANDIDATE_DIRS[@]}"; do
  [[ -d "$d" ]] && DIRS+=("$d")
done

FILES=()
if [[ ${#DIRS[@]} -gt 0 ]]; then
  mapfile -d '' -t FILES < <(
    find "${DIRS[@]}" -type f \
      \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' \) -print0
  )
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "format.sh: no C++ sources found yet (nothing to do)."
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  "$CLANG_FORMAT" --dry-run --Werror "${FILES[@]}"
  echo "format.sh: ${#FILES[@]} file(s) correctly formatted."
else
  "$CLANG_FORMAT" -i "${FILES[@]}"
  echo "format.sh: formatted ${#FILES[@]} file(s)."
fi
