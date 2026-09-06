// pipeline.h - core data types and the small in-memory stages of logpipe:
// level model, thread-safe queue, line parser, filter and metrics counters.

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

namespace logpipe {

// Severity model. RAW ranks above every parsed level so that unparsed lines
// pass any configured threshold: they are meant to be forwarded verbatim.
enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3, Raw = 4 };

// Number of distinct Level values; sizes Metrics' per-level counters.
inline constexpr int kLevelCount = static_cast<int>(Level::Raw) + 1;

inline const char* level_name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO";
    case Level::Warn:  return "WARN";
    case Level::Error: return "ERROR";
    case Level::Raw:   return "RAW";
  }
  return "RAW";
}

// Accepts the canonical names plus common aliases (TRACE, WARNING, ERR,
// FATAL, CRITICAL, ...). Returns false for anything else.
inline bool level_from_string(const std::string& text, Level& out) {
  std::string upper;
  upper.reserve(text.size());
  for (char c : text) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (upper == "DEBUG" || upper == "TRACE") { out = Level::Debug; return true; }
  if (upper == "INFO" || upper == "NOTICE") { out = Level::Info; return true; }
  if (upper == "WARN" || upper == "WARNING") { out = Level::Warn; return true; }
  if (upper == "ERROR" || upper == "ERR" || upper == "FATAL" ||
      upper == "CRITICAL" || upper == "CRIT") { out = Level::Error; return true; }
  if (upper == "RAW") { out = Level::Raw; return true; }
  return false;
}

// Output line format: human-readable text (default) or one JSON object per
// line. Selected via the output_format configuration key.
enum class OutputFormat { Text, JsonLines };

// Accepts "text" and "json_lines" (case-insensitive; "jsonlines" alias).
inline bool output_format_from_string(const std::string& text, OutputFormat& out) {
  std::string lower;
  lower.reserve(text.size());
  for (char c : text) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "text") { out = OutputFormat::Text; return true; }
  if (lower == "json_lines" || lower == "jsonlines") { out = OutputFormat::JsonLines; return true; }
  return false;
}

inline const char* output_format_name(OutputFormat format) {
  return format == OutputFormat::JsonLines ? "json_lines" : "text";
}

// A line as read by the tailer thread, before parsing.
struct RawLine {
  std::string source;       // input file the line came from
  int64_t ingested_ms = 0;  // read time (via util), used to stamp RAW records
  std::string text;         // line content without the trailing newline
};

// A parsed (or deliberately unparsed) record handed to filter and writer.
// `fields` carries the key/value pairs extracted from the line by the optional
// KV extractor stage (extract.kv = true, see kv_extractor.h); it stays empty
// when extraction is disabled or the line has no key=value shape. The DSL can
// reference these fields with the kv("key") comparison syntax (see dsl.h).
struct LogRecord {
  std::string source;
  int64_t ingested_ms = 0;
  std::string timestamp_text;  // verbatim for parsed lines; ingest time for RAW
  Level level = Level::Raw;
  std::string message;         // extracted message, or the whole line for RAW
  bool parsed = false;
  std::map<std::string, std::string> fields;  // extracted KV fields (may be empty)
};

// Shared push retry cadence (ms) used by the input sources when the bounded
// queue is full (see BlockingQueue::push_for).
inline constexpr int kPushTimeoutMs = 200;

// Bounded queue coupling the reader thread (push) with the main processing
// thread (pop). push blocks while full, so a slow consumer applies back
// pressure to the tailer; timeouts keep shutdown responsive.
template <typename T>
class BlockingQueue {
 public:
  enum class PopResult { Got, Timeout, Closed };

  explicit BlockingQueue(size_t capacity) : capacity_(capacity) {}

  // Enqueues a copy of `item`; blocks up to timeout_ms while full. Returns
  // false if the queue was closed or the timeout elapsed (nothing enqueued).
  bool push_for(const T& item, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready =
        not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return closed_ || items_.size() < capacity_; });
    if (!ready || closed_) return false;
    items_.push_back(item);
    not_empty_.notify_one();
    return true;
  }

  // Dequeues into `out`, blocking up to timeout_ms while empty; Got on
  // success, Timeout when nothing arrived, Closed when closed and drained.
  PopResult pop_for(T& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready =
        not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return closed_ || !items_.empty(); });
    if (!ready) return PopResult::Timeout;
    if (items_.empty()) return PopResult::Closed;
    out = std::move(items_.front());
    items_.pop_front();
    not_full_.notify_one();
    return PopResult::Got;
  }

  // Marks the queue closed and wakes every waiter.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<T> items_;
  size_t capacity_;
  bool closed_ = false;
};

