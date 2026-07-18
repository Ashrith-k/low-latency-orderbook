#ifndef LOB_TOOLS_LOB_REPLAY_LIB_H_
#define LOB_TOOLS_LOB_REPLAY_LIB_H_

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#include "lob/matching_engine.h"
#include "lob/replay_format.h"
#include "lob/stats.h"
#include "lob/types.h"
#include "lob/workload_gen.h"

// ---------------------------------------------------------------------------
// lob_replay CLI (DESIGN §8; Day 5 task 5): everything except main() lives
// here so tests can drive the real code paths through run_cli(). Tool tier.
//
//   lob_replay generate --out FILE [...]   workload generator → replay file
//   lob_replay run FILE                    replay file → engine → summary
//
// Exit codes: 0 success, 1 file/runtime error, 2 usage error.
// ---------------------------------------------------------------------------

namespace lob::tools {

inline constexpr int kExitOk = 0;
inline constexpr int kExitRuntime = 1;
inline constexpr int kExitUsage = 2;

inline void print_usage(std::ostream& os) {
  os << "lob_replay - deterministic replay workloads for the LOB engine\n"
        "\n"
        "Usage:\n"
        "  lob_replay generate --out FILE [--ops N] [--seed S] [--anchor P]\n"
        "                      [--band R] [--pool C]\n"
        "      Generate a replay file (defaults: ops 100000, seed 1,\n"
        "      anchor 10000, band "
     << kDefaultBandRadius << ", pool " << kDefaultPoolCapacity
     << ").\n"
        "\n"
        "  lob_replay run FILE\n"
        "      Replay FILE through a fresh engine and print a stats summary.\n"
        "\n"
        "  lob_replay --help\n";
}

inline bool parse_u64(const std::string& s, std::uint64_t& out) {
  const char* const first = s.data();
  const char* const last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, out);
  return ec == std::errc{} && ptr == last && !s.empty();
}

// The reader validates format only; anything constructing an engine from a
// file's config must sanity-check it first — a corrupt-but-well-formed file
// must not become a huge allocation or (release-mode) unchecked-assert UB.
inline constexpr std::uint32_t kMaxBandRadius = 1u << 20;
inline constexpr std::uint32_t kMaxPoolCapacity = 1u << 24;

inline bool validate_engine_config(const EngineConfig& cfg, std::string& why) {
  if (cfg.anchor_price <= 0) {
    why = "anchor_price must be > 0";
    return false;
  }
  if (cfg.band_radius == 0 || cfg.band_radius > kMaxBandRadius) {
    why = "band_radius out of range [1, " + std::to_string(kMaxBandRadius) + "]";
    return false;
  }
  if (cfg.pool_capacity == 0 || cfg.pool_capacity > kMaxPoolCapacity) {
    why = "pool_capacity out of range [1, " + std::to_string(kMaxPoolCapacity) + "]";
    return false;
  }
  return true;
}

inline int run_generate(const std::vector<std::string>& args, std::ostream& out,
                        std::ostream& err) {
  std::string out_path;
  std::uint64_t ops = 100'000;
  WorkloadConfig cfg;
  cfg.engine.anchor_price = 10'000;

  for (std::size_t i = 0; i < args.size(); i += 2) {
    const std::string& flag = args[i];
    if (i + 1 >= args.size()) {
      err << "error: " << flag << " needs a value\n";
      return kExitUsage;
    }
    const std::string& value = args[i + 1];
    std::uint64_t parsed = 0;
    if (flag == "--out") {
      out_path = value;
      continue;
    }
    if (!parse_u64(value, parsed)) {
      err << "error: " << flag << " expects an unsigned integer, got '" << value << "'\n";
      return kExitUsage;
    }
    if (flag == "--ops") {
      ops = parsed;
    } else if (flag == "--seed") {
      cfg.seed = parsed;
    } else if (flag == "--anchor") {
      cfg.engine.anchor_price = static_cast<PriceTicks>(parsed);
    } else if (flag == "--band") {
      cfg.engine.band_radius = static_cast<std::uint32_t>(parsed);
      cfg.walk_radius = cfg.engine.band_radius;
    } else if (flag == "--pool") {
      cfg.engine.pool_capacity = static_cast<std::uint32_t>(parsed);
    } else {
      err << "error: unknown flag '" << flag << "'\n";
      return kExitUsage;
    }
  }
  if (out_path.empty()) {
    err << "error: generate requires --out FILE\n";
    return kExitUsage;
  }
  std::string why;
  if (!validate_engine_config(cfg.engine, why)) {
    err << "error: invalid config: " << why << "\n";
    return kExitUsage;
  }

  WorkloadGenerator gen(cfg);
  std::vector<Command> cmds;
  cmds.reserve(ops);
  for (std::uint64_t i = 0; i < ops; ++i) {
    cmds.push_back(gen.next());
  }

  std::ofstream file(out_path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << out_path << "' for writing\n";
    return kExitRuntime;
  }
  if (!write_replay(file, cfg, cmds)) {
    err << "error: write to '" << out_path << "' failed\n";
    return kExitRuntime;
  }

  const WorkloadCounts& c = gen.counts();
  out << "wrote " << out_path << ": " << ops << " commands (seed " << cfg.seed << ", anchor "
      << cfg.engine.anchor_price << ", band " << cfg.engine.band_radius << ", pool "
      << cfg.engine.pool_capacity << ")\n"
      << "  mix: " << c.limits << " limits, " << c.markets << " markets, " << c.iocs << " iocs, "
      << c.cancels << " cancels (" << c.degraded_cancels << " degraded)\n";
  return kExitOk;
}

