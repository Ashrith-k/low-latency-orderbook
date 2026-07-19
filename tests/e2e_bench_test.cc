#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "e2e_bench_lib.h"
#include "lob/replay_format.h"
#include "lob/types.h"
#include "lob/workload_gen.h"

namespace lob {
namespace {

// Day 6 task 3: the lob_e2e_bench harness. main() is a thin shell around
// bench::run_e2e_cli, so these tests drive the real code paths: the full
// four-thread pipeline on small generated replay files, every exit code, and
// the reporting surfaces. Latency values are hardware-dependent — the tests
// pin down the deterministic parts: exit codes, per-op-type counts (which
// must reconcile exactly with the generator), warmup accounting, percentile
// ordering, and the binary log.

struct CliResult {
  int exit_code;
  std::string out;
  std::string err;
};

CliResult RunCli(const std::vector<std::string>& args) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = bench::run_e2e_cli(args, out, err);
  return CliResult{code, out.str(), err.str()};
}

std::string TempPath(const std::string& name) { return testing::TempDir() + name; }

// Writes a deterministic workload replay file; returns the generator's op
// counts, the ground truth the harness's per-op-type counts must reproduce.
// (Degraded cancels are emitted as limit commands and already counted in
// `limits`, so command-stream classification matches these counts exactly.)
WorkloadCounts WriteWorkload(const std::string& path, std::uint64_t ops, std::uint64_t seed) {
  WorkloadConfig cfg;
  cfg.engine.anchor_price = 10'000;
  cfg.seed = seed;
  WorkloadGenerator gen(cfg);
  std::vector<Command> cmds;
  cmds.reserve(ops);
  for (std::uint64_t i = 0; i < ops; ++i) {
    cmds.push_back(gen.next());
  }
  std::ofstream file(path, std::ios::binary);
  EXPECT_TRUE(file) << path;
  EXPECT_TRUE(write_replay(file, cfg, cmds));
  return gen.counts();
}

struct CsvRow {
  std::string op;
  std::uint64_t count = 0;
  std::uint64_t min_ns = 0;
  std::uint64_t p50 = 0;
  std::uint64_t p90 = 0;
  std::uint64_t p99 = 0;
  std::uint64_t p999 = 0;
  std::uint64_t max_ns = 0;
};

std::vector<CsvRow> ParseCsv(const std::string& text) {
  std::vector<CsvRow> rows;
  std::istringstream lines(text);
  std::string line;
  EXPECT_TRUE(std::getline(lines, line));
  EXPECT_EQ(line, "op,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns");
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    CsvRow row;
    std::string tok;
    EXPECT_TRUE(std::getline(fields, row.op, ','));
    const std::array<std::uint64_t*, 7> nums = {&row.count, &row.min_ns, &row.p50,   &row.p90,
                                                &row.p99,   &row.p999,   &row.max_ns};
    for (std::uint64_t* num : nums) {
      // EXPECT (not ASSERT): gtest fatal assertions need a void function.
      EXPECT_TRUE(std::getline(fields, tok, ',')) << line;
      if (tok.empty()) {
        return rows;
      }
      *num = std::stoull(tok);
    }
    rows.push_back(row);
  }
  return rows;
}

TEST(E2eBenchCli, HelpExitsZeroAndPrintsUsage) {
  const CliResult r = RunCli({"--help"});
  EXPECT_EQ(r.exit_code, tools::kExitOk);
  EXPECT_NE(r.out.find("Usage:"), std::string::npos);
  EXPECT_TRUE(r.err.empty());
}

TEST(E2eBenchCli, NoArgumentsIsUsageError) {
  const CliResult r = RunCli({});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("Usage:"), std::string::npos);
}

