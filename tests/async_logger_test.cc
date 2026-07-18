#include "lob/async_logger.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lob/types.h"

namespace lob {
namespace {

// Day 5 task 1: the 32-byte binary log record and the AsyncLogger stage
// (DESIGN §6). Threaded throughout (the logger owns a consumer thread), so
// the whole binary carries the `concurrency` label and runs under the TSan
// CI job.

// ---------------------------------------------------------------------------
// Record layout and mapping (the static_asserts in the header are the real
// contract; these runtime twins give named failures).
// ---------------------------------------------------------------------------

TEST(LogRecordLayout, MatchesWireContract) {
  EXPECT_EQ(sizeof(LogRecord), 32u);
  EXPECT_EQ(alignof(LogRecord), 8u);
  EXPECT_EQ(offsetof(LogRecord, timestamp), 0u);
  EXPECT_EQ(offsetof(LogRecord, order_id), 8u);
  EXPECT_EQ(offsetof(LogRecord, price_ticks), 16u);
  EXPECT_EQ(offsetof(LogRecord, qty), 24u);
  EXPECT_EQ(offsetof(LogRecord, kind), 28u);
  EXPECT_EQ(offsetof(LogRecord, side), 29u);
  EXPECT_EQ(offsetof(LogRecord, reason), 30u);
  EXPECT_EQ(offsetof(LogRecord, pad0), 31u);
}

TEST(LogRecordLayout, MakeLogRecordMapsTradedEvent) {
  const Event traded{EventType::kTraded,
                     Side::kSell,
                     RejectReason::kNone,
                     0,
                     /*qty=*/7,
                     /*remaining=*/3,
                     0,
                     /*price_ticks=*/101,
                     /*order_id=*/42};
  const LogRecord rec = make_log_record(9'000'000'001ull, traded);
  EXPECT_EQ(rec.timestamp, 9'000'000'001ull);
  EXPECT_EQ(rec.order_id, 42u);
  EXPECT_EQ(rec.price_ticks, 101);
  EXPECT_EQ(rec.qty, 7u) << "kTraded carries the fill qty";
  EXPECT_EQ(rec.kind, EventType::kTraded);
  EXPECT_EQ(rec.side, Side::kSell);
  EXPECT_EQ(rec.reason, RejectReason::kNone);
  EXPECT_EQ(rec.pad0, 0u);
}

TEST(LogRecordLayout, MakeLogRecordMapsRejectedEvent) {
  const Event rejected{EventType::kRejected,
                       Side::kBuy,
                       RejectReason::kInvalidQty,
                       0,
                       /*qty=*/0,
                       /*remaining=*/0,
                       0,
                       /*price_ticks=*/55,
                       kInvalidOrderId};
  const LogRecord rec = make_log_record(17ull, rejected);
  EXPECT_EQ(rec.timestamp, 17ull);
  EXPECT_EQ(rec.order_id, kInvalidOrderId);
  EXPECT_EQ(rec.price_ticks, 55);
  EXPECT_EQ(rec.kind, EventType::kRejected);
  EXPECT_EQ(rec.reason, RejectReason::kInvalidQty);
}

// ---------------------------------------------------------------------------
// AsyncLogger round trips. Helpers: every record's bytes derive from its
// sequence number, so any torn, reordered, duplicated, or dropped record
// fails the memcmp (the spsc_queue_stress payload pattern).
// ---------------------------------------------------------------------------

LogRecord RecordFor(std::uint64_t i) {
  LogRecord rec{};
  rec.timestamp = 0x9E3779B97F4A7C15ull * (i + 1);  // golden-ratio hash: all 8 bytes vary
  rec.order_id = i;
  rec.price_ticks = static_cast<PriceTicks>(1'000 + (i % 512));
  rec.qty = static_cast<Qty>(i % 1'000 + 1);
  rec.kind = static_cast<EventType>(i % 4);
  rec.side = static_cast<Side>(i % 2);
  rec.reason = static_cast<RejectReason>(i % 5);
  rec.pad0 = 0;
  return rec;
}

std::vector<LogRecord> ParseRecords(const std::string& bytes) {
  EXPECT_EQ(bytes.size() % sizeof(LogRecord), 0u) << "stream holds partial records";
  std::vector<LogRecord> recs(bytes.size() / sizeof(LogRecord));
  std::memcpy(recs.data(), bytes.data(), recs.size() * sizeof(LogRecord));
  return recs;
}

void ExpectFifoSequence(const std::vector<LogRecord>& recs, std::uint64_t count) {
  ASSERT_EQ(recs.size(), count);
  for (std::uint64_t i = 0; i < count; ++i) {
    const LogRecord want = RecordFor(i);
    ASSERT_EQ(std::memcmp(&recs[i], &want, sizeof(LogRecord)), 0)
        << "record #" << i << " diverges (kind " << to_cstr(recs[i].kind) << ", order_id "
        << recs[i].order_id << ")";
  }
}

TEST(AsyncLogger, WritesAllRecordsInFifoOrder) {
  constexpr std::uint64_t kCount = 100'000;  // ~390 wraps of the ring, real batching
  std::ostringstream oss;
  {
    AsyncLogger logger(oss, /*ring_capacity=*/256);
    for (std::uint64_t i = 0; i < kCount; ++i) {
      // Test-local lossless policy so counts must reconcile exactly; the
      // production caller drops on false (count-and-drop is task 2).
      while (!logger.try_log(RecordFor(i))) {
        std::this_thread::yield();
      }
    }
    logger.stop();
    EXPECT_EQ(logger.records_written(), kCount);
  }
  ExpectFifoSequence(ParseRecords(oss.str()), kCount);
}

TEST(AsyncLogger, EventOverloadWritesMappedRecord) {
  const Event traded{EventType::kTraded, Side::kBuy, RejectReason::kNone, 0, 5, 0, 0, 77, 9};
  std::ostringstream oss;
  {
    AsyncLogger logger(oss, /*ring_capacity=*/64);
    ASSERT_TRUE(logger.try_log(123ull, traded));
    logger.stop();
  }
  const std::vector<LogRecord> recs = ParseRecords(oss.str());
  ASSERT_EQ(recs.size(), 1u);
  const LogRecord want = make_log_record(123ull, traded);
  EXPECT_EQ(std::memcmp(&recs[0], &want, sizeof(LogRecord)), 0);
}

TEST(AsyncLogger, StopDrainsBacklog) {
  // A capacity's worth of pushes can never see a full ring (the consumer
  // only adds room), so every try_log must succeed — and the stop/drain
  // contract must then land every one of them in the stream.
  constexpr std::uint64_t kCount = 1'024;
  std::ostringstream oss;
  AsyncLogger logger(oss, /*ring_capacity=*/kCount);
  for (std::uint64_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(logger.try_log(RecordFor(i))) << "push #" << i << " found a full ring";
  }
  logger.stop();
  EXPECT_EQ(logger.records_written(), kCount);
  ExpectFifoSequence(ParseRecords(oss.str()), kCount);
}

TEST(AsyncLogger, StopWithNoRecordsWritesNothing) {
  std::ostringstream oss;
  AsyncLogger logger(oss, /*ring_capacity=*/64);
  logger.stop();
  EXPECT_EQ(logger.records_written(), 0u);
  EXPECT_TRUE(oss.str().empty());
}

TEST(AsyncLogger, StopIsIdempotent) {
  std::ostringstream oss;
  AsyncLogger logger(oss, /*ring_capacity=*/64);
  ASSERT_TRUE(logger.try_log(RecordFor(0)));
  logger.stop();
  logger.stop();  // second stop: no thread to join, no effect
  EXPECT_EQ(logger.records_written(), 1u);
  ExpectFifoSequence(ParseRecords(oss.str()), 1);
}

TEST(AsyncLogger, DestructorStopsAndFlushes) {
  std::ostringstream oss;
  {
    AsyncLogger logger(oss, /*ring_capacity=*/64);
    for (std::uint64_t i = 0; i < 3; ++i) {
      ASSERT_TRUE(logger.try_log(RecordFor(i)));
    }
    // No explicit stop(): the destructor owns shutdown.
  }
  ExpectFifoSequence(ParseRecords(oss.str()), 3);
}

TEST(AsyncLogger, ProducerThreadStress) {
  // Same shape as WritesAllRecordsInFifoOrder but with a dedicated producer
  // thread, so TSan watches the ring + stop contract across three threads
  // (producer, logger, main). Main calls stop() only after joining the
  // producer — the documented shutdown contract.
  constexpr std::uint64_t kCount = 200'000;
  std::ostringstream oss;
  AsyncLogger logger(oss, /*ring_capacity=*/256);

  std::thread producer([&logger] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!logger.try_log(RecordFor(i))) {
        std::this_thread::yield();
      }
    }
  });
  producer.join();
  logger.stop();

  EXPECT_EQ(logger.records_written(), kCount);
  ExpectFifoSequence(ParseRecords(oss.str()), kCount);
}

}  // namespace
}  // namespace lob
