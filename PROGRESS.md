# PROGRESS.md — build log

Status as of **2026-07-19**: **all 7 days complete — v1.0.0 shipped** (Day 1:
13/13 tasks, `0e15777..1ab59da`; Day 2: 9/9 tasks, `8603dbf..8b2f59a`; Day 3:
8/8 tasks, `864a12c..1c97a09`; Day 4: 7/7 tasks, `6335fd5..3135d20`; Day 5:
7/7 tasks, `a4b1b03..ab44e14`; Day 6: 7/7 tasks, `19a5f62..5fb21ee`; Day 7:
7/7 tasks, `dd0a41e..4ca737f` — tasks 1–5 one commit each; task 6's artifact
is the annotated tag `v1.0.0` on `4ca737f` plus repo description/topics; task 7
is out-of-repo by design).
CI green: {gcc-13, clang-17} × {debug, release}, ASan+UBSan, TSan
(concurrency label), clang-tidy gate, format gate.
Test suite: 290 tests in debug/asan (285 in release: 6 death tests compile out
under NDEBUG, 1 release-only variant compiles in), including two 1M-op
differential runs — add/cancel and full matching — and 34 concurrency-labeled
threaded tests across four binaries that the TSan job runs. Two env-gated
replay hooks (`LOB_DIFF_SEED`/`LOB_DIFF_MATCH_SEED`) skip unless a seed is
exported.

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

**Observability + workload tooling (Day 5, tasks 1–7)**

- `include/lob/async_logger.h` (tasks 1–2): `LogRecord`, the 32-byte binary
  log record (DESIGN §6: u64 timestamp, order id, price, qty,
  kind/side/reason + explicit pad; every offset static_asserted,
  unique-object-representations so on-disk bytes are the in-memory
  representation), `make_log_record(ts, Event)` as the one blessed mapping,
  and `AsyncLogger`: ctor allocates the record ring (default 16384 × 32 B)
  and spawns the consumer thread; the thread batch-pops 256 records, writes
  raw bytes to a caller-supplied `std::ostream`, flushes when idle and at
  shutdown. `stop()` reuses the run() release/acquire stop-then-drain
  contract verbatim — every accepted record reaches the stream; the
  destructor stops if the caller didn't. Backpressure is split into two
  APIs: `log()` is the production policy — count-and-drop, returns void (a
  caller reacting to a drop would be a retry policy), bumps an
  `alignas(64)` relaxed-atomic drop counter readable live from any thread —
  while `try_log()` is the raw non-counting primitive for callers with
  their own policy, so "dropped" always means a record was actually lost.
  17 tests in the concurrency-labeled `lob_async_logger_test` binary,
  including a 100k FIFO byte-exact round trip, a 200k three-thread stress,
  and a `GatedStringbuf` that parks the logger thread inside `write()` so a
  provably full ring demonstrates count-and-drop deterministically.
- `include/lob/workload_gen.h` (task 3): `WorkloadGenerator` — deterministic
  Command streams: weighted op mix (limit/market/IOC/cancel), qty range,
  clamped random-walk mid ± Zipf-ish offset (integer harmonic weights,
  precomputed u64 CDF + binary search), 1-in-N aggressive orders through
  the mid so streams produce real fills. Cancels come from a private
  *shadow MatchingEngine*: every emitted command is applied to it and its
  events maintain the live-id set cancels draw from — engine determinism
  then guarantees a fresh engine with the same EngineConfig mints identical
  ids, so every cancel in a pregenerated stream is valid at replay time.
  Centerpiece test: 100k-command replay with zero unknown/invalid/exhausted
  rejects and shadow-book parity.
