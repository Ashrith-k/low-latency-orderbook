#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lob/replay_format.h"
#include "lob/workload_gen.h"
#include "lob_replay_lib.h"

namespace lob {
namespace {

// Day 5 task 5: the lob_replay CLI. main() is a thin shell around
// tools::run_cli, so these tests cover the real code paths: both
// subcommands, every exit code, and the error surfaces.

struct CliResult {
  int exit_code;
  std::string out;
  std::string err;
};

CliResult RunCli(const std::vector<std::string>& args) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = tools::run_cli(args, out, err);
  return CliResult{code, out.str(), err.str()};
}

std::string TempPath(const std::string& name) { return testing::TempDir() + name; }

std::string ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << path;
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

TEST(ReplayCli, HelpExitsZeroAndPrintsUsage) {
  const CliResult r = RunCli({"--help"});
  EXPECT_EQ(r.exit_code, tools::kExitOk);
  EXPECT_NE(r.out.find("Usage:"), std::string::npos);
  EXPECT_TRUE(r.err.empty());
}

TEST(ReplayCli, NoArgumentsIsUsageError) {
  const CliResult r = RunCli({});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("Usage:"), std::string::npos);
}

TEST(ReplayCli, UnknownCommandIsUsageError) {
  const CliResult r = RunCli({"frobnicate"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("unknown command"), std::string::npos);
}

TEST(ReplayCli, GenerateRequiresOut) {
  const CliResult r = RunCli({"generate", "--ops", "10"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("--out"), std::string::npos);
}

TEST(ReplayCli, GenerateRejectsBadInteger) {
  const CliResult r = RunCli({"generate", "--out", TempPath("x.lobr"), "--ops", "abc"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("unsigned integer"), std::string::npos);
}

TEST(ReplayCli, GenerateRejectsUnknownFlag) {
  const CliResult r = RunCli({"generate", "--out", TempPath("x.lobr"), "--frobs", "3"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("unknown flag"), std::string::npos);
}

TEST(ReplayCli, GenerateWritesLoadableFile) {
  const std::string path = TempPath("cli_gen.lobr");
  const CliResult r =
      RunCli({"generate", "--out", path, "--ops", "5000", "--seed", "7", "--anchor", "20000"});
  ASSERT_EQ(r.exit_code, tools::kExitOk) << r.err;
  EXPECT_NE(r.out.find("wrote"), std::string::npos);
  EXPECT_NE(r.out.find("5000 commands"), std::string::npos);

  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file);
  const ReplayReadResult res = read_replay(file);
  ASSERT_TRUE(res.ok()) << to_cstr(res.error);
  EXPECT_EQ(res.commands.size(), 5'000u);
  EXPECT_EQ(res.config.seed, 7u);
  EXPECT_EQ(res.config.engine.anchor_price, 20'000);
}

TEST(ReplayCli, GenerateIsDeterministic) {
  const std::string a = TempPath("cli_det_a.lobr");
  const std::string b = TempPath("cli_det_b.lobr");
  const std::vector<std::string> flags = {"--ops", "2000", "--seed", "11"};
  std::vector<std::string> args_a = {"generate", "--out", a};
  std::vector<std::string> args_b = {"generate", "--out", b};
  args_a.insert(args_a.end(), flags.begin(), flags.end());
  args_b.insert(args_b.end(), flags.begin(), flags.end());
  ASSERT_EQ(RunCli(args_a).exit_code, tools::kExitOk);
  ASSERT_EQ(RunCli(args_b).exit_code, tools::kExitOk);
  EXPECT_EQ(ReadFileBytes(a), ReadFileBytes(b)) << "same flags must produce identical files";
}

TEST(ReplayCli, RunReplaysGeneratedFile) {
  const std::string path = TempPath("cli_run.lobr");
  ASSERT_EQ(RunCli({"generate", "--out", path, "--ops", "5000", "--seed", "42"}).exit_code,
            tools::kExitOk);

  const CliResult r = RunCli({"run", path});
  ASSERT_EQ(r.exit_code, tools::kExitOk) << r.err;
  EXPECT_TRUE(r.err.empty());
  EXPECT_NE(r.out.find("processed 5000 commands"), std::string::npos) << r.out;
  // A generated file replays cleanly: the unknown-order count printed in the
  // events line must be zero.
  EXPECT_NE(r.out.find("(0 unknown-order)"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("book: best bid"), std::string::npos) << r.out;
}

TEST(ReplayCli, RunTakesExactlyOneFile) {
  const CliResult r = RunCli({"run", "a.lobr", "b.lobr"});
  EXPECT_EQ(r.exit_code, tools::kExitUsage);
  EXPECT_NE(r.err.find("exactly one"), std::string::npos);
}

TEST(ReplayCli, RunMissingFileFails) {
  const CliResult r = RunCli({"run", TempPath("does_not_exist.lobr")});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("cannot open"), std::string::npos);
}

TEST(ReplayCli, RunCorruptFileFailsWithTypedError) {
  const std::string path = TempPath("cli_corrupt.lobr");
  {
    std::ofstream f(path, std::ios::binary);
    const std::string garbage(200, 'x');
    f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
  const CliResult r = RunCli({"run", path});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("BadMagic"), std::string::npos) << r.err;
}

TEST(ReplayCli, RunRejectsInsaneConfig) {
  // Well-formed file, nonsense config (anchor 0): the CLI must refuse before
  // constructing an engine — the format reader deliberately doesn't.
  const std::string path = TempPath("cli_insane.lobr");
  {
    WorkloadConfig cfg;  // anchor_price stays 0
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(write_replay(f, cfg, nullptr, 0));
  }
  const CliResult r = RunCli({"run", path});
  EXPECT_EQ(r.exit_code, tools::kExitRuntime);
  EXPECT_NE(r.err.find("anchor_price"), std::string::npos) << r.err;
}

}  // namespace
}  // namespace lob
