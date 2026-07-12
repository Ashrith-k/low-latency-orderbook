# LOB — Low-Latency Limit Order Book & Matching Engine

A single-threaded-hot-path, zero-allocation limit order book and matching engine in C++20,
with lock-free ingress/egress queues, an async binary logger, and a rigorous benchmark suite.

## 1. Goals

- **Correctness first**: price-time priority matching, verified by differential testing against a naive reference implementation.
- **Latency**: p99 add/cancel/match under ~1 µs on a modern x86 core (target, to be measured honestly).
- **Throughput**: ≥ 1M operations/sec single engine thread.
- **Zero heap allocation on the hot path** after startup (verified with an allocation-counting test).
- **Sanitizer-clean**: ASan, UBSan, TSan all green in CI.
- **Reproducible benchmarks**: pinned CPU, warmup, deterministic replay files, documented environment.

## 2. Non-Goals (v1)

Explicitly out of scope for the one-week build — listed in README as roadmap:
network gateway (FIX/ITCH/TCP), multi-symbol thread sharding, persistence/snapshots,
risk checks, order replace (cancel+new covers it). Cutting these is a *feature*:
scope discipline is part of the engineering story.

## 3. Architecture

```
┌────────────────────┐   commands    ┌──────────────────────────┐   events    ┌────────────────┐
│ Producer thread    │  SPSC ring    │ Engine thread (pinned)    │  SPSC ring  │ Logger thread  │
│ workload generator ├──────────────▶│  MatchingEngine           ├────────────▶│ AsyncLogger    │
│ or replay reader   │               │   └─ OrderBook            │             │ binary log +   │
└────────────────────┘               │   └─ OrderPool (arena)    │             │ stats          │
                                     │   └─ LatencyRecorder      │             └────────────────┘
                                     └──────────────────────────┘
```

**Single-writer principle**: the engine thread exclusively owns all book state.
No locks anywhere on the hot path; the only synchronization is the two SPSC rings.
This is the same design philosophy real exchanges use (LMAX, NASDAQ-style) and is
the core interview talking point.

## 4. Core Data Structures

### 4.1 Order (one cache line)
POD struct, `static_assert(sizeof(Order) == 64)`:
`order_id (u64)`, `price_ticks (i64)`, `qty (u32)`, `remaining (u32)`,
`side (u8)`, `type (u8)`, `flags (u16)`, plus intrusive links `prev_idx, next_idx (i32)`
and `level_idx (i32)` — **pool indices, not pointers** (half the size, cheaper to
follow with a hot base pointer, and trivially serializable).

Prices are **fixed-point integer ticks**. No floating point anywhere in the engine.

### 4.2 OrderPool
`std::vector<Order>` pre-sized at startup (default 1M), free list threaded through
`next_idx`. O(1) alloc/free, zero malloc after construction. The engine assigns
order IDs = pool index + generation counter in the high bits, so **cancel lookup is
a direct index**, no hash map on the hot path.

### 4.3 PriceLevel & Ladder
Each `PriceLevel`: FIFO intrusive list (head/tail indices), `total_qty`, `order_count`.

Each book side is a **contiguous array of PriceLevels indexed by tick offset** within
a configurable band (default ±4096 ticks) around an anchor price, plus a best-price
cursor that scans to the next non-empty level. Out-of-band orders fall back to a
`std::map` overflow region (rare path).

**Why not `std::map<price, level>` everywhere?** Pointer-chasing red-black trees
miss cache on every hop; the banded array turns best-level access into one or two
predictable loads. We keep a `std::map`-based `NaiveBook` as both the correctness
oracle *and* the benchmark baseline — the measured speedup becomes a resume number.

### 4.4 SPSCQueue<T>
Lock-free single-producer/single-consumer ring: power-of-two capacity, cached
head/tail, `acquire`/`release` atomics, `alignas(64)` padding to kill false sharing.
Batched pop for the engine loop. Memory-ordering rationale documented in comments —
expect interviewers to drill exactly here.

## 5. Matching Semantics

- Order types: **limit**, **market**, **IOC**. Operations: **new**, **cancel**.
- **Price-time priority**: aggressive orders sweep opposing levels best-first,
  FIFO within a level; partial fills supported; remainder of a passive limit rests.
