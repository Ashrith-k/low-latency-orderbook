#ifndef LOB_MATCHING_ENGINE_H_
#define LOB_MATCHING_ENGINE_H_

#include <cassert>
#include <cstdint>
#include <utility>

#include "lob/order_book.h"
#include "lob/order_pool.h"
#include "lob/price_ladder.h"
#include "lob/types.h"

namespace lob {

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
  template <typename EventSink>
  void process(const Command& cmd, EventSink&& sink) noexcept {
    switch (cmd.kind) {
      case CommandType::kNew:
        book_.add(cmd.side, cmd.type, cmd.price_ticks, cmd.qty, std::forward<EventSink>(sink));
        return;
      case CommandType::kCancel:
        book_.cancel(cmd.order_id, std::forward<EventSink>(sink));
        return;
    }
    assert(false && "unknown CommandType");
  }

  // Direct-call convenience API (DESIGN.md §7): same semantics as process()
  // with a kNew/kCancel command, plus the return value.
  template <typename EventSink>
  OrderId submit(Side side, OrderType type, PriceTicks price, Qty qty, EventSink&& sink) noexcept {
    return book_.add(side, type, price, qty, std::forward<EventSink>(sink));
  }

  OrderId submit(Side side, OrderType type, PriceTicks price, Qty qty) noexcept {
    return book_.add(side, type, price, qty);
  }

  template <typename EventSink>
  bool cancel(OrderId id, EventSink&& sink) noexcept {
    return book_.cancel(id, std::forward<EventSink>(sink));
  }

  bool cancel(OrderId id) noexcept { return book_.cancel(id); }

  // Query surface (tests, stats, benchmarks) — the book remains the single
  // source of truth.
  const OrderBook& book() const noexcept { return book_; }

 private:
  OrderBook book_;
};

}  // namespace lob

#endif  // LOB_MATCHING_ENGINE_H_
