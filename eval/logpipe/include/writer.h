// writer.h - rolling output file writer with per-line CRC32 stamps,
// optional RLE compression, daily rotation and per-source output files.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "pipeline.h"
#include "rle.h"

namespace logpipe {

// Output compression mode: raw lines (None) or the self-implemented RLE
// container per line (Rle), selected via the output.compress config key.
enum class CompressMode { None, Rle };

// Accepts "none" and "rle" (case-insensitive).
inline bool compress_mode_from_string(const std::string& text, CompressMode& out) {
  std::string lower;
  lower.reserve(text.size());
  for (char c : text) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "none" || lower == "off") { out = CompressMode::None; return true; }
  if (lower == "rle") { out = CompressMode::Rle; return true; }
  return false;
}

inline const char* compress_mode_name(CompressMode mode) {
  return mode == CompressMode::Rle ? "rle" : "none";
}

// Aggregated options for the output stage (built from Config in main).
struct WriterOptions {
  std::filesystem::path output_dir{"out"};
  std::string base_name{"logpipe_out.log"};
  uint64_t max_bytes_per_file = 10ull * 1024 * 1024;
  int max_backups = 5;
  OutputFormat format = OutputFormat::Text;
  CompressMode compress = CompressMode::None;
  bool daily_rotation = false;    // rotate when the calendar date changes
  bool per_source_files = false;  // one output file per input source
  // Injectable wall clock (epoch ms) used for daily rotation; defaults to
  // util::now_ms(). Tests inject a fixed clock to exercise date changes.
  std::function<int64_t()> clock;
};

// Common interface implemented by both sinks (single file and per-source);
// main.cpp only talks to this interface.
class OutputSink {
 public:
  virtual ~OutputSink() = default;
  virtual bool open() = 0;
  virtual bool write(const LogRecord& rec, uint64_t& bytes_written) = 0;
  virtual void close() = 0;
  virtual bool failed() const = 0;
};

// Writes one formatted line per record into a rolling file under the output
// directory. The active file is <output_dir>/<base>; when it exceeds
// max_bytes_per_file the oldest backup is deleted, backups shift
// (logpipe_out_1.log -> _2.log, ...) and the active file becomes the new
// logpipe_out_1.log; at most max_backups are kept. Text format writes
//   <timestamp> [LEVEL] [source] message |crc=XXXXXXXX
// where the CRC-32 (via the util module) covers everything before " |crc=".
// The json_lines format writes one single-line JSON object per record,
//   {"ts":"...","level":"...","source":"...","message":"...","crc":N}
// with the CRC-32 covering the exact serialized prefix before the "crc"
// field, so consumers can recompute it. Fallback rotation names embed a
// util timestamp.
//
// Compression (CompressMode::Rle): every formatted line is wrapped in its
// own RLE container (see rle.h) and written as one length-prefixed frame,
//   [uint32 LE frame bytes][LRLE container]
// in binary mode; the active file name gains a ".rle" suffix.
//
// Daily rotation (options.daily_rotation): the active file name carries the
// current local date (<stem>_<YYYYMMDD><ext>); when the injected clock moves
// to another day the current file is closed and a new dated file starts, so
// both rotation triggers (size and day) coexist.
//
// Metrics: when a Metrics pointer is supplied, the writer reports rotations
// (by size / by day, counted separately) and the compressed/raw byte totals.
// Single-threaded use.
class RollingWriter : public OutputSink {
 public:
  explicit RollingWriter(const WriterOptions& options, Metrics* metrics = nullptr);

  // Creates the output directory and the first active file. If an active
  // file from a previous run exists it is rotated into the backup chain so
  // no data is overwritten. Returns false on unrecoverable errors.
  bool open() override;

  // Formats and appends `rec`; sets `bytes_written` to the number of bytes
  // appended (frame size for compressed output, line + newline otherwise).
  // Returns false on I/O failure.
  bool write(const LogRecord& rec, uint64_t& bytes_written) override;

  void close() override;

  // True when close() detected that flushing the pending output failed
  // (data may have been lost).
  bool failed() const override { return failed_; }

  // The name (without directory) currently used for the active file; exposed
  // for the daily-rotation naming tests.
  std::string active_file_name() const;

  // True while an output stream is open (per-source dispatch uses this).
  bool is_open() const { return out_.is_open(); }

 private:
  bool open_active();
  bool rotate();               // size-triggered rotation (backup chain)
  bool rotate_daily();         // day-change rotation (date-named archives)
  std::string date_suffix() const;  // "YYYYMMDD" from the injected clock
  std::filesystem::path active_path() const;
  std::filesystem::path backup_path(int index) const;
  std::string format_line(const LogRecord& rec) const;
  std::string format_json_line(const LogRecord& rec) const;
  std::string encode_payload(const std::string& line) const;

  WriterOptions options_;
  Metrics* metrics_ = nullptr;
  std::string stem_;   // base without extension, e.g. "logpipe_out"
  std::string ext_;    // extension including the dot, e.g. ".log" (+ ".rle")
  std::ofstream out_;
  std::string active_name_;   // current active file name (no directory)
  std::string current_date_;  // date the active file was opened under
  uint64_t file_bytes_ = 0;   // bytes written to the current active file
  bool failed_ = false;
};

// Per-source dispatcher (config per_source_files = true): keeps one
// RollingWriter per distinct input source. Each writer's base name embeds a
// sanitized form of the source (path separators and other unsafe characters
// become '_'), e.g. "logs/app.log" -> "logpipe_out_app.log". The sink-level
// Metrics still aggregate everything; per-source output bytes are recorded
// through Metrics::record_written().
class PerSourceWriter : public OutputSink {
 public:
  explicit PerSourceWriter(const WriterOptions& options, Metrics* metrics = nullptr);

  bool open() override;  // lazily opens per-source writers; always succeeds
  bool write(const LogRecord& rec, uint64_t& bytes_written) override;
  void close() override;
  bool failed() const override;

  // Number of distinct per-source files opened so far (test hook).
  size_t sink_count() const { return sinks_.size(); }

 private:
  RollingWriter& sink_for(const LogRecord& rec);

  WriterOptions options_;
  Metrics* metrics_ = nullptr;
  std::map<std::string, std::unique_ptr<RollingWriter>> sinks_;
  bool any_failed_ = false;
};

// Reads a compressed output file produced by RollingWriter (a sequence of
// length-prefixed RLE frames) and concatenates all decoded lines into `out`.
// Returns false on any malformed frame. Test/round-trip helper.
bool read_compressed_output(const std::filesystem::path& path, std::string& out);

}  // namespace logpipe
