#ifndef LOB_TESTS_EVENT_TEST_UTIL_H_
#define LOB_TESTS_EVENT_TEST_UTIL_H_

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "lob/types.h"

// Shared event-assertion helpers: expected sequences read like a spec table.
// Used by the NaiveBook spec tests, the OrderBook event tests, and (Day 3
// task 5) the differential harness.

namespace lob::testutil {

inline Event MakeEvent(EventType kind, Side side, RejectReason reason, Qty qty, Qty remaining,
                       PriceTicks price, OrderId id) {
  return Event{.kind = kind,
               .side = side,
               .reason = reason,
               .pad0 = 0,
               .qty = qty,
               .remaining = remaining,
               .pad1 = 0,
               .price_ticks = price,
               .order_id = id};
}

inline std::string Describe(const Event& e) {
  std::ostringstream os;
  os << to_cstr(e.kind) << "{side=" << to_cstr(e.side) << " reason=" << to_cstr(e.reason)
     << " qty=" << e.qty << " remaining=" << e.remaining << " px=" << e.price_ticks
     << " id=" << e.order_id << "}";
  return os.str();
}

// Whole-struct comparison is legal: Event has unique object representations
// (asserted in types.h), so equal bytes <=> equal fields.
inline testing::AssertionResult EventEq(const Event& want, const Event& got) {
  if (std::memcmp(&want, &got, sizeof(Event)) == 0) return testing::AssertionSuccess();
  return testing::AssertionFailure()
         << "\n  want: " << Describe(want) << "\n  got:  " << Describe(got);
}

// Shorthand builders so expected event sequences read like a spec table.
inline Event Accepted(Side s, Qty qty, PriceTicks px, OrderId id) {
  return MakeEvent(EventType::kAccepted, s, RejectReason::kNone, qty, qty, px, id);
}
inline Event Traded(Side s, Qty fill, Qty remaining, PriceTicks px, OrderId id) {
  return MakeEvent(EventType::kTraded, s, RejectReason::kNone, fill, remaining, px, id);
}
inline Event Canceled(Side s, Qty qty, PriceTicks px, OrderId id) {
  return MakeEvent(EventType::kCanceled, s, RejectReason::kNone, qty, 0, px, id);
}
inline Event Rejected(Side s, RejectReason r, Qty qty, PriceTicks px, OrderId id) {
  return MakeEvent(EventType::kRejected, s, r, qty, 0, px, id);
}

inline void ExpectEvents(const std::vector<Event>& got, const std::vector<Event>& want) {
  EXPECT_EQ(want.size(), got.size());
  const std::size_t n = std::min(want.size(), got.size());
  for (std::size_t i = 0; i < n; ++i) {
    SCOPED_TRACE("event " + std::to_string(i));
    EXPECT_TRUE(EventEq(want[i], got[i]));
  }
}

}  // namespace lob::testutil

#endif  // LOB_TESTS_EVENT_TEST_UTIL_H_
