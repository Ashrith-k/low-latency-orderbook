# PROGRESS.md — build log

Status as of **2026-07-17**: **Days 1–3 complete** (Day 1: 13/13 tasks,
`0e15777..1ab59da`; Day 2: 9/9 tasks, `8603dbf..8b2f59a`; Day 3: 8/8 tasks,
`864a12c..1c97a09`; one commit per task).
CI green: {gcc-13, clang-17} × {debug, release}, ASan+UBSan, format gate.
Test suite: 174 tests in debug/asan (170 in release: 5 death tests compile out
under NDEBUG, 1 release-only variant compiles in), including two 1M-op
differential runs — add/cancel and full matching.

## What is implemented and tested so far

**Infrastructure (Day 1, tasks 1–9)**

- Repo scaffold: `.gitignore`, MIT `LICENSE`, README stub with the one-paragraph pitch.
- CMake: C++20, no extensions; warnings-as-errors (`-Wall -Wextra -Wpedantic -Werror`)
  carried on a `lob_warnings` INTERFACE target so vendored deps are exempt; `lob` is a
  header-only INTERFACE library; tests/bench gated behind `PROJECT_IS_TOP_LEVEL`.
- Presets: `debug` / `asan` (ASan+UBSan, halt-on-error) / `tsan` / `release` (portable
  `-O3`; `-march=native` variant deferred to Day 6).
- Formatting: `.clang-format` (Google style, 100 col), `scripts/format.sh` (in-place or
  `--check` CI mode). clang-tidy config: explicit `bugprone-* performance-* modernize-*`.
- GoogleTest v1.15.2 and Google Benchmark v1.9.1 via FetchContent, pinned, includes
  marked SYSTEM so they build despite our `-Werror`; smoke test + dummy bench prove the wiring.
- CI: build+test matrix, asan+ubsan job (clang), format gate.

**Reference book (Day 1, tasks 10–13)**

- `include/lob/types.h`: `PriceTicks` (int64 fixed-point ticks), `Qty`, `OrderId`;
  stable, explicitly-encoded enums (`Side`, `OrderType`, `CommandType`, `EventType`,
  `RejectReason` — future wire/log format); `Command` (24 B) and `Event` (32 B) PODs with
  explicit padding, static_asserts on size/alignment/trivial-copyability/
  `has_unique_object_representations` (makes memcmp-based test comparison legal);
  `to_cstr` diagnostic helpers. Covered by `types_test.cc`.
- `include/lob/naive_book.h`: `NaiveBook`, the reference book (`std::map` levels +
  `std::list` FIFO + id→iterator index). `add_limit`/`cancel` and full matching `add()`:
  price-time priority, fills at the resting order's price, maker-then-taker `Traded`
  pairs, limit remainder rests, IOC/market remainder canceled, market ignores price
  (events carry 0), validation rejects (qty/price), unknown-id and double-cancel rejects.
- `tests/naive_book_test.cc`: 29 tests + 1 death test asserting exact event-stream
  bytes — the executable spec the optimized `OrderBook` must reproduce.

**Optimized book structures (Day 2, tasks 1–9)**

- `include/lob/order_pool.h`: 64-byte cache-line `Order` POD (`alignas(64)`, full
  DESIGN §4.1 field set + `generation`, zero internal padding, every offset
  static_asserted, memcmp-safe) and `OrderPool` — fixed-capacity slab, free list
  threaded through `next_idx`, LIFO reuse, O(1) noexcept alloc/free/find. Ids encode
  `generation << 32 | index`; generations start at 1 and skip 0 on wrap, so
  `kInvalidOrderId` is never minted and cancel lookup is a bounds check plus one
  64-bit compare — no hash map on the hot path. `find()` explicitly rejects
  `kInvalidOrderId` (the one value a free slot's stored id can't be distinguished
  from — caught by the tests on first run). 16 tests incl. generation-wrap and
  10k-cycle churn.
