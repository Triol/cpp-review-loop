// config.h - key=value configuration file for logpipe.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dsl.h"       // FilterExpr (compiled filter.expr)
#include "pipeline.h"  // Level, OutputFormat, CompressMode
#include "util.h"      // DiagLevel
#include "writer.h"    // CompressMode

namespace logpipe {

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

  // Rate limit: lines beyond max_lines_per_sec are dropped and counted
  // separately in the metrics (0 = unlimited).
  int max_lines_per_sec = 0;

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

}  // namespace logpipe
