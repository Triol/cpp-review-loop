// writer.h - rolling output file writer with per-line CRC32 stamps.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "pipeline.h"

namespace logpipe {

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
// util timestamp. Single-threaded use.
class RollingWriter {
 public:
  RollingWriter(std::filesystem::path output_dir, std::string base_name,
                uint64_t max_bytes_per_file, int max_backups, OutputFormat format);

  // Creates the output directory and the first active file. If an active
  // file from a previous run exists it is rotated into the backup chain so
  // no data is overwritten. Returns false on unrecoverable errors.
  bool open();

  // Formats and appends `rec`; sets `bytes_written` to the number of bytes
  // appended (including the newline). Returns false on I/O failure.
  bool write(const LogRecord& rec, uint64_t& bytes_written);

  void close();

  // True when close() detected that flushing the pending output failed
  // (data may have been lost).
  bool failed() const { return failed_; }

 private:
  bool open_active();
  bool rotate();
  std::string format_line(const LogRecord& rec) const;
  std::string format_json_line(const LogRecord& rec) const;
  std::filesystem::path active_path() const;
  std::filesystem::path backup_path(int index) const;

  std::filesystem::path dir_;
  std::string base_;
  std::string stem_;  // base without extension, e.g. "logpipe_out"
  std::string ext_;   // extension including the dot, e.g. ".log"
  uint64_t max_bytes_;
  int max_backups_;
  OutputFormat format_;
  std::ofstream out_;
  uint64_t file_bytes_ = 0;   // bytes written to the current active file
  int rotations_ = 0;
  bool failed_ = false;
};

}  // namespace logpipe