- `include/lob/replay_format.h` (task 4): 96-byte versioned `ReplayHeader`
  (`"LOBRPLY\0"`, u32 version = 1, u64 count, full WorkloadConfig verbatim)
  + packed 24-byte Commands. Strict v1 reader with typed errors
  (`ReplayReadError` + `to_cstr`): bad magic / unsupported version /
  truncated header / truncated body / trailing bytes — never a partial
  load; the body is read in 4096-command chunks so a corrupt count degrades
  to `kTruncatedBody`, not a giant allocation. Golden-bytes test pins the
  format at raw offsets; round-trip test replays the file through an engine
  built from its own header.
- `tools/` (task 5): `lob_replay` CLI. All logic lives in
  `tools/lob_replay_lib.h` behind `run_cli(args, out, err) → exit code`
  (0 ok / 1 runtime / 2 usage) so 13 gtest cases drive the real code
  paths; `lob_replay.cc` is a three-line main; a ctest smoke runs the
  binary. `generate --out FILE [--ops/--seed/--anchor/--band/--pool]`
  wraps the workload generator; `run FILE` replays with a stats summary
  (wall-clock throughput explicitly labeled indicative). The CLI validates
  config *semantics* (anchor > 0, band ≤ 2^20, pool ≤ 2^24) before
  constructing an engine — the format reader deliberately doesn't.
- `include/lob/latency.h` (task 6): `rdtsc_now()` (plain rdtsc, deliberately
  unfenced — rationale in-header; steady_clock fallback off x86),
  `TscCalibration` (spin-measured ticks/second vs steady_clock, ≤1 s window
  caps the tick product; degenerate readings fall back to identity 1 GHz;
  `ticks_to_nanos` uses exact overflow-safe split math), and
  `LatencyRecorder`: 65 counters indexed by `bit_width(ticks)`, O(1)
  allocation-free lock-free record, integer-only ceil-rank percentiles
  (`percentile_ticks(999, 1000)`) with in-bucket linear interpolation
  clamped to observed [min, max] — degenerate distributions exact, p100 ≡
  max, uniform data within a few percent (pinned by tests). `merge()` for
  per-thread recorders, `reset()` for warmup discard, `LatencySummary` POD
  (56 B) with `to_nanos`.
