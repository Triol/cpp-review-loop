// writer.cpp - implementation of the rolling output writer: backup-chain
// shifting, CRC32 stamping via util, timestamped fallback rotation names.

#include "writer.h"

#include <cstdio>
#include <sstream>
#include <system_error>

#include "util.h"

namespace logpipe {

namespace {

// JSON string escaping (self-implemented, no external library): quotes,
// backslash and the short control escapes verbatim; other control bytes as
// \u00XX. Bytes >= 0x80 pass through unchanged (assumed UTF-8).
std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04X", uc);
          out += escape;
        } else {
          out += c;
        }
      }
    }
  }
  return out;
}

}  // namespace

RollingWriter::RollingWriter(std::filesystem::path output_dir, std::string base_name,
                             uint64_t max_bytes_per_file, int max_backups,
                             OutputFormat format)
    : dir_(std::move(output_dir)),
      base_(std::move(base_name)),
      max_bytes_(max_bytes_per_file),
      max_backups_(max_backups),
      format_(format) {
  const std::filesystem::path base_path(base_);
  stem_ = base_path.stem().string();
  ext_ = base_path.extension().string();
  if (stem_.empty()) {  // degenerate base such as ".log"
    stem_ = base_;
    ext_.clear();
  }
}

std::filesystem::path RollingWriter::active_path() const { return dir_ / base_; }

std::filesystem::path RollingWriter::backup_path(int index) const {
  return dir_ / (stem_ + "_" + std::to_string(index) + ext_);
}

bool RollingWriter::open() {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    util::log_error("writer: cannot create output directory '" + dir_.string() +
                    "': " + ec.message());
    return false;
  }

  // Preserve whatever a previous run left in the active file.
  ec.clear();
  const uintmax_t existing = std::filesystem::file_size(active_path(), ec);
  if (!ec && existing > 0) {
    util::log_info("writer: found previous output file, rotating it into the backup chain");
    if (!rotate()) return false;
  }
  return open_active();
}

bool RollingWriter::open_active() {
  if (out_.is_open()) out_.close();
  out_.clear();
  out_.open(active_path(), std::ios::app | std::ios::binary);
  if (!out_.is_open()) {
    util::log_error("writer: cannot open output file '" + active_path().string() + "'");
    return false;
  }
  file_bytes_ = 0;  // rotation always hands us a fresh file in append mode
  util::log_info("writer: active output file '" + active_path().string() + "'");
  return true;
}

bool RollingWriter::rotate() {
  out_.close();
  out_.clear();

  // 1. drop the oldest backup, 2. shift the chain by one, 3. archive the
  // active file as the new _1.
  std::error_code ec;
  if (max_backups_ >= 1) {
    std::filesystem::remove(backup_path(max_backups_), ec);  // may not exist: fine
  }
  for (int index = max_backups_ - 1; index >= 1; --index) {
    const std::filesystem::path from = backup_path(index);
    ec.clear();
    std::filesystem::rename(from, backup_path(index + 1), ec);
    std::error_code exists_ec;
    if (ec && std::filesystem::exists(from, exists_ec)) {
      util::log_error("writer: cannot shift backup '" + from.string() + "': " + ec.message());
      return false;
    }
  }
  ec.clear();
  std::filesystem::rename(active_path(), backup_path(1), ec);
  if (ec) {
    // Fall back to a timestamped name (time obtained through the util module)
    // so one stuck backup cannot wedge the whole pipeline.
    const std::filesystem::path fallback =
        dir_ / (stem_ + "_1_" + util::timestamp_compact(util::now_ms()) + ext_);
    ec.clear();
    std::filesystem::rename(active_path(), fallback, ec);
    if (ec) {
      util::log_error("writer: cannot archive active output file: " + ec.message());
      return false;
    }
    util::log_warn("writer: used fallback name '" + fallback.string() + "'");
  }
  ++rotations_;
  util::log_info("writer: rotated output (" + std::to_string(rotations_) +
                 " rotation(s) so far)");
  return true;
}

std::string RollingWriter::format_line(const LogRecord& rec) const {
  std::ostringstream body;
  body << rec.timestamp_text << " [" << level_name(rec.level) << "] [" << rec.source << "] "
       << rec.message;
  const std::string text = body.str();
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), " |crc=%08X", util::crc32(text));
  return text + suffix;
}

std::string RollingWriter::format_json_line(const LogRecord& rec) const {
  // Canonical serialization order ts, level, source, message; the CRC-32
  // covers this exact prefix so a consumer can recompute it by rebuilding
  // the object without the "crc" field.
  const std::string body =
      "{\"ts\":\"" + json_escape(rec.timestamp_text) +
      "\",\"level\":\"" + level_name(rec.level) +
      "\",\"source\":\"" + json_escape(rec.source) +
      "\",\"message\":\"" + json_escape(rec.message) + "\"";
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), ",\"crc\":%u}", util::crc32(body));
  return body + suffix;
}

bool RollingWriter::write(const LogRecord& rec, uint64_t& bytes_written) {
  const std::string line =
      format_ == OutputFormat::JsonLines ? format_json_line(rec) : format_line(rec);
  // Rotate before the active file would exceed the size limit. A single line
  // larger than the limit is still written whole (logs must not be split).
  if (out_.is_open() && file_bytes_ > 0 && file_bytes_ + line.size() + 1 > max_bytes_) {
    if (!rotate()) return false;
    if (!open_active()) return false;
  }
  out_ << line << '\n';
  if (!out_.good()) {
    util::log_error("writer: write to '" + active_path().string() + "' failed");
    return false;
  }
  const uint64_t written = line.size() + 1;
  file_bytes_ += written;
  bytes_written = written;
  return true;
}

void RollingWriter::close() {
  if (out_.is_open()) {
    out_.flush();
    if (!out_) {
      failed_ = true;
      util::log_error("writer: flush on close failed, data may be lost");
    }
    out_.close();
  }
}

}  // namespace logpipe
