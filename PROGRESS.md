# PROGRESS.md — build log

Status as of **2026-07-18**: **Days 1–4 complete** (Day 1: 13/13 tasks,
`0e15777..1ab59da`; Day 2: 9/9 tasks, `8603dbf..8b2f59a`; Day 3: 8/8 tasks,
`864a12c..1c97a09`; Day 4: 7/7 tasks, `6335fd5..3135d20`; one commit per task).
CI green: {gcc-13, clang-17} × {debug, release}, ASan+UBSan, TSan
(concurrency label), format gate.
Test suite: 208 tests in debug/asan (203 in release: 6 death tests compile out
under NDEBUG, 1 release-only variant compiles in), including two 1M-op
differential runs — add/cancel and full matching — and 6 concurrency-labeled
threaded tests that the TSan job runs (~15 s instrumented).

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

**Concurrency (Day 4, tasks 1–7)**

- `include/lob/spsc_queue.h`: `SPSCQueue<T>` — the lock-free SPSC ring
  (DESIGN §4.4). Power-of-two capacity over a startup-allocated
  `std::vector<T>`; monotonic uint64 head/tail masked on access, so all
  `capacity` slots are usable and full/empty are unambiguous. Producer block
  (`tail_` + cached view of `head_`) and consumer block (`head_` + cached view
  of `tail_`) each `alignas(64)`: steady-state ops never write a line the peer
  polls, and the peer's atomic is reloaded only when the cached value says
  full/empty. Owner reads its own index relaxed (single writer), publishing
  stores are release, peer refreshes acquire — rationale commented at every
  atomic. `try_pop_batch` (task 4) amortizes to one acquire refresh + one
  release store per batch, masking per element so batches span the wrap.
  Layout static_asserts pin the three-cache-line structure. 21 single-threaded
  tests (FIFO across ~2.5k wraps, both cache-refresh paths, batch across the
  wrap seam, capacity-1, byte-exact Command/Event transport, debug death test
  on non-power-of-two capacity).
- `tests/spsc_queue_stress_test.cc` (own binary, ctest label `concurrency`):
  four 2-thread stress tests — 1M-op FIFO sequence, capacity-1 boundary churn
  (every op crosses empty↔full), batched-consumer FIFO, and an Event-payload
  test deriving all 32 bytes from the sequence number and memcmp-ing on
  arrival (catches torn/reordered slot writes). Workers record failures into
  relaxed atomics; the main thread asserts after join (join provides the
  happens-before).
- CI `tsan` job (clang-17, tsan preset): runs `ctest -L concurrency` only,
  with the `vm.mmap_rnd_bits=28` sysctl step (the ASLR crash below hits
  GitHub runners too).
- `bench/false_sharing_bench.cc`: 2-thread transfer harness over three
  variants of the identical release/acquire protocol. Medians (release,
  gcc-13, 7×1.5 s, EPYC 7763 Codespace): naive unpadded+uncached 68.0 M/s,
  unpadded+cached 67.3 M/s, **padded+cached 101.7 M/s (+51%)**. Big caveat,
  README-bound: this VM's 2 vCPUs are SMT siblings of one physical core
  (shared L1/L2), which is why index caching measures ≈0 here and why the
  padding delta understates the cross-core effect. Bare-metal rerun on Day 6.
- `MatchingEngine::run(commands, stop, sink)` (task 4): batch-pops into a
  256-command stack buffer (`kEngineBatchSize`, 6 KiB, zero allocation),
  processes FIFO, busy-spins while idle, returns commands processed. Shutdown
  contract: producer sets `stop` (release) only after its final push; the
  loop observes it with acquire and re-polls once more, guaranteeing a
  complete drain. Tested by a scout-vs-loop differential: a deterministic
  1,000-command script through ring+run() must reproduce the directly-driven
  twin's event stream byte for byte (>3 batches + remainder), plus book
  parity and empty-ring/drain cases.
- `include/lob/affinity.h` (task 5): best-effort pinning —
  `pin_thread_to_cpu` / `pin_current_thread_to_cpu` / `current_cpu` over
  `pthread_setaffinity_np`/`sched_getcpu`; non-Linux stubs return false/-1;
  refusals (container cpusets) degrade to "unpinned and documented", never
  abort. Tests are container-aware: targets come from `sched_getaffinity`'s
  allowed set, an RAII restorer un-pins after each test.