- `include/lob/stats.h` + engine wiring (task 7): `EngineStats` — DESIGN §6
  counters as plain u64s (memcmp-legal), engine-thread-owned, read at
  quiescence. `submit`/`cancel` are the counting choke points (they wrap
  the caller's sink); `process()` is pure dispatch to them, so ring and
  direct paths count identically by construction (also proven by memcmp
  test). Reconciliation identities pinned: `events == accepted + rejected +
  traded + canceled`, `news == accepted + invalid + exhausted`.
  `SPSCQueue::try_push` now maintains the DESIGN §6 ring occupancy
  high-water mark on the producer line (relaxed atomic; exact whenever the
  ring truly fills since the full check refreshes first, otherwise an upper
  bound from the producer's cached view — contract documented and tested);
  `AsyncLogger::ring_high_water()` exposes its ring's mark. The replay CLI
  dropped its ad-hoc counters and prints from `engine.stats()`.

**Benchmarks and tuning (Day 6, tasks 1–7)**

All numbers below: release preset (gcc 13.3 `-O3`), AMD EPYC 7763 Codespace
VM (2 vCPUs = SMT siblings of one physical core, unpinned), mean of
repetitions — indicative per DESIGN §11; `docs/results.md` is the citable
table.

- `bench/book_micro_bench.cc` (task 1): BookAddRest (fill an empty book) and
  BookSteadyAddCancel (64Ki-op 50/50 churn at constant depth), NaiveBook vs
  OrderBook over depths 1024–65536 on identical pregenerated scripts.
  Cancels name victims by *position* in the live-order set under a shared
  swap-remove discipline, so both books execute structurally identical
  sequences even though they mint different ids; every cancel hits the real
  removal path. Untimed per-iteration setup via `optional::emplace` +
  `PauseTiming`. Retired `bench/dummy_bench.cc`. Book wins 10.5–14.5×
  (add/rest) and 5.2–6.0× (steady churn).
- `bench/matching_bench.cc` (task 2) + `bench/script_rng.h` (shared
  splitmix64 + harmonic-CDF sampling, extracted from task 1): MatchSteady —
  maker/taker pairs at the mid over an untouched Zipf-shaped background
  book, self-restoring so the timed loop needs no PauseTiming (book 3.5–3.8×
  naive); MatchSweep — K makers one-per-level + one IOC sweeping all K,
  alternating sides, two permanent floor orders keeping the bench on
  per-level sweep cost (book 4.4–6.3×, gap widening with K: map
  erase+rebalance per exhausted level vs cursor step). Found and recorded
  the **empty-side pathology**: draining a side truly empty cost an
  O(band_radius) cursor rescan (~0.9 µs) — fixed in task 5.
- `bench/e2e_bench_lib.h` + `bench/lob_e2e_bench.cc` (task 3): the DESIGN §3
  pipeline assembled for real — replay-file producer → command ring (64Ki) →
  pinned engine thread → AsyncLogger ring → logger thread (null-device sink
  by default, `--log` for a file). Per-command rdtsc pairs recorded into
  four per-op-type LatencyRecorders on the engine thread; `--warmup N`
  discards via reset at the boundary (default 10%); log records reuse the
  command's admission timestamp (one rdtsc per command); `--csv` feeds the
  plot tool. The engine loop mirrors `run()`'s batch/shutdown contract
  bench-locally (run() deliberately has no per-command hook), and commands
  are pre-validated before classification (the reader validates format
  only). 15 tests drive `run_e2e_cli()` in a fourth concurrency-labeled
  binary (counts reconcile exactly with `WorkloadCounts`), plus a binary
  smoke. Result: **7.5M cmd/s through the four-thread pipeline; p50 41 ns,
  p99 201 ns, p99.9 380 ns** (11M-command file, 10M measured).
- `scripts/perf_stat.sh` (task 4): perf stat wrapper with the §11 counter
  set + software counters, `-r N` mean±stddev, `--check` probe, and a
  degradation ladder (install hint → paranoid hint → PMU-hidden note) in the
  affinity.h spirit; shellcheck-clean, every path exercised. Empirical
  finding recorded in the script header: **the Azure VM hides the PMU even
  from root** — hardware rows all `<not supported>`; software counters
  measured the pipeline's ~61K context switches/s vs ~300/s for the
  single-threaded microbenches (direct evidence for the e2e tail maxima).
- Targeted optimizations (task 5, `docs/optlog.md` has full before/after):
  profiled via `perf record -e cpu-clock` (software event; ~85% of samples
  inline into book code). **Kept:** (1) O(1) empty-side transition —
  `PriceLadder` counts in-band orders and drops the cursor directly when
  the count hits zero instead of the O(band) rescan; `MatchSweepDrain`
  (floor-less variant, added as permanent regression guard) went from
  **0.94M cmd/s (23× slower than the map) to 88.5M (4.6× faster)** at K=1;
  invariant walk reconciles the counter. (2) One `best_price()` per fill in
  `Match` via new `PriceLadder::level_at(price)` (+19–31% on sweeps).
  **Rejected with numbers:** `[[likely]]`/`[[unlikely]]` on
  validation/overflow branches — consistent +5–8% on one scenario and
  −8–9% on another (layout roulette, the branches predict perfectly at
  runtime anyway); reverted, null result logged.
- `tools/plot_bench.py` (task 6): `latency` (e2e CSV → percentile chart)
  and `compare` (Google Benchmark JSON → auto-paired naive-vs-book bars
  with ratio labels) subcommands; byte-deterministic SVG output (fixed
  hashsalt, no date) so regenerated charts diff cleanly; exit codes 0/1/2.
  `docs/img/latency_percentiles.svg` + `docs/img/naive_vs_book.svg`
  rendered from fresh post-opt runs.
- `docs/results.md` (task 7): the README-ready results section — headline
  ratios, full depth/breadth scaling tables (recomputed from one
  self-consistent run), e2e percentile table, honest-tail narrative, and a
  reproduce-commands block.

**Polish and ship (Day 7, tasks 1–7)**

- README (task 1): full engineering-blog rewrite per PLAN's outline — badge +
  latency chart up top, five design decisions in problem → options → choice →
  measured-result form (the empty-side pathology as the featured story),
  results inlined verbatim from `docs/results.md` with the environment caveat
  attached, correctness section, build/run, limitations, roadmap. Full scaling
  tables stay in `docs/results.md` (linked) to keep the README readable.
- DESIGN.md finalized (task 2): every as-built deviation recorded below folded
  into the owning section (+110/−48) — measured status in §1, `generation`/id
  scheme/empty-side counter/monotonic ring indices in §4, RejectReason +
  determinism rules + invariant split in §5, logger/latency/stats contracts in
  §6, the real per-call-sink API and expanded header list in §7, actual tree in
  §8, pinned dep versions and as-built test statuses in §9–11. Implementation
  minutiae (sentinel values, TSan link collision, elision fix) deliberately
  stay here, not in DESIGN.
- API doc-comments pass (task 3): audit found ~95% coverage already; +15
  comment-only lines fill the gaps where a contract fact wasn't recoverable
  from the signature (OrderId encoding pointer, contains() staleness,
  live_count() quiescence identity, ReplayReadResult validity, quiescence
  caveats, reset() purpose). Self-describing getters left uncommented.
- Dockerfile + .dockerignore (task 4): ubuntu:24.04, explicit gcc-13/g++-13
  (default CC/CXX — the toolchain the published numbers cite) + clang-17 for
  the sanitizer presets. `docker build` IS the verification: release configure
  + build, full 285-test release suite, then a 10k-command replay through
  lob_e2e_bench (PLAN's CI-skeleton bench-smoke, realized here); default CMD
  reruns a 100k-command bench with percentile output. Verified end to end
  locally (zero cached layers; in-container run consistent with results.md).
- clang-tidy clean pass (task 5): survey over all 31 first-party TUs found 62
  unique findings across 5 checks, all resolved with fixes — zero NOLINTs,
  zero checks disabled. Notables: `Order::pad0` → `std::array` (all 64-byte
  layout asserts unchanged), int-typed static_assert multiplication in
  spsc_queue.h, a test loop retyped to OrderId dissolving three
  misplaced-widening casts, C arrays → std::array in six test files, two
  missing reserve()s, and [[nodiscard]] auto-applied to 41 const queries in
  the public headers (+1 bench helper) — zero call sites broke under -Werror.
  Enforcement now exists: `scripts/tidy.sh` (format.sh ergonomics, --fix mode,
  -warnings-as-errors='*' gate, shellcheck-clean) + a configure-only CI job.
  Verified: tidy gate clean, debug + ASan 290/290, TSan concurrency 34/34,
  clang-17 build 290/290.
- Release (task 6): pushed Day-7 commits, watched all 8 CI jobs green on
  `4ca737f` (first CI run of the tidy gate included), then tagged annotated
  `v1.0.0` on that exact SHA — the tag message carries the headline numbers
  with the VM caveat. Badge (in README since task 1) now renders green. Repo
  description + 7 topics set by Ashrith via the web UI — the Codespace
  GITHUB_TOKEN can push contents but 403s on repo administration.
- RESUME notes (task 7): delivered as a personal file, not committed — PLAN
  gives task 7 no commit message and it is interview prep, not repo content.
  Pitch, quantified bullets, the numbers block for the resume-writing session,
  ten anticipated interview questions with answers, four weak spots to own
  proactively.

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
- **The counter-wiring probe calls `::operator new` directly instead of using
  a new-expression** — found on CI's first clang×release run (that cell never
  executed anywhere until Days 2–4 were pushed together): [expr.new]/10
  permits eliding replaceable allocation functions for new-expressions, and
  clang -O3 removed the probe's `new int`/`delete` pair outright, so the
  counter never moved and the wiring test failed. A direct call is an
  ordinary function call and must execute on every compiler at every
  optimization level.

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

Day 5:

- **`AsyncLogger`'s sink is `std::ostream&`** (DESIGN §6 only says the logger
  thread "formats/flushes"): virtual dispatch runs on the logger thread, off
  the latency path, and ostringstream makes byte-exact tests trivial while
  ofstream covers real files.
- **`LogRecord.timestamp` is a caller-supplied u64** — the logger is
  clock-agnostic; rdtsc and calibration belong to `LatencyRecorder`, and the
  engine wires them together later. `remaining` is not in the record:
  DESIGN §6's field list (timestamp, type, ids, price, qty) omits it and
  32 bytes has no spare room; a fill's two Traded records carry both ids.
- **`log()` vs `try_log()` split**: count-and-drop lives in `log()` only, so
  the drop counter never counts failed attempts a retrying caller (tests'
  lossless spins) later satisfied.
- **The workload generator core lives in `include/lob/workload_gen.h`, not
  `tools/`** (DESIGN §8): tests and Day-6 benches drive it as a library;
  the tools/ CLI wrapper is the task-5 `lob_replay generate` subcommand.
- **Randomness is in-house** (splitmix64 + integer harmonic-CDF Zipf +
  modulo bounding, bias documented as negligible) instead of `<random>`
  distributions, whose output is stdlib-specific: replay files must be
  byte-identical no matter which toolchain generated them.
- **Cancel synthesis runs a shadow engine inside the generator** — the only
  way a pregenerated stream can carry concrete ids that are valid at replay
  time (ids are minted deterministically from EngineConfig). Consequence:
  a replay is only faithful against the config it was generated with, so
  **`WorkloadConfig` became wire format** (explicit tail pad, 72 B, frozen
  asserted offsets, EngineConfig's 16 packed bytes riding along) and the
  replay header embeds it verbatim.
- **`replay_format.h`, `stats.h`, and `workload_gen.h` extend DESIGN §7's
  header list** (tool/observability tier, same spirit as the Day-4
  `affinity.h` addition).
- **Replay reader validates format only; the CLI validates semantics**
  (anchor > 0, band/pool caps) before constructing an engine — a
  corrupt-but-well-formed file must not become a huge allocation or
  release-mode unchecked-assert UB. Split pinned by tests on both layers.
- **`rdtsc_now()` is deliberately unfenced** — few-ns out-of-order skew is
  noise at op granularity and a serializing fence would perturb the
  pipeline being measured; revisit on Day 6 only if the numbers demand it.
- **Percentiles are integer-only** (num/den ceil-rank + in-bucket linear
  interpolation clamped to observed min/max): keeps the "no floating point"
  discipline; precision contract (worst case one log2-bucket span) stated
  in the header instead of pretending exactness.
- **Ring high-water marks live on `SPSCQueue` itself** (DESIGN §6 lists them
  with the counters; task 7): producer-line relaxed atomic, one relaxed
  load + compare per push, no new shared cache lines. The mark is an upper
  bound (producer's cached view) and exact whenever the ring truly fills —
  over-claiming exactness would be dishonest, so the contract is documented.
- **Engine stats are plain non-atomic u64s** read at quiescence (the
  records_written_ convention) — live cross-thread sampling would need
  atomics the hot path shouldn't pay for; revisit if a live dashboard ever
  materializes. The event-less `submit`/`cancel` overloads now route
  through the counting sink wrapper, so both entry paths count identically.

Day 6:

- **Bench scripts don't reuse `WorkloadGenerator`** (tasks 1–2): its cancel
  synthesis runs a full-matching shadow engine, so its ids are only valid on
  a full-matching replay — useless for the structural add/cancel bench and
  an unwanted coupling for the matching bench. Bench-local scripts
  (`bench/script_rng.h`, positional cancel victims) drive both books
  identically instead; the generator still owns the e2e workload.
- **The e2e bench is a standalone harness, not a Google Benchmark target**:
  per-op-type percentile tables over a four-thread pipeline don't fit GB's
  per-iteration timing model. It follows the lob_replay thin-main pattern
  (logic in `bench/e2e_bench_lib.h`, tested via `run_e2e_cli()`), reuses
  the replay CLI's parse/validate helpers, and mirrors `run()`'s
  batch-pop/shutdown contract bench-locally because run() deliberately has
  no per-command hook (per-command rdtsc must bracket `process()`).
- **The e2e harness pre-validates command enums** before the pipeline
  starts: the replay reader validates format, not semantics, and
  `classify()` indexes an array by kind/type while the engine only
  debug-asserts — a corrupt-but-well-formed file must be refused, not UB.
- **MatchSweep keeps two permanent floor orders; MatchSweepDrain exists
  because it doesn't.** The floors keep the sweep bench measuring per-level
  cost; the drain variant pins the empty-side transition and stays
  registered so the fixed pathology can never silently return.
- **Branch hints were tried and rejected on measurement** (optlog entry 3):
  the honest half of "apply measured optimizations" is reverting the ones
  that don't measure. Prefetch-on-level-walk wasn't attempted — the bench
  levels hold ~1 order, so it would measure nothing here; left to
  bare-metal with real multi-order levels.
- **rdtsc stays unfenced in the e2e per-op timing** (the Day-5 decision
  revisited on schedule): at 40–400 ns per op the few-ns skew is noise, and
  the numbers never demanded the fence.

## Known issues / TODOs

- **TSan on this Codespace requires `sudo sysctl vm.mmap_rnd_bits=28` each session**,
  otherwise TSan-instrumented binaries crash at startup (kernel ASLR entropy vs
  clang-17 TSan). The CI tsan job runs the same sysctl as a step.
- **Every Day-6 number is a Codespaces number** — one bare-metal session
  would upgrade the lot: IPC/cache/branch counters (the Azure VM hides the
  PMU even from root; `scripts/perf_stat.sh` is ready), the false-sharing
  cross-core deltas (SMT-siblings caveat from Day 4), the e2e tail maxima
  (millisecond scheduler preemptions with four threads on 2 vCPUs), and a
  `-march=native` release variant (deferred from Day 1, only worth measuring
  on the machine it targets). `docs/results.md` carries the caveat inline.
- The debug/asan suite wall time is still dominated by the two 1M-op
  differential runs. Fine for CI so far; if it grows, gate the scale runs
  behind a ctest label and keep the 4×25k runs as the default.
- The `lob_replay run` throughput line stays wall-clock and labeled
  indicative — `lob_e2e_bench` owns the real methodology now (pinning,
  warmup discard, per-op-type percentiles).
- `NaiveBook` allocates freely and is slow by design — test/bench oracle only, never on
  the hot path.

## Next steps (post-v1.0.0)

The 7-day plan is complete: v1.0.0 is tagged on `4ca737f` with every CI gate
green. In priority order:

1. **Bare-metal rerun** — one session upgrades every number at once: PMU
   counters (`scripts/perf_stat.sh` is ready), cross-core false-sharing
   deltas, real tail maxima on isolated cores, and a `-march=native` release
   variant. The harnesses need no code changes; replace the tables in
   `docs/results.md` and the README wholesale, per the honesty rules.
2. **Roadmap (DESIGN §12)**: epoll TCP gateway with a binary protocol →
   multi-symbol sharding (symbol hash → engine threads) → book
   snapshot/recovery → ITCH 5.0 replay support.
