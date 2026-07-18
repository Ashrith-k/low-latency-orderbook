#ifndef LOB_BENCH_E2E_BENCH_LIB_H_
#define LOB_BENCH_E2E_BENCH_LIB_H_

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "lob/affinity.h"
#include "lob/async_logger.h"
#include "lob/latency.h"
#include "lob/matching_engine.h"
#include "lob/replay_format.h"
#include "lob/spsc_queue.h"
#include "lob/stats.h"
#include "lob/types.h"
#include "lob_replay_lib.h"

// ---------------------------------------------------------------------------
// lob_e2e_bench (DESIGN §11; Day 6 task 3): replay file through the full
// DESIGN §3 pipeline — producer thread → command ring → engine thread
// (pinned, best-effort) → AsyncLogger ring → logger thread — reporting
// throughput and per-op-type latency percentiles. A standalone harness, not
// a Google Benchmark target: percentile tables from a threaded pipeline
// don't fit GB's per-iteration timing model. Thin-main pattern like
// tools/lob_replay: everything except main() lives here so tests can drive
// run_e2e_cli() directly.
//
//   lob_e2e_bench FILE [--warmup N] [--pin CPU] [--log PATH] [--csv]
//
// Latency method: an rdtsc pair brackets each process() call on the engine
// thread; deltas land in one LatencyRecorder per op type (limit, market,
// IOC, cancel). The pair itself (~tens of cycles) is inside the measured
// window — an acknowledged, constant overhead on every op. Log records
// carry the command's admission timestamp: one rdtsc per command, not per
// event. The first --warmup commands are processed normally, then recorders
// and stats reset — the reported window covers steady state only.
//
// Honesty (DESIGN §11): Codespaces numbers are indicative — 2 vCPUs that
// are SMT siblings of one physical core (four pipeline threads share it),
// unpinned when the cpuset refuses, in a VM. Numbers that leave this
// harness travel with CPU model, kernel, compiler, and flags.
//
// Exit codes: 0 success, 1 file/runtime error, 2 usage error (lob_replay's).
// ---------------------------------------------------------------------------

namespace lob::bench {

// Command ring sized well above kEngineBatchSize so the producer genuinely
// decouples from the engine: 65536 × 24 B = 1.5 MiB, allocated at startup.
inline constexpr std::size_t kCmdRingCapacity = std::size_t{1} << 16;

// The four measured op classes, in report order. Values index recorders_.
enum class OpClass : std::uint8_t { kLimit = 0, kMarket = 1, kIoc = 2, kCancel = 3 };

inline constexpr std::size_t kOpClasses = 4;
inline constexpr std::array<const char*, kOpClasses> kOpClassNames{"limit", "market", "ioc",
                                                                   "cancel"};

// Precondition: cmd came through validate_commands() below.
inline OpClass classify(const Command& cmd) noexcept {
  if (cmd.kind == CommandType::kCancel) {
    return OpClass::kCancel;
  }
  switch (cmd.type) {
    case OrderType::kLimit:
      return OpClass::kLimit;
    case OrderType::kMarket:
      return OpClass::kMarket;
    case OrderType::kIoc:
      return OpClass::kIoc;
  }
  assert(false && "validate_commands admitted an unknown OrderType");
  return OpClass::kLimit;
}

// The replay reader validates format, not semantics — enum fields arrive
// untrusted. classify() indexes an array by them and the engine debug-asserts
// on unknown kinds, so a corrupt-but-well-formed file must be refused before
// the pipeline starts. Returns the index of the first bad command, or npos.
inline constexpr std::size_t kAllCommandsValid = static_cast<std::size_t>(-1);

inline std::size_t validate_commands(const std::vector<Command>& cmds) noexcept {
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const Command& c = cmds[i];
    if (c.kind != CommandType::kNew && c.kind != CommandType::kCancel) {
      return i;
    }
    if (c.kind == CommandType::kNew && c.type != OrderType::kLimit &&
        c.type != OrderType::kMarket && c.type != OrderType::kIoc) {
      return i;
    }
  }
  return kAllCommandsValid;
}

// Discards everything successfully; the default logger sink. The logger
// thread still pays its batching and virtual write calls — the production
// stage runs whole, only the disk is taken out of the picture.
class NullBuf final : public std::streambuf {
 protected:
  int_type overflow(int_type c) override { return traits_type::not_eof(c); }
  std::streamsize xsputn(const char* /*s*/, std::streamsize n) override { return n; }
};

