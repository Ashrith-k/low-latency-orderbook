# LOB — Low-Latency Limit Order Book & Matching Engine

A single-threaded-hot-path, zero-allocation limit order book and matching engine in C++20,
with lock-free ingress/egress queues, an async binary logger, and a rigorous benchmark suite.

## 1. Goals

- **Correctness first**: price-time priority matching, verified by differential testing against a naive reference implementation.
- **Latency**: p99 add/cancel/match under ~1 µs on a modern x86 core. Measured:
  p99 201 ns per command end-to-end on the dev VM (`docs/results.md`; indicative —
  bare-metal rerun pending).
- **Throughput**: ≥ 1M operations/sec single engine thread. Measured: 7.5M
  commands/s through the full four-thread pipeline on the same VM.
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
POD struct, `static_assert(sizeof(Order) == 64)` with every offset asserted:
`order_id (u64)`, `price_ticks (i64)`, `qty (u32)`, `remaining (u32)`,
`side (u8)`, `type (u8)`, `flags (u16)`, `generation (u32)`, plus intrusive links
`prev_idx, next_idx (i32)` and `level_idx (i32)` — **pool indices, not pointers**
(half the size, cheaper to follow with a hot base pointer, and trivially
serializable). `generation` lives in the slot because `free()` zeroes `order_id`
as the liveness signal — the last generation must survive for the next alloc to bump.

Prices are **fixed-point integer ticks**. No floating point anywhere in the engine.

### 4.2 OrderPool
`std::vector<Order>` pre-sized at startup (default 1M), free list threaded through
`next_idx`. O(1) alloc/free, zero malloc after construction. Order ids are
`generation << 32 | index` (generations start at 1 and skip 0 on wrap, so
`kInvalidOrderId` is never minted), so **cancel lookup is a bounds check plus one
64-bit compare** — no hash map on the hot path, and stale ids (ABA) are rejected
deterministically.

### 4.3 PriceLevel & Ladder
Each `PriceLevel`: FIFO intrusive list (head/tail indices), `total_qty` (u64 — a
level of u32 remainings can exceed 32 bits), `order_count`.

Each book side is a **contiguous array of PriceLevels indexed by tick offset** within
a configurable band (default ±4096 ticks) around an anchor price, plus a best-price
cursor that scans to the next non-empty level. A per-side count of in-band orders
makes the emptied-side transition O(1) instead of an O(band) rescan — the measured
worst-case fix in `docs/optlog.md` §1. Out-of-band orders fall back to a
`std::map` overflow region (rare path).

**Why not `std::map<price, level>` everywhere?** Pointer-chasing red-black trees
miss cache on every hop; the banded array turns best-level access into one or two
predictable loads. We keep a `std::map`-based `NaiveBook` as both the correctness
oracle *and* the benchmark baseline. Measured: 5–14× across every scenario, the
gap widening with sweep breadth (`docs/results.md`).

### 4.4 SPSCQueue<T>
Lock-free single-producer/single-consumer ring: power-of-two capacity over
monotonic uint64 indices masked on access (all `capacity` slots usable, full/empty
unambiguous), cached peer indices, `acquire`/`release` atomics, `alignas(64)`
producer and consumer blocks to kill false sharing. Batched pop for the engine
loop; the producer side maintains the ring-occupancy high-water mark (§6).
Memory-ordering rationale documented at every atomic — expect interviewers to
drill exactly here.

## 5. Matching Semantics

- Order types: **limit**, **market**, **IOC**. Operations: **new**, **cancel**.
- **Price-time priority**: aggressive orders sweep opposing levels best-first,
  FIFO within a level, every fill at the resting order's price; partial fills
  supported; a limit remainder rests, market/IOC remainders are canceled.
  Internally a market order is a limit at an unbeatable sentinel price, so the
  sweep has no market branch.
- Events emitted per command: `Accepted`, `Rejected` (with a `RejectReason`:
  validation, unknown/stale id, pool exhausted), `Traded` (a maker-then-taker
  pair per fill), `Canceled`. Events are fixed-size PODs pushed to the egress
  ring. Determinism rules for byte-exact comparison: market-order events carry
  price 0 (the price argument is ignored) and a cancel-reject's unknowable side
  is fixed at 0.
- A full pool rejects up front: the taker's id is minted before the sweep, so
  even a taker that would fully fill without resting needs a free slot.
- Debug-build invariants are split: cheap O(1) per-op asserts inline (uncrossed
  book, fill progress), plus a full `check_invariants()` walk (FIFO link
  symmetry, level aggregates vs contents, cursor correctness, census vs pool)
  that tests run per scenario and harnesses at sweep cadence — a full walk per
  op would make the 1M-op differential runs quadratic.

## 6. Observability

- **AsyncLogger**: hot path writes a 32-byte binary record (timestamp, event type,
  ids, price, qty) into an SPSC ring; the logger thread writes raw bytes to a
  caller-supplied `std::ostream`, flushing when idle and at shutdown. Backpressure
  policy: count-and-drop (never block the engine) — the drop counter is itself a
  metric. `log()` is the counting production policy; `try_log()` is the raw
  primitive for callers with their own retry policy, so "dropped" always means a
  record was actually lost.
- **LatencyRecorder**: `rdtsc`-based timestamps (calibrated against `steady_clock`;
  deliberately unfenced — few-ns skew is noise at op granularity and a serializing
  fence would perturb the pipeline being measured), log2-bucketed histogram →
  p50/p90/p99/p99.9/max via integer-only ceil-rank percentiles with in-bucket
  interpolation. No allocation, no locks, no floating point.
