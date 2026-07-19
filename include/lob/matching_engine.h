#ifndef LOB_MATCHING_ENGINE_H_
#define LOB_MATCHING_ENGINE_H_

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "lob/order_book.h"
#include "lob/order_pool.h"
#include "lob/price_ladder.h"
#include "lob/spsc_queue.h"
#include "lob/stats.h"
#include "lob/types.h"

namespace lob {

// Commands consumed per batched pop in MatchingEngine::run(): one
// acquire/release pair per batch instead of per command. 256 × 24 B = 6 KiB
// of engine-thread stack.
inline constexpr std::size_t kEngineBatchSize = 256;

// Startup configuration (DESIGN.md §7's `cfg`). All sizing happens at
// construction; nothing here changes at runtime.
struct EngineConfig {
  PriceTicks anchor_price = 0;  // required, > 0: center of the banded ladders
  std::uint32_t band_radius = kDefaultBandRadius;
  std::uint32_t pool_capacity = kDefaultPoolCapacity;
};

// ---------------------------------------------------------------------------
// MatchingEngine: the facade the pipeline drives — consumes Command PODs,
// produces Event PODs (DESIGN.md §3, §7). Owns the book; single-writer by
// design: exactly one thread calls process/submit/cancel (the pinned engine
// thread once the Day-4 ring loop lands; tests call it directly).
//
// Events go to a caller-supplied sink per call rather than a stored
// on_event callback: a std::function member would cost an allocation and an
// indirect call on the hot path, and the Day-4 loop just passes the
// ring-push sink. Deviation from the DESIGN §7 sketch, recorded in
// PROGRESS.md.
// ---------------------------------------------------------------------------

class MatchingEngine {
 public:
  // Startup-only; all allocation happens in the book's constructor.
  explicit MatchingEngine(const EngineConfig& cfg)
      : book_(cfg.anchor_price, cfg.band_radius, cfg.pool_capacity) {}

  // One command in, zero or more events out (shapes and order per
  // OrderBook::add/cancel). kNew: the engine mints the id — cmd.order_id is
  // ignored (types.h) and the id is reported via the Accepted event.
  // kCancel: targets cmd.order_id; the new-order fields are ignored. Any
  // other kind is a corrupted command: debug-asserts, no-op in release —
  // the Day-5 replay/wire readers validate before submitting.
  //
  // Pure dispatch to submit/cancel: those are the stats choke points, so the
  // ring path and the direct-call path count identically by construction.
  template <typename EventSink>
  void process(const Command& cmd, EventSink&& sink) noexcept {
    switch (cmd.kind) {
      case CommandType::kNew:
        submit(cmd.side, cmd.type, cmd.price_ticks, cmd.qty, std::forward<EventSink>(sink));
        return;
      case CommandType::kCancel:
        cancel(cmd.order_id, std::forward<EventSink>(sink));
        return;
    }
    assert(false && "unknown CommandType");
  }

  // The engine-thread run loop (DESIGN.md §3): batch-pops the command ring
  // and processes in FIFO order, forwarding events to `sink`. Busy-spins
  // while idle — the engine thread owns its core, and parking would put a
  // wakeup on the latency path. Returns the number of commands processed.
  //
  // Shutdown contract: the producer pushes its final command, then sets
  // `stop` with release ordering, and pushes nothing afterwards. The loop
  // drains the ring completely after observing stop, so no command is lost.
  template <typename EventSink>
  std::uint64_t run(SPSCQueue<Command>& commands, const std::atomic<bool>& stop,
                    EventSink&& sink) noexcept {
    std::array<Command, kEngineBatchSize> batch;
    std::uint64_t processed = 0;
    bool stopping = false;
    for (;;) {
      const std::size_t n = commands.try_pop_batch(batch.data(), batch.size());
      for (std::size_t i = 0; i < n; ++i) {
        process(batch[i], sink);
      }
      processed += n;
      if (n != 0) {
        continue;
      }
      if (stopping) {
        // Empty pop after stop was observed: the acquire below ordered us
        // after the producer's final push, so the ring is truly drained.
        return processed;
      }
      // acquire: pairs with the producer's release store of stop after its
      // final push — once stop is seen, the next pop attempt is guaranteed
      // to see every command pushed before it (hence loop again, not break).
      stopping = stop.load(std::memory_order_acquire);
    }
  }

  // Direct-call convenience API (DESIGN.md §7): same semantics as process()
  // with a kNew/kCancel command, plus the return value. Both entry points
  // update the DESIGN §6 counters — a few predictable engine-thread-local
  // increments per op — by wrapping the caller's sink; the event-less
  // overloads delegate with a no-op sink so they count identically.
  template <typename EventSink>
  OrderId submit(Side side, OrderType type, PriceTicks price, Qty qty, EventSink&& sink) noexcept {
    stats_.on_new();
    return book_.add(side, type, price, qty, [this, &sink](const Event& e) {
      stats_.on_event(e);
      sink(e);
    });
  }

  OrderId submit(Side side, OrderType type, PriceTicks price, Qty qty) noexcept {
    return submit(side, type, price, qty, [](const Event&) {});
  }

  template <typename EventSink>
  bool cancel(OrderId id, EventSink&& sink) noexcept {
    stats_.on_cancel_request();
    return book_.cancel(id, [this, &sink](const Event& e) {
      stats_.on_event(e);
      sink(e);
    });
  }

  bool cancel(OrderId id) noexcept {
    return cancel(id, [](const Event&) {});
  }

  // Query surface (tests, stats, benchmarks) — the book remains the single
  // source of truth.
  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

  // DESIGN §6 counters. Engine-thread-owned (stats.h): read at quiescence
  // or from the engine thread itself. reset_stats() supports the Day-6
  // warmup-then-measure workflow alongside LatencyRecorder::reset().
  [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_.reset(); }

 private:
  OrderBook book_;
  EngineStats stats_;
};

}  // namespace lob

#endif  // LOB_MATCHING_ENGINE_H_
