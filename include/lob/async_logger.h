#ifndef LOB_ASYNC_LOGGER_H_
#define LOB_ASYNC_LOGGER_H_

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <thread>
#include <type_traits>

#include "lob/spsc_queue.h"
#include "lob/types.h"

namespace lob {

// Records consumed per batched pop on the logger thread: one acquire/release
// pair per batch (the same amortization as kEngineBatchSize). 256 × 32 B =
// 8 KiB of logger-thread stack.
inline constexpr std::size_t kLoggerBatchSize = 256;

// Default log ring capacity: 16384 records × 32 B = 512 KiB, allocated once at
// construction. At 1M events/s this absorbs ~16 ms of logger-thread stall
// before the producer sees a full ring.
inline constexpr std::size_t kDefaultLogRingCapacity = std::size_t{1} << 14;

// ---------------------------------------------------------------------------
// LogRecord: the 32-byte binary log record (DESIGN.md §6: timestamp, event
// type, ids, price, qty). One record per Event; a fill's two Traded events
// become two consecutive records, so both sides' ids reach the log. The
// on-disk format is the in-memory representation (native-endian memcpy) —
// explicit padding plus has_unique_object_representations (asserted below)
// make the bytes fully determined, exactly like Command/Event in types.h.
// `remaining` is deliberately not carried: DESIGN's record field list omits
// it, and 32 bytes has no room without evicting a listed field.
// ---------------------------------------------------------------------------

struct LogRecord {
  std::uint64_t timestamp;  // caller-supplied tick: ns or raw rdtsc — the logger is
                            // clock-agnostic; calibration is LatencyRecorder's job (Day 5 task 6)
  OrderId order_id;         // kInvalidOrderId when rejecting a New that got no id
  PriceTicks price_ticks;
  Qty qty;  // kTraded: fill qty; otherwise the order's qty
  EventType kind;
  Side side;            // side of the order this record refers to
  RejectReason reason;  // kRejected only; kNone otherwise
  std::uint8_t pad0;    // explicit padding; keep zero
};

static_assert(sizeof(LogRecord) == 32);
static_assert(alignof(LogRecord) == 8);
static_assert(std::is_trivially_copyable_v<LogRecord>);
static_assert(std::is_standard_layout_v<LogRecord>);
static_assert(std::has_unique_object_representations_v<LogRecord>);

// Field offsets are part of the wire layout: renumbering is a format break.
static_assert(offsetof(LogRecord, timestamp) == 0);
static_assert(offsetof(LogRecord, order_id) == 8);
static_assert(offsetof(LogRecord, price_ticks) == 16);
static_assert(offsetof(LogRecord, qty) == 24);
static_assert(offsetof(LogRecord, kind) == 28);
static_assert(offsetof(LogRecord, side) == 29);
static_assert(offsetof(LogRecord, reason) == 30);
static_assert(offsetof(LogRecord, pad0) == 31);

// The one blessed way to build a record from an engine event; explicitly
// zeroes the pad so record bytes stay fully determined.
constexpr LogRecord make_log_record(std::uint64_t timestamp, const Event& e) noexcept {
  return LogRecord{timestamp, e.order_id, e.price_ticks, e.qty, e.kind, e.side, e.reason, 0};
}

// ---------------------------------------------------------------------------
// AsyncLogger: the DESIGN §3/§6 logger stage. The hot path (exactly one
// producer thread — the engine) pushes 32-byte records into an SPSC ring via
// try_log; a dedicated consumer thread, spawned at construction, batch-pops
// and writes the raw bytes to a caller-supplied std::ostream, flushing when
// the ring goes idle and again at shutdown.
//
// The sink is an ostream (not a FILE*/template): its virtual dispatch runs on
// the logger thread, off the latency path, and ostringstream makes byte-exact
// tests trivial while ofstream covers real files. The stream must outlive the
// logger; between construction and the return of stop() it is touched
// exclusively by the logger thread — read it only after stop().
//
// Backpressure (DESIGN §6): log() is the production policy — count-and-drop.
// A full ring discards the record and bumps records_dropped(); the engine is
// never blocked and never retries, and the drop counter is itself a metric
// (wired through stats in Day 5 task 7). try_log() is the raw primitive for
// callers implementing their own policy.
// ---------------------------------------------------------------------------

class AsyncLogger {
 public:
  // Startup-only: the ring allocation and the thread spawn are the only
  // resources this class ever acquires.
  explicit AsyncLogger(std::ostream& out, std::size_t ring_capacity = kDefaultLogRingCapacity)
      : out_(out), ring_(ring_capacity), thread_([this] { consume(); }) {}

