# PROGRESS.md — build log

Status as of **2026-07-13**: **Day 1 complete** (13/13 tasks, one commit per task,
`0e15777..1ab59da`). CI green: {gcc-13, clang-17} × {debug, release}, ASan+UBSan,
format gate.

## What is implemented and tested so far

**Infrastructure (tasks 1–9)**

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

**Library (tasks 10–13)**

- `include/lob/types.h`: `PriceTicks` (int64 fixed-point ticks), `Qty`, `OrderId`;
  stable, explicitly-encoded enums (`Side`, `OrderType`, `CommandType`, `EventType`,
  `RejectReason` — future wire/log format); `Command` (24 B) and `Event` (32 B) PODs with
  explicit padding, static_asserts on size/alignment/trivial-copyability/
  `has_unique_object_representations` (makes memcmp-based test comparison legal);
  `to_cstr` diagnostic helpers. Covered by `types_test.cc`.
- `include/lob/naive_book.h`: `NaiveBook`, the reference book (`std::map` levels +
  `std::list` FIFO + id→iterator index). `add_limit`/`cancel` (task 11) and full
  matching `add()` (task 12): price-time priority, fills at the resting order's price,
  maker-then-taker `Traded` pairs, limit remainder rests, IOC/market remainder canceled,
  market ignores price (events carry 0), validation rejects (qty/price), unknown-id and
  double-cancel rejects.
- `tests/naive_book_test.cc` (task 13): 29 tests + 1 death test asserting exact
  event-stream bytes — the executable spec the optimized `OrderBook` must reproduce.
  Covers FIFO within level, best-first sweeps, price improvement, partial-fill priority
  retention, IOC/market symmetry both sides, cancel-after-fill, qty conservation.

## Deviations from DESIGN.md and why

- **`NaiveBook::add_limit` (rest-without-matching primitive) is not in DESIGN.md.**
  Added so the Day-2 differential harness can compare add/cancel book structure against
  `OrderBook` before matching exists (Day 3). `add()` is the real entry point.
- **Market-order events carry price 0** and the price argument is ignored entirely.
  DESIGN.md §5 doesn't pin this down; echoing a meaningless caller price into the event
  stream would make byte-exact differential comparison fragile.
- **Cancel-reject side is left zero (`kBuy`).** The side of an unknown id is unknowable;
  fixing it to zero keeps `Event` deterministic for memcmp comparison.
- **`RejectReason` enum + `reason` field on `Event`** are an extension of DESIGN.md §5's
  four-event list, needed to express validation failures (`kPoolExhausted` reserved for
  the real pool).
- **CI has no tsan or bench-smoke jobs yet** (PLAN.md skeleton lists both). Deliberate
  sequencing: tsan job lands with the SPSC queue (Day 4 task 2), bench-smoke needs the
  replay CLI (Day 5).
- **`src/`, `tools/`, `docs/` from DESIGN.md §8 don't exist yet.** Everything so far is
  header-only; directories appear when their first real file does.

## Known issues / TODOs

- **TSan on this Codespace requires `sudo sysctl vm.mmap_rnd_bits=28` each session**,
  otherwise TSan-instrumented binaries crash at startup (kernel ASLR entropy vs
  clang-17 TSan). Becomes relevant on Day 4.
- Hardware perf counters are blocked in Codespaces; Day 6 `perf stat` numbers must come
  from a bare-metal Linux box (documented limitation, per PLAN.md).
- clang-tidy is configured but not yet enforced anywhere (no CI job, no script); the
  clean-up pass is Day 7 task 5.
- `bench/dummy_bench.cc` is placeholder wiring; real benchmarks start Day 6.
- README is a stub by design until measured v1.0 numbers exist.
- `NaiveBook` allocates freely and is slow by design — test/bench oracle only, never on
  the hot path.

## Next task

**Day 2, task 1: `OrderPool` with index free list + tests (alloc/free/exhaustion/
generation bits). [feat: order pool]**

Per DESIGN.md §4.2: `std::vector<Order>` pre-sized at startup, free list threaded
through `next_idx`, O(1) alloc/free, ids = pool index + generation counter in the high
bits (generation starts at 1 so `kInvalidOrderId == 0` is never minted — see the
`types.h` comment). Requires the 64-byte `Order` POD skeleton (fleshed out in task 2).
