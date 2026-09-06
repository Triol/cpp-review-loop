// stdin_reader.h - standard-input ISource feeding the shared pipeline.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "pipeline.h"
#include "source.h"
#include "tail_engine.h"  // SourceTags

namespace logpipe {

// Reads standard input line by line and pushes the lines onto the same
// BlockingQueue the file sources feed, so all input sources share one
// parsing pipeline. Runs on its own std::thread (run() directly, or the
// ISource start()/join() pair); lines carry the source name "stdin" and any
// configured source tags (source.<n>.tags on the "stdin:" entry).
//
// The loop ends on request_stop(), on queue close, or at end-of-stream
// (EOF / closed pipe). When stdin is the only configured source it also
// closes the queue on exit so the main loop sees a clean end-of-stream
// (the file sources, which normally close the queue, have nothing to tail
// then). Note: shutdown waits for the current blocking getline to yield a
// line or EOF; feeding from a pipe or redirected file makes that immediate.
class StdinReader : public ISource {
 public:
  // `close_queue_on_exit` must be true when no file sources are configured.
  // `tags` (optional) is injected into every emitted RawLine.
  StdinReader(BlockingQueue<RawLine>& queue, bool close_queue_on_exit,
              SourceTags tags = {});

  // Blocking main loop; returns after request_stop(), queue close or EOF.
  void run();

  void request_stop() override { stop_.store(true, std::memory_order_relaxed); }

  // True when the loop ended because standard input reached its end.
  bool eof() const { return eof_.load(std::memory_order_relaxed); }

  // Lines given up on while the queue stayed full during shutdown.
  uint64_t dropped_lines() const override { return dropped_.load(std::memory_order_relaxed); }

  // ---- ISource contract (source.h) ----------------------------------------
  SourceType type() const override { return SourceType::Stdin; }
  std::string name() const override { return "stdin"; }
  // Spawns the producer thread running run(); at most once per instance.
  void start() override;
  // Everything is driven from the producer thread; no extra periodic work.
  void poll() override {}
  // Waits for the thread spawned by start() (no-op when it never started).
  void join() override;
  // True once the loop exited (EOF reached or stop processed).
  bool finished() const override {
    return eof_.load(std::memory_order_relaxed) || done_.load(std::memory_order_relaxed);
  }

 private:
  BlockingQueue<RawLine>& queue_;
  bool close_queue_on_exit_;
  SourceTags tags_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> eof_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> done_{false};
  std::atomic<uint64_t> dropped_{0};
  std::thread thread_;
};

}  // namespace logpipe