// Extracts "timestamp [LEVEL] message" lines like
//   2026-08-30 12:00:00 [INFO] message...
// ('T' separator, fractional seconds and brackets optional). Anything else
// becomes a RAW record carrying the whole line, stamped via the util time.
class LogParser {
 public:
  LogParser()
      // "re" delimiter: the default `)"` sequence inside the pattern
      // (`...(?:[.,]\d{1,6})?)`) would terminate the raw string early.
      : line_re_(R"re(^\s*(\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:[.,]\d{1,6})?)\s*\[?\s*([A-Za-z]+)\s*\]?\s*(.*)$)re") {}

  LogRecord parse(const std::string& source, int64_t ingested_ms,
                  const std::string& line) const {
    LogRecord rec;
    rec.source = source;
    rec.ingested_ms = ingested_ms;
    std::smatch match;
    if (std::regex_match(line, match, line_re_)) {
      Level level;
      if (level_from_string(match[2].str(), level)) {
        rec.parsed = true;
        rec.level = level;
        rec.timestamp_text = match[1].str();
        rec.message = match[3].str();
        return rec;
      }
    }
    rec.level = Level::Raw;  // unparseable: forward verbatim
    rec.timestamp_text = util::format_time_ms(ingested_ms);
    rec.message = line;
    return rec;
  }

 private:
  std::regex line_re_;
};

// Drops records below the level threshold or missing the optional keyword
// (case-insensitive substring). RAW always passes the threshold (see Level).
// An optional pre-filter predicate (the compiled filter.expr DSL, see dsl.h)
// runs FIRST when installed: only records it accepts reach the threshold and
// keyword stages, so DSL + threshold/keyword compose in that order.
class LogFilter {
 public:
  // Predicate evaluated before threshold/keyword; may be empty (no DSL).
  using PreFilter = std::function<bool(const LogRecord&)>;

  LogFilter() = default;
  LogFilter(Level threshold, std::string keyword)
      : threshold_(threshold), keyword_on_(!keyword.empty()) {
    keyword_lower_.reserve(keyword.size());
    for (char c : keyword) keyword_lower_.push_back(to_lower(c));
  }
  LogFilter(Level threshold, std::string keyword, PreFilter pre_filter)
      : LogFilter(threshold, std::move(keyword)) {
    pre_filter_ = std::move(pre_filter);
  }

  bool passes(const LogRecord& rec) const {
    if (pre_filter_ && !pre_filter_(rec)) return false;  // DSL: first stage
    if (static_cast<int>(rec.level) < static_cast<int>(threshold_)) return false;
    if (keyword_on_ && !contains_keyword(rec.message)) return false;
    return true;
  }

  // True when a DSL pre-filter is installed (for startup diagnostics).
  bool dsl_active() const { return static_cast<bool>(pre_filter_); }

 private:
  static char to_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  bool contains_keyword(const std::string& text) const {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) lower.push_back(to_lower(c));
    return lower.find(keyword_lower_) != std::string::npos;
  }

  Level threshold_ = Level::Debug;
  std::string keyword_lower_;
  bool keyword_on_ = false;
  PreFilter pre_filter_;  // optional DSL stage (empty = not configured)
};

// Fixed-window rate limiter shared by all input sources: admits at most
// `lines_per_sec` lines per rolling one-second window (measured with the
// util time). lines_per_sec <= 0 disables limiting. When allow() returns
// false the caller drops the line and counts it in Metrics.
class RateLimiter {
 public:
  explicit RateLimiter(int lines_per_sec)
      : limit_(lines_per_sec > 0 ? lines_per_sec : 0) {}

