// tail_engine.h - the shared tail-mode engine behind the file-family input
// sources (Tailer with a fixed file list, GlobSource with pattern discovery).
//
// The engine owns the per-file state (open stream, checkpointed offset,
// pending partial line), splits read blocks into complete lines and pushes
// RawLine values onto the shared bounded queue. It supports a dynamic file
// set: add_file()/remove_file() let the glob source register and retire
// matched files between poll rounds. All methods are meant to be driven from
// a single producer thread (the owning source's thread), except that the
// constructor may add files before the thread starts.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pipeline.h"
#include "index_sidecar.h"

namespace logpipe {

// Source-level metadata fields injected into every RawLine the source emits
// (source.<n>.tags; see config.h).
using SourceTags = std::map<std::string, std::string>;

// One configured input file with its tags.
struct FileSourceSpec {
  std::filesystem::path path;
  SourceTags tags;
};

struct TailOptions {
  int poll_ms = 500;                  // delay between polls
  std::filesystem::path offset_file;  // empty = do not persist offsets
  int offset_save_sec = 10;           // periodic checkpoint interval
  // read.chunk_bytes: bytes per read() while tailing (config-driven, bounds
  // checked in config.cpp); non-positive values fall back to the default.
  int chunk_bytes = 8192;
  // Suffix for the temporary file used by the atomic offset checkpoint. Two
  // engines sharing one offset_file (Tailer + GlobSource) must use different
  // suffixes so their concurrent writes cannot clobber each other.
  std::string tmp_suffix = ".tmp";
  // replay.index_interval_bytes: cadence of the per-file sidecar line index
  // ("<file>.idx"): one [offset -> last line timestamp] entry every N bytes.
  // 0 disables the sidecar index (and with it the fast replay positioning).
  uint64_t index_interval_bytes = 4096;
  // replay.since: epoch ms of the configured replay start (0 = replay off).
  // When > 0, begin_replay() positions every file at the sidecar entry whose
  // line timestamp is the closest one at or before the requested time and
  // skips forward to the first line whose in-line timestamp is >= the value
  // (that line is the first one emitted; see poll_file).
  int64_t replay_since_ms = 0;
};

// Per-file tail state and the line-splitting/push loop, shared by Tailer and
// GlobSource. A file that shrank below its recorded offset is re-read from
// the start; missing files are reopened on each poll round. Offsets count
// only complete lines, so a crash never loses an unterminated line.
class TailEngine {
 public:
  TailEngine(BlockingQueue<RawLine>& queue, const TailOptions& options);

  // Registers a file for tailing. Returns false (and changes nothing) when
  // the normalized path is already registered.
  bool add_file(const FileSourceSpec& spec);

  // Drops a file from the set (closing its stream). Returns the number of
  // removed entries (0 when the path was not registered).
  size_t remove_file(const std::filesystem::path& path);

  size_t file_count() const;

  // Normalized identities of the registered files (state-file keys).
  std::vector<std::string> file_keys() const;

  // Resume support. load_offsets() applies the state file written by a
  // previous run; save_offsets() checkpoints the current positions.
  void load_offsets();
  void save_offsets();

  // One poll round over every registered file: truncation check + read.
  // `stop` short-circuits the round (and the blocking push retries).
  // The round also carries the sidecar index save schedule: when an index is
  // maintained and the save interval (offset_save_sec) has elapsed, every
  // dirty sidecar is flushed to its "<file>.idx" file.
  void poll_all(const std::atomic<bool>& stop);

  uint64_t dropped_lines() const { return dropped_.load(std::memory_order_relaxed); }

  // ---- sidecar line index and replay positioning ---------------------------

  // Loads every per-file sidecar index from its "<file>.idx" file (best
  // effort; files without an index simply start with an empty one).
  void load_indexes();

  // Flushes every dirty sidecar index to disk (periodic schedule inside
  // poll_all and the shutdown checkpoint in the owning source).
  void save_indexes();

  // Replay startup positioning (config: replay.since). Must be called once
  // before the first poll round, after the resume offsets were applied. For
  // every registered file:
  //   - with a usable sidecar index: binary-search the last entry whose line
  //     timestamp is <= since and start reading from that offset (counted as
  //     an index hit);
  //   - without a usable index (missing file, no entry at/before since):
  //     start from offset 0 and scan the whole file forward (counted as an
  //     index fallback).
  // In both cases the engine then skips lines until the first line whose
  // in-line timestamp is >= since; that line is the first one emitted.
  void begin_replay();

