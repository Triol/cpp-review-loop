// prom_stats.h - periodic Prometheus text-format statistics export.
//
// Enabled via the `stats.interval_sec` configuration key (0 = off, the
// default). Every interval the current Metrics snapshot is rendered in the
// Prometheus text exposition format and written into the stats directory as a
// timestamped "<base>_<YYYYMMDD_HHMMSS>.prom" file; only the most recent
// `stats.keep_files` files are kept, older ones are pruned after each write.
//
// Metric naming: every line carries the logpipe_ prefix. Counters use the
// _total suffix per Prometheus convention; gauges do not. Exposed metrics:
//
//   logpipe_lines_ingested_total{level="INFO"}     counter (per level)
//   logpipe_lines_ingested_total                   counter (sum)
//   logpipe_lines_filtered_total                   counter
//   logpipe_lines_written_total                    counter
//   logpipe_output_bytes_total                     counter
//   logpipe_lines_rate_dropped_total               counter
//   logpipe_compression_raw_bytes_total            counter
//   logpipe_compression_packed_bytes_total         counter
//   logpipe_output_rotations_total{trigger="size"|"day"}  counter
//   logpipe_kv_lines_attempted_total               counter
//   logpipe_kv_lines_extracted_total               counter
//   logpipe_kv_fields_extracted_total              counter
//   logpipe_kv_extraction_ratio                    gauge (0-100 percent)
//   logpipe_source_lines_total{source="..."}       counter (per input source)
//   logpipe_source_output_bytes_total{source="..."} counter (per input source)
//   logpipe_field_value_count{key="...",value="..."} counter (KV Top-N stats)
//
// The exporter itself is single-threaded (main-thread use); Metrics::snapshot
// is taken under the Metrics locks, so the rest of the pipeline may run
// concurrently.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "pipeline.h"

namespace logpipe {

// Options for the statistics writer (built from the stats.* config keys).
struct PromStatsOptions {
  std::filesystem::path dir{"stats"};     // stats.dir: export directory
  std::string base_name{"logpipe_stats"}; // file stem for the .prom files
  int keep_files = 10;                    // stats.keep_files: retention
  // Injectable wall clock (epoch ms) used for the file-name timestamp;
  // defaults to util::now_ms(). Tests inject a fixed clock.
  std::function<int64_t()> clock;
};

// Renders the Metrics snapshot as Prometheus text and writes timestamped
// .prom files with retention pruning.
class PromStatsWriter {
 public:
  // Metric-name prefix used for every exported line.
  static constexpr const char* kPrefix = "logpipe_";

  explicit PromStatsWriter(PromStatsOptions options);

  // Renders `snap` in the Prometheus text exposition format. Pure function:
  // no I/O, no member access, so tests can assert on the exact output.
  static std::string render(const Metrics::Snapshot& snap);

  // Escapes a label value per the text exposition format (backslash, double
  // quote and newline); exposed for tests.
  static std::string escape_label(const std::string& value);

  // Writes one .prom file for `snap` (timestamped via the injected clock),
  // creates the stats directory when missing and prunes old exports beyond
  // options.keep_files. Returns the path written, or an empty path on failure
  // (failure details go through the util diagnostics; the run is never aborted
  // by a statistics hiccup).
  std::filesystem::path write(const Metrics::Snapshot& snap);

  // True when the last write() failed (directory creation or file I/O).
  bool failed() const { return failed_; }

  // Number of .prom files currently kept in the stats directory (after the
  // last prune). Test hook.
  size_t kept_files() const;

 private:
  // Deletes the oldest exports beyond keep_files; returns false when the
  // directory scan itself failed (individual deletions only log a warning).
  bool prune_old_files();

  PromStatsOptions options_;
  bool failed_ = false;
};

}  // namespace logpipe