- Counters (`EngineStats`): orders in/out, fills, cancels, drops — plain u64s
  owned by the engine thread, read at quiescence. Ring occupancy high-water marks
  live on `SPSCQueue` itself (producer-side relaxed atomic; exact whenever the
  ring truly fills, otherwise an upper bound from the producer's cached view).

## 7. Public API

```cpp
lob::MatchingEngine engine({.anchor_price = px0, .band_radius = 4096, .pool_capacity = 1 << 20});
auto sink = [](const lob::Event& e) { ... };  // templated per-call sink, inlinable
lob::OrderId id = engine.submit(lob::Side::kBuy, lob::OrderType::kLimit, px, qty, sink);
engine.cancel(id, sink);
engine.process(cmd, sink);                    // Command POD dispatch (the ring path)
engine.run(command_ring, stop, ring_sink);    // pinned-thread batch loop
```

Events go to a caller-supplied sink per call, not a stored `on_event` callback —
a `std::function` member would cost an allocation and an indirect call on the
hot path. The ring loop `run()` lives on `MatchingEngine` itself.

Headers under `include/lob/`: `types.h`, `order_pool.h`, `price_ladder.h`,
`order_book.h`, `matching_engine.h` (includes the engine run loop),
`spsc_queue.h`, `async_logger.h`, `latency.h`, `stats.h`, `affinity.h`,
`workload_gen.h`, `replay_format.h` (the versioned replay header embeds the
generator config verbatim — ids are minted deterministically, so a replay is
only faithful against the config that generated it), `naive_book.h` (test/bench
oracle only).

## 8. Repository Layout

```
low-latency-orderbook/
├── include/lob/          # public headers (header-only INTERFACE library; no src/)
├── tests/                # GoogleTest: unit + differential + invariants
├── bench/                # Google Benchmark micro + standalone e2e harness
├── tools/                # lob_replay CLI, plot_bench.py (generator core is a header)
├── scripts/              # format.sh, perf_stat.sh
├── docs/                 # optlog.md, results.md, img/ plots
├── .github/workflows/ci.yml
├── CMakeLists.txt / CMakePresets.json
├── .clang-format / .clang-tidy
├── Dockerfile            # pinned build/bench environment
└── README.md / DESIGN.md (this file) / PLAN.md / PROGRESS.md
```

## 9. Tech Stack & Build

- **C++20**, GCC 13 and Clang 17 (both in CI).
- **CMake ≥ 3.22 + Ninja**, presets: `debug`, `asan`, `tsan`, `release` (portable
  `-O3`; a `-march=native` variant is deferred to a bare-metal session — only
  worth measuring on the machine it targets).
- **GoogleTest v1.15.2** and **Google Benchmark v1.9.1** via `FetchContent`,
  pinned (no system deps).
- **clang-format** (Google style, 100 col) + **clang-tidy** (bugprone-*, performance-*, modernize-*).
- **Python 3 + matplotlib** for benchmark plots (tooling only).
- **Docker**: one `Dockerfile` producing a pinned build/bench environment.

## 10. Testing Strategy

1. **Unit tests** per module (pool, ladder, queue, logger, book ops).
2. **Differential testing** (the centerpiece): drive `OrderBook` and `NaiveBook`
   with the same stream of 1M+ seeded random commands; after every command compare
   best bid/ask, level quantities, and emitted events byte-for-byte. Failing seed
   is printed for deterministic reproduction (`LOB_DIFF_SEED` /
   `LOB_DIFF_MATCH_SEED` replay hooks). As built: 4 seeds × 25k ops in CI plus
   two 1M-op convergence runs — zero divergences found. This is how you trust
   95% AI-generated code.
3. **Invariant checks** compiled into debug builds (the per-op asserts + full
   walk split, §5).
4. **Concurrency tests** under TSan: queue stress (producer/consumer churn),
   logger stress, full-pipeline smoke and stress with deliberately tiny rings.
   The TSan CI job runs the `concurrency`-labeled tests (34 across four
   binaries) — the single-threaded 1M-op runs exercise zero synchronization
   and are excluded.
5. **Zero-allocation test**: a counting replacement of all eight global
   `operator new` forms must not move during steady-state processing. The one
   documented exception: an out-of-band price resting in the overflow map —
   pinned by its own test as the boundary of the guarantee.

## 11. Benchmark Methodology

- Release build, engine thread pinned (`pthread_setaffinity_np`), 1M-op warmup,
  10M-op measurement, deterministic replay file as workload.
- Report: throughput (ops/s), latency percentiles per op type, naive-vs-optimized
  comparison table, `perf stat` cache-miss/branch-miss/IPC deltas.
- **Honesty rules**: document CPU model, kernel, compiler flags; note that
  Codespaces/VM numbers are indicative — rerun on bare metal if available;
  never average away tail latency.
- The end-to-end harness (`lob_e2e_bench`) is a standalone binary, not a Google
  Benchmark target — per-op-type percentile tables over a four-thread pipeline
  don't fit GB's per-iteration timing model. Results live in `docs/results.md`;
  optimization receipts, kept *and* rejected, in `docs/optlog.md`.

## 12. Roadmap (post-v1, README material)

epoll TCP gateway with a simple binary protocol → multi-symbol sharding (symbol-hash
to engine threads) → book snapshot/recovery → ITCH 5.0 replay support.
Each maps to a system-design interview answer: "here's how I'd scale it."