struct E2eOptions {
  std::string file;
  std::string log_path;      // empty: logger writes to a null device
  std::uint64_t warmup = 0;  // resolved to ops/10 when not given
  bool warmup_given = false;
  // Default: last CPU, so the engine tends away from cpu 0's housekeeping.
  // < 0 (hardware_concurrency unknown) skips pinning entirely.
  int pin_cpu = static_cast<int>(std::thread::hardware_concurrency()) - 1;
  bool csv = false;
};

// Everything the engine thread produces, read by main strictly after join()
// — the join is the only synchronization these plain fields need.
struct EngineRun {
  std::array<LatencyRecorder, kOpClasses> recorders{};
  std::chrono::steady_clock::time_point measure_start{};
  std::chrono::steady_clock::time_point measure_end{};
  std::uint64_t processed = 0;
  bool pinned = false;
};

inline void print_usage(std::ostream& os) {
  os << "lob_e2e_bench - end-to-end pipeline benchmark for the LOB engine\n"
        "\n"
        "Replays FILE through the full pipeline (producer thread -> command\n"
        "ring -> pinned engine thread -> log ring -> logger thread) and\n"
        "reports throughput plus latency percentiles per op type.\n"
        "\n"
        "Usage:\n"
        "  lob_e2e_bench FILE [--warmup N] [--pin CPU] [--log PATH] [--csv]\n"
        "      --warmup N   commands processed then discarded before the\n"
        "                   measured window (default: 10% of the file)\n"
        "      --pin CPU    pin the engine thread to CPU, best-effort\n"
        "                   (default: the last cpu)\n"
        "      --log PATH   logger writes binary records to PATH\n"
        "                   (default: records are formatted then discarded)\n"
        "      --csv        print machine-readable percentile rows only\n"
        "\n"
        "  lob_e2e_bench --help\n";
}

inline int parse_args(const std::vector<std::string>& args, E2eOptions& opt, std::ostream& err) {
  bool have_file = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--csv") {
      opt.csv = true;
      continue;
    }
    if (arg == "--warmup" || arg == "--pin" || arg == "--log") {
      if (i + 1 >= args.size()) {
        err << "error: " << arg << " needs a value\n";
        return tools::kExitUsage;
      }
      const std::string& value = args[++i];
      if (arg == "--log") {
        opt.log_path = value;
        continue;
      }
      std::uint64_t parsed = 0;
      if (!tools::parse_u64(value, parsed)) {
        err << "error: " << arg << " expects an unsigned integer, got '" << value << "'\n";
        return tools::kExitUsage;
      }
      if (arg == "--warmup") {
        opt.warmup = parsed;
        opt.warmup_given = true;
      } else if (parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        // Merely non-existent CPUs are pin_thread_to_cpu's problem (it
        // refuses and the run degrades to unpinned-and-documented); only an
        // int overflow is rejected here.
        err << "error: --pin cpu out of range\n";
        return tools::kExitUsage;
      } else {
        opt.pin_cpu = static_cast<int>(parsed);
      }
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      err << "error: unknown flag '" << arg << "'\n";
      return tools::kExitUsage;
    }
    if (have_file) {
      err << "error: exactly one FILE argument\n";
      return tools::kExitUsage;
    }
    opt.file = arg;
    have_file = true;
  }
  if (!have_file) {
    err << "error: a replay FILE argument is required\n";
    return tools::kExitUsage;
  }
  return tools::kExitOk;
}

inline void print_latency_row(std::ostream& out, const char* name, const LatencySummary& ns) {
  out << "    " << std::left << std::setw(10) << name << std::right << std::setw(12) << ns.count
      << std::setw(10) << ns.min_ticks << std::setw(10) << ns.p50 << std::setw(10) << ns.p90
      << std::setw(10) << ns.p99 << std::setw(10) << ns.p999 << std::setw(10) << ns.max_ticks
      << "\n";
}

inline void print_csv_row(std::ostream& out, const char* name, const LatencySummary& ns) {
  out << name << ',' << ns.count << ',' << ns.min_ticks << ',' << ns.p50 << ',' << ns.p90 << ','
      << ns.p99 << ',' << ns.p999 << ',' << ns.max_ticks << "\n";
}