  bool allow(int64_t now_ms) {
    if (limit_ == 0) return true;  // unlimited: lock-free fast path
    std::lock_guard<std::mutex> lock(mutex_);
    if (now_ms - window_start_ms_ >= kWindowMs) {
      window_start_ms_ = now_ms;
      window_used_ = 0;
    }
    if (window_used_ >= limit_) return false;  // over rate: caller drops
    ++window_used_;
    return true;
  }

 private:
  static constexpr int64_t kWindowMs = 1000;  // fixed window length, ms

  const int limit_;
  std::mutex mutex_;
  int64_t window_start_ms_ = 0;  // 0 = the first call starts the first window
  int window_used_ = 0;
};

// Thread-safe counters for the end-of-run summary (uptime via util::steady_now_ms).
class Metrics {
 public:
  // Immutable point-in-time copy of every counter, consumed by the Prometheus
  // statistics exporter (prom_stats.h) and by tests. kv_top_values carries the
  // full per (key, value) counter map unless kv_top_n > 0, in which case at
  // most kv_top_n values per key are kept (the most frequent ones; ties are
  // broken by value name for determinism).
  struct Snapshot {
    uint64_t ingested = 0;
    uint64_t filtered = 0;
    uint64_t written_lines = 0;
    uint64_t written_bytes = 0;
    uint64_t rate_dropped = 0;
    uint64_t compress_raw_bytes = 0;
    uint64_t compress_packed_bytes = 0;
    uint64_t rotations_by_size = 0;
    uint64_t rotations_by_day = 0;
    uint64_t kv_lines_attempted = 0;
    uint64_t kv_lines_extracted = 0;
    uint64_t kv_fields_extracted = 0;
    uint64_t level_counts[kLevelCount]{};
    std::map<std::string, uint64_t> source_counts;
    std::map<std::string, uint64_t> source_written_bytes;
    // Per-output fan-out accounting (requirement 1): written lines and bytes
    // keyed by the configured output name.
    std::map<std::string, uint64_t> output_written_lines;
    std::map<std::string, uint64_t> output_written_bytes;
    std::map<std::string, std::map<std::string, uint64_t>> kv_top_values;
  };

  // Collects a consistent snapshot; kv_top_n caps the values kept per key
  // (0 = keep all).
  Snapshot snapshot(size_t kv_top_n = 0) const {
    Snapshot snap;
    snap.ingested = ingested_.load();
    snap.filtered = filtered_.load();
    snap.written_lines = written_lines_.load();
    snap.written_bytes = written_bytes_.load();
    snap.rate_dropped = rate_dropped_.load();
    snap.compress_raw_bytes = compress_raw_bytes_.load();
    snap.compress_packed_bytes = compress_packed_bytes_.load();
    snap.rotations_by_size = rotations_by_size_.load();
    snap.rotations_by_day = rotations_by_day_.load();
    snap.kv_lines_attempted = kv_lines_attempted_.load();
    snap.kv_lines_extracted = kv_lines_extracted_.load();
    snap.kv_fields_extracted = kv_fields_extracted_.load();
    for (int i = 0; i < kLevelCount; ++i) {
      snap.level_counts[i] = level_counts_[i].load();
    }
    {
      std::lock_guard<std::mutex> lock(source_mutex_);
      snap.source_counts = source_counts_;
    }
    {
      std::lock_guard<std::mutex> lock(source_bytes_mutex_);
      snap.source_written_bytes = source_written_bytes_;
    }
    {
      std::lock_guard<std::mutex> lock(output_mutex_);
      snap.output_written_lines = output_written_lines_;
      snap.output_written_bytes = output_written_bytes_;
    }
    {
      std::lock_guard<std::mutex> lock(kv_mutex_);
      snap.kv_top_values = kv_value_counts_;
    }
    if (kv_top_n > 0) {
      trim_kv_top_values(snap.kv_top_values, kv_top_n);
    }
    return snap;
  }

