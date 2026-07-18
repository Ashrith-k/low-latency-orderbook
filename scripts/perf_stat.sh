#!/usr/bin/env bash
#
# perf_stat.sh — hardware-counter methodology for the Day-6 benches (DESIGN
# §11: record cache-miss / branch-miss / IPC numbers alongside the timing
# runs, bare metal if possible).
#
#   ./scripts/perf_stat.sh --check                 # what can this host count?
#   ./scripts/perf_stat.sh [-r N] -- CMD [ARGS...] # perf stat around a run
#
# Counter set: cycles + instructions (IPC), cache-references/cache-misses,
# branches/branch-misses — the rows the README results table cites — plus
# task-clock, context-switches, cpu-migrations, page-faults, which count even
# where the PMU is hidden. -r N repeats the run and reports mean ± stddev.
#
# Degradation ladder (a refused counter must explain itself, never fail
# cryptically — the same contract as affinity.h):
#   * perf missing or Ubuntu's kernel-version shim unsatisfied -> install
#     hint, exit 1.
#   * kernel.perf_event_paranoid blocks unprivileged perf_event_open ->
#     sudo/sysctl hint, exit 1.
#   * PMU not exposed (VM: hardware rows "<not supported>") -> the run still
#     records the software counters and the report is stamped with an honest
#     rerun-on-bare-metal note; exit follows the command.
#
# Codespaces reality (recorded 2026-07-18): Azure VMs do not virtualize the
# PMU — every hardware row is "<not supported>" even as root, and the stock
# image needs linux-tools-$(uname -r) installed plus sudo (paranoid=4).
# Software counters work; README-grade IPC/cache/branch numbers require a
# bare-metal rerun.
set -euo pipefail

HW_EVENTS="cycles,instructions,cache-references,cache-misses,branches,branch-misses"
SW_EVENTS="task-clock,context-switches,cpu-migrations,page-faults"

usage() {
  cat <<'EOF'
perf_stat.sh - perf stat wrapper for the LOB benches (cache/branch/IPC)

Usage:
  ./scripts/perf_stat.sh --check
      Probe perf, permissions, and PMU exposure on this host.

  ./scripts/perf_stat.sh [-r N] -- CMD [ARGS...]
      Run CMD under perf stat with the DESIGN paragraph-11 counter set.
      -r N repeats the run N times and reports mean and stddev (default 1).

  ./scripts/perf_stat.sh --help
EOF
}

# Exit codes match the repo's CLI tools: 0 ok, 1 environment/runtime, 2 usage.
die_env() {
  echo "error: $1" >&2
  shift
  for hint in "$@"; do
    echo "hint: $hint" >&2
  done
  exit 1
}

require_perf() {
  command -v perf >/dev/null 2>&1 ||
    die_env "perf is not installed" \
      "sudo apt-get install linux-tools-common linux-tools-$(uname -r)"
  # Ubuntu ships /usr/bin/perf as a shim that fails when the kernel-matched
  # tools package is missing; --version exposes that without counting anything.
  perf --version >/dev/null 2>&1 ||
    die_env "perf cannot run on this kernel ($(uname -r))" \
      "sudo apt-get install linux-tools-$(uname -r)"
}

paranoid_level() {
  cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unknown"
}

require_counter_access() {
  local paranoid
  paranoid="$(paranoid_level)"
  # Debian/Ubuntu's paranoid=3+ refuses unprivileged perf_event_open outright;
  # root bypasses the sysctl entirely.
  if [[ "$EUID" -ne 0 && "$paranoid" != "unknown" && "$paranoid" -gt 2 ]]; then
    die_env "kernel.perf_event_paranoid=$paranoid blocks unprivileged counters" \
      "rerun under sudo" \
      "or persistently: sudo sysctl kernel.perf_event_paranoid=1"
  fi
}

# True when the probe output shows the PMU is hidden (typical VM/container).
pmu_hidden() {
  grep -q '<not supported>' <<<"$1"
}

run_check() {
  require_perf
  echo "perf:     $(perf --version)"
  local paranoid
  paranoid="$(paranoid_level)"
  echo "paranoid: kernel.perf_event_paranoid=$paranoid (unprivileged use needs <= 2)"
  if [[ "$EUID" -ne 0 && "$paranoid" != "unknown" && "$paranoid" -gt 2 ]]; then
    echo "counters: blocked for this user — rerun '--check' under sudo for the PMU probe"
    return 0
  fi
  local probe
  probe="$(perf stat -e cycles -x, -- true 2>&1 || true)"
  if pmu_hidden "$probe"; then
    echo "counters: PMU NOT EXPOSED (VM without PMU passthrough) — hardware"
    echo "          cache/branch/IPC rows will read '<not supported>'; only the"
    echo "          software counters count here. Bare metal owns those numbers."
  else
    echo "counters: hardware PMU available — cache/branch/IPC can be recorded here"
  fi
}

main() {
  local repeat=1
  local check=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h | --help)
        usage
        exit 0
        ;;
      --check)
        check=1
        shift
        ;;
      -r | --repeat)
        [[ $# -ge 2 ]] || {
          echo "error: $1 needs a value" >&2
          exit 2
        }
        repeat="$2"
        [[ "$repeat" =~ ^[1-9][0-9]*$ ]] || {
          echo "error: -r expects a positive integer, got '$repeat'" >&2
          exit 2
        }
        shift 2
        ;;
      --)
        shift
        break
        ;;
      -*)
        echo "error: unknown flag '$1'" >&2
        usage >&2
        exit 2
        ;;
      *)
        break
        ;;
    esac
  done

  if [[ "$check" -eq 1 ]]; then
    run_check
    exit 0
  fi
  if [[ $# -eq 0 ]]; then
    usage >&2
    exit 2
  fi

  require_perf
  require_counter_access

  local report
  report="$(mktemp)"
  trap 'rm -f "$report"' EXIT

  # The command's own stdout/stderr flow through untouched; the counter
  # report lands in a file so it can be echoed and post-processed afterwards.
  local status=0
  perf stat -e "$HW_EVENTS,$SW_EVENTS" -r "$repeat" -o "$report" -- "$@" || status=$?

  cat "$report"
  if pmu_hidden "$(cat "$report")"; then
    cat <<'EOF'
NOTE: hardware counters are not exposed on this host (VM without PMU
passthrough) — the cache/branch/IPC rows above read '<not supported>'.
Only the software counters were recorded; rerun on bare metal for
README-grade IPC / cache-miss / branch-miss numbers (DESIGN paragraph 11).
EOF
  fi
  exit "$status"
}

main "$@"
