// prom_stats.cpp - Prometheus text exposition rendering, timestamped export
// files and retention pruning (see prom_stats.h).

#include "prom_stats.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#include "util.h"

namespace logpipe {
namespace {

// Appends one "# HELP / # TYPE" header pair plus the sample lines for a
// counter whose value is constant (no labels).
void emit_counter(std::string& out, const std::string& name,
                  const char* help, uint64_t value) {
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
  out += '\n';
  out += "# TYPE ";
  out += name;
  out += " counter\n";
  out += name;
  out += ' ';
  out += std::to_string(value);
  out += '\n';
}

// Same for a gauge (value may go down).
void emit_gauge(std::string& out, const std::string& name,
                const char* help, uint64_t value) {
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
  out += '\n';
  out += "# TYPE ";
  out += name;
  out += " gauge\n";
  out += name;
  out += ' ';
  out += std::to_string(value);
  out += '\n';
}

// Emits one sample per label value of a labelled counter, plus the header
// pair once. `samples` must already be sorted by label value (std::map
// guarantees this) so the output is deterministic.
void emit_labelled_counter(std::string& out, const std::string& name,
                           const char* label_name, const char* help,
                           const std::vector<std::pair<std::string, uint64_t>>& samples) {
  if (samples.empty()) return;
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
  out += '\n';
  out += "# TYPE ";
  out += name;
  out += " counter\n";
  for (const auto& sample : samples) {
    out += name;
    out += '{';
    out += label_name;
    out += "=\"";
    out += PromStatsWriter::escape_label(sample.first);
    out += "\"} ";
    out += std::to_string(sample.second);
    out += '\n';
  }
}

}  // namespace

PromStatsWriter::PromStatsWriter(PromStatsOptions options)
    : options_(std::move(options)) {}

std::string PromStatsWriter::escape_label(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default:   out.push_back(c); break;
    }
  }
  return out;
}

