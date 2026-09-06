// config.h - key=value configuration file for logpipe.

#pragma once

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dsl.h"       // FilterExpr (compiled filter.expr)
#include "pipeline.h"  // Level, OutputFormat, CompressMode
#include "transform.h" // TransformStep, TransformPosition
#include "util.h"      // DiagLevel
#include "writer.h"    // CompressMode

namespace logpipe {

// One named output of the fan-out group (requirement: multiple named outputs
// with independent file/format/compress/filter/level/transform settings).
// Declared via the output.<N>.name / output.<N>.file / ... config keys; the
// indices must start at 1 and may be sparse (gaps are fine, order is by
// index). A config without any output.<N>.* key maps its legacy single-output
// settings onto exactly one default output named "default" (backward
// compatibility; see Config::load).
struct OutputConfig {
  int index = 0;  // the N from output.<N>.* (ordering key)
  std::string name;                 // unique; defaults to "default" / "out<N>"
  std::string file;                 // base file name inside output.dir
  OutputFormat format = OutputFormat::Text;
  CompressMode compress = CompressMode::None;
  Level level = Level::Debug;       // per-output level threshold
  std::string filter_expr_text;     // per-output DSL expression (raw text)
  std::shared_ptr<const dsl::FilterExpr> filter_expr;  // compiled (or null)
  std::string transform_text;       // raw transform chain spec
  std::vector<TransformStep> transform;  // parsed chain (empty = none)
  TransformPosition transform_position = TransformPosition::After;
};

struct Config {
  // input.files: comma-separated list of files to tail (repeated
  // "input.file" entries are also accepted). The special entry "stdin:"
  // enables standard input as an additional source instead of a file.
  std::vector<std::filesystem::path> input_files;
  bool input_stdin = false;  // "stdin:" seen in input.files

  // Rolling output: active file is output_dir/output_file; rotation keeps at
  // most rotate_backups files of rotate_size_bytes each.
  std::filesystem::path output_dir{"out"};
  std::string output_base{"logpipe_out.log"};
  uint64_t rotate_size_bytes = 10ull * 1024 * 1024;
  int rotate_backups = 5;

  // Output line format: text (default) or json_lines (one single-line JSON
  // object per record with ts, level, source, message and crc fields).
  OutputFormat output_format = OutputFormat::Text;

  // output.compress: "none" (default) or "rle" (self-implemented run-length
  // encoding; compressed files carry a ".rle" extension).
  CompressMode output_compress = CompressMode::None;
  // Rotate the output when the calendar date changes (date-stamped names);
  // coexists with the size trigger (either one rotates).
  bool rotate_daily = false;
  // per_source_files: write each input source into its own output file
  // (file names embed a sanitized form of the source identifier).
  bool per_source_files = false;

  // Filtering: minimum level (DEBUG < INFO < WARN < ERROR) and an optional
  // case-insensitive keyword (empty disables it).
  Level level_threshold = Level::Info;
  std::string keyword;

  // filter.expr: an optional DSL expression (see dsl.h) applied as the FIRST
  // filter stage, ahead of threshold/keyword. filter_expr_text keeps the raw
  // text for diagnostics; filter_expr is the compiled form (null when the key
  // is absent or empty). Compilation happens in Config::load, so a malformed
  // expression aborts startup with a positioned dsl::Error.
  std::string filter_expr_text;
  std::shared_ptr<const dsl::FilterExpr> filter_expr;

  // Fan-out outputs (see OutputConfig). When the config file defines at least
  // one output.<N>.* key, `outputs` holds the declared group and
  // `outputs_explicit` is true (main switches to the fan-out writer). Without
  // those keys `outputs` still holds exactly one entry built from the legacy
  // single-output settings, so consumers can always iterate `outputs`.
  std::vector<OutputConfig> outputs;
  bool outputs_explicit = false;

  // Rate limit: lines beyond max_lines_per_sec are dropped and counted
  // separately in the metrics (0 = unlimited).
  int max_lines_per_sec = 0;

  // extract.kv: run the key=value field extractor (kv_extractor.h) over every
  // ingested line. Extracted fields attach to the LogRecord (visible to the
  // DSL kv("key") syntax), feed the Metrics Top-N field-value counters and
  // drive the extraction-rate diagnostic.
  bool extract_kv = false;

  // stats.interval_sec: when > 0, the Metrics snapshot is exported in the
  // Prometheus text format (prom_stats.h) every interval into stats.dir as a
  // timestamped .prom file; 0 disables the export. stats.keep_files bounds
  // the retention (oldest exports are pruned after each write).
  int stats_interval_sec = 0;
  std::filesystem::path stats_dir{"stats"};
  int stats_keep_files = 10;

  // Tail behaviour and resume state.
  int tail_poll_ms = 500;
  std::filesystem::path offset_file{"logpipe_offsets.txt"};  // empty disables resume

  // Runtime knobs.
  int run_duration_sec = 0;  // 0 = run until interrupted
  // Graceful stop trigger: the run ends when this file appears (empty = off).
  std::filesystem::path stop_file;
  util::DiagLevel diag_level = util::DiagLevel::Info;
  std::string diag_file;  // empty = stderr

  // Parses `path`; throws std::runtime_error with a descriptive message on
  // I/O or validation problems.
  static Config load(const std::string& path);

  // One-line dump for the startup diagnostic message.
  std::string describe() const;
};

// Writes config.describe() plus a newline to `out` and returns 0. Backs the
// --dump-config command line switch (main.cpp routes it here so the behaviour
// is testable without spawning a process); the stdout variant used by the
// switch simply passes stdout.
int dump_config(std::FILE* out, const Config& config);

}  // namespace logpipe
