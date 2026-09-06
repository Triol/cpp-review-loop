// tailer.h - tail-mode reader for the configured input files.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pipeline.h"

namespace logpipe {

struct TailOptions {
  int poll_ms = 500;                  // delay between polls
  std::filesystem::path offset_file;  // empty = do not persist offsets
  int offset_save_sec = 10;           // periodic checkpoint interval
  // read.chunk_bytes: bytes per read() while tailing (config-driven, bounds
  // checked in config.cpp); non-positive values fall back to the default.
  int chunk_bytes = 8192;
};

// Reads all configured input files in tail mode and pushes complete lines
// onto a BlockingQueue consumed by the main thread; one Tailer serves every
// input file on its own std::thread (run()).
//
// Per file the tailer remembers the offset of the last complete line and
// checkpoints it (periodically and at shutdown) to a state file, so a
// restarted process resumes where it left off. Missing files are reopened
// on each poll; a file that shrank below its recorded offset is re-read
// from the start. run() closes the queue on exit (clean end-of-stream).
class Tailer {
 public:
  Tailer(std::vector<std::filesystem::path> paths, BlockingQueue<RawLine>& queue,
         const TailOptions& options);

  // Blocking main loop; returns shortly after request_stop() is called.
  void run();

  void request_stop() { stop_.store(true, std::memory_order_relaxed); }

  // Resume support. load_offsets() applies the state file written by a
  // previous run; save_offsets() checkpoints the current positions.
  void load_offsets();
  void save_offsets();

  uint64_t dropped_lines() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  struct FileState {
    std::filesystem::path path;
    std::string key;              // normalized path used in the state file
    std::ifstream stream;         // kept open between polls
    uint64_t offset = 0;          // bytes of complete lines already emitted
    std::string pending;          // bytes of a trailing, still incomplete line
    bool announced_missing = false;
  };

  bool ensure_open(FileState& file);
  void handle_truncation(FileState& file);
  void poll_file(FileState& file);
  bool emit_line(const FileState& file, const std::string& text);

  std::vector<FileState> files_;
  BlockingQueue<RawLine>& queue_;
  TailOptions options_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace logpipe