std::string PromStatsWriter::render(const Metrics::Snapshot& snap) {
  std::string out;
  // Rough capacity: a handful of headers plus per-source / per-value lines.
  out.reserve(2048);

  // --- ingested lines: per level first, then the unlabeled sum. ------------
  std::vector<std::pair<std::string, uint64_t>> level_samples;
  level_samples.reserve(kLevelCount);
  for (int i = 0; i < kLevelCount; ++i) {
    level_samples.emplace_back(level_name(static_cast<Level>(i)),
                               snap.level_counts[i]);
  }
  emit_labelled_counter(out, std::string(kPrefix) + "lines_ingested_total",
                        "level", "Lines ingested (per level and total).",
                        level_samples);
  if (!level_samples.empty()) {
    // Same metric family: append the unlabeled total under the header above
    // (the text format allows one HELP/TYPE pair per metric name only).
    out += std::string(kPrefix) + "lines_ingested_total ";
    out += std::to_string(snap.ingested);
    out += '\n';
  } else {
    emit_counter(out, std::string(kPrefix) + "lines_ingested_total",
                 "Total lines ingested.", snap.ingested);
  }

  // --- single-value pipeline counters. --------------------------------------
  emit_counter(out, std::string(kPrefix) + "lines_filtered_total",
               "Lines dropped by the filter stages.", snap.filtered);
  emit_counter(out, std::string(kPrefix) + "lines_written_total",
               "Lines written to the output.", snap.written_lines);
  emit_counter(out, std::string(kPrefix) + "output_bytes_total",
               "Bytes written to the output.", snap.written_bytes);
  emit_counter(out, std::string(kPrefix) + "lines_rate_dropped_total",
               "Lines dropped by the rate limiter.", snap.rate_dropped);

  // --- compression accounting. ----------------------------------------------
  emit_counter(out, std::string(kPrefix) + "compression_raw_bytes_total",
               "Raw bytes fed to the RLE encoder.", snap.compress_raw_bytes);
  emit_counter(out, std::string(kPrefix) + "compression_packed_bytes_total",
               "Packed bytes produced by the RLE encoder.",
               snap.compress_packed_bytes);

  // --- rotations: one counter, two trigger labels. ---------------------------
  std::vector<std::pair<std::string, uint64_t>> rotation_samples;
  rotation_samples.emplace_back("size", snap.rotations_by_size);
  rotation_samples.emplace_back("day", snap.rotations_by_day);
  emit_labelled_counter(out, std::string(kPrefix) + "output_rotations_total",
                        "trigger", "Output file rotations by trigger.",
                        rotation_samples);

  // --- write buffer high-water marks (batched-write buffer statistics). ------
  emit_gauge(out, std::string(kPrefix) + "write_buffer_lines_high_water",
             "Largest number of lines held in the output write buffer at once.",
             snap.buffer_lines_high_water);
  emit_gauge(out, std::string(kPrefix) + "write_buffer_bytes_high_water",
             "Largest number of bytes held in the output write buffer at once.",
             snap.buffer_bytes_high_water);

  // --- KV extraction accounting. ---------------------------------------------
  emit_counter(out, std::string(kPrefix) + "kv_lines_attempted_total",
               "Lines offered to the KV extractor.", snap.kv_lines_attempted);
  emit_counter(out, std::string(kPrefix) + "kv_lines_extracted_total",
               "Lines the KV extractor recognized.", snap.kv_lines_extracted);
  emit_counter(out, std::string(kPrefix) + "kv_fields_extracted_total",
               "Individual key=value pairs extracted.",
               snap.kv_fields_extracted);
  const int ratio = snap.kv_lines_attempted == 0
                        ? 100
                        : static_cast<int>(
                              (snap.kv_lines_extracted * 100 +
                               snap.kv_lines_attempted / 2) /
                              snap.kv_lines_attempted);
  emit_gauge(out, std::string(kPrefix) + "kv_extraction_ratio",
             "KV extraction success rate, percent (0-100).",
             static_cast<uint64_t>(ratio));

  // --- replay mode accounting (replay.since + sidecar line index). ----------
  emit_counter(out, std::string(kPrefix) + "replay_index_hits_total",
               "Files positioned via the sidecar line index at replay start.",
               snap.replay_index_hits);
  emit_counter(out, std::string(kPrefix) + "replay_index_fallbacks_total",
               "Files replay-positioned by a full-file scan (no usable index).",
               snap.replay_index_fallbacks);
  emit_counter(out, std::string(kPrefix) + "replay_skipped_bytes_total",
               "Bytes of pre-since lines skipped by the replay phase.",
               snap.replay_skipped_bytes);

  // --- per-source breakdowns. -------------------------------------------------
  std::vector<std::pair<std::string, uint64_t>> source_samples(
      snap.source_counts.begin(), snap.source_counts.end());
  emit_labelled_counter(out, std::string(kPrefix) + "source_lines_total",
                        "source", "Lines ingested per input source.",
                        source_samples);
  std::vector<std::pair<std::string, uint64_t>> source_bytes(
      snap.source_written_bytes.begin(), snap.source_written_bytes.end());
  emit_labelled_counter(out, std::string(kPrefix) + "source_output_bytes_total",
                        "source", "Output bytes per input source.",
                        source_bytes);

  // --- per-output fan-out breakdowns. -----------------------------------------
  std::vector<std::pair<std::string, uint64_t>> output_lines(
      snap.output_written_lines.begin(), snap.output_written_lines.end());
  emit_labelled_counter(out, std::string(kPrefix) + "output_lines_written_total",
                        "output", "Lines written per named output.",
                        output_lines);
  std::vector<std::pair<std::string, uint64_t>> output_bytes(
      snap.output_written_bytes.begin(), snap.output_written_bytes.end());
  emit_labelled_counter(out, std::string(kPrefix) + "output_bytes_written_total",
                        "output", "Output bytes per named output.",
                        output_bytes);

  // --- per-source-type breakdowns (input source abstraction). ----------------
  std::vector<std::pair<std::string, uint64_t>> type_lines(
      snap.source_type_lines.begin(), snap.source_type_lines.end());
  emit_labelled_counter(out, std::string(kPrefix) + "source_type_lines_total",
                        "type", "Lines ingested per input source type.",
                        type_lines);
  if (!snap.source_type_active.empty()) {
    out += "# HELP ";
    out += kPrefix;
    out += "source_type_active Currently active input sources per type.\n";
    out += "# TYPE ";
    out += kPrefix;
    out += "source_type_active gauge\n";
    for (const auto& entry : snap.source_type_active) {
      out += kPrefix;
      out += "source_type_active{type=\"";
      out += escape_label(entry.first);
      out += "\"} ";
      out += std::to_string(entry.second);
      out += '\n';
    }
  }

  // --- KV field-value Top-N statistics. ---------------------------------------
  // Nested map iteration (key asc, then value asc) is deterministic; the
  // snapshot was already trimmed to the configured Top-N by Metrics.
  bool header_done = false;
  for (const auto& per_key : snap.kv_top_values) {
    if (!header_done) {
      out += "# HELP ";
      out += kPrefix;
      out += "field_value_count Number of extracted values per field key.\n";
      out += "# TYPE ";
      out += kPrefix;
      out += "field_value_count counter\n";
      header_done = true;
    }
    for (const auto& item : per_key.second) {
      out += kPrefix;
      out += "field_value_count{key=\"";
      out += escape_label(per_key.first);
      out += "\",value=\"";
      out += escape_label(item.first);
      out += "\"} ";
      out += std::to_string(item.second);
      out += '\n';
    }
  }

  return out;
}

