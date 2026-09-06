// glob_source.h - the pattern-discovery ISource: periodically matches a
// filename glob ("glob:/var/log/app/*.log") against the filesystem, adds
// every newly matched file to the shared TailEngine and retires files that
// disappeared. See source.h for the ISource contract.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "pipeline.h"
#include "source.h"
#include "tail_engine.h"

namespace logpipe {

// Options for a GlobSource; mirrors the tail knobs (the glob source tails
// exactly like the fixed-list Tailer, it only discovers files dynamically).
struct GlobOptions {
  int poll_ms = 500;                  // discovery + read cadence
  std::filesystem::path offset_file;  // empty = do not persist offsets
  int offset_save_sec = 10;           // periodic checkpoint interval
  int chunk_bytes = 8192;             // bytes per read() while tailing
  // glob.exclude patterns (config: comma-separated). A candidate matching
  // any exclude pattern (glob against the file name or the full path) is
  // never tailed. Exclude checks run after the include match.
  std::vector<std::string> exclude;
};

// Pattern-discovering tail source. The pattern is "<dir>/<mask>" where the
// mask (the last path component) may contain '*' and '?' wildcards; the
// directory part must be literal. Every poll round the source:
//   1. scans the directory for new matches and registers them (missing
//      directories are retried, a late-created directory is picked up),
//   2. drops registrations whose file no longer exists,
//   3. runs one tail round over every registered file.
// Discovered files start at their current end (tail semantics) unless a
// resume offset exists in the shared offset state file. Emitted RawLines
// carry the source tags and SourceType::Glob for the per-type Metrics.
class GlobSource : public ISource {
 public:
  // `close_queue_on_exit` must be true when this is the only configured
  // source (it then hands the consumer the end-of-stream on shutdown).
  GlobSource(std::string pattern, SourceTags tags, BlockingQueue<RawLine>& queue,
             const GlobOptions& options, bool close_queue_on_exit = false);

  // Blocking main loop; returns shortly after request_stop().
  void run();

  void request_stop() override { stop_.store(true, std::memory_order_relaxed); }

  uint64_t dropped_lines() const override { return engine_.dropped_lines(); }

  // ---- ISource contract (source.h) ----------------------------------------
  SourceType type() const override { return SourceType::Glob; }
  std::string name() const override { return "glob:" + pattern_; }
  void start() override;
  // One discovery + tail round; driven from the producer thread's loop, but
  // part of the ISource contract (external schedulers may call it too).
  void poll() override;
  void join() override;
  bool finished() const override { return done_.load(std::memory_order_relaxed); }

  // Number of currently registered (matched and still existing) files.
  size_t active_files() const { return engine_.file_count(); }

  // Glob matcher: '*' matches any run of characters, '?' exactly one; all
  // other characters compare literally. Exposed for tests.
  static bool glob_match(const std::string& text, const std::string& pattern);

  // Splits "<dir>/<mask>" into the literal directory part and the wildcard
  // mask (the last path component). Exposed for tests.
  static void split_pattern(const std::string& pattern, std::filesystem::path& directory,
                            std::string& mask);

 private:
  // One discovery round: add new matches, retire vanished files. Returns the
  // number of files added (for diagnostics/tests).
  size_t discover();

  // True when `candidate` matches the include mask and no exclude pattern.
  bool matches(const std::filesystem::path& candidate) const;

  std::string pattern_;
  std::filesystem::path directory_;
  std::string mask_;
  SourceTags tags_;
  TailEngine engine_;
  GlobOptions options_;
  bool close_queue_on_exit_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> done_{false};
  std::thread thread_;
};

}  // namespace logpipe
