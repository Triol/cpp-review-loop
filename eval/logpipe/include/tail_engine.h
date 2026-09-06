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
#include <string>
#include <vector>

#include "pipeline.h"

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
  void poll_all(const std::atomic<bool>& stop);

  uint64_t dropped_lines() const { return dropped_.load(std::memory_order_relaxed); }

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
  };

  bool ensure_open(FileState& file, const std::atomic<bool>& stop);
  void handle_truncation(FileState& file);
  void poll_file(FileState& file, const std::atomic<bool>& stop);
  bool emit_line(const FileState& file, const std::string& text,
                 const std::atomic<bool>& stop);

  std::vector<FileState> files_;
  BlockingQueue<RawLine>& queue_;
  TailOptions options_;
  SourceType source_type_ = SourceType::File;
  std::atomic<uint64_t> dropped_{0};
};

// Canonical identity of a file for the offset state file.
std::string normalize_file_key(const std::filesystem::path& path);

}  // namespace logpipe
