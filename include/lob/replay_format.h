#ifndef LOB_REPLAY_FORMAT_H_
#define LOB_REPLAY_FORMAT_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <type_traits>
#include <vector>

#include "lob/types.h"
#include "lob/workload_gen.h"

namespace lob {

// ---------------------------------------------------------------------------
// Replay file format (DESIGN §11's deterministic replay workload; Day 5
// task 4). Tool tier — never on the hot path.
//
// Layout:  [ReplayHeader (96 B)] [Command × command_count (24 B each)]
//
// Bytes on disk are the in-memory representation (native little-endian
// memcpy), the same convention as LogRecord: every embedded struct is
// explicitly padded and has_unique_object_representations, so a file is
// byte-determined by its contents. The header carries the full
// WorkloadConfig — provenance (seed, op mix) plus the EngineConfig a
// faithful replay MUST construct its engine with: ids are minted
// deterministically from that config, so the file's cancel commands are only
// valid against it (workload_gen.h).
//
// v1 is strict: any deviation — wrong magic, unknown version, truncation,
// trailing bytes — is a typed error and nothing is half-loaded.
// ---------------------------------------------------------------------------

inline constexpr std::array<char, 8> kReplayMagic = {'L', 'O', 'B', 'R', 'P', 'L', 'Y', '\0'};
inline constexpr std::uint32_t kReplayFormatVersion = 1;

struct ReplayHeader {
  std::array<char, 8> magic;  // kReplayMagic
  std::uint32_t version;      // kReplayFormatVersion
  std::uint32_t pad0;         // explicit padding; keep zero
  std::uint64_t command_count;
  WorkloadConfig config;
};

static_assert(sizeof(ReplayHeader) == 96);
static_assert(alignof(ReplayHeader) == 8);
static_assert(std::is_trivially_copyable_v<ReplayHeader>);
static_assert(std::is_standard_layout_v<ReplayHeader>);
static_assert(std::has_unique_object_representations_v<ReplayHeader>);

// Field offsets are the wire layout: renumbering is a format break, guarded
// by the version field.
static_assert(offsetof(ReplayHeader, magic) == 0);
static_assert(offsetof(ReplayHeader, version) == 8);
static_assert(offsetof(ReplayHeader, pad0) == 12);
static_assert(offsetof(ReplayHeader, command_count) == 16);
static_assert(offsetof(ReplayHeader, config) == 24);

enum class ReplayReadError : std::uint8_t {
  kNone = 0,
  kTruncatedHeader = 1,     // fewer than sizeof(ReplayHeader) bytes
  kBadMagic = 2,            // not a replay file
  kUnsupportedVersion = 3,  // a future (or corrupt) format version
  kTruncatedBody = 4,       // fewer commands than the header promises
  kTrailingBytes = 5,       // more bytes than the header promises
};

constexpr const char* to_cstr(ReplayReadError e) noexcept {
  switch (e) {
    case ReplayReadError::kNone:
      return "None";
    case ReplayReadError::kTruncatedHeader:
      return "TruncatedHeader";
    case ReplayReadError::kBadMagic:
      return "BadMagic";
    case ReplayReadError::kUnsupportedVersion:
      return "UnsupportedVersion";
    case ReplayReadError::kTruncatedBody:
      return "TruncatedBody";
    case ReplayReadError::kTrailingBytes:
      return "TrailingBytes";
  }
  return "?";
}

// Writes header + packed commands; returns false if the stream failed at any
// point (the file is then unusable — delete it, don't ship it).
inline bool write_replay(std::ostream& out, const WorkloadConfig& config, const Command* commands,
                         std::size_t count) {
  ReplayHeader hdr{};
  hdr.magic = kReplayMagic;
  hdr.version = kReplayFormatVersion;
  hdr.pad0 = 0;
  hdr.command_count = count;
  hdr.config = config;
  out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  if (count > 0) {
    out.write(reinterpret_cast<const char*>(commands),
              static_cast<std::streamsize>(count * sizeof(Command)));
  }
  out.flush();
  return static_cast<bool>(out);
}

inline bool write_replay(std::ostream& out, const WorkloadConfig& config,
                         const std::vector<Command>& commands) {
  return write_replay(out, config, commands.data(), commands.size());
}

// On any error `commands` is empty (never a partial load); `config` is
// meaningful only when ok() — header-level failures leave it default.
struct ReplayReadResult {
  ReplayReadError error = ReplayReadError::kNone;
  WorkloadConfig config{};
  std::vector<Command> commands;

  [[nodiscard]] bool ok() const noexcept { return error == ReplayReadError::kNone; }
};

// Reads and validates a whole replay file. On any error the result carries
// the reason and an empty command list — never a partial load. The reader
// validates format, not semantics: config sanity (anchor > 0, pool size) is
// the consumer's problem, checked where the engine is constructed.
inline ReplayReadResult read_replay(std::istream& in) {
  ReplayReadResult res;
  ReplayHeader hdr{};
  in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  if (in.gcount() != static_cast<std::streamsize>(sizeof(hdr))) {
    res.error = ReplayReadError::kTruncatedHeader;
    return res;
  }
  if (hdr.magic != kReplayMagic) {
    res.error = ReplayReadError::kBadMagic;
    return res;
  }
  if (hdr.version != kReplayFormatVersion) {
    res.error = ReplayReadError::kUnsupportedVersion;
    return res;
  }
  res.config = hdr.config;

  // command_count is untrusted input: grow in chunks while bytes actually
  // arrive instead of handing a corrupt count to one giant resize — a bogus
  // huge count degrades into kTruncatedBody, not an allocation blowup.
  constexpr std::uint64_t kChunkCommands = 4096;
  std::uint64_t remaining = hdr.command_count;
  while (remaining > 0) {
    const std::uint64_t take = std::min(remaining, kChunkCommands);
    const std::size_t old_size = res.commands.size();
    res.commands.resize(old_size + static_cast<std::size_t>(take));
    const auto want_bytes = static_cast<std::streamsize>(take * sizeof(Command));
    in.read(reinterpret_cast<char*>(res.commands.data() + old_size), want_bytes);
    if (in.gcount() != want_bytes) {
      res.error = ReplayReadError::kTruncatedBody;
      res.commands.clear();
      return res;
    }
    remaining -= take;
  }

  // Strict v1: exactly header + body. peek() sets eofbit iff we are at the
  // end; anything else is unaccounted-for data.
  in.peek();
  if (!in.eof()) {
    res.error = ReplayReadError::kTrailingBytes;
    res.commands.clear();
    return res;
  }
  return res;
}

}  // namespace lob

#endif  // LOB_REPLAY_FORMAT_H_