  void record_input(Level level) {
    level_counts_[static_cast<int>(level)].fetch_add(1, std::memory_order_relaxed);
    ingested_.fetch_add(1, std::memory_order_relaxed);
  }
  void record_filtered() { filtered_.fetch_add(1, std::memory_order_relaxed); }
  // bytes: line/frame size on disk; source: the input source of the record
  // (per-source output byte totals for the summary).
  void record_written(uint64_t bytes, const std::string& source) {
    written_lines_.fetch_add(1, std::memory_order_relaxed);
    written_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(source_bytes_mutex_);
    source_written_bytes_[source] += bytes;
  }
  // Compressed output accounting: raw bytes fed to the encoder and the
  // resulting on-disk payload bytes (frame incl. container header).
  void record_compression(uint64_t raw_bytes, uint64_t packed_bytes) {
    compress_raw_bytes_.fetch_add(raw_bytes, std::memory_order_relaxed);
    compress_packed_bytes_.fetch_add(packed_bytes, std::memory_order_relaxed);
  }
  // Output file rotations; counted separately by trigger (size / day).
  void record_rotation(bool by_size) {
    if (by_size) {
      rotations_by_size_.fetch_add(1, std::memory_order_relaxed);
    } else {
      rotations_by_day_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Lines admitted past the rate limit, counted per input source
  // (source = file path, or "stdin" for the standard-input source).
  void record_source_line(const std::string& source) {
    std::lock_guard<std::mutex> lock(source_mutex_);
    ++source_counts_[source];
  }
  // Lines dropped because the configured rate limit was exceeded.
  void record_rate_dropped() { rate_dropped_.fetch_add(1, std::memory_order_relaxed); }

  // Fan-out accounting (requirement 1): one call per record written to a
  // named output, so each output gets its own lines/bytes columns.
  void record_output_written(const std::string& output_name, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    ++output_written_lines_[output_name];
    output_written_bytes_[output_name] += bytes;
  }

  // KV extraction accounting (extract.kv = true): one record_kv_line call per
  // ingested line offered to the extractor, with `extracted` telling whether
  // the line had the key=value shape. Each extracted pair is counted once more
  // through record_kv_value, which also maintains the per (key, value) counter
  // map used for the Top-N field-value statistics.
  void record_kv_line(bool extracted) {
    kv_lines_attempted_.fetch_add(1, std::memory_order_relaxed);
    if (extracted) {
      kv_lines_extracted_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void record_kv_value(const std::string& key, const std::string& value) {
    kv_fields_extracted_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(kv_mutex_);
    ++kv_value_counts_[key][value];
  }

  // Read-only accessors for the KV counters (used by tests and the summary).
  uint64_t kv_lines_attempted() const { return kv_lines_attempted_.load(); }
  uint64_t kv_lines_extracted() const { return kv_lines_extracted_.load(); }
  uint64_t kv_fields_extracted() const { return kv_fields_extracted_.load(); }

  // KV extraction rate in percent (0-100); 100 when nothing was offered yet
  // (the rate is only meaningful once lines were attempted).
  int kv_extraction_rate_percent() const {
    const uint64_t attempted = kv_lines_attempted_.load();
    if (attempted == 0) return 100;
    const uint64_t extracted = kv_lines_extracted_.load();
    return static_cast<int>((extracted * 100 + attempted / 2) / attempted);
  }

  // Read-only accessors for the new counters (used by tests and the writer).
  uint64_t compressed_raw_bytes() const { return compress_raw_bytes_.load(); }
  uint64_t compressed_packed_bytes() const { return compress_packed_bytes_.load(); }
  uint64_t rotations_by_size() const { return rotations_by_size_.load(); }
  uint64_t rotations_by_day() const { return rotations_by_day_.load(); }

  std::string summary(int64_t start_ms) const {
    std::ostringstream out;
    out << "==== logpipe run summary ====\n"
        << "uptime         : " << util::format_duration(util::steady_now_ms() - start_ms) << "\n"
        << "lines ingested : " << ingested_.load() << "\n";
    for (int i = 0; i < kLevelCount; ++i) {
      out << "  " << std::left << std::setw(6)
          << level_name(static_cast<Level>(i)) << ": " << level_counts_[i].load() << "\n";
    }
    out << "lines filtered : " << filtered_.load() << "\n"
        << "lines written  : " << written_lines_.load() << "\n"
        << "output bytes   : " << written_bytes_.load() << "\n"
        << "compress raw bytes   : " << compress_raw_bytes_.load() << "\n"
        << "compress packed bytes: " << compress_packed_bytes_.load() << "\n"
        << "rotations by size : " << rotations_by_size_.load() << "\n"
        << "rotations by day  : " << rotations_by_day_.load() << "\n"
        << "lines rate-dropped : " << rate_dropped_.load() << "\n";
    {
      std::lock_guard<std::mutex> lock(source_mutex_);
      if (!source_counts_.empty()) {
        out << "lines per source:\n";
        for (const auto& entry : source_counts_) {
          out << "  " << entry.first << ": " << entry.second << "\n";
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(source_bytes_mutex_);
      if (!source_written_bytes_.empty()) {
        out << "output bytes per source:\n";
        for (const auto& entry : source_written_bytes_) {
          out << "  " << entry.first << ": " << entry.second << "\n";
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(output_mutex_);
      if (!output_written_lines_.empty()) {
        out << "lines per output:\n";
        for (const auto& entry : output_written_lines_) {
          out << "  " << entry.first << ": " << entry.second << "\n";
        }
      }
      if (!output_written_bytes_.empty()) {
        out << "output bytes per output:\n";
        for (const auto& entry : output_written_bytes_) {
          out << "  " << entry.first << ": " << entry.second << "\n";
        }
      }
    }
    if (kv_lines_attempted_.load() > 0) {
      out << "kv lines attempted : " << kv_lines_attempted_.load() << "\n"
          << "kv lines extracted : " << kv_lines_extracted_.load() << "\n"
          << "kv fields extracted: " << kv_fields_extracted_.load() << "\n"
          << "kv extraction rate : " << kv_extraction_rate_percent() << "%\n";
    }
    return out.str();
  }

 private:
  // Keeps at most top_n most-frequent values per key (ties broken by value
  // name, ascending) so the exported statistics stay bounded.
  static void trim_kv_top_values(
      std::map<std::string, std::map<std::string, uint64_t>>& values,
      size_t top_n) {
    for (auto& entry : values) {
      std::map<std::string, uint64_t>& per_key = entry.second;
      if (per_key.size() <= top_n) continue;
      // Move into a vector, sort by count desc then value asc, keep top_n.
      std::vector<std::pair<const std::string*, uint64_t>> ranked;
      ranked.reserve(per_key.size());
      for (const auto& item : per_key) {
        ranked.emplace_back(&item.first, item.second);
      }
      std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return *a.first < *b.first;
                });
      std::map<std::string, uint64_t> kept;
      for (size_t i = 0; i < top_n && i < ranked.size(); ++i) {
        kept[*ranked[i].first] = ranked[i].second;
      }
      per_key = std::move(kept);
    }
  }

  std::atomic<uint64_t> level_counts_[kLevelCount]{};
  std::atomic<uint64_t> ingested_{0};
  std::atomic<uint64_t> filtered_{0};
  std::atomic<uint64_t> written_lines_{0};
  std::atomic<uint64_t> written_bytes_{0};
  std::atomic<uint64_t> rate_dropped_{0};
  std::atomic<uint64_t> compress_raw_bytes_{0};
  std::atomic<uint64_t> compress_packed_bytes_{0};
  std::atomic<uint64_t> rotations_by_size_{0};
  std::atomic<uint64_t> rotations_by_day_{0};
  mutable std::mutex source_mutex_;  // guards source_counts_
  std::map<std::string, uint64_t> source_counts_;
  mutable std::mutex source_bytes_mutex_;  // guards source_written_bytes_
  std::map<std::string, uint64_t> source_written_bytes_;
  mutable std::mutex output_mutex_;  // guards the per-output fan-out counters
  std::map<std::string, uint64_t> output_written_lines_;
  std::map<std::string, uint64_t> output_written_bytes_;
  // KV extraction accounting; the (key, value) counter map feeds the Top-N
  // field-value statistics exported through the Prometheus writer.
  std::atomic<uint64_t> kv_lines_attempted_{0};
  std::atomic<uint64_t> kv_lines_extracted_{0};
  std::atomic<uint64_t> kv_fields_extracted_{0};
  mutable std::mutex kv_mutex_;  // guards kv_value_counts_
  std::map<std::string, std::map<std::string, uint64_t>> kv_value_counts_;
};

}  // namespace logpipe
