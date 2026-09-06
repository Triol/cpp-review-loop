// writer.cpp - implementation of the rolling output writer: backup-chain
// shifting, CRC32 stamping via util, timestamped fallback rotation names,
// per-line RLE compression frames, daily rotation and per-source dispatch.

#include "writer.h"

#include <cctype>
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

// Local calendar date "YYYYMMDD" for an epoch-ms timestamp, going through the
// util module's formatting so all time handling stays in one place.
std::string date_of(int64_t epoch_ms) {
  const std::string stamp = util::timestamp_compact(epoch_ms);  // YYYYMMDD_HHMMSS
  return stamp.substr(0, 8);
}

// Turns an input source into a filename-safe fragment: the file-name part of
// a path with every non-alphanumeric character (including '.') mapped to '_',
// so the configured extension is appended exactly once.
std::string sanitize_source(const std::string& source) {
  const std::filesystem::path path(source);
  std::string name = path.filename().string();
  if (name.empty()) name = "stdin";
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    const bool safe = std::isalnum(uc) || c == '-' || c == '_';
    out.push_back(safe ? static_cast<char>(uc) : '_');
  }
  return out;
}

void append_u32_le(std::string& out, uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

bool read_u32_le(const std::string& data, size_t offset, uint32_t& value) {
  if (data.size() < offset + 4) return false;
  value = 0;
  for (int index = 3; index >= 0; --index) {
    value = (value << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(data[offset + index]));
  }
  return true;
}

}  // namespace

RollingWriter::RollingWriter(const WriterOptions& options, Metrics* metrics)
    : options_(options), metrics_(metrics) {
  const std::filesystem::path base_path(options_.base_name);
  stem_ = base_path.stem().string();
  ext_ = base_path.extension().string();
  if (stem_.empty()) {  // degenerate base such as ".log"
    stem_ = options_.base_name;
    ext_.clear();
  }
  // Compressed output always carries the .rle extension in addition to the
  // configured one so consumers can tell the encodings apart by name.
  if (options_.compress == CompressMode::Rle) ext_ += ".rle";
  if (!options_.clock) options_.clock = util::now_ms;
}

std::string RollingWriter::date_suffix() const {
  return date_of(options_.clock());
}

std::string RollingWriter::active_file_name() const { return active_name_; }

std::filesystem::path RollingWriter::active_path() const {
  return options_.output_dir / active_name_;
}

std::filesystem::path RollingWriter::backup_path(int index) const {
  // Backups chain off the current active name so size rotation works for
  // both the plain and the date-stamped naming scheme.
  const std::filesystem::path active(active_name_);
  return options_.output_dir /
         (active.stem().string() + "_" + std::to_string(index) + active.extension().string());
}