inline int run_e2e(const E2eOptions& opt, std::ostream& out, std::ostream& err) {
  std::ifstream file(opt.file, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << opt.file << "'\n";
    return tools::kExitRuntime;
  }
  const ReplayReadResult res = read_replay(file);
  if (!res.ok()) {
    err << "error: invalid replay file '" << opt.file << "': " << to_cstr(res.error) << "\n";
    return tools::kExitRuntime;
  }
  std::string why;
  if (!tools::validate_engine_config(res.config.engine, why)) {
    err << "error: invalid config in '" << opt.file << "': " << why << "\n";
    return tools::kExitRuntime;
  }
  const std::uint64_t ops = res.commands.size();
  if (ops == 0) {
    err << "error: '" << opt.file << "' contains no commands\n";
    return tools::kExitRuntime;
  }
  const std::size_t bad = validate_commands(res.commands);
  if (bad != kAllCommandsValid) {
    err << "error: '" << opt.file << "' command #" << bad << " has an unknown kind/type\n";
    return tools::kExitRuntime;
  }
  const std::uint64_t warmup = opt.warmup_given ? opt.warmup : ops / 10;
  if (warmup >= ops) {
    err << "error: --warmup " << warmup << " must be below the file's " << ops << " commands\n";
    return tools::kExitUsage;
  }

  std::ofstream log_file;
  if (!opt.log_path.empty()) {
    log_file.open(opt.log_path, std::ios::binary);
    if (!log_file) {
      err << "error: cannot open '" << opt.log_path << "' for writing\n";
      return tools::kExitRuntime;
    }
  }
  NullBuf null_buf;
  std::ostream null_stream(&null_buf);
  std::ostream& log_out = opt.log_path.empty() ? null_stream : log_file;

  // Startup-only, before any pipeline thread exists (spinning threads would
  // perturb the calibration spin, and vice versa).
  const TscCalibration cal = TscCalibration::measure();

  MatchingEngine engine(res.config.engine);
  SPSCQueue<Command> cmd_ring(kCmdRingCapacity);
  AsyncLogger logger(log_out);
  std::atomic<bool> stop{false};
  EngineRun run;

  std::thread producer([&res, &cmd_ring, &stop] {
    for (const Command& cmd : res.commands) {
      while (!cmd_ring.try_push(cmd)) {
        // Full ring means the engine is the bottleneck — exactly what is
        // being measured. Yielding keeps 2-vCPU hosts live; on dedicated
        // cores it is nearly free because the ring is rarely full.
        std::this_thread::yield();
      }
    }
    // release: the run-loop shutdown contract (matching_engine.h) — set only
    // after the final push, so a drain after observing stop misses nothing.
    stop.store(true, std::memory_order_release);
  });

  std::thread engine_thread([&engine, &cmd_ring, &logger, &stop, &run, &opt, warmup] {
    if (opt.pin_cpu >= 0) {
      run.pinned = pin_current_thread_to_cpu(opt.pin_cpu);
    }
    // Mirrors MatchingEngine::run()'s batch-pop loop and shutdown contract;
    // run() itself deliberately offers no per-command hook, and the rdtsc
    // bracket must sit around each process() call, so the loop is inlined
    // here (bench tier).
    std::array<Command, kEngineBatchSize> batch;
    std::uint64_t cmd_ts = 0;  // admission timestamp shared with the sink
    const auto sink = [&logger, &cmd_ts](const Event& e) { logger.log(cmd_ts, e); };
    std::uint64_t processed = 0;
    bool stopping = false;
    for (;;) {
      const std::size_t n = cmd_ring.try_pop_batch(batch.data(), batch.size());
      for (std::size_t i = 0; i < n; ++i) {
        if (processed == warmup) {
          // Warmup boundary (fires exactly once, first command included when
          // warmup is 0): discard cache/branch/allocator warm-in and open the
          // measured window. One predictable compare per command otherwise.
          for (LatencyRecorder& rec : run.recorders) {
            rec.reset();
          }
          engine.reset_stats();
          run.measure_start = std::chrono::steady_clock::now();
        }
        const Command& cmd = batch[i];
        LatencyRecorder& rec = run.recorders[static_cast<std::size_t>(classify(cmd))];
        cmd_ts = rdtsc_now();
        engine.process(cmd, sink);
        rec.record(rdtsc_now() - cmd_ts);
        ++processed;
      }
      if (n != 0) {
        continue;
      }
      if (stopping) {
        break;
      }
      // acquire: pairs with the producer's release store of stop after its
      // final push — once seen, the next pop attempt drains everything.
      stopping = stop.load(std::memory_order_acquire);
    }
    run.measure_end = std::chrono::steady_clock::now();
    run.processed = processed;
  });

  producer.join();
  engine_thread.join();
  // Logger shutdown contract: its producer (the engine thread) is joined, so
  // this thread has synchronized with the final log() call.
  logger.stop();

  engine.book().check_invariants();
  assert(run.processed == ops && "the pipeline must drain every command");

  LatencyRecorder all;
  for (const LatencyRecorder& rec : run.recorders) {
    all.merge(rec);
  }
  std::array<LatencySummary, kOpClasses> per_op{};
  for (std::size_t i = 0; i < kOpClasses; ++i) {
    per_op[i] = run.recorders[i].summary().to_nanos(cal);
  }
  const LatencySummary total = all.summary().to_nanos(cal);

  if (opt.csv) {
    out << "op,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n";
    for (std::size_t i = 0; i < kOpClasses; ++i) {
      print_csv_row(out, kOpClassNames[i], per_op[i]);
    }
    print_csv_row(out, "all", total);
    return tools::kExitOk;
  }

  const std::uint64_t measured = ops - warmup;
  const auto us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(run.measure_end - run.measure_start)
          .count());
  out << "e2e " << opt.file << ": " << ops << " commands (seed " << res.config.seed << ", anchor "
      << res.config.engine.anchor_price << ", band " << res.config.engine.band_radius << ", pool "
      << res.config.engine.pool_capacity << ")\n";
  out << "  pipeline: producer -> cmd ring " << cmd_ring.capacity() << " -> engine -> log ring "
      << logger.ring_capacity() << " -> logger ("
      << (opt.log_path.empty() ? "discarded" : opt.log_path) << ")\n";
  out << "  engine thread: ";
  if (run.pinned) {
    out << "pinned to cpu " << opt.pin_cpu;
  } else if (opt.pin_cpu >= 0) {
    out << "UNPINNED (pin to cpu " << opt.pin_cpu << " refused)";
  } else {
    out << "UNPINNED (no cpu available to choose)";
  }
  out << "; tsc " << cal.ticks_per_second / 1'000'000 << " MHz\n";
  out << "  warmup: " << warmup << " commands discarded\n";
  out << "  measured: " << measured << " commands in " << us << " us";
  if (us > 0) {
    out << " -> " << (measured * 1'000'000) / us << " cmd/s";
  }
  out << "\n";
  out << "  latency (ns):    " << std::right << std::setw(9) << "count" << std::setw(10) << "min"
      << std::setw(10) << "p50" << std::setw(10) << "p90" << std::setw(10) << "p99" << std::setw(10)
      << "p99.9" << std::setw(10) << "max" << "\n";
  for (std::size_t i = 0; i < kOpClasses; ++i) {
    print_latency_row(out, kOpClassNames[i], per_op[i]);
  }
  print_latency_row(out, "all", total);
  const EngineStats& s = engine.stats();
  out << "  events (measured window): " << s.accepted << " accepted, " << s.rejected
      << " rejected (" << s.rejected_unknown_order << " unknown-order), " << s.traded_events
      << " traded (" << s.fills() << " fills, qty " << s.traded_qty() << "), " << s.canceled
      << " canceled\n";
  out << "  rings: cmd high-water " << cmd_ring.high_water() << "/" << cmd_ring.capacity()
      << ", log high-water " << logger.ring_high_water() << "/" << logger.ring_capacity() << "\n";
  out << "  log: " << logger.records_written() << " records written, " << logger.records_dropped()
      << " dropped\n";

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
  return tools::kExitOk;
}

// args = argv[1..argc); returns the process exit code.
inline int run_e2e_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    print_usage(err);
    return tools::kExitUsage;
  }
  if (args[0] == "--help" || args[0] == "-h") {
    print_usage(out);
    return tools::kExitOk;
  }
  E2eOptions opt;
  const int parse_code = parse_args(args, opt, err);
  if (parse_code != tools::kExitOk) {
    return parse_code;
  }
  return run_e2e(opt, out, err);
}

}  // namespace lob::bench

#endif  // LOB_BENCH_E2E_BENCH_LIB_H_
