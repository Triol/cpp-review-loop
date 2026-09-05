// main.cpp - logpipe entry point: configuration, thread wiring, the main
// processing loop (parse -> filter -> write) and graceful shutdown.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#include "config.h"
#include "pipeline.h"
#include "stdin_reader.h"
#include "tailer.h"
#include "util.h"
#include "writer.h"

namespace {

std::atomic<bool> g_stop{false};

// Handlers may only touch async-signal-safe state: store a flag, let the
// main loop react.
extern "C" void handle_stop_signal(int) {
  g_stop.store(true, std::memory_order_relaxed);
}

// True when the given path currently exists (I/O errors count as "not there").
bool path_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace logpipe;
  using PopResult = BlockingQueue<RawLine>::PopResult;

  util::init_diag(util::DiagLevel::Info, "");  // stderr until the config is loaded
  if (argc > 2) {
    util::log_error("usage: logpipe [config-file]");
    return 2;
  }
  const std::string config_path = (argc == 2) ? argv[1] : "logpipe.conf";

  Config config;
  try {
    config = Config::load(config_path);
  } catch (const std::exception& error) {
    util::log_error(std::string("logpipe: cannot start: ") + error.what());
    return 2;
  }
  util::init_diag(config.diag_level, config.diag_file);  // apply configured sink/verbosity
  util::log_info("logpipe: starting (config: " + config_path + ")");
  util::log_debug("logpipe: config: " + config.describe());

  const int64_t start_ms = util::steady_now_ms();  // elapsed-time measurement (monotonic)
  const int64_t duration_limit_ms = static_cast<int64_t>(config.run_duration_sec) * 1000;

  // Wire the pipeline: reader threads -> queue -> main thread (this one).
  BlockingQueue<RawLine> queue(1024);
  LogParser parser;
  LogFilter filter(config.level_threshold, config.keyword);
  RateLimiter limiter(config.max_lines_per_sec);
  Metrics metrics;
  RollingWriter writer(config.output_dir, config.output_base, config.rotate_size_bytes,
                       config.rotate_backups, config.output_format);
  if (!writer.open()) {
    util::log_error("logpipe: cannot start: output file is not writable");
    return 3;
  }

  TailOptions options;
  options.poll_ms = config.tail_poll_ms;
  options.offset_file = config.offset_file;
  Tailer tailer(config.input_files, queue, options);
  tailer.load_offsets();  // resume where a previous run stopped

  // stdin runs on its own reader thread so the file tailer keeps polling
  // while standard input blocks waiting for its next line. When no files
  // are configured the stdin reader is the only producer and therefore
  // closes the queue at its end of stream.
  StdinReader stdin_reader(queue, /*close_queue_on_exit=*/config.input_files.empty());

  std::signal(SIGINT, handle_stop_signal);
  std::signal(SIGTERM, handle_stop_signal);
  std::thread reader_thread([&tailer] { tailer.run(); });
  std::thread stdin_thread;
  if (config.input_stdin) {
    stdin_thread = std::thread([&stdin_reader] { stdin_reader.run(); });
  }

  // Main processing loop; the 200ms pop timeout keeps stop requests responsive.
  uint64_t write_failures = 0;
  std::string stop_reason;
  auto begin_shutdown = [&](const std::string& reason) {
    if (stop_reason.empty()) {
      stop_reason = reason;
      util::log_info("logpipe: stopping: " + reason);  // shutdown reason for the diag log
    }
    tailer.request_stop();
    stdin_reader.request_stop();
  };

  for (;;) {
    if (g_stop.load(std::memory_order_relaxed)) {
      begin_shutdown("stop signal received");
    } else if (duration_limit_ms > 0 &&
               util::steady_now_ms() - start_ms >= duration_limit_ms) {
      begin_shutdown("configured run duration reached");
    } else if (!config.stop_file.empty() && path_exists(config.stop_file)) {
      begin_shutdown("stop file '" + config.stop_file.string() + "' detected");
    }

    RawLine raw;
    const PopResult popped = queue.pop_for(raw, 200);
    if (popped == PopResult::Got) {
      const LogRecord record = parser.parse(raw.source, raw.ingested_ms, raw.text);
      metrics.record_input(record.level);
      if (limiter.allow(util::now_ms())) {
        metrics.record_source_line(record.source);
        if (filter.passes(record)) {
          uint64_t bytes = 0;
          if (writer.write(record, bytes)) {
            metrics.record_written(bytes);
          } else {
            ++write_failures;
            break;  // disk trouble: stop cleanly and keep what we have
          }
        } else {
          metrics.record_filtered();
        }
      } else {
        metrics.record_rate_dropped();  // over rate: line dropped here
      }
    } else if (popped == PopResult::Closed) {
      break;  // reader thread(s) finished and the queue is fully drained
    }
    // PopResult::Timeout: loop around and re-evaluate the exit conditions.
  }

  tailer.request_stop();  // no-op if already stopping
  stdin_reader.request_stop();
  if (stdin_thread.joinable()) stdin_thread.join();
  reader_thread.join();
  queue.close();  // safety net; the readers normally close it on their own
  if (tailer.dropped_lines() > 0) {
    util::log_debug("logpipe: tailer dropped " + std::to_string(tailer.dropped_lines()) +
                    " pending line(s) at shutdown");
  }
  if (config.input_stdin && stdin_reader.dropped_lines() > 0) {
    util::log_debug("logpipe: stdin reader dropped " + std::to_string(stdin_reader.dropped_lines()) +
                    " pending line(s) at shutdown");
  }
  if (config.input_stdin && stdin_reader.eof()) {
    util::log_debug("logpipe: stdin source reached end of stream");
  }
  writer.close();
  if (writer.failed()) ++write_failures;  // flush on close lost data: report exit code 4

  if (write_failures > 0) {
    util::log_error("logpipe: stopped after " + std::to_string(write_failures) +
                    " output write failure(s)");
  } else {
    util::log_info(stop_reason.empty()
                       ? "logpipe: run finished"
                       : "logpipe: run finished (stop reason: " + stop_reason + ")");
  }

  // Diagnostics go through the util module; this summary is the program's
  // primary report and therefore goes to stdout.
  std::cout << metrics.summary(start_ms) << std::flush;
  return write_failures > 0 ? 4 : 0;
}
