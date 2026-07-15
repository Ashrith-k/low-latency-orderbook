# PROGRESS.md — build log

Status as of **2026-07-15**: **Day 1 and Day 2 complete** (Day 1: 13/13 tasks,
`0e15777..1ab59da`; Day 2: 9/9 tasks, `8603dbf..8b2f59a`; one commit per task).
CI green: {gcc-13, clang-17} × {debug, release}, ASan+UBSan, format gate.
Test suite: 108 tests, including a 1M-op differential run.

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

## Known issues / TODOs

- **TSan on this Codespace requires `sudo sysctl vm.mmap_rnd_bits=28` each session**,
  otherwise TSan-instrumented binaries crash at startup (kernel ASLR entropy vs
  clang-17 TSan). Becomes relevant on Day 4.
- Hardware perf counters are blocked in Codespaces; Day 6 `perf stat` numbers must come
  from a bare-metal Linux box (documented limitation, per PLAN.md).
- The differential harness compares **book state only**; event-stream comparison
  arrives with Day 3 task 5 once `OrderBook` emits events.
- clang-tidy is configured but not yet enforced anywhere (no CI job, no script); the
  clean-up pass is Day 7 task 5.
- `bench/dummy_bench.cc` is placeholder wiring; real benchmarks start Day 6.
- README is a stub by design until measured v1.0 numbers exist.
- `NaiveBook` allocates freely and is slow by design — test/bench oracle only, never on
  the hot path.

## Next task

**Day 3, task 1: Aggressive limit matching: sweep opposing levels, partial fills,
rest remainder. [feat: limit matching]**

Per DESIGN.md §5: price-time priority, aggressive orders sweep opposing levels
best-first, FIFO within a level, fills at the resting order's price. The executable
spec is `tests/naive_book_test.cc` — `OrderBook` matching must reproduce
`NaiveBook::add()` exactly, including the event stream once task 3 lands. The
sweep loop will use `best_level()` + head-order fills + `remove_order` on
exhausted orders; watch the sweep-through-band edge case (Day 3 task 6) and keep
partial-fill `total_qty` maintenance in mind (fills decrement `remaining` without
unlinking, so the level aggregate must be adjusted explicitly).
