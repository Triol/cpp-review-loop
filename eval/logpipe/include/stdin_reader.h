// stdin_reader.h - standard-input line source feeding the shared pipeline.

#pragma once

#include <atomic>
#include <cstdint>

#include "pipeline.h"

namespace logpipe {

// Reads standard input line by line and pushes the lines onto the same
// BlockingQueue the Tailer feeds, so file and stdin sources share one
// parsing pipeline. Runs on its own std::thread (run()) alongside the
// tailer thread; lines carry the source name "stdin".
//
// The loop ends on request_stop(), on queue close, or at end-of-stream
// (EOF / closed pipe). When stdin is the only configured source it also
// closes the queue on exit so the main loop sees a clean end-of-stream
// (the Tailer, which normally closes the queue, has nothing to tail then).
// Note: shutdown waits for the current blocking getline to yield a line or
// EOF; feeding from a pipe or redirected file makes that immediate.
class StdinReader {
 public:
  // `close_queue_on_exit` must be true when no file sources are configured.
  StdinReader(BlockingQueue<RawLine>& queue, bool close_queue_on_exit);

  // Blocking main loop; returns after request_stop(), queue close or EOF.
  void run();

  void request_stop() { stop_.store(true, std::memory_order_relaxed); }

  // True when the loop ended because standard input reached its end.
  bool eof() const { return eof_.load(std::memory_order_relaxed); }

  // Lines given up on while the queue stayed full during shutdown.
  uint64_t dropped_lines() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  BlockingQueue<RawLine>& queue_;
  bool close_queue_on_exit_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> eof_{false};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace logpipe
