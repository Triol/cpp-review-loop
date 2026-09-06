// main.cpp - logpipe entry point: configuration, thread wiring, the main
// processing loop (parse -> filter -> write) and graceful shutdown.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include "config.h"
#include "dsl.h"
#include "kv_extractor.h"
#include "pipeline.h"
#include "prom_stats.h"
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

  // Command line: logpipe [config-file] [--dump-config] [--check-config]
  // [--no-env]. Flags may appear before or after the config path;
  // --dump-config loads the configuration (so validation errors still abort
  // with exit code 2), prints describe() to stdout and exits 0 without
  // touching any input or output file. --check-config additionally compiles
  // every DSL expression, then prints the per-key report table and exits 0.
  // --no-env disables the LOGPIPE_<KEY> environment override layer.
  bool dump_only = false;
  bool check_only = false;
  bool use_env = true;
  std::string config_path = "logpipe.conf";
  bool config_path_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dump-config") {
      dump_only = true;
    } else if (arg == "--check-config") {
      check_only = true;
    } else if (arg == "--no-env") {
      use_env = false;
    } else if (!config_path_seen) {
      config_path = arg;
      config_path_seen = true;
    } else {
      util::log_error(
          "usage: logpipe [config-file] [--dump-config] [--check-config] [--no-env]");
      return 2;
    }
  }

  Config config;
  try {
    LoadOptions load_options;
    load_options.use_env = use_env;
    config = Config::load(config_path, load_options);
  } catch (const std::exception& error) {
    util::log_error(std::string("logpipe: cannot start: ") + error.what());
    return 2;
  }

  if (check_only) {
    // Full load/validation/DSL compilation already happened above; print the
    // per-key effective-value report and exit without touching any file.
    const int rc = check_config_report(stdout, config_path, config);
    if (rc != 0) {
      util::log_error("logpipe: --check-config could not write to stdout");
      return 5;
    }
    return 0;
  }

  if (dump_only) {
    const int rc = dump_config(stdout, config);
    if (rc != 0) {
      util::log_error("logpipe: --dump-config could not write to stdout");
      return 5;
    }
    return 0;
  }

  util::init_diag(config.diag_level, config.diag_file);  // apply configured sink/verbosity
  util::log_info("logpipe: starting (config: " + config_path + ")");
  util::log_debug("logpipe: config: " + config.describe());
  // Report the DSL filter state explicitly: it is the first filter stage and
  // changes which lines survive (demo output mirrors this in the summary log).
  if (config.filter_expr) {
    util::log_info("logpipe: filter DSL enabled: " + config.filter_expr->text());
  } else {
    util::log_info("logpipe: filter DSL disabled (threshold/keyword only)");
  }

  const int64_t start_ms = util::steady_now_ms();  // elapsed-time measurement (monotonic)
  const int64_t duration_limit_ms = static_cast<int64_t>(config.run_duration_sec) * 1000;

  // Wire the pipeline: reader threads -> queue -> main thread (this one).
  // queue.capacity is config-driven (bounds-checked in config.cpp).
  BlockingQueue<RawLine> queue(static_cast<size_t>(config.queue_capacity));
  LogParser parser;
  // The compiled filter.expr (if any) is wired in as the first filter stage;
  // threshold and keyword keep their existing behaviour after it.
  LogFilter::PreFilter dsl_stage;
  if (config.filter_expr) {
    const std::shared_ptr<const dsl::FilterExpr> expr = config.filter_expr;
    dsl_stage = [expr](const LogRecord& rec) { return expr->passes(rec); };
  }
  LogFilter filter(config.level_threshold, config.keyword, std::move(dsl_stage));
  RateLimiter limiter(config.max_lines_per_sec);
  Metrics metrics;

  // Optional KV field extraction (extract.kv = true): runs after the rate
  // limiter and before the filter so the DSL kv("key") syntax can use the
  // extracted fields, and so only admitted lines are counted.
  KvExtractor kv_extractor;
  const bool kv_enabled = config.extract_kv;
  if (kv_enabled) {
    util::log_info("logpipe: KV field extraction enabled");
  }

  // Optional periodic Prometheus statistics export (stats.interval_sec > 0).
  PromStatsOptions stats_options;
  stats_options.dir = config.stats_dir;
  stats_options.keep_files = config.stats_keep_files;
  PromStatsWriter stats_writer(stats_options);
  const int64_t stats_interval_ms =
      static_cast<int64_t>(config.stats_interval_sec) * 1000;
  int64_t next_stats_ms = stats_interval_ms > 0
                              ? util::now_ms() + stats_interval_ms
                              : 0;
  auto maybe_export_stats = [&]() {
    if (stats_interval_ms <= 0 || util::now_ms() < next_stats_ms) return;
    const auto exported = stats_writer.write(metrics.snapshot(/*kv_top_n=*/10));
    if (!exported.empty()) {
      util::log_debug("logpipe: statistics exported to " + exported.string());
    }
    next_stats_ms += stats_interval_ms;  // keep the schedule drift-free
    if (util::now_ms() > next_stats_ms) {
      // Fell far behind (e.g. a suspended process): re-anchor on now.
      next_stats_ms = util::now_ms() + stats_interval_ms;
    }
  };

  WriterOptions writer_options;
  writer_options.output_dir = config.output_dir;
  writer_options.base_name = config.output_base;
  writer_options.max_bytes_per_file = config.rotate_size_bytes;
  writer_options.max_backups = config.rotate_backups;
  writer_options.format = config.output_format;
  writer_options.compress = config.output_compress;
  writer_options.daily_rotation = config.rotate_daily;
  writer_options.per_source_files = config.per_source_files;
  // Batched write buffer (write.buffer_lines / write.buffer_bytes) and the
  // optional XOR output encryption (encrypt.password): both global, applied
  // to every output route.
  writer_options.buffer_lines = config.write_buffer_lines;
  writer_options.buffer_bytes = config.write_buffer_bytes;
  writer_options.encrypt_password = config.encrypt_password;
  std::unique_ptr<OutputSink> writer;
  if (config.outputs_explicit) {
    // Fan-out mode (requirement 1+2): one route per configured output, each
    // with its own file/format/compress/level/DSL/transform settings. The
    // rotation knobs (size, backups, daily) and the output directory stay
    // shared with the global configuration.
    std::vector<OutputRoute> routes;
    routes.reserve(config.outputs.size());
    for (const OutputConfig& entry : config.outputs) {
      OutputRoute route;
      route.name = entry.name;
      route.options = writer_options;
      route.options.base_name = entry.file;
      route.options.format = entry.format;
      route.options.compress = entry.compress;
      route.level = entry.level;
      route.filter_expr = entry.filter_expr;
      route.transform = entry.transform;
      route.transform_position = entry.transform_position;
      util::log_info("logpipe: output '" + route.name + "' -> " +
                     (config.output_dir / entry.file).string() +
                     " (transform " +
                     transform_position_name(route.transform_position) + ")");
      routes.push_back(std::move(route));
    }
    writer = std::unique_ptr<OutputSink>(new FanOutWriter(std::move(routes), &metrics));
  } else {
    writer = config.per_source_files
                 ? std::unique_ptr<OutputSink>(new PerSourceWriter(writer_options, &metrics))
                 : std::unique_ptr<OutputSink>(new RollingWriter(writer_options, &metrics));
  }
  if (!writer->open()) {
    util::log_error("logpipe: cannot start: output file is not writable");
    return 3;
  }

  TailOptions options;
  options.poll_ms = config.tail_poll_ms;
  options.offset_file = config.offset_file;
  options.chunk_bytes = config.read_chunk_bytes;  // read.chunk_bytes, pre-validated
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
      LogRecord record = parser.parse(raw.source, raw.ingested_ms, raw.text);
      metrics.record_input(record.level);
      if (limiter.allow(util::now_ms())) {
        metrics.record_source_line(record.source);
        if (kv_enabled) {
          // Extract "k=v" fields. First try the whole raw line (RAW records
          // are pure key=value payloads); for parsed records fall back to the
          // message part so timestamped lines still contribute their pairs.
          // The record keeps whatever was found (empty for lines without the
          // key=value shape).
          std::map<std::string, std::string> fields;
          bool extracted = kv_extractor.extract(raw.text, fields);
          if (!extracted && record.parsed) {
            // Message part first (strict), then tolerate a free-form prefix
            // before the "k=v" payload ("evt user=bob host=web-01").
            extracted = kv_extractor.extract(record.message, fields) ||
                        kv_extractor.extract_tail(record.message, fields);
          }
          if (extracted) {
            record.fields = std::move(fields);
            for (const auto& pair : record.fields) {
              metrics.record_kv_value(pair.first, pair.second);
            }
          }
          metrics.record_kv_line(extracted);
        }
        if (filter.passes(record)) {
          uint64_t bytes = 0;
          if (writer->write(record, bytes)) {
            if (bytes > 0) {
              // Fan-out branches count themselves via record_output_written;
              // the aggregate per-source total only counts real output bytes.
              metrics.record_written(bytes, record.source);
            }
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
    maybe_export_stats();
  }
  // Final statistics export so short runs still produce a .prom file.
  if (stats_interval_ms > 0) {
    stats_writer.write(metrics.snapshot(/*kv_top_n=*/10));
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
  writer->close();
  if (writer->failed()) ++write_failures;  // flush on close lost data: report exit code 4

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
  std::cout << metrics.summary(start_ms);
  if (kv_enabled && metrics.kv_lines_attempted() > 0) {
    // Extraction-rate diagnostic for the KV extractor (see extract.kv).
    std::cout << "kv extraction rate: " << metrics.kv_lines_extracted() << "/"
              << metrics.kv_lines_attempted() << " lines ("
              << metrics.kv_extraction_rate_percent() << "%), "
              << metrics.kv_fields_extracted() << " field(s)\n";
  }
  std::cout << "filter DSL      : "
            << (filter.dsl_active() ? "enabled" : "disabled") << "\n"
            << std::flush;
  return write_failures > 0 ? 4 : 0;
}