std::filesystem::path PromStatsWriter::write(const Metrics::Snapshot& snap) {
  failed_ = false;
  const int64_t now = options_.clock ? options_.clock() : util::now_ms();
  const std::string file_name =
      options_.base_name + "_" + util::timestamp_compact(now) + ".prom";

  std::error_code ec;
  std::filesystem::create_directories(options_.dir, ec);
  if (ec) {
    util::log_warn("prom_stats: cannot create stats directory '" +
                   options_.dir.string() + "': " + ec.message());
    failed_ = true;
    return {};
  }

  const std::filesystem::path target = options_.dir / file_name;
  {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      util::log_warn("prom_stats: cannot open '" + target.string() + "'");
      failed_ = true;
      return {};
    }
    const std::string body = render(snap);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.flush();
    if (!out) {
      util::log_warn("prom_stats: write to '" + target.string() + "' failed");
      failed_ = true;
      return {};
    }
  }

  if (!prune_old_files()) {
    // Directory listing trouble: the export itself succeeded, keep going.
    util::log_warn("prom_stats: could not list '" + options_.dir.string() +
                   "' for retention pruning");
  }
  return target;
}

bool PromStatsWriter::prune_old_files() {
  const size_t keep = options_.keep_files > 0
                          ? static_cast<size_t>(options_.keep_files)
                          : 1;
  const std::string suffix = ".prom";
  const std::string stem_prefix = options_.base_name + "_";

  std::error_code ec;
  std::vector<std::filesystem::path> exports;
  for (std::filesystem::directory_iterator it(options_.dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::filesystem::path candidate = it->path();
    const std::string name = candidate.filename().string();
    if (name.size() > stem_prefix.size() + suffix.size() &&
        name.compare(0, stem_prefix.size(), stem_prefix) == 0 &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      exports.push_back(candidate);
    }
  }
  if (ec) return false;

  // Timestamped names sort chronologically as strings; oldest first.
  std::sort(exports.begin(), exports.end());
  if (exports.size() <= keep) return true;
  const size_t remove_count = exports.size() - keep;
  for (size_t i = 0; i < remove_count; ++i) {
    std::error_code rm_ec;
    std::filesystem::remove(exports[i], rm_ec);
    if (rm_ec) {
      util::log_warn("prom_stats: cannot prune '" + exports[i].string() +
                     "': " + rm_ec.message());
    }
  }
  return true;
}

size_t PromStatsWriter::kept_files() const {
  std::error_code ec;
  size_t count = 0;
  const std::string suffix = ".prom";
  const std::string stem_prefix = options_.base_name + "_";
  for (std::filesystem::directory_iterator it(options_.dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() > stem_prefix.size() + suffix.size() &&
        name.compare(0, stem_prefix.size(), stem_prefix) == 0 &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      ++count;
    }
  }
  return count;
}

}  // namespace logpipe
