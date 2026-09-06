// tailer.cpp - the fixed-file-list ISource implementation: delegates the
// per-file tail work to the shared TailEngine and drives one polling loop
// (truncation checks, reads, offset checkpoints) on the producer thread.

#include "tailer.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "util.h"

namespace logpipe {

Tailer::Tailer(std::vector<std::filesystem::path> paths, BlockingQueue<RawLine>& queue,
               const TailOptions& options)
    : engine_(queue, options), options_(options) {
  for (const auto& path : paths) {
    FileSourceSpec spec;
    spec.path = path;
    engine_.add_file(spec);
  }
}

Tailer::Tailer(std::vector<FileSourceSpec> specs, BlockingQueue<RawLine>& queue,
               const TailOptions& options)
    : engine_(queue, options), options_(options) {
  for (const auto& spec : specs) {
    engine_.add_file(spec);
  }
}

void Tailer::start() {
  // At most one producer thread per instance; a second start() is a no-op.
  if (started_.exchange(true, std::memory_order_relaxed)) return;
  thread_ = std::thread([this] { run(); });
}

void Tailer::join() {
  if (thread_.joinable()) thread_.join();
}

void Tailer::run() {
  util::log_info("tailer: started, " + std::to_string(engine_.file_count()) +
                 " input file(s)");
  const int64_t save_interval_ms =
      static_cast<int64_t>(std::max(options_.offset_save_sec, 1)) * 1000;
  int64_t last_save_ms = util::steady_now_ms();

  while (!stop_.load(std::memory_order_relaxed)) {
    engine_.poll_all(stop_);
    if (!options_.offset_file.empty() &&
        util::steady_now_ms() - last_save_ms >= save_interval_ms) {
      engine_.save_offsets();
      last_save_ms = util::steady_now_ms();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.poll_ms));
  }

  engine_.save_offsets();  // final checkpoint, best effort
  engine_.close_queue();   // end-of-stream for the consumer
  util::log_info("tailer: stopped");
  done_.store(true, std::memory_order_relaxed);
}

}  // namespace logpipe