- `include/lob/price_ladder.h`: `PriceLevel` (24 B POD; intrusive doubly-linked FIFO
  of pool indices; `order_count`; `total_qty`) and `PriceLadder` — per-side contiguous
  level array over `[anchor − radius, anchor + radius]` (default 4096), best-price
  cursor that scans toward worse prices when the best level empties, plus the
  out-of-band `std::map` overflow region (entries always non-empty, erased eagerly;
  best combines band cursor and map extremum behind one usually-true branch).
  `push_order`/`remove_order` wrap the level ops so cursor and contents cannot drift;
  the ladder owns `Order::level_idx` (band offset, or `kOverflowIdx`). 33 tests
  across level + ladder + overflow.
- `include/lob/order_book.h`: `OrderBook` facade — pool + bid/ask ladders.
  `add_limit` (resting only; matching is Day 3) validates like `NaiveBook`, mints the
  id, rests the order; `cancel` is O(1) end to end off the id itself. Query surface
  mirrors `NaiveBook` name-for-name for the harness. 19 tests incl. the ABA case
  (stale id vs. recycled slot) and crossed-prices-rest semantics.
- `tests/differential_add_cancel_test.cc`: the differential harness (DESIGN §10.2).
  One seeded stream drives both books (OrderBook mints ids, NaiveBook consumes them);
  asserts return values, best bid/ask, `open_orders`, and touched-level qty/count
  after every op, with periodic full sweeps of every price ever used. Op mix: ~5%
  invalid adds, stale-id cancels from a graveyard, forged ids, band-edge walk + 1%
  out-of-band excursions. Failures print `seed/step/op`; `LOB_DIFF_SEED` replays
  deterministically. Four seeds × 25k ops in CI plus a 1M-op convergence run:
  **zero divergences found**, ASan/UBSan clean.

**Matching engine (Day 3, tasks 1–8)**

- `OrderBook::add` (order_book.h): the full matching entry point. Price-time
  priority sweep in a private `Match` helper — best opposing level first
  (`best_price`/`best_level`), FIFO within a level via head fills, every fill
  at the resting order's price. Fill bookkeeping is asymmetric by design: a
  fully filled maker is `remove_order`ed *before* its `remaining` is touched
  (unlink debits `total_qty` by exactly that remaining), then its slot is
  recycled; a partially filled maker keeps its FIFO slot with `remaining` and
  `level.total_qty` debited by hand. Limit remainders rest at the limit;
  market/IOC remainders are canceled. A market order is a limit at an
  unbeatable sentinel price (`numeric_limits<PriceTicks>::max()/min()`), so
  the sweep has no market branch — `Crosses` only compares, never does
  arithmetic on the limit. Validation matches `NaiveBook` exactly: qty first,
  then price for non-market; market ignores its price argument. The taker
  slot is allocated *before* the sweep (a never-rests taker still needs an
  id), so a full pool rejects up front.
- Events (Day 3 task 3): `add`/`cancel` take a templated `EventSink` (any
  callable over `const Event&`; a template, not `std::function` — no
  allocation, inlinable); the event-less overloads remain for structural
  callers. Emission is byte-identical to `NaiveBook`: Accepted before fills
  (market events carry price 0), maker-then-taker `Traded` pairs at the
  maker's price with post-fill remainings, `Canceled` for market/IOC
  remainders and cancels, `Rejected` with reasons. Shared spec-table test
  helpers extracted to `tests/event_test_util.h`.
- Invariants (Day 3 task 4): `PriceLadder::check_invariants(pool)` — full
  walk verifying FIFO link symmetry, per-order sanity (live id naming its own
  slot, side/price/level_idx, `1 <= remaining <= qty`), level aggregates
  against the walk, best-cursor correctness, overflow entries non-empty and
  out-of-band — returning a census that `OrderBook::check_invariants()`
  cross-checks against `pool.live_count()`, plus an uncrossed assert. Bodies
  are `#ifndef NDEBUG`; hot path keeps only cheap per-op asserts.
- Differential matching harness (`tests/differential_matching_test.cc`,
  task 5): the Day-2 harness pattern upgraded to the real engine path —
  limit/IOC/market/cancel streams with byte-exact event-stream comparison,
  `contains()` parity, state at the op price plus both post-op bests, and the
  full invariant walk at sweep cadence. 4 seeds × 25k in CI plus a 1M-op
  scale run; `LOB_DIFF_MATCH_SEED` replays deterministically. **Zero
  divergences found on first convergence run**, ASan/UBSan clean.