- Events emitted per command: `Accepted`, `Rejected`, `Traded` (one per fill,
  both sides), `Canceled`. Events are fixed-size PODs pushed to the egress ring.
- Debug-build invariants checked after every op: best_bid < best_ask,
  level qty == sum of order qtys, global qty conservation across fills.

## 6. Observability

- **AsyncLogger**: hot path writes a 32-byte binary record (timestamp, event type,
  ids, price, qty) into an SPSC ring; the logger thread formats/flushes. Backpressure
  policy: count-and-drop (never block the engine) — the drop counter is itself a metric.
- **LatencyRecorder**: `rdtsc`-based timestamps (calibrated against `steady_clock`),
  log2-bucketed histogram → p50/p90/p99/p99.9/max. No allocation, no locks.
- Counters: orders in/out, fills, cancels, ring occupancy high-water marks.

## 7. Public API

```cpp
lob::MatchingEngine engine(cfg);
engine.on_event([](const lob::Event& e) { ... });   // or drain event ring manually
lob::OrderId id = engine.submit(lob::NewOrder{sym, Side::Buy, Type::Limit, px, qty});
engine.cancel(id);
```

Headers under `include/lob/`: `types.h`, `order_pool.h`, `price_ladder.h`,
`order_book.h`, `matching_engine.h`, `spsc_queue.h`, `async_logger.h`, `latency.h`,
`naive_book.h` (test/bench only).

## 8. Repository Layout

```
lob/
├── include/lob/          # public headers (header-mostly library)
├── src/                  # non-template implementations
├── tests/                # GoogleTest: unit + differential + invariants
├── bench/                # Google Benchmark micro + end-to-end harness
├── tools/                # lob_replay CLI, workload generator, plot script (Python)
├── scripts/              # format.sh, perf_stat.sh, run_bench.sh
├── docs/                 # DESIGN.md (this file), img/ for plots
├── .github/workflows/ci.yml
├── CMakeLists.txt / CMakePresets.json
├── .clang-format / .clang-tidy
└── README.md
```

## 9. Tech Stack & Build

- **C++20**, GCC 13 and Clang 17 (both in CI).
- **CMake ≥ 3.22 + Ninja**, presets: `debug`, `asan`, `tsan`, `release` (`-O3 -march=native` for local bench; `-O3` only in CI for portability).
- **GoogleTest** and **Google Benchmark** via `FetchContent` (no system deps).
- **clang-format** (Google style, 100 col) + **clang-tidy** (bugprone-*, performance-*, modernize-*).
- **Python 3 + matplotlib** for benchmark plots (tooling only).
- **Docker**: one `Dockerfile` producing a pinned build/bench environment.

## 10. Testing Strategy

1. **Unit tests** per module (pool, ladder, queue, logger, book ops).
2. **Differential testing** (the centerpiece): drive `OrderBook` and `NaiveBook`
   with the same stream of 1M+ seeded random commands; after every command compare
   best bid/ask, level quantities, and emitted events. Failing seed is printed for
   deterministic reproduction. This is how you trust 95% AI-generated code.
3. **Invariant checks** compiled into debug builds.
4. **Concurrency tests** under TSan: queue stress (producer/consumer churn),
   logger stress, full pipeline smoke test.
5. **Zero-allocation test**: global `operator new` counter must not move during
   steady-state processing.

## 11. Benchmark Methodology

- Release build, engine thread pinned (`pthread_setaffinity_np`), 1M-op warmup,
  10M-op measurement, deterministic replay file as workload.
- Report: throughput (ops/s), latency percentiles per op type, naive-vs-optimized
  comparison table, `perf stat` cache-miss/branch-miss/IPC deltas.
- **Honesty rules**: document CPU model, kernel, compiler flags; note that
  Codespaces/VM numbers are indicative — rerun on bare metal if available;
  never average away tail latency.

## 12. Roadmap (post-v1, README material)

epoll TCP gateway with a simple binary protocol → multi-symbol sharding (symbol-hash
to engine threads) → book snapshot/recovery → ITCH 5.0 replay support.
Each maps to a system-design interview answer: "here's how I'd scale it."