- `tests/pipeline_smoke_test.cc` + `tests/pipeline_stress_test.cc` (binary
  `lob_pipeline_test`, label `concurrency`): the full DESIGN §3 pipeline —
  producer → command ring → engine thread (run()) → event ring → drainer.
  Rings deliberately small so both backpressure edges see real contention.
  Because SPSC rings preserve FIFO and the engine is deterministic, thread
  timing may change batching but never order/content: the 3-thread event
  stream must match the single-threaded scout's byte for byte, plus count
  reconciliation (Accepted + validation-rejects == #New, Traded even, rings
  empty, book parity, invariant walk). Smoke: 50k commands, rings 256/512.
  Stress: 250k commands × 3 rounds with asymmetric ring pressure ({64,128},
  {1024,64} event-starved, {64,2048} command-starved), fresh engine/rings per
  round so the stop/drain protocol is exercised three times; 9.6 s under
  TSan. `LOB_PIPELINE_STRESS_OPS` scales ops-per-round for soaks (verified at
  600k/round).
- Shared script builder extracted to `tests/script_test_util.h`
  (`lob::testutil`): a scout engine builds a deterministic mixed
  command script (resting/crossing limits, IOC, market, invalid adds, cancels
  of minted ids incl. dead ones) and defines the expected event stream +
  end state; engine-loop, smoke, and stress tests all replay against it.

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

Day 4:

- **`lob_zero_alloc_test` is excluded from TSan builds** (CMake gates on
  `-fsanitize=thread` in `CMAKE_CXX_FLAGS`): `libclang_rt.tsan_cxx` strongly
  defines every C++ `operator new/delete` the test replaces — a hard link
  collision. The zero-allocation property is single-threaded anyway, and
  TSan's own runtime allocates. Debug/ASan/release still build and run it.
- **The TSan CI job runs only `-L concurrency`**, not the full suite: TSan
  would multiply the two single-threaded 1M-op differential runs into
  minutes while exercising zero synchronization. Every threaded test carries
  the label (enforced by construction: they live in the two labeled
  binaries).
- **`run()` is a `MatchingEngine` member, not a separate `engine_loop.h`** —
  DESIGN §7's header list has no loop header, and the facade's comments
  already anticipated the Day-4 ring loop. matching_engine.h now includes
  spsc_queue.h/<atomic>.
- **Event-ring backpressure in the pipeline tests is a test-local lossless
  spin** (so counts must reconcile exactly). The production policy —
  count-and-drop, never block the engine (DESIGN §6) — belongs to the Day-5
  logger and lands with its drop-counter metric and test.
- **SPSCQueue makes all `capacity` slots usable** (monotonic uint64 indices,
  full = `tail − head == capacity`) rather than the classic masked-index
  N−1 scheme; DESIGN doesn't pin this. Indices cannot wrap in practice
  (2^64 ops ≈ centuries at 1 G ops/s).
- **`affinity.h` is an addition beyond DESIGN §7's header list** (the §11
  pinning requirement needs a home); best-effort bool returns instead of
  exceptions so cpuset-restricted environments degrade gracefully.
- **False-sharing numbers are SMT-sibling numbers.** Recorded as measured,
  with topology documented; the honest cross-core deltas require the Day-6
  bare-metal rerun.

## Known issues / TODOs

- **TSan on this Codespace requires `sudo sysctl vm.mmap_rnd_bits=28` each session**,
  otherwise TSan-instrumented binaries crash at startup (kernel ASLR entropy vs
  clang-17 TSan). The CI tsan job runs the same sysctl as a step.
- Hardware perf counters are blocked in Codespaces; Day 6 `perf stat` numbers must come
  from a bare-metal Linux box (documented limitation, per PLAN.md). The
  false-sharing bench numbers carry the extra SMT-siblings caveat above.
- The debug/asan suite wall time is dominated by the two 1M-op differential
  runs (~65 s debug, ~130 s asan total; Day 4 added only ~5 s). Fine for CI so
  far; if it grows, gate the scale runs behind a ctest label and keep the
  4×25k runs as the default.
- clang-tidy is configured but not yet enforced anywhere (no CI job, no script); the
  clean-up pass is Day 7 task 5.
- `bench/dummy_bench.cc` is placeholder wiring; the false-sharing bench is
  real, book/matching/e2e benches start Day 6.
- README is a stub by design until measured v1.0 numbers exist.
- `NaiveBook` allocates freely and is slow by design — test/bench oracle only, never on
  the hot path.

## Next task

**Day 5, task 1: 32-byte binary log record + `AsyncLogger` over SPSC.
[feat: async logger]**

Per DESIGN.md §6: the hot path writes a 32-byte binary record (timestamp,
event type, ids, price, qty) into an SPSC ring; the logger thread formats and
flushes. The ring, batched pop, and the engine-side sink pattern all exist
now — the logger consumes an `SPSCQueue` of records the way the pipeline
tests' drainer thread does. Static_assert the record layout like Command/
Event (explicit padding, unique object representations). Backpressure
(count-and-drop + drop-counter metric + test) is deliberately task 2 — keep
task 1 to the record format, the ring plumbing, and the formatting/flushing
thread with clean startup/shutdown (reuse the stop/drain contract from
`MatchingEngine::run`). Run the `vm.mmap_rnd_bits` sysctl before any TSan
work; the logger thread tests belong under the `concurrency` label.