- Edge cases (`tests/edge_cases_test.cc`, task 6): sweep-through-band into
  overflow in both directions with event-order assertions across the seam,
  taker remainders resting out-of-band, near-side overflow as best price,
  market draining band+overflow to a truly empty side, band-edge offsets 0
  and 2r, ABA slot reuse after fill-freed slots, market-order pool
  exhaustion, IOC overflow sweeps.
- `MatchingEngine` facade (`include/lob/matching_engine.h`, task 7):
  `EngineConfig{anchor_price, band_radius, pool_capacity}`;
  `process(Command, sink)` dispatches kNew → `add` (engine mints the id,
  `cmd.order_id` ignored) and kCancel → `cancel` (only `order_id` read);
  unknown kinds debug-assert and are release no-ops. `submit`/`cancel`
  direct-call convenience per DESIGN §7; `book()` exposes queries.
- Zero-allocation test (`tests/zero_alloc_test.cc`, task 8): a complete
  counting replacement of global operator new (plain/array × unaligned/
  aligned × throwing/nothrow + matching deletes) in its own binary
  (`lob_zero_alloc_test`). After construction and a 10k-op warmup, a 100k-op
  in-band limit/IOC/market/cancel window makes **zero** operator-new calls —
  verified under ASan too. Companion tests prove the counter is actually
  wired (including the aligned path the 64-byte `Order` vector uses) and
  that an out-of-band rest *does* allocate: the DESIGN-blessed overflow
  exception, pinned as the boundary of the guarantee.

## Deviations from DESIGN.md and why

Day 1 (unchanged):

- **`NaiveBook::add_limit` (rest-without-matching primitive) is not in DESIGN.md.**
  Added so the Day-2 differential harness can compare add/cancel book structure against
  `OrderBook` before matching exists (Day 3). `add()` is the real entry point.
- **Market-order events carry price 0** and the price argument is ignored entirely.
  DESIGN.md §5 doesn't pin this down; echoing a meaningless caller price into the event
  stream would make byte-exact differential comparison fragile.
- **Cancel-reject side is left zero (`kBuy`).** The side of an unknown id is unknowable;
  fixing it to zero keeps `Event` deterministic for memcmp comparison.
- **`RejectReason` enum + `reason` field on `Event`** extend DESIGN.md §5's four-event
  list, needed to express validation failures.

Day 2:

