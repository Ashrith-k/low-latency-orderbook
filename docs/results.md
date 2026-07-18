# Benchmark results — README draft

Day 6 task 7 (`[docs: results table]`): the numbers section the Day-7 README
inlines, drafted while the data is fresh. Every figure below comes from one
session's release-preset runs on one host; regenerate with the commands at
the bottom.

## Environment (read this before the numbers)

| | |
|---|---|
| CPU | AMD EPYC 7763 (GitHub Codespace VM) — 2 vCPUs that are **SMT siblings of one physical core** |
| Kernel / OS | Linux 6.8.0-1052-azure, Ubuntu 24.04 |
| Compiler | gcc 13.3.0, `-O3` (`release` preset), C++20 |
| Benchmark | Google Benchmark v1.9.1; mean of 3 repetitions per number, single run (2026-07-18) |
| Caveats | Unpinned VM: run-to-run variance ±5–10%; the PMU is not exposed, so no IPC/cache/branch counters here (`scripts/perf_stat.sh --check`). **Indicative numbers — a bare-metal rerun owns the final table.** |

## Headline: array beats map

`OrderBook` (banded-array price ladder + arena order pool, pool indices, no
per-op allocation) vs `NaiveBook` (`std::map<price, std::list<Order>>`, the
correctness oracle) on identical pregenerated scripts:

![NaiveBook vs OrderBook](img/naive_vs_book.svg)

| scenario (what it measures) | naive | book | speedup |
|---|---|---|---|
| BookAddRest/4096 — fill an empty book with 4096 passive limits | 7.1M ops/s | 102.6M ops/s | **14.5×** |
| BookSteadyAddCancel/4096 — 64Ki-op 50/50 add/cancel churn at constant depth | 8.7M ops/s | 52.2M ops/s | **6.0×** |
| MatchSteady/4096 — maker/taker pairs at the touch over a 4096-order book | 16.4M cmd/s | 62.3M cmd/s | **3.8×** |
| MatchSweep/16 — one IOC sweeping 16 price levels per round | 8.5M cmd/s | 45.0M cmd/s | **5.3×** |
| MatchSweepDrain/16 — same, draining the side truly empty every round | 8.5M cmd/s | 44.8M cmd/s | **5.3×** |

## Scaling by book depth / sweep breadth

Throughput in M ops/s (commands/s for the matching scenarios); ratio = book ÷ naive.

**BookAddRest** (depth = orders added):

| depth | 1024 | 4096 | 32768 | 65536 |
|---|---|---|---|---|
| naive | 7.5 | 7.1 | 7.3 | 7.6 |
| book | 90.3 | 102.6 | 79.2 | 80.0 |
| ratio | 12.0× | 14.5× | 10.9× | 10.5× |

**BookSteadyAddCancel** (depth = resting orders maintained):

| depth | 1024 | 4096 | 32768 | 65536 |
|---|---|---|---|---|
| naive | 9.7 | 8.7 | 7.6 | 7.3 |
| book | 50.5 | 52.2 | 40.8 | 38.4 |
| ratio | 5.2× | 6.0× | 5.3× | 5.3× |

**MatchSteady** (depth = background book size; every pair is one rest + one full fill):

| depth | 1024 | 4096 | 32768 | 65536 |
|---|---|---|---|---|
| naive | 17.0 | 16.4 | 17.1 | 16.3 |
| book | 60.0 | 62.3 | 62.6 | 58.6 |
| ratio | 3.5× | 3.8× | 3.7× | 3.6× |

**MatchSweep** (K = levels swept per IOC; two resting floor orders):

| K | 1 | 4 | 16 | 64 |
|---|---|---|---|---|
| naive | 18.8 | 10.7 | 8.5 | 6.8 |
| book | 82.0 | 52.5 | 45.0 | 42.7 |
| ratio | 4.4× | 4.9× | 5.3× | 6.3× |

**MatchSweepDrain** (K levels swept and the side drained *truly empty* every
round — the regime that used to be the ladder's worst case):

| K | 1 | 4 | 16 | 64 |
|---|---|---|---|---|
| naive | 19.2 | 11.5 | 8.5 | 7.7 |
| book | 88.5 | 59.8 | 44.8 | 45.9 |
| ratio | 4.6× | 5.2× | 5.3× | 5.9× |

The map's gap widens with sweep breadth (an erase + rebalance per exhausted
level vs a cursor step), and shrinks with depth (both structures feel a big
book, but the map feels it more). Before the Day-6 targeted optimizations
(`docs/optlog.md`), MatchSweepDrain/1 was **0.94M cmd/s — 23× slower than
the map**: draining a side empty cost an O(band_radius) cursor rescan. An
O(1) empty-side transition (a per-side order counter) plus one saved
best-price computation per fill turned the worst case into a 4.6× win.

## End-to-end pipeline

`lob_e2e_bench`: replay file → producer thread → SPSC command ring → engine
thread (pinned) → SPSC log ring → logger thread, per-command rdtsc latency
recorded on the engine thread. 11M-command workload (60% limit / 5% market /
15% IOC / 20% cancel, Zipf-clustered prices, seed 42), 1M warmup discarded,
10M measured:

**Throughput: 7.5M commands/s** through the full four-thread pipeline
(engine-side book latency, not queueing delay, is what the percentiles
below measure).

![Latency percentiles](img/latency_percentiles.svg)

| op | count | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| limit | 6.00M | 42 ns | 98 ns | 202 ns | 379 ns | 7.5 ms |
| market | 0.50M | 81 ns | 181 ns | 336 ns | 412 ns | 4.3 ms |
| ioc | 1.50M | 42 ns | 99 ns | 202 ns | 380 ns | 3.9 ms |
| cancel | 2.00M | 34 ns | 48 ns | 68 ns | 103 ns | 5.5 ms |
| **all** | 10.0M | 41 ns | 96 ns | 201 ns | 380 ns | 7.5 ms |

Honest reading of the tail: the millisecond maxima are scheduler preemption
— four pipeline threads share 2 SMT-sibling vCPUs, and `perf stat` shows
~61K context switches/s during the run (vs ~300/s for the single-threaded
microbenches). Never average away tail latency; on isolated cores this max
collapses, but that claim belongs to a bare-metal rerun, not this table.
Backpressure behaves as designed: the command ring saturates (the engine is
the bottleneck being measured) and the logger's count-and-drop policy sheds
load without ever blocking the engine.

## Reproduce

```sh
cmake --preset release && cmake --build --preset release

# Microbenchmarks (JSON for tools/plot_bench.py compare):
./build/release/bench/lob_bench --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    --benchmark_out=bench.json --benchmark_out_format=json

# End-to-end (CSV for tools/plot_bench.py latency):
./build/release/tools/lob_replay generate --out e2e.lobr --ops 11000000 --seed 42
./build/release/bench/lob_e2e_bench e2e.lobr --warmup 1000000
./build/release/bench/lob_e2e_bench e2e.lobr --warmup 1000000 --csv > e2e.csv

# Charts:
./tools/plot_bench.py compare --json bench.json --out docs/img/naive_vs_book.svg
./tools/plot_bench.py latency --csv e2e.csv --out docs/img/latency_percentiles.svg

# Hardware counters (bare metal; degrades honestly in VMs):
./scripts/perf_stat.sh --check
sudo ./scripts/perf_stat.sh -r 3 -- ./build/release/bench/lob_bench
```
