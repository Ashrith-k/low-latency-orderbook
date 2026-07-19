#ifndef LOB_TYPES_H_
#define LOB_TYPES_H_

#include <cstdint>
#include <type_traits>

namespace lob {

// ---------------------------------------------------------------------------
// Scalar types
// ---------------------------------------------------------------------------

// Prices are fixed-point integer ticks. No floating point anywhere in the
// engine (DESIGN.md §4.1).
using PriceTicks = std::int64_t;
using Qty = std::uint32_t;
// Opaque to callers; internally generation << 32 | pool index (order_pool.h),
// which is what makes O(1) cancel lookup and deterministic ABA rejection work.
using OrderId = std::uint64_t;

// Reserved "no order" id (e.g. in Rejected events for a New that never got
// one). The pool's id scheme must never mint it: generation bits start at 1.
inline constexpr OrderId kInvalidOrderId = 0;

// ---------------------------------------------------------------------------
// Enums. Encodings are explicit and stable: they are written verbatim into
// the binary replay/log formats (Day 5), so renumbering is a format break.
// ---------------------------------------------------------------------------

enum class Side : std::uint8_t { kBuy = 0, kSell = 1 };

enum class OrderType : std::uint8_t { kLimit = 0, kMarket = 1, kIoc = 2 };

enum class CommandType : std::uint8_t { kNew = 0, kCancel = 1 };

enum class EventType : std::uint8_t {
  kAccepted = 0,
  kRejected = 1,
  kTraded = 2,
  kCanceled = 3,
};

enum class RejectReason : std::uint8_t {
  kNone = 0,
  kInvalidQty = 1,
  kInvalidPrice = 2,
  kPoolExhausted = 3,
  kUnknownOrder = 4,
};

// ---------------------------------------------------------------------------
// Wire PODs. Both travel through SPSC rings by memcpy, so they carry explicit
// padding: has_unique_object_representations (asserted below) proves there are
// no hidden padding bytes, which makes memcmp comparison in tests legal.
// ---------------------------------------------------------------------------

// One instruction to the engine.
struct Command {
  CommandType kind;        // kNew | kCancel
  Side side;               // kNew only
  OrderType type;          // kNew only
  std::uint8_t pad0;       // explicit padding; keep zero
  Qty qty;                 // kNew only
  PriceTicks price_ticks;  // kNew limit/IOC only; ignored for market
  OrderId order_id;        // kCancel: target id. kNew: ignored — the engine assigns
                           // ids and reports them via the Accepted event.
};

static_assert(sizeof(Command) == 24);
static_assert(alignof(Command) == 8);
static_assert(std::is_trivially_copyable_v<Command>);
static_assert(std::is_standard_layout_v<Command>);
static_assert(std::has_unique_object_representations_v<Command>);

// One engine outcome. A fill emits two Traded events, one per side, each
// carrying that order's id/side/remaining; price is the execution (resting
// order's) price.
struct Event {
  EventType kind;
  Side side;            // side of the order this event refers to
  RejectReason reason;  // kRejected only; kNone otherwise
  std::uint8_t pad0;    // explicit padding; keep zero
  Qty qty;              // kTraded: fill qty; otherwise the order's qty
  Qty remaining;        // open qty of this order after the event
  std::uint32_t pad1;   // explicit padding; keep zero
  PriceTicks price_ticks;
  OrderId order_id;  // kInvalidOrderId when rejecting a New that got no id
};

static_assert(sizeof(Event) == 32);
static_assert(alignof(Event) == 8);
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<Event>);
static_assert(std::has_unique_object_representations_v<Event>);

// Enum widths are part of the wire layout too.
static_assert(sizeof(Side) == 1 && sizeof(OrderType) == 1 && sizeof(CommandType) == 1 &&
              sizeof(EventType) == 1 && sizeof(RejectReason) == 1);

// ---------------------------------------------------------------------------
// Diagnostic helpers (logger / test failure messages; not hot-path critical).
// ---------------------------------------------------------------------------

constexpr const char* to_cstr(Side s) noexcept {
  switch (s) {
    case Side::kBuy:
      return "Buy";
    case Side::kSell:
      return "Sell";
  }
  return "?";
}

constexpr const char* to_cstr(OrderType t) noexcept {
  switch (t) {
    case OrderType::kLimit:
      return "Limit";
    case OrderType::kMarket:
      return "Market";
    case OrderType::kIoc:
      return "IOC";
  }
  return "?";
}

constexpr const char* to_cstr(CommandType c) noexcept {
  switch (c) {
    case CommandType::kNew:
      return "New";
    case CommandType::kCancel:
      return "Cancel";
  }
  return "?";
}

constexpr const char* to_cstr(EventType e) noexcept {
  switch (e) {
    case EventType::kAccepted:
      return "Accepted";
    case EventType::kRejected:
      return "Rejected";
    case EventType::kTraded:
      return "Traded";
    case EventType::kCanceled:
      return "Canceled";
  }
  return "?";
}

constexpr const char* to_cstr(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::kNone:
      return "None";
    case RejectReason::kInvalidQty:
      return "InvalidQty";
    case RejectReason::kInvalidPrice:
      return "InvalidPrice";
    case RejectReason::kPoolExhausted:
      return "PoolExhausted";
    case RejectReason::kUnknownOrder:
      return "UnknownOrder";
  }
  return "?";
}

}  // namespace lob

#endif  // LOB_TYPES_H_
