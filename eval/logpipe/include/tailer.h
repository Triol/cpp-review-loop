// tailer.h - tail-mode ISource over a fixed set of configured input files.
//
// The tailer is one implementation of the input source abstraction (source.h):
// it produces RawLine values through the shared TailEngine (tail_engine.h),
// which owns the per-file state, the line splitting and the offset resume
// file. The public surface used by tests and main is unchanged:
//   Tailer(paths, queue, options) / run() / request_stop() /
//   load_offsets() / save_offsets() / dropped_lines()
// plus the ISource contract (start/stop/poll/join/finished) so main can drive
// every input source uniformly.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pipeline.h"
#include "source.h"
#include "tail_engine.h"

namespace logpipe {

// Reads all configured input files in tail mode and pushes complete lines
// onto a BlockingQueue consumed by the main thread; one Tailer serves every
// input file, either driven by run() on a caller-supplied thread (the tests
// do this) or through the ISource start()/join() pair (main.cpp).
//
// Per file the tailer remembers the offset of the last complete line and
// checkpoints it (periodically and at shutdown) to a state file, so a
// restarted process resumes where it left off. Missing files are reopened
// on each poll; a file that shrank below its recorded offset is re-read
// from the start. run() closes the queue on exit (clean end-of-stream).
class Tailer : public ISource {
 public:
  // Plain file list without source tags (test-compatible constructor).
  Tailer(std::vector<std::filesystem::path> paths, BlockingQueue<RawLine>& queue,
         const TailOptions& options);
  // Full form: per-file source tags (source.<n>.tags) ride along as
  // FileSourceSpec entries and are injected into every emitted RawLine.
  Tailer(std::vector<FileSourceSpec> specs, BlockingQueue<RawLine>& queue,
         const TailOptions& options);

  // Blocking main loop; returns shortly after request_stop() is called.
  void run();

  void request_stop() override { stop_.store(true, std::memory_order_relaxed); }

  // Resume support. load_offsets() applies the state file written by a
  // previous run; save_offsets() checkpoints the current positions.
  void load_offsets() { engine_.load_offsets(); }
  void save_offsets() { engine_.save_offsets(); }

  uint64_t dropped_lines() const override {
    return engine_.dropped_lines();
  }

  // ---- ISource contract (source.h) ----------------------------------------
  SourceType type() const override { return SourceType::File; }
  std::string name() const override { return "tailer"; }
  // Spawns the producer thread running run(); at most once per instance.
  void start() override;
  // Everything is driven from the producer thread; no extra periodic work.
  void poll() override {}
  // Waits for the thread spawned by start() (no-op when it never started).
  void join() override;
  // True once run() finished (after a stop request or a fatal exit).
  bool finished() const override { return done_.load(std::memory_order_relaxed); }

  // Number of files currently registered for tailing (diagnostics/tests).
  size_t file_count() const { return engine_.file_count(); }

 private:
  TailEngine engine_;
  TailOptions options_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> done_{false};
  std::thread thread_;
};

}  // namespace logpipe
