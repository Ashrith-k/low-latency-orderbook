# PLAN.md — 7-Day Build Plan + Environment Setup

Tasks are 15–30 minutes each and individually commit-worthy. Suggested commit
message in brackets. Rhythm for every task: Claude Code implements → you read every
line → run tests → commit. Never accept a diff you couldn't explain in an interview.

---

## Day 0 setup (first hour of Day 1) — Codespaces

```bash
# toolchain
sudo apt-get update && sudo apt-get install -y \
  ninja-build clang-17 clang-format clang-tidy gcc-13 g++-13 \
  python3-pip linux-tools-common
pip3 install matplotlib

# repo
git init lob && cd lob
git checkout -b main
printf 'build*/\n.cache/\n*.log\n__pycache__/\n' > .gitignore
```

Note: hardware perf counters (`perf stat -e cache-misses`) are usually **blocked in
Codespaces containers**. Develop and measure throughput/percentiles there; run the
`perf` numbers on any bare-metal Linux box (or note the limitation in the README —
documented limitations read as maturity, not weakness).

---

## Day 1 — Scaffold, CI, naive reference book

1. [x] `.gitignore`, MIT `LICENSE`, README stub with one-paragraph pitch. [chore: repo scaffold]
2. [x] Root `CMakeLists.txt` (C++20, warnings-as-errors: `-Wall -Wextra -Wpedantic -Werror`). [build: cmake skeleton]
3. [x] `CMakePresets.json`: debug / asan / tsan / release presets. [build: presets]
4. [x] `.clang-format` + `scripts/format.sh`. [chore: formatting]
5. [x] `.clang-tidy` config. [chore: clang-tidy]
6. [x] FetchContent GoogleTest + one passing dummy test. [test: wire gtest]
7. [x] FetchContent Google Benchmark + one dummy bench. [bench: wire gbench]
8. [x] `.github/workflows/ci.yml`: build+test matrix {gcc,clang} × {debug,release}. [ci: build+test]
9. [x] Add asan job + format-check job to CI. [ci: sanitizers + format gate]
10. [x] `include/lob/types.h`: Side, OrderType, Command, Event PODs + static_asserts. [feat: core types]
11. [x] `NaiveBook` (std::map + std::list): add/cancel. [feat: naive book add/cancel]
12. [x] `NaiveBook` matching (limit/market/IOC, partial fills). [feat: naive matching]
13. [x] 15+ unit tests on NaiveBook — this is your executable spec of matching semantics. [test: matching semantics]

## Day 2 — Pool, ladder, optimized book structure

1. [x] `OrderPool` with index free list + tests (alloc/free/exhaustion/generation bits). [feat: order pool]
2. [x] `Order` 64-byte layout + static_asserts on size/offsets. [feat: order layout]
3. [x] `PriceLevel` intrusive FIFO ops (push_back/unlink) + tests. [feat: price level]
4. [x] `PriceLadder` banded array: level lookup, best-price cursor + tests. [feat: price ladder]
5. [x] Ladder overflow fallback (out-of-band map) + tests. [feat: ladder overflow]
6. [x] `OrderBook::add_limit` (resting only, no match yet) + tests. [feat: book add]
7. [x] `OrderBook::cancel` via direct pool index + tests. [feat: book cancel]
8. [x] Differential harness: seeded random add/cancel stream vs NaiveBook, compare book state each step. [test: differential add/cancel]
9. [x] Fix divergences until 1M-op runs pass; asan/ubsan green. [fix: converge books]

## Day 3 — Matching engine

1. [x] Aggressive limit matching: sweep opposing levels, partial fills, rest remainder. [feat: limit matching]
2. [x] Market + IOC handling. [feat: market/ioc]
3. [x] Event emission into a caller-supplied buffer/callback. [feat: events]
4. [x] Debug-build invariant checks (qty conservation, uncrossed book). [test: invariants]
5. [x] Extend differential harness to full matching + event-stream comparison. [test: differential matching]
6. [x] Edge cases: market on empty book, sweep-through-band, pool exhaustion, cancel-after-fill. [test: edge cases]
7. [x] `MatchingEngine` facade: consumes Command, produces Events. [feat: engine facade]
8. [x] Zero-allocation test (operator new counter). [test: zero alloc]