  ~AsyncLogger() { stop(); }

  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  // Producer thread only — the production count-and-drop policy. Never
  // blocks, never retries: a full ring discards the record and bumps the
  // drop counter. Returns nothing by design: a caller that reacted to a
  // drop would be implementing a retry policy, which this API forbids —
  // records_dropped() is the signal.
  void log(const LogRecord& rec) noexcept {
    if (!ring_.try_push(rec)) {
      // relaxed: single-writer monotonic metric; no data is published on its
      // strength, so no ordering is required.
      records_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void log(std::uint64_t timestamp, const Event& e) noexcept { log(make_log_record(timestamp, e)); }

  // Producer thread only — the raw primitive: false = ring full, record NOT
  // logged, drop counter NOT touched. For callers implementing their own
  // policy (the lossless tests spin on it), so "dropped" always means a
  // record was actually lost, never "an attempt failed before a retry".
  bool try_log(const LogRecord& rec) noexcept { return ring_.try_push(rec); }

  bool try_log(std::uint64_t timestamp, const Event& e) noexcept {
    return try_log(make_log_record(timestamp, e));
  }

  // Shutdown contract (mirrors MatchingEngine::run): the producer makes its
  // final try_log, then stop() is called by a thread that has synchronized
  // with the producer (the producer itself, or after joining it). The logger
  // thread drains the ring completely after observing stop, so every
  // accepted record reaches the stream, then flushes and exits. Idempotent;
  // single-owner — the logger cannot be restarted.
  void stop() {
    if (!thread_.joinable()) {
      return;
    }
    // release: pairs with the acquire load in consume() — the producer's
    // final push happens-before this store (caller contract), so once the
    // logger thread sees stop it is guaranteed to drain that push too.
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }

  // Records the logger thread has written to the stream. Written by the
  // logger thread only — call after stop() (the join makes it visible).
  std::uint64_t records_written() const noexcept {
    assert(!thread_.joinable() && "records_written() is only valid after stop()");
    return records_written_;
  }

  // Live metric (DESIGN §6): records log() discarded because the ring was
  // full. Readable from any thread at any time, including mid-run.
  std::uint64_t records_dropped() const noexcept {
    // relaxed: a monotonic counter read for reporting; no data is read on
    // the strength of the value, so no ordering is required.
    return records_dropped_.load(std::memory_order_relaxed);
  }

  std::size_t ring_capacity() const noexcept { return ring_.capacity(); }

  // DESIGN §6 ring occupancy high-water mark (see SPSCQueue::high_water for
  // the exactness contract). Any thread, any time.
  std::size_t ring_high_water() const noexcept { return ring_.high_water(); }

 private:
  void consume() {
    std::array<LogRecord, kLoggerBatchSize> batch;
    bool stopping = false;
    bool dirty = false;  // bytes written since the last flush
    for (;;) {
      const std::size_t n = ring_.try_pop_batch(batch.data(), batch.size());
      if (n != 0) {
        out_.write(reinterpret_cast<const char*>(batch.data()),
                   static_cast<std::streamsize>(n * sizeof(LogRecord)));
        records_written_ += n;
        dirty = true;
        continue;
      }
      if (stopping) {
        // Empty pop after stop was observed: the acquire below ordered us
        // after the producer's final push, so the ring is truly drained.
        out_.flush();
        return;
      }
      if (dirty) {
        // Idle moment: push buffered bytes toward the OS so a long quiet
        // spell (or a crash) doesn't strand records in the stream buffer.
        out_.flush();
        dirty = false;
      }
      // acquire: pairs with the release store in stop() — once stop is seen,
      // the next pop attempt sees every record pushed before it (hence loop
      // again, not break).
      stopping = stop_.load(std::memory_order_acquire);
      if (!stopping) {
        // Not latency-critical: unlike the engine loop, the logger yields its
        // core while idle instead of busy-spinning.
        std::this_thread::yield();
      }
    }
  }

  std::ostream& out_;
  SPSCQueue<LogRecord> ring_;
  std::atomic<bool> stop_{false};
  std::uint64_t records_written_ = 0;  // logger thread only; read after join
  // Producer-owned line: log()'s drop count gets its own cache line so drop
  // increments never contend with records_written_ above, which the logger
  // thread writes every batch.
  alignas(64) std::atomic<std::uint64_t> records_dropped_{0};
  // Declared last: the thread starts inside the constructor and must find
  // every member above fully initialized.
  std::thread thread_;
};

}  // namespace lob

#endif  // LOB_ASYNC_LOGGER_H_