  // Replay-mode counters (Metrics: hit/fallback/skip accounting). Hits and
  // fallbacks are per file; skipped bytes only cover the bytes consumed by
  // lines that were dropped because their timestamp predates replay.since.
  uint64_t replay_index_hits() const {
    return replay_index_hits_.load(std::memory_order_relaxed);
  }
  uint64_t replay_index_fallbacks() const {
    return replay_index_fallbacks_.load(std::memory_order_relaxed);
  }
  uint64_t replay_skipped_bytes() const {
    return replay_skipped_bytes_.load(std::memory_order_relaxed);
  }

  // Kind stamped onto every emitted RawLine (Metrics per-type accounting):
  // the Tailer leaves it at File, the GlobSource switches it to Glob.
  void set_source_type(SourceType type) { source_type_ = type; }

  // Closes the shared queue (end-of-stream for the consumer). Exposed so the
  // owning source can hand the stream a clean EOF on shutdown.
  void close_queue() { queue_.close(); }

 private:
  struct FileState {
    std::filesystem::path path;
    std::string key;              // normalized path used in the state file
    SourceTags tags;              // source-level metadata for emitted lines
    std::ifstream stream;         // kept open between polls
    uint64_t offset = 0;          // bytes of complete lines already emitted
    std::string pending;          // bytes of a trailing, still incomplete line
    bool announced_missing = false;
    // Sidecar line index state ("<file>.idx"): one entry per interval mark,
    // carrying the timestamp of the most recent complete line at/before the
    // mark. Null when replay.index_interval_bytes is 0 (index disabled).
    std::unique_ptr<LineIndexSidecar> index;
    int64_t last_line_ts_ms = 0;  // in-line timestamp of the last parsed line
    uint64_t next_index_mark = 0; // next interval offset to record (0 = none)
    // Replay state: while replay_skip is set and replay_anchored is not, the
    // poll loop drops every line whose in-line timestamp is older than the
    // configured replay.since (bytes counted as replay-skipped).
    bool replay_skip = false;
    bool replay_anchored = false;
    // Set when the replay positioned the file at a sidecar mark that may sit
    // in the middle of a line: the first poll round after the open backs the
    // read position up to the start of the surrounding line (see
    // align_replay_start) so no line is split and the anchor line is never
    // lost to a mis-parsed partial read.
    bool replay_align_pending = false;
  };

  bool ensure_open(FileState& file, const std::atomic<bool>& stop);
  void handle_truncation(FileState& file);
  void poll_file(FileState& file, const std::atomic<bool>& stop);
  bool emit_line(FileState& file, const std::string& text,
                 const std::atomic<bool>& stop);
  // Records the sidecar index entry when `file.offset` reached the next
  // interval mark (no-op when the index is disabled).
  void maybe_record_index_mark(FileState& file);
  // Recomputes the next interval mark after `offset` (resume/replay setup).
  void recompute_next_mark(FileState& file);
  // Backs `file.offset` up to the start of the line surrounding the current
  // position (replay index-hit setup): scans backwards from the mark for the
  // previous '\n' (bounded by kReplayMaxBackscan, BOF otherwise) and seeks
  // there. Guaranteed no-op for marks that already sit on a line boundary.
  void align_replay_start(FileState& file);

  std::vector<FileState> files_;
  BlockingQueue<RawLine>& queue_;
  TailOptions options_;
  SourceType source_type_ = SourceType::File;
  std::atomic<uint64_t> dropped_{0};
  // Replay accounting (see the public accessors above).
  std::atomic<uint64_t> replay_index_hits_{0};
  std::atomic<uint64_t> replay_index_fallbacks_{0};
  std::atomic<uint64_t> replay_skipped_bytes_{0};
  // Sidecar index save schedule inside poll_all (monotonic clock via util).
  int64_t last_index_save_ms_ = 0;
};

// Canonical identity of a file for the offset state file.
std::string normalize_file_key(const std::filesystem::path& path);

}  // namespace logpipe