- **`Order` carries an explicit `generation` field** (not in DESIGN §4.1's field list).
  `free()` zeroes `order_id` as the liveness signal, so the last generation must
  survive elsewhere for the next alloc to bump. 44 data bytes + explicit tail pad
  still fit the 64-byte line with room to spare.
- **`PriceLevel::total_qty` is u64** (DESIGN doesn't pin the width): a level of u32
  remainings can legitimately exceed 32 bits. It sums `remaining`, matching
  `NaiveBook::qty_at` semantics.
- **`kOverflowIdx = -2` sentinel** in `Order::level_idx` marks overflow-map residency;
  overflow removals look the level up by the order's own price (O(log n), rare path).
- **`PriceLadder::push_order` is noexcept despite the overflow-map insert allocating.**
  DESIGN blesses the rare-path allocation; if `bad_alloc` ever fires there, terminating
  beats unwinding the hot path.
- **`OrderBook::add_limit` returns `OrderId` instead of emitting events** — event
  emission is deliberately deferred to Day 3 task 3 per PLAN.md; rejects are signaled
  by `kInvalidOrderId` for now. Crossing prices rest (both books), which is exactly
  what the add/cancel harness needs pre-matching.
- **Task 9 found nothing to fix.** The commit keeps PLAN.md's `fix: converge books`
  message for traceability, but the 1M-op run passed on first attempt; the only
  changes were harness scalability fixes (O(1) swap-remove, scaled sweep cadence).
- **Harness pool capacity = op count**, so pool exhaustion never occurs in
  differential runs — `NaiveBook` has no pool to mirror it against. Exhaustion stays
  covered by unit tests.

Day 3:

- **No stored `on_event` callback on `MatchingEngine`** (DESIGN §7 sketches
  one). A `std::function` member costs an allocation and an indirect call on
  the hot path; events go to a per-call templated sink instead, and the Day-4
  ring loop will pass the ring-push sink. The "drain the event ring manually"
  half of §7 is unaffected.
- **DESIGN §5's "invariants checked after every op" is implemented as a
  split**: cheap O(1) per-op asserts (uncrossed after `add`, fill progress in
  the sweep, the level ops' own membership/aggregate asserts) plus the full
  O(band + orders) `check_invariants()` walk on demand — tests call it per
  scenario, harnesses at sweep cadence. A full walk per op would make the
  1M-op differential quadratic.
- **The uncrossed assert lives in `add()` and `check_invariants()` only** —
  deliberately not in `cancel()`/`add_limit`: `add_limit` crosses by design
  (the Day-2 structural harness cancels on crossed books) and a cancel
  cannot cross an uncrossed book.
- **Alloc-before-sweep exhaustion semantics**: a full pool rejects even an
  order that would have fully filled without resting, because the id is
  minted before matching. Pinned by tests; the alternative (match first,
  alloc only to rest) cannot return an id for fully-filled takers.
- **Add-rejects carry `order_id = kInvalidOrderId`** (already anticipated by
  types.h) while `NaiveBook` echoes its caller-supplied id — the matching
  harness normalizes the naive side only. The `kPoolExhausted` reject event
  has no `NaiveBook` counterpart and is shaped by analogy with the
  validation rejects (raw qty/price echoed).
- **`add_limit` stays event-less**: it is the Day-2 rest-without-matching
  primitive kept for the structural harness; `add()` is the event-emitting
  entry point.
- **The zero-alloc replacement set must be complete** — found empirically:
  leaving the nothrow operator-new forms to ASan's interceptor while our
  deletes call `free()` trips alloc-dealloc-mismatch (libstdc++ temporary
  buffers under gtest's stable_sort use nothrow new). All eight new forms
  and matching deletes are replaced.

## Known issues / TODOs

- **TSan on this Codespace requires `sudo sysctl vm.mmap_rnd_bits=28` each session**,
  otherwise TSan-instrumented binaries crash at startup (kernel ASLR entropy vs
  clang-17 TSan). **Becomes relevant now: Day 4 task 2 adds the TSan job.**
- Hardware perf counters are blocked in Codespaces; Day 6 `perf stat` numbers must come
  from a bare-metal Linux box (documented limitation, per PLAN.md).
- The debug/asan suite wall time is now dominated by the two 1M-op differential
  runs (~65 s debug, ~130 s asan total). Fine for CI so far; if it grows, gate
  the scale runs behind a ctest label and keep the 4×25k runs as the default.
- clang-tidy is configured but not yet enforced anywhere (no CI job, no script); the
  clean-up pass is Day 7 task 5.
- `bench/dummy_bench.cc` is placeholder wiring; real benchmarks start Day 6.
- README is a stub by design until measured v1.0 numbers exist.
- `NaiveBook` allocates freely and is slow by design — test/bench oracle only, never on
  the hot path.

## Next task

**Day 4, task 1: `SPSCQueue<T>`: single-threaded correctness tests first.
[feat: spsc queue]**

Per DESIGN.md §4.4: lock-free single-producer/single-consumer ring —
power-of-two capacity, cached head/tail, acquire/release atomics,
`alignas(64)` padding against false sharing, batched pop for the engine loop.
Day 4 task 1 is the single-threaded correctness layer (push/pop/full/empty/
wraparound); the two-thread stress test and TSan CI job are task 2 — run the
`vm.mmap_rnd_bits` sysctl above before touching the tsan preset. Memory-ordering
rationale must be documented at every atomic (CLAUDE.md); expect interviewers
to drill exactly here. `Command` and `Event` are already memcpy-safe PODs
(static_asserted in types.h), sized 24 B and 32 B, for ring transport.