bool RollingWriter::open() {
  std::error_code ec;
  std::filesystem::create_directories(options_.output_dir, ec);
  if (ec) {
    util::log_error("writer: cannot create output directory '" +
                    options_.output_dir.string() + "': " + ec.message());
    return false;
  }

  current_date_ = date_suffix();
  if (options_.daily_rotation) {
    // The date is baked into the active name; a previous run left files with
    // older dates, which simply stay where they are.
    active_name_ = stem_ + "_" + current_date_ + ext_;
  } else {
    // stem_/ext_ already include the possible ".rle" suffix.
    active_name_ = stem_ + ext_;
    // Preserve whatever a previous run left in the active file.
    ec.clear();
    const uintmax_t existing = std::filesystem::file_size(active_path(), ec);
    if (!ec && existing > 0) {
      util::log_info("writer: found previous output file, rotating it into the backup chain");
      if (!rotate()) return false;
    }
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
  current_date_ = date_suffix();
  util::log_info("writer: active output file '" + active_path().string() + "'");
  return true;
}

bool RollingWriter::rotate() {
  out_.close();
  out_.clear();

  // 1. drop the oldest backup, 2. shift the chain by one, 3. archive the
  // active file as the new _1.
  std::error_code ec;
  if (options_.max_backups >= 1) {
    std::filesystem::remove(backup_path(options_.max_backups), ec);  // may not exist: fine
  }
  for (int index = options_.max_backups - 1; index >= 1; --index) {
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
        options_.output_dir /
        (active_name_ + "_1_" + util::timestamp_compact(util::now_ms()));
    ec.clear();
    std::filesystem::rename(active_path(), fallback, ec);
    if (ec) {
      util::log_error("writer: cannot archive active output file: " + ec.message());
      return false;
    }
    util::log_warn("writer: used fallback name '" + fallback.string() + "'");
  }
  if (metrics_ != nullptr) metrics_->record_rotation(/*by_size=*/true);
  util::log_info("writer: rotated output (size trigger)");
  return true;
}

bool RollingWriter::rotate_daily() {
  // The active file already carries its date in the name, so archiving is
  // just closing it; the next open_active() picks the new date's name.
  out_.close();
  out_.clear();
  if (metrics_ != nullptr) metrics_->record_rotation(/*by_size=*/false);
  util::log_info("writer: rotated output (day change, new date " + current_date_ + ")");
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

std::string RollingWriter::encode_payload(const std::string& line) const {
  if (options_.compress != CompressMode::Rle) return line;
  const std::string container = rle::compress(line);
  std::string frame;
  frame.reserve(container.size() + 4);
  append_u32_le(frame, static_cast<uint32_t>(container.size()));
  frame += container;
  return frame;
}

bool RollingWriter::write(const LogRecord& rec, uint64_t& bytes_written) {
  // Daily rotation first: a date change always starts a fresh file, even if
  // the size trigger would fire for the same line as well.
  if (options_.daily_rotation) {
    const std::string today = date_suffix();
    if (out_.is_open() && today != current_date_) {
      rotate_daily();
      active_name_ = stem_ + "_" + today + ext_;
      if (!open_active()) return false;
    }
  }

  const std::string line =
      options_.format == OutputFormat::JsonLines ? format_json_line(rec) : format_line(rec);
  const std::string payload = encode_payload(line);
  const uint64_t on_disk = payload.size() +
                           (options_.compress == CompressMode::Rle ? 0u : 1u /* newline */);
  // Rotate before the active file would exceed the size limit. A single line
  // larger than the limit is still written whole (logs must not be split).
  if (out_.is_open() && file_bytes_ > 0 && file_bytes_ + on_disk > options_.max_bytes_per_file) {
    if (!rotate()) return false;
    active_name_ = options_.daily_rotation ? stem_ + "_" + current_date_ + ext_
                                           : stem_ + ext_;
    if (!open_active()) return false;
  }
  if (options_.compress == CompressMode::Rle) {
    out_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  } else {
    out_ << payload << '\n';
  }
  if (!out_.good()) {
    util::log_error("writer: write to '" + active_path().string() + "' failed");
    return false;
  }
  if (metrics_ != nullptr && options_.compress == CompressMode::Rle) {
    metrics_->record_compression(line.size(), payload.size());
  }
  file_bytes_ += on_disk;
  bytes_written = on_disk;
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

// ------------------------------ PerSourceWriter ------------------------------

PerSourceWriter::PerSourceWriter(const WriterOptions& options, Metrics* metrics)
    : options_(options), metrics_(metrics) {}

bool PerSourceWriter::open() {
  // Sinks are created lazily per source in sink_for(); nothing to do here.
  return true;
}

RollingWriter& PerSourceWriter::sink_for(const LogRecord& rec) {
  auto it = sinks_.find(rec.source);
  if (it != sinks_.end()) return *it->second;
  // Per-source base name: <stem>_<sanitized source><ext>. The sanitized
  // fragment is the file-name part of the path, safe for filenames.
  const std::filesystem::path base_path(options_.base_name);
  std::string stem = base_path.stem().string();
  if (stem.empty()) stem = options_.base_name;
  WriterOptions opts = options_;
  opts.base_name = stem + "_" + sanitize_source(rec.source) + base_path.extension().string();
  auto sink = std::make_unique<RollingWriter>(opts, metrics_);
  RollingWriter& ref = *sink;
  sinks_.emplace(rec.source, std::move(sink));
  return ref;
}

bool PerSourceWriter::write(const LogRecord& rec, uint64_t& bytes_written) {
  RollingWriter& sink = sink_for(rec);
  if (!sink.is_open()) {
    if (!sink.open()) {
      any_failed_ = true;
      return false;
    }
  }
  return sink.write(rec, bytes_written);
}

void PerSourceWriter::close() {
  for (auto& entry : sinks_) entry.second->close();
}

bool PerSourceWriter::failed() const {
  if (any_failed_) return true;
  for (const auto& entry : sinks_) {
    if (entry.second->failed()) return true;
  }
  return false;
}

// ------------------------------ FanOutWriter ---------------------------------

FanOutWriter::FanOutWriter(std::vector<OutputRoute> routes, Metrics* metrics)
    : routes_(std::move(routes)), metrics_(metrics) {
  sinks_.reserve(routes_.size());
}

bool FanOutWriter::open() {
  sinks_.clear();
  sinks_.reserve(routes_.size());
  for (const OutputRoute& route : routes_) {
    // per_source_files composes with fan-out: each branch may itself be a
    // per-source dispatcher.
    std::unique_ptr<OutputSink> sink =
        route.options.per_source_files
            ? std::unique_ptr<OutputSink>(
                  new PerSourceWriter(route.options, metrics_))
            : std::unique_ptr<OutputSink>(
                  new RollingWriter(route.options, metrics_));
    if (!sink->open()) {
      any_failed_ = true;
      util::log_error("writer: cannot open output '" + route.name + "'");
      return false;
    }
    sinks_.push_back(std::move(sink));
  }
  return true;
}

// Applies the route's level threshold and optional DSL expression to `work`
// (the possibly already-transformed record). Mirrors LogFilter's semantics:
// RAW ranks above every threshold so unparsed lines are forwarded.
bool FanOutWriter::route_accepts(const OutputRoute& route,
                                 LogRecord& work) const {
  if (static_cast<int>(work.level) < static_cast<int>(route.level)) return false;
  if (route.filter_expr && !route.filter_expr->passes(work)) return false;
  return true;
}

bool FanOutWriter::write(const LogRecord& rec, uint64_t& bytes_written) {
  bytes_written = 0;
  for (size_t i = 0; i < routes_.size(); ++i) {
    const OutputRoute& route = routes_[i];
    LogRecord work = rec;
    if (route.transform_position == TransformPosition::Before) {
      work = apply_transforms(route.transform, work);
    }
    if (!route_accepts(route, work)) continue;
    if (route.transform_position == TransformPosition::After) {
      work = apply_transforms(route.transform, work);
    }
    uint64_t branch_bytes = 0;
    if (!sinks_[i]->write(work, branch_bytes)) {
      any_failed_ = true;
      util::log_error("writer: write to output '" + route.name + "' failed");
      return false;  // disk trouble: stop cleanly like the single sink
    }
    if (metrics_ != nullptr) {
      metrics_->record_output_written(route.name, branch_bytes);
    }
    bytes_written += branch_bytes;
  }
  // A record routed to no output (every branch filtered it) is not a failure;
  // the caller still sees bytes_written == 0.
  return true;
}

void FanOutWriter::close() {
  for (auto& sink : sinks_) sink->close();
}

bool FanOutWriter::failed() const {
  if (any_failed_) return true;
  for (const auto& sink : sinks_) {
    if (sink->failed()) return true;
  }
  return false;
}

// --------------------------- compressed file reader --------------------------
bool read_compressed_output(const std::filesystem::path& path, std::string& out) {
  out.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::string packed((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  size_t offset = 0;
  while (offset < packed.size()) {
    uint32_t frame_size = 0;
    if (!read_u32_le(packed, offset, frame_size)) return false;
    offset += 4;
    if (frame_size == 0 || packed.size() - offset < frame_size) return false;
    std::string line;
    if (!rle::decompress(packed.substr(offset, frame_size), line)) return false;
    out += line;
    out += '\n';
    offset += frame_size;
  }
  return true;
}

}  // namespace logpipe
