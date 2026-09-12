// tail_engine.cpp - implementation of the shared tail engine: complete lines
// into the bounded queue, partial trailing lines buffered until their newline
// arrives, offsets checkpointed for resume. See tail_engine.h.

#include "tail_engine.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include "util.h"

namespace logpipe {
namespace {

constexpr int kDefaultReadChunkSize = 8192;  // bytes per read() while tailing
// Upper bound for the backwards scan that aligns a replay index mark to the
// start of its surrounding line (guards against pathological no-newline data).
constexpr uint64_t kReplayMaxBackscan = 1u << 20;  // 1 MiB

// Extracts the in-line timestamp of a log line ("2026-01-01 10:00:00 ...",
// 'T' separator and fractional seconds tolerated, as in LogParser) as epoch
// milliseconds via the util module. Returns 0 for lines without a parseable
// leading timestamp (RAW lines) - such lines never update the sidecar index
// timestamp and never anchor a replay.
int64_t line_timestamp_ms(const std::string& text) {
  if (text.size() < 19) return 0;
  static const char* kShapes = "####-##-##X##:##:##";
  for (int i = 0; i < 19; ++i) {
    const char shape = kShapes[i];
    const char c = text[static_cast<size_t>(i)];
    if (shape == '#') {
      if (c < '0' || c > '9') return 0;
    } else if (shape == 'X') {
      if (c != ' ' && c != 'T' && c != 't') return 0;
    } else if (c != shape) {
      return 0;
    }
  }
  int64_t ms = 0;
  if (!util::parse_datetime_ms(text.substr(0, 19), ms)) return 0;
  return ms;
}

}  // namespace

std::string normalize_file_key(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

TailEngine::TailEngine(BlockingQueue<RawLine>& queue, const TailOptions& options)
    : queue_(queue), options_(options) {
  // read.chunk_bytes sanity: fall back to the default for non-positive values
  // (config.cpp already bounds-checks the parsed value, this protects direct
  // construction from code that bypasses the config layer).
  if (options_.chunk_bytes <= 0) options_.chunk_bytes = kDefaultReadChunkSize;
}

bool TailEngine::add_file(const FileSourceSpec& spec) {
  const std::string key = normalize_file_key(spec.path);
  for (const auto& existing : files_) {
    if (existing.key == key) return false;  // duplicate: ignore
  }
  FileState file;
  file.path = spec.path;
  file.key = key;
  file.tags = spec.tags;
  if (options_.index_interval_bytes > 0) {
    // Sidecar line index ("<file>.idx") for the replay positioning; the
    // entries themselves are loaded on demand (load_indexes/begin_replay).
    file.index = std::make_unique<LineIndexSidecar>(spec.path,
                                                    options_.index_interval_bytes);
    recompute_next_mark(file);
  }
  files_.push_back(std::move(file));
  return true;
}

size_t TailEngine::remove_file(const std::filesystem::path& path) {
  const std::string key = normalize_file_key(path);
  size_t removed = 0;
  for (auto it = files_.begin(); it != files_.end();) {
    if (it->key == key) {
      if (it->stream.is_open()) it->stream.close();
      it = files_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t TailEngine::file_count() const { return files_.size(); }

std::vector<std::string> TailEngine::file_keys() const {
  std::vector<std::string> keys;
  keys.reserve(files_.size());
  for (const auto& file : files_) keys.push_back(file.key);
  return keys;
}

void TailEngine::load_offsets() {
  if (options_.offset_file.empty()) return;
  std::ifstream in(options_.offset_file);
  if (!in.is_open()) {
    util::log_info("tailer: no offset state at '" + options_.offset_file.string() +
                   "', starting fresh");
    return;
  }
  // Format: "<offset>\t<path>" (path last, may contain spaces); stale entries
  // for files no longer configured are skipped.
  std::string line;
  size_t applied = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    try {
      const uint64_t offset = std::stoull(line.substr(0, tab));
      const std::string& path = line.substr(tab + 1);
      for (auto& file : files_) {
        if (file.key == path) {
          file.offset = offset;
          ++applied;
          break;
        }
      }
    } catch (const std::exception&) {
      util::log_warn("tailer: skipping malformed offset entry: " + line);
    }
  }
  util::log_info("tailer: resumed " + std::to_string(applied) + " offset(s) from '" +
                 options_.offset_file.string() + "'");
}

void TailEngine::save_offsets() {
  if (options_.offset_file.empty()) return;
  // Write to a temporary file first, then replace the state file, so a crash
  // mid-write cannot destroy the previous checkpoint.
  const std::filesystem::path tmp = options_.offset_file.string() + options_.tmp_suffix;
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) {
      util::log_warn("tailer: cannot write offset state to '" + tmp.string() + "'");
      return;
    }
    out << "# logpipe tail offsets (offset<TAB>path)\n";
    for (const auto& file : files_) {
      out << file.offset << '\t' << file.key << '\n';
    }
    out.flush();
    if (!out.good()) {
      util::log_warn("tailer: failed while writing offset state");
      return;
    }
  }
  std::error_code ec;
  std::filesystem::remove(options_.offset_file, ec);  // rename() fails if target exists (Windows)
  ec.clear();
  std::filesystem::rename(tmp, options_.offset_file, ec);
  if (ec) {
    util::log_warn("tailer: cannot replace offset state: " + ec.message());
  }
}

void TailEngine::recompute_next_mark(FileState& file) {
  // Next interval mark strictly after the current offset; marks at or below
  // the resume offset already exist in a loaded sidecar (record_mark keeps
  // them idempotent if a re-crossing happens).
  const uint64_t interval = options_.index_interval_bytes;
  if (interval == 0 || !file.index) {
    file.next_index_mark = 0;
    return;
  }
  file.next_index_mark = (file.offset / interval + 1) * interval;
}

void TailEngine::maybe_record_index_mark(FileState& file) {
  if (!file.index || file.next_index_mark == 0) return;
  // The index entry carries the most recent in-line line timestamp known at
  // the mark (last_line_ts_ms is updated by emit_line for every parsed line;
  // RAW lines keep the previous value so the entry never goes backwards).
  while (file.offset >= file.next_index_mark) {
    file.index->record_mark(file.next_index_mark, file.last_line_ts_ms);
    file.next_index_mark += options_.index_interval_bytes;
  }
}

void TailEngine::load_indexes() {
  if (options_.index_interval_bytes == 0) return;
  for (auto& file : files_) {
    if (!file.index) continue;
    file.index->load();
    recompute_next_mark(file);
  }
}

void TailEngine::save_indexes() {
  if (options_.index_interval_bytes == 0) return;
  for (auto& file : files_) {
    if (file.index) file.index->save();
  }
}

void TailEngine::begin_replay() {
  if (options_.replay_since_ms <= 0) return;
  const std::string since_text = util::format_time_ms(options_.replay_since_ms);
  util::log_info("tailer: replay mode: since \"" + since_text + "\"");
  for (auto& file : files_) {
    file.replay_skip = true;
    file.replay_anchored = false;
    uint64_t start_offset = 0;
    bool positioned = false;
    if (file.index && file.index->empty()) {
      file.index->load();  // sidecar written by a previous run
    }
    if (file.index && file.index->find_start_offset(options_.replay_since_ms,
                                                    start_offset)) {
      // Index hit: start reading at the closest indexed mark at/before the
      // requested time; the skip phase below advances to the first line
      // whose in-line timestamp is >= since (which is always emitted).
      file.offset = start_offset;
      file.replay_align_pending = true;  // mark may sit mid-line
      replay_index_hits_.fetch_add(1, std::memory_order_relaxed);
      util::log_info("tailer: replay '" + file.path.string() + "' via sidecar index at offset " +
                     std::to_string(start_offset));
      positioned = true;
    }
    if (!positioned) {
      // Index missing, empty or entirely newer than `since`: fall back to a
      // full-file scan from offset 0 (the skip phase still lands exactly on
      // the first line with a timestamp >= since).
      file.offset = 0;
      replay_index_fallbacks_.fetch_add(1, std::memory_order_relaxed);
      util::log_warn("tailer: replay '" + file.path.string() +
                     "' without usable sidecar index, scanning the whole file");
    }
    recompute_next_mark(file);
  }
}

bool TailEngine::ensure_open(FileState& file, const std::atomic<bool>& stop) {
  (void)stop;
  if (file.stream.is_open()) return true;
  file.stream.open(file.path, std::ios::in | std::ios::binary);
  if (!file.stream.is_open()) {
    if (!file.announced_missing) {
      util::log_info("tailer: waiting for input file '" + file.path.string() + "'");
      file.announced_missing = true;
    }
    return false;
  }
  file.announced_missing = false;
  if (file.offset > 0) {
    file.stream.seekg(static_cast<std::streamoff>(file.offset));
  }
  util::log_info("tailer: tailing '" + file.path.string() + "' from offset " +
                 std::to_string(file.offset));
  return true;
}

void TailEngine::handle_truncation(FileState& file) {
  if (!file.stream.is_open()) return;
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(file.path, ec);
  if (ec || size >= file.offset) return;
  // The file shrank below the recorded offset: truncated or rotated. Restart
  // from the beginning (a same-size replacement file is not detected).
  util::log_warn("tailer: '" + file.path.string() +
                 "' shrank (truncated or rotated), restarting from the beginning");
  file.stream.close();
  file.stream.clear();
  file.offset = 0;
  file.pending.clear();
  // The old sidecar entries refer to the pre-truncation content and would
  // poison a later replay: drop them and restart the interval grid.
  file.last_line_ts_ms = 0;
  if (file.index) file.index->clear();
  recompute_next_mark(file);
}

void TailEngine::align_replay_start(FileState& file) {
  if (file.offset == 0 || !file.stream.is_open()) return;
  const uint64_t mark = file.offset;
  uint64_t aligned = 0;  // BOF when no previous newline exists
  std::vector<char> window(4096);
  uint64_t window_end = mark;
  while (window_end > 0) {
    const uint64_t size = std::min<uint64_t>(window.size(), window_end);
    const uint64_t start = window_end - size;
    file.stream.clear();
    file.stream.seekg(static_cast<std::streamoff>(start));
    file.stream.read(window.data(), static_cast<std::streamsize>(size));
    const std::streamsize got = file.stream.gcount();
    bool found = false;
    for (std::streamsize i = got - 1; i >= 0; --i) {
      if (window[static_cast<size_t>(i)] == '\n') {
        aligned = start + static_cast<uint64_t>(i) + 1;  // first byte after '\n'
        found = true;
        break;
      }
    }
    if (found) break;
    window_end = start;
    if (mark - window_end > kReplayMaxBackscan) break;  // pathological file
  }
  file.stream.clear();
  file.stream.seekg(static_cast<std::streamoff>(aligned));
  file.offset = aligned;
  util::log_info("tailer: replay '" + file.path.string() +
                 "' aligned index mark " + std::to_string(mark) +
                 " to line start " + std::to_string(aligned));
}

void TailEngine::poll_file(FileState& file, const std::atomic<bool>& stop) {
  if (!ensure_open(file, stop)) return;
  if (file.replay_align_pending) {
    // The index mark may sit inside a line: back up to its start so the skip
    // phase parses complete lines only (a mis-parsed partial would never
    // match the anchor timestamp and could swallow the anchor itself).
    file.replay_align_pending = false;
    align_replay_start(file);
  }
  // One read block of the configured size per poll round (read.chunk_bytes).
  std::vector<char> buffer(static_cast<size_t>(options_.chunk_bytes));
  while (!stop.load(std::memory_order_relaxed)) {
    // A short read at EOF sets eofbit|failbit; without clearing it the sentry
    // makes every later read a silent no-op and appended lines are lost.
    file.stream.clear();
    file.stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = file.stream.gcount();
    if (got <= 0) break;

    // Split what we read into complete lines. The trailing partial line
    // stays in `pending` and is NOT counted into the offset yet, so a crash
    // never loses the position of an unterminated line. `file.offset` is
    // advanced per line (not per block) because the replay skip phase and
    // the sidecar index marks both need exact per-line byte positions.
    file.pending.append(buffer.data(), static_cast<size_t>(got));
    size_t newline = file.pending.find('\n');
    while (newline != std::string::npos) {
      std::string text = file.pending.substr(0, newline);
      const uint64_t line_bytes = newline + 1;  // includes the newline itself
      if (!text.empty() && text.back() == '\r') text.pop_back();  // tolerate CRLF

      if (file.replay_skip && !file.replay_anchored) {
        // Replay skip phase: drop every line whose in-line timestamp is
        // older than replay.since (or missing entirely); the first line at
        // or after the requested time anchors the stream and is emitted.
        const int64_t ts = line_timestamp_ms(text);
        if (ts < options_.replay_since_ms) {
          file.offset += line_bytes;
          replay_skipped_bytes_.fetch_add(line_bytes, std::memory_order_relaxed);
          file.pending.erase(0, newline + 1);
          newline = file.pending.find('\n');
          continue;
        }
        file.replay_anchored = true;
        util::log_info("tailer: replay '" + file.path.string() +
                       "' anchored at offset " + std::to_string(file.offset));
      }

      if (!emit_line(file, text, stop)) return;  // consumer went away: stop tailing
      file.offset += line_bytes;
      maybe_record_index_mark(file);
      file.pending.erase(0, newline + 1);
      newline = file.pending.find('\n');
    }
    if (static_cast<size_t>(got) < sizeof(buffer)) break;  // drained for now
  }
}

bool TailEngine::emit_line(FileState& file, const std::string& text,
                           const std::atomic<bool>& stop) {
  // Timestamp capture: RAW lines keep the ingest time (team convention),
  // while every successfully parsed line contributes its IN-LINE timestamp
  // to the per-file replay state (last_line_ts_ms), which feeds the sidecar
  // index entries recorded at the interval marks. The RawLine handed to the
  // pipeline is unaffected: the LogParser owns the record timestamp.
  const int64_t line_ts = line_timestamp_ms(text);
  if (line_ts > 0) {
    file.last_line_ts_ms = line_ts;
  }
  RawLine raw;
  raw.source = file.path.string();
  raw.ingested_ms = util::now_ms();  // time via the util module (team convention)
  raw.text = text;
  raw.fields = file.tags;            // source-level metadata injection
  raw.source_type = source_type_;
  while (!stop.load(std::memory_order_relaxed)) {
    if (queue_.push_for(raw, kPushTimeoutMs)) return true;
    if (queue_.closed()) return false;  // consumer shut down
    // Queue full: retry, which is exactly the back pressure we want.
  }
  // Stop was requested while the queue stayed full: drop the line and move on.
  dropped_.fetch_add(1, std::memory_order_relaxed);
  util::log_warn("tailer: dropped pending line from '" + file.path.string() +
                 "' during shutdown");
  return true;
}

void TailEngine::poll_all(const std::atomic<bool>& stop) {
  // Sidecar index save schedule (index flush branch of the polling main
  // loop): once per offset_save_sec interval, flush dirty sidecars.
  const bool index_enabled = options_.index_interval_bytes > 0;
  if (index_enabled && last_index_save_ms_ == 0) {
    last_index_save_ms_ = util::steady_now_ms();
  }
  for (auto& file : files_) {
    if (stop.load(std::memory_order_relaxed)) break;
    handle_truncation(file);
    poll_file(file, stop);
  }
  if (index_enabled &&
      util::steady_now_ms() - last_index_save_ms_ >=
          static_cast<int64_t>(std::max(options_.offset_save_sec, 1)) * 1000) {
    save_indexes();
    last_index_save_ms_ = util::steady_now_ms();
  }
}

}  // namespace logpipe