TEST(E2eBenchCli, UnknownFlagIsUsageError) {
  const CliResult r = RunCli({TempPath("x.lobr"), "--frobs"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("unknown flag"), std::string::npos);
}

TEST(E2eBenchCli, SecondFileIsUsageError) {
  const CliResult r = RunCli({"a.lobr", "b.lobr"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("exactly one"), std::string::npos);
}

TEST(E2eBenchCli, FlagMissingValueIsUsageError) {
  const CliResult r = RunCli({TempPath("x.lobr"), "--warmup"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("needs a value"), std::string::npos);
}

TEST(E2eBenchCli, BadIntegerIsUsageError) {
  const CliResult r = RunCli({TempPath("x.lobr"), "--warmup", "abc"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("unsigned integer"), std::string::npos);
}

TEST(E2eBenchCli, MissingFileFails) {
  const CliResult r = RunCli({TempPath("does_not_exist.lobr")});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("cannot open"), std::string::npos);
}

TEST(E2eBenchCli, CorruptFileFailsWithTypedError) {
  const std::string path = TempPath("e2e_corrupt.lobr");
  {
    std::ofstream f(path, std::ios::binary);
    const std::string garbage(200, 'x');
    f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
  const CliResult r = RunCli({path});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("BadMagic"), std::string::npos) << r.err;
}

TEST(E2eBenchCli, EmptyWorkloadFails) {
  const std::string path = TempPath("e2e_empty.lobr");
  {
    WorkloadConfig cfg;
    cfg.engine.anchor_price = 10'000;
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(write_replay(f, cfg, nullptr, 0));
  }
  const CliResult r = RunCli({path});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("no commands"), std::string::npos) << r.err;
}

TEST(E2eBenchCli, CorruptCommandKindFailsBeforeThePipeline) {
  // The reader validates format, not semantics: a well-formed file can carry
  // garbage enum bytes, and the harness must refuse them up front (classify()
  // indexes an array by op class; the engine debug-asserts on unknown kinds).
  const std::string path = TempPath("e2e_badkind.lobr");
  {
    WorkloadConfig cfg;
    cfg.engine.anchor_price = 10'000;
    WorkloadGenerator gen(cfg);
    std::vector<Command> cmds;
    cmds.reserve(100);
    for (int i = 0; i < 100; ++i) {
      cmds.push_back(gen.next());
    }
    cmds[50].kind = static_cast<CommandType>(7);
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(write_replay(f, cfg, cmds));
  }
  const CliResult r = RunCli({path});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("command #50"), std::string::npos) << r.err;
}

TEST(E2eBenchCli, WarmupMustBeBelowTheFileSize) {
  const std::string path = TempPath("e2e_warmup_cap.lobr");
  WriteWorkload(path, 500, 3);
  const CliResult r = RunCli({path, "--warmup", "500"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("below"), std::string::npos) << r.err;
}

TEST(E2eBenchCli, CsvCountsReconcileWithTheGenerator) {
  const std::string path = TempPath("e2e_csv.lobr");
  const WorkloadCounts counts = WriteWorkload(path, 4000, 7);
  const CliResult r = RunCli({path, "--warmup", "0", "--csv"});
  ASSERT_EQ(r.exit_code, tools::kExitOk) << r.err;
  EXPECT_TRUE(r.err.empty());

  const std::vector<CsvRow> rows = ParseCsv(r.out);
  ASSERT_EQ(rows.size(), 5u) << r.out;
  EXPECT_EQ(rows[0].op, "limit");
  EXPECT_EQ(rows[1].op, "market");
  EXPECT_EQ(rows[2].op, "ioc");
  EXPECT_EQ(rows[3].op, "cancel");
  EXPECT_EQ(rows[4].op, "all");

  // Nothing lost, nothing invented: with no warmup discard, the harness's
  // per-op-type counts are exactly the generator's.
  EXPECT_EQ(rows[0].count, counts.limits);
  EXPECT_EQ(rows[1].count, counts.markets);
  EXPECT_EQ(rows[2].count, counts.iocs);
  EXPECT_EQ(rows[3].count, counts.cancels);
  EXPECT_EQ(rows[4].count, 4000u);
  EXPECT_EQ(rows[0].count + rows[1].count + rows[2].count + rows[3].count, 4000u);

  // Percentiles are monotone within every row that saw at least one op.
  for (const CsvRow& row : rows) {
    if (row.count == 0) {
      continue;
    }
    EXPECT_LE(row.min_ns, row.p50) << row.op;
    EXPECT_LE(row.p50, row.p90) << row.op;
    EXPECT_LE(row.p90, row.p99) << row.op;
    EXPECT_LE(row.p99, row.p999) << row.op;
    EXPECT_LE(row.p999, row.max_ns) << row.op;
  }
}

TEST(E2eBenchCli, WarmupIsDiscardedFromTheMeasuredWindow) {
  const std::string path = TempPath("e2e_warm.lobr");
  WriteWorkload(path, 4000, 7);
  const CliResult r = RunCli({path, "--warmup", "1000", "--csv"});
  ASSERT_EQ(r.exit_code, tools::kExitOk) << r.err;
  const std::vector<CsvRow> rows = ParseCsv(r.out);
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[4].count, 3000u);
  EXPECT_EQ(rows[0].count + rows[1].count + rows[2].count + rows[3].count, 3000u);
}

TEST(E2eBenchCli, HumanReportHasTheKeySections) {
  const std::string path = TempPath("e2e_human.lobr");
  WriteWorkload(path, 4000, 11);
  const CliResult r = RunCli({path});  // default warmup: 10% of the file
  ASSERT_EQ(r.exit_code, tools::kExitOk) << r.err;
  EXPECT_TRUE(r.err.empty());
  EXPECT_NE(r.out.find("4000 commands"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("pipeline:"), std::string::npos);
  EXPECT_NE(r.out.find("warmup: 400 commands discarded"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("measured: 3600 commands"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("latency (ns):"), std::string::npos);
  EXPECT_NE(r.out.find("limit"), std::string::npos);
  EXPECT_NE(r.out.find("cancel"), std::string::npos);
  EXPECT_NE(r.out.find("records written"), std::string::npos);
  EXPECT_NE(r.out.find("book: best bid"), std::string::npos);
}

TEST(E2eBenchCli, LogFileReceivesWholeBinaryRecords) {
  const std::string path = TempPath("e2e_logged.lobr");
  const std::string log_path = TempPath("e2e_logged.bin");
  WriteWorkload(path, 2000, 5);
  const CliResult r = RunCli({path, "--warmup", "0", "--log", log_path});
  ASSERT_EQ(r.exit_code, tools::kExitOk) << r.err;

  std::ifstream log(log_path, std::ios::binary | std::ios::ate);
  ASSERT_TRUE(log) << log_path;
  const auto size = static_cast<std::uint64_t>(log.tellg());
  // Count-and-drop may shed records under pressure, but whatever was written
  // is whole 32-byte LogRecords, and 2000 commands emit far more events than
  // an idle-drained ring drops in total.
  EXPECT_GT(size, 0u);
  EXPECT_EQ(size % 32, 0u);
}

}  // namespace
}  // namespace lob