inline int run_replay(const std::string& path, std::ostream& out, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitRuntime;
  }
  const ReplayReadResult res = read_replay(file);
  if (!res.ok()) {
    err << "error: invalid replay file '" << path << "': " << to_cstr(res.error) << "\n";
    return kExitRuntime;
  }
  std::string why;
  if (!validate_engine_config(res.config.engine, why)) {
    err << "error: invalid config in '" << path << "': " << why << "\n";
    return kExitRuntime;
  }

  // Counting is the engine's own job now (Day 5 task 7): the CLI just
  // drains events into a no-op sink and reads EngineStats afterwards.
  MatchingEngine engine(res.config.engine);
  const auto t0 = std::chrono::steady_clock::now();
  for (const Command& cmd : res.commands) {
    engine.process(cmd, [](const Event&) {});
  }
  const auto t1 = std::chrono::steady_clock::now();
  engine.book().check_invariants();

  const auto us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  const std::uint64_t ops = res.commands.size();

  out << "replay " << path << ": v" << kReplayFormatVersion << ", " << ops << " commands (seed "
      << res.config.seed << ", anchor " << res.config.engine.anchor_price << ", band "
      << res.config.engine.band_radius << ", pool " << res.config.engine.pool_capacity << ")\n";
  out << "  processed " << ops << " commands in " << us << " us";
  if (us > 0) {
    // Wall-clock, single run, unpinned: indicative only. Day 6's benches own
    // the real methodology.
    out << " (~" << (ops * 1'000'000) / us << " ops/s, wall-clock)";
  }
  out << "\n";
  const EngineStats& s = engine.stats();
  out << "  events: " << s.accepted << " accepted, " << s.rejected << " rejected ("
      << s.rejected_unknown_order << " unknown-order), " << s.traded_events << " traded ("
      << s.fills() << " fills, qty " << s.traded_qty() << "), " << s.canceled << " canceled\n";

  const auto bid = engine.book().best_bid();
  const auto ask = engine.book().best_ask();
  out << "  book: best bid ";
  if (bid) {
    out << *bid;
  } else {
    out << "-";
  }
  out << ", best ask ";
  if (ask) {
    out << *ask;
  } else {
    out << "-";
  }
  out << ", open orders " << engine.book().open_orders() << "\n";
  return kExitOk;
}

// args = argv[1..argc); returns the process exit code.
inline int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    print_usage(err);
    return kExitUsage;
  }
  if (args[0] == "--help" || args[0] == "-h") {
    print_usage(out);
    return kExitOk;
  }
  if (args[0] == "generate") {
    return run_generate({args.begin() + 1, args.end()}, out, err);
  }
  if (args[0] == "run") {
    if (args.size() != 2) {
      err << "error: run takes exactly one FILE argument\n";
      return kExitUsage;
    }
    return run_replay(args[1], out, err);
  }
  err << "error: unknown command '" << args[0] << "'\n";
  print_usage(err);
  return kExitUsage;
}

}  // namespace lob::tools

#endif  // LOB_TOOLS_LOB_REPLAY_LIB_H_