## Day 4 — Concurrency

1. [x] `SPSCQueue<T>`: single-threaded correctness tests first. [feat: spsc queue]
2. [x] Two-thread stress test; add TSan CI job. [ci: tsan]
3. [x] Cache-line padding + a false-sharing before/after microbench (keep both numbers — README material). [bench: false sharing]
4. [x] Batched pop; engine thread run loop consuming the command ring. [feat: engine loop]
5. [x] CPU pinning utility. [feat: affinity]
6. [x] End-to-end smoke: producer → engine → event ring, counts reconcile. [test: pipeline smoke]
7. [x] Long-running TSan stress of the full pipeline. [test: tsan stress]

## Day 5 — Logger, workload, replay

1. [x] 32-byte binary log record + `AsyncLogger` over SPSC. [feat: async logger]
2. [x] Backpressure: count-and-drop + drop-counter metric + test. [feat: logger backpressure]
3. [x] Workload generator: configurable op mix, Zipf-ish random-walk prices. [feat: workload gen]
4. [x] Replay file format (versioned header + packed commands) + writer/reader. [feat: replay format]
5. [x] `lob_replay` CLI: file → engine → stats summary. [feat: replay cli]
6. [x] `LatencyRecorder`: rdtsc + calibration + log2 histogram + percentile extraction + tests. [feat: latency recorder]
7. [x] Stats/counters module wired through engine. [feat: stats]

## Day 6 — Benchmarks and tuning

1. [ ] Microbench: add/cancel, NaiveBook vs OrderBook. [bench: book micro]
2. [ ] Microbench: matching throughput by book depth. [bench: matching]
3. [ ] End-to-end bench: replay file through full pipeline, percentiles per op type. [bench: e2e]
4. [ ] `scripts/perf_stat.sh` + record cache/branch/IPC numbers (bare metal if possible). [bench: perf counters]
5. [ ] Profile; apply 1–2 *measured* optimizations (e.g. branch hints, prefetch on level walk, batch sizes). Keep before/after in `docs/optlog.md`. [perf: targeted opts]
6. [ ] `tools/plot_bench.py` → latency percentile chart + naive-vs-optimized bar chart into `docs/img/`. [docs: plots]
7. [ ] Final numbers table drafted for README. [docs: results table]

## Day 7 — Polish and ship

1. [ ] README: pitch, architecture diagram, design decisions ("why an array beats a map"), results, build/run instructions, limitations, roadmap. [docs: readme]
2. [ ] Finalize DESIGN.md against as-built reality. [docs: design sync]
3. [ ] Header doc-comments pass on public API. [docs: api comments]
4. [ ] `Dockerfile` (pinned toolchain, builds + runs tests + bench smoke). [build: docker]
5. [ ] clang-tidy clean pass. [chore: tidy]
6. [ ] CI badge, GitHub topics/description, tag `v1.0.0`. [release: v1.0]
7. [ ] Write RESUME notes: bullets with the real measured numbers, elevator pitch, anticipated interview questions. (Bring the numbers back to Claude for Step 7.)

---

## README outline (write it like an engineering blog post)

1. One-line pitch + CI badge + latency plot right at the top
2. Why this exists / design goals
3. Architecture diagram
4. Key design decisions (each: problem → options → choice → measured result)
5. Benchmark results + methodology + environment
6. Correctness story (differential testing, sanitizers, invariants)
7. Build & run
8. Limitations & roadmap

## GitHub Actions skeleton

```yaml
jobs:
  build-test:            # matrix: {gcc-13, clang-17} x {Debug, Release}
  sanitizers:            # asan+ubsan job, tsan job (Debug, clang)
  format:                # clang-format --dry-run --Werror
  bench-smoke:           # release build, 10k-op replay, assert it completes
```
