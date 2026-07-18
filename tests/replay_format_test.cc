#include "lob/replay_format.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/types.h"
#include "lob/workload_gen.h"

namespace lob {
namespace {

// Day 5 task 4: the replay file format. Round trips must be byte-exact, a
// file's own header must be enough to replay it faithfully, and every
// corruption mode must map to its typed error with nothing half-loaded.

WorkloadConfig TestConfig(std::uint64_t seed = 1) {
  WorkloadConfig cfg;
  cfg.engine.anchor_price = 10'000;
  cfg.engine.pool_capacity = std::uint32_t{1} << 16;
  cfg.seed = seed;
  return cfg;
}

std::string WriteToString(const WorkloadConfig& cfg, const std::vector<Command>& cmds) {
  std::ostringstream oss;
  EXPECT_TRUE(write_replay(oss, cfg, cmds));
  return oss.str();
}

ReplayReadResult ReadFromString(const std::string& bytes) {
  std::istringstream iss(bytes);
  return read_replay(iss);
}

TEST(ReplayFormat, HeaderLayoutMatchesWireContract) {
  EXPECT_EQ(sizeof(ReplayHeader), 96u);
  EXPECT_EQ(offsetof(ReplayHeader, magic), 0u);
  EXPECT_EQ(offsetof(ReplayHeader, version), 8u);
  EXPECT_EQ(offsetof(ReplayHeader, pad0), 12u);
  EXPECT_EQ(offsetof(ReplayHeader, command_count), 16u);
  EXPECT_EQ(offsetof(ReplayHeader, config), 24u);
  EXPECT_EQ(sizeof(WorkloadConfig), 72u);
  EXPECT_EQ(sizeof(Command), 24u);
}

TEST(ReplayFormat, GoldenBytes) {
  // Pin the format byte-for-byte with a two-command file: magic string,
  // version word, count, embedded config anchor, and the packed body at
  // offset 96. (Native little-endian, the documented convention.)
  const WorkloadConfig cfg = TestConfig(9);
  const std::vector<Command> cmds = generate_workload(cfg, 2);
  const std::string bytes = WriteToString(cfg, cmds);

  ASSERT_EQ(bytes.size(), 96u + 2 * sizeof(Command));
  EXPECT_EQ(std::memcmp(bytes.data(), "LOBRPLY\0", 8), 0);

  std::uint32_t version = 0;
  std::memcpy(&version, bytes.data() + 8, sizeof(version));
  EXPECT_EQ(version, kReplayFormatVersion);

  std::uint64_t count = 0;
  std::memcpy(&count, bytes.data() + 16, sizeof(count));
  EXPECT_EQ(count, 2u);

  WorkloadConfig embedded{};
  std::memcpy(&embedded, bytes.data() + 24, sizeof(embedded));
  EXPECT_EQ(std::memcmp(&embedded, &cfg, sizeof(WorkloadConfig)), 0);

  EXPECT_EQ(std::memcmp(bytes.data() + 96, cmds.data(), 2 * sizeof(Command)), 0);
}

TEST(ReplayFormat, RoundTripIsByteExactAndReplayable) {
  constexpr std::size_t kOps = 10'000;
  const WorkloadConfig cfg = TestConfig(42);
  const std::vector<Command> cmds = generate_workload(cfg, kOps);

  const ReplayReadResult res = ReadFromString(WriteToString(cfg, cmds));
  ASSERT_TRUE(res.ok()) << to_cstr(res.error);
  EXPECT_EQ(std::memcmp(&res.config, &cfg, sizeof(WorkloadConfig)), 0);
  ASSERT_EQ(res.commands.size(), cmds.size());
  EXPECT_EQ(std::memcmp(res.commands.data(), cmds.data(), cmds.size() * sizeof(Command)), 0);

  // The file is self-describing: an engine built from the file's OWN config
  // must accept every command (this is why the header carries the config).
  MatchingEngine engine(res.config.engine);
  std::uint64_t unknown = 0;
  std::uint64_t invalid = 0;
  for (const Command& cmd : res.commands) {
    engine.process(cmd, [&](const Event& e) {
      if (e.kind == EventType::kRejected) {
        unknown += e.reason == RejectReason::kUnknownOrder ? 1 : 0;
        invalid += e.reason != RejectReason::kUnknownOrder ? 1 : 0;
      }
    });
  }
  EXPECT_EQ(unknown, 0u);
  EXPECT_EQ(invalid, 0u);
  engine.book().check_invariants();
}

TEST(ReplayFormat, EmptyCommandListRoundTrips) {
  const WorkloadConfig cfg = TestConfig(3);
  const std::string bytes = WriteToString(cfg, {});
  EXPECT_EQ(bytes.size(), sizeof(ReplayHeader));

  const ReplayReadResult res = ReadFromString(bytes);
  ASSERT_TRUE(res.ok()) << to_cstr(res.error);
  EXPECT_EQ(std::memcmp(&res.config, &cfg, sizeof(WorkloadConfig)), 0);
  EXPECT_TRUE(res.commands.empty());
}

TEST(ReplayFormat, TruncatedHeaderIsRejected) {
  const std::string bytes = WriteToString(TestConfig(), generate_workload(TestConfig(), 4));
  const ReplayReadResult res = ReadFromString(bytes.substr(0, 50));
  EXPECT_EQ(res.error, ReplayReadError::kTruncatedHeader);
  EXPECT_TRUE(res.commands.empty());
}

TEST(ReplayFormat, BadMagicIsRejected) {
  std::string bytes = WriteToString(TestConfig(), generate_workload(TestConfig(), 4));
  bytes[3] = 'X';
  const ReplayReadResult res = ReadFromString(bytes);
  EXPECT_EQ(res.error, ReplayReadError::kBadMagic);
  EXPECT_TRUE(res.commands.empty());
}

TEST(ReplayFormat, UnsupportedVersionIsRejected) {
  std::string bytes = WriteToString(TestConfig(), generate_workload(TestConfig(), 4));
  const std::uint32_t future_version = kReplayFormatVersion + 1;
  std::memcpy(bytes.data() + 8, &future_version, sizeof(future_version));
  const ReplayReadResult res = ReadFromString(bytes);
  EXPECT_EQ(res.error, ReplayReadError::kUnsupportedVersion);
  EXPECT_TRUE(res.commands.empty());
}

TEST(ReplayFormat, TruncatedBodyIsRejected) {
  const std::string bytes = WriteToString(TestConfig(), generate_workload(TestConfig(), 4));
  // Chop mid-command and at a command boundary: both are truncation.
  for (const std::size_t cut : {std::size_t{5}, sizeof(Command)}) {
    const ReplayReadResult res = ReadFromString(bytes.substr(0, bytes.size() - cut));
    EXPECT_EQ(res.error, ReplayReadError::kTruncatedBody) << "cut " << cut;
    EXPECT_TRUE(res.commands.empty()) << "no partial load on truncation";
  }
}

TEST(ReplayFormat, HugeCorruptCountDegradesToTruncation) {
  // A corrupt count must not translate into one giant allocation — the
  // chunked reader hits end-of-stream long before that.
  std::string bytes = WriteToString(TestConfig(), generate_workload(TestConfig(), 4));
  const std::uint64_t absurd = std::uint64_t{1} << 60;
  std::memcpy(bytes.data() + 16, &absurd, sizeof(absurd));
  const ReplayReadResult res = ReadFromString(bytes);
  EXPECT_EQ(res.error, ReplayReadError::kTruncatedBody);
  EXPECT_TRUE(res.commands.empty());
}

TEST(ReplayFormat, TrailingBytesAreRejected) {
  std::string bytes = WriteToString(TestConfig(), generate_workload(TestConfig(), 4));
  bytes.push_back('\0');
  const ReplayReadResult res = ReadFromString(bytes);
  EXPECT_EQ(res.error, ReplayReadError::kTrailingBytes);
  EXPECT_TRUE(res.commands.empty());
}

TEST(ReplayFormat, WriterReportsFailedStream) {
  std::ostringstream oss;
  oss.setstate(std::ios::badbit);
  EXPECT_FALSE(write_replay(oss, TestConfig(), generate_workload(TestConfig(), 2)));
}

}  // namespace
}  // namespace lob
