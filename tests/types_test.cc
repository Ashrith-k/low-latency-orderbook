#include "lob/types.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>

namespace lob {
namespace {

// Encodings are serialized into the binary replay/log formats; freezing them
// here turns an accidental renumbering into a test failure, not a format break.
TEST(Types, EnumEncodingsAreStable) {
  EXPECT_EQ(0, static_cast<int>(Side::kBuy));
  EXPECT_EQ(1, static_cast<int>(Side::kSell));

  EXPECT_EQ(0, static_cast<int>(OrderType::kLimit));
  EXPECT_EQ(1, static_cast<int>(OrderType::kMarket));
  EXPECT_EQ(2, static_cast<int>(OrderType::kIoc));

  EXPECT_EQ(0, static_cast<int>(CommandType::kNew));
  EXPECT_EQ(1, static_cast<int>(CommandType::kCancel));

  EXPECT_EQ(0, static_cast<int>(EventType::kAccepted));
  EXPECT_EQ(1, static_cast<int>(EventType::kRejected));
  EXPECT_EQ(2, static_cast<int>(EventType::kTraded));
  EXPECT_EQ(3, static_cast<int>(EventType::kCanceled));

  EXPECT_EQ(0, static_cast<int>(RejectReason::kNone));
  EXPECT_EQ(4, static_cast<int>(RejectReason::kUnknownOrder));
}

TEST(Types, CommandLayout) {
  EXPECT_EQ(24u, sizeof(Command));
  EXPECT_EQ(8u, alignof(Command));
  EXPECT_EQ(0u, offsetof(Command, kind));
  EXPECT_EQ(1u, offsetof(Command, side));
  EXPECT_EQ(2u, offsetof(Command, type));
  EXPECT_EQ(4u, offsetof(Command, qty));
  EXPECT_EQ(8u, offsetof(Command, price_ticks));
  EXPECT_EQ(16u, offsetof(Command, order_id));
}

TEST(Types, EventLayout) {
  EXPECT_EQ(32u, sizeof(Event));
  EXPECT_EQ(8u, alignof(Event));
  EXPECT_EQ(0u, offsetof(Event, kind));
  EXPECT_EQ(1u, offsetof(Event, side));
  EXPECT_EQ(2u, offsetof(Event, reason));
  EXPECT_EQ(4u, offsetof(Event, qty));
  EXPECT_EQ(8u, offsetof(Event, remaining));
  EXPECT_EQ(16u, offsetof(Event, price_ticks));
  EXPECT_EQ(24u, offsetof(Event, order_id));
}

// Value-initialization must produce all-zero bytes (padding included) — the
// SPSC rings and replay writer rely on fully deterministic representations.
TEST(Types, ValueInitIsAllZeroBytes) {
  const Command cmd{};
  const Event ev{};
  const std::array<char, sizeof(Event)> zeros{};
  EXPECT_EQ(0, std::memcmp(&cmd, zeros.data(), sizeof(cmd)));
  EXPECT_EQ(0, std::memcmp(&ev, zeros.data(), sizeof(ev)));
}

// With unique object representations (asserted in the header), equal field
// values imply equal bytes — so memcmp is a valid equality in differential
// tests. Also proves designated init zeroes the unnamed padding members.
TEST(Types, MemcmpEqualityAndCopyRoundTrip) {
  const Command a{.kind = CommandType::kNew,
                  .side = Side::kBuy,
                  .type = OrderType::kLimit,
                  .pad0 = 0,
                  .qty = 100,
                  .price_ticks = 10'000,
                  .order_id = 42};
  const Command b{.kind = CommandType::kNew,
                  .side = Side::kBuy,
                  .type = OrderType::kLimit,
                  .pad0 = 0,
                  .qty = 100,
                  .price_ticks = 10'000,
                  .order_id = 42};
  EXPECT_EQ(0, std::memcmp(&a, &b, sizeof(Command)));

  Command c{};
  std::memcpy(&c, &a, sizeof(Command));  // ring transport is a memcpy
  EXPECT_EQ(0, std::memcmp(&c, &a, sizeof(Command)));
  EXPECT_EQ(a.order_id, c.order_id);
  EXPECT_EQ(a.price_ticks, c.price_ticks);
}

TEST(Types, ToCstr) {
  EXPECT_STREQ("Buy", to_cstr(Side::kBuy));
  EXPECT_STREQ("Sell", to_cstr(Side::kSell));
  EXPECT_STREQ("IOC", to_cstr(OrderType::kIoc));
  EXPECT_STREQ("Cancel", to_cstr(CommandType::kCancel));
  EXPECT_STREQ("Traded", to_cstr(EventType::kTraded));
  EXPECT_STREQ("PoolExhausted", to_cstr(RejectReason::kPoolExhausted));
}

}  // namespace
}  // namespace lob
