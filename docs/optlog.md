# Optimization log

Day 6 task 5: profile, apply *measured* optimizations, keep the receipts.
Every entry records its motivation (a measured finding), the exact change,
before/after numbers from the same host and method, and a verdict. Rejected
attempts stay in the log — a null result is a result.

## Environment and method (DESIGN §11 honesty)

- gcc 13.3, `-O3` (release preset), Google Benchmark v1.9.1; mean of 5
  repetitions with stddev unless noted.
- GitHub Codespace: AMD EPYC 7763, 2 vCPUs that are SMT siblings of one
  physical core, unpinned, VM kernel 6.8.0-1052-azure. **Indicative only** —
  README-grade numbers come from bare metal.
- The VM hides the PMU (`scripts/perf_stat.sh --check`), so there are no
  cache/branch/IPC counters here. Attribution rests on cpu-clock sampling
  (`perf record -e cpu-clock`: ~85% of samples inline into the book code in
  the steady-state add/cancel bench — the book, not the harness, is what's
  being measured) plus targeted microbench deltas, one change at a time.
- Throughputs below are `items_per_second` (book commands/s) from
  `bench/lob_bench`; "Drain" is `MatchSweepDrain` (sweeps that leave a side
  truly empty every round), added in this task as the measuring instrument
  for entry 1 and kept registered so the pathology cannot silently return.

## 1. O(1) empty-side transition in `PriceLadder::remove_order` — KEPT

**Motivation (measured).** `bench: matching` (Day 6 task 2) found that
draining a ladder side *truly* empty forced `AdvanceBest()` to scan
O(band_radius) levels — ~4096 sequential loads, ~0.9 µs — just to discover
emptiness. In `MatchSweepDrain` at K=1 the optimized book managed 0.94M
cmd/s against the std::map baseline's 21.5M: **23× slower** than the
structure it exists to beat.

**Change.** `PriceLadder` now maintains `band_orders_`, a count of in-band
resting orders (one increment in `push_order`, one decrement in
`remove_order`). When removing the last order of the best level leaves the
count at zero, the cursor drops straight to "side empty" instead of
scanning. `check_invariants` reconciles the counter against the full-ladder
walk, and `AdvanceBest` now asserts its non-empty precondition.

**Before → after** (this change alone; K = sweep breadth):

| MatchSweepDrain/book | before | after | change |
|---|---|---|---|
| K=1 | 0.94M/s | 79.1M/s | **84×** |
| K=4 | 2.78M/s | 56.0M/s | 20× |
| K=16 | 8.65M/s | 40.3M/s | 4.7× |
| K=64 | 22.3M/s | 43.0M/s | 1.9× |

Cost check — every other scenario flat within noise (BookAddRest 101.0 →
98.9M/s, BookSteadyAddCancel/4096 55.7 → 55.1M/s, MatchSteady/4096 67.9 →
66.6M/s; baseline stddevs ±1–5M/s): the two extra arithmetic ops per
push/remove do not show above this host's variance.

**Verdict: kept.** The drained-book regime moves from "23× worse than a
map" to 3.7× better, for two integer ops on the steady path.

## 2. One best-price computation per fill in `OrderBook::Match` — KEPT

**Motivation (code reading, confirmed by measurement).** The sweep loop
computed `best_price()` twice per fill — once in the loop condition, then
again inside `best_level()` — plus a redundant `in_band` check, and
`best_price()` carries an overflow-map branch each time.

**Change.** The loop hoists `level_px = best_price()` and feeds it to both
the cross check and a new `PriceLadder::level_at(price)` lookup;
`best_level()` now delegates to `level_at(best_price())` so the pair cannot
drift.

**Before → after** (measured on top of entry 1):

| scenario | before | after | change |
|---|---|---|---|
| MatchSweep/book/1 | 73.5M/s | 90.1M/s ±4.4 | +23% |
| MatchSweepDrain/book/1 | 79.1M/s | 103.3M/s ±4.5 | +31% |
| MatchSweepDrain/book/64 | 43.0M/s | 51.1M/s ±1.7 | +19% |
| MatchSweep/book/64 | 45.2M/s | 47.4M/s ±3.4 | +5% (≈noise) |
| MatchSteady/book/4096 | 66.6M/s | 68.4M/s ±2.8 | flat |

MatchSteady stays flat as expected: one fill per maker/taker pair, so the
level walk never dominates there.

**Verdict: kept.**

## 3. `[[likely]]`/`[[unlikely]]` on validation and overflow branches — REJECTED

**Attempted.** `[[unlikely]]` on the three `add()` reject paths (zero qty,
non-positive price, pool exhausted) and on `push_order`'s out-of-band
branch; `[[likely]]` on `best_price()`'s `overflow_.empty()`.

**Measured** (two independent 5-rep runs after the change, vs entry 2's
numbers):

| scenario | without hints | with hints (run 1 / run 2) |
|---|---|---|
| MatchSteady/book/4096 | 68.4M/s ±2.8 | 72.0 ±4.7 / 73.9 ±2.5 |
| MatchSweep/book/1 | 90.1M/s ±4.4 | 82.6 ±2.0 / 81.6 ±4.6 |
| BookSteadyAddCancel/4096 | 55.6M/s ±1.4 | 55.5 ±1.5 |
| BookAddRest/4096 | 100.1M/s ±2.7 | 100.9 ±3.9 |

**Analysis.** Consistent +5–8% on MatchSteady, consistent −8–9% on
MatchSweep — opposite signs for the same change. These branches are
perfectly predicted at runtime anyway (the workloads never reject), so the
attributes only reshuffle basic-block layout: one scenario's icache layout
wins, another's loses. That is layout roulette, not a predictability win.

**Verdict: reverted.** Revisit only with PGO or bare-metal counters that
can attribute layout effects.

## Net effect and the end-to-end view

Kept changes vs the pre-task baseline: sweep-heavy matching up 17–31%
(MatchSweep/1 76.7 → 90.1M/s), the drained-book pathology gone (0.94 →
103.3M/s at K=1), add/cancel throughput unchanged. The e2e pipeline bench
(`lob_e2e_bench`, 10M measured commands) reads 6.9M cmd/s, p50 41ns / p99
201ns — unchanged within this host's run-to-run variance, as expected: it
is ring/thread-bound on 2 shared vCPUs and its background book keeps both
sides populated, so neither optimized scenario dominates its mix.
