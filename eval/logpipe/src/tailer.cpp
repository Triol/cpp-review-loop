// tailer.cpp - implementation of the multi-file tail reader: one polling
// thread, complete lines into the bounded queue, partial trailing lines
// buffered until their newline arrives.

#include "tailer.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "util.h"

namespace logpipe {
namespace {

constexpr int kDefaultReadChunkSize = 8192;  // bytes per read() while tailing

// Canonical identity of a file for the offset state file.
std::string normalize_key(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

}  // namespace

Tailer::Tailer(std::vector<std::filesystem::path> paths, BlockingQueue<RawLine>& queue,
               const TailOptions& options)
    : queue_(queue), options_(options) {
  // read.chunk_bytes sanity: fall back to the default for non-positive values
  // (config.cpp already bounds-checks the parsed value, this protects direct
  // construction from code that bypasses the config layer).
  if (options_.chunk_bytes <= 0) options_.chunk_bytes = kDefaultReadChunkSize;
  for (const auto& path : paths) {
    const std::string key = normalize_key(path);
    bool duplicate = false;
    for (const auto& existing : files_) {
      if (existing.key == key) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      util::log_warn("tailer: duplicate input path ignored: " + path.string());
      continue;
    }
    FileState file;
    file.path = path;
    file.key = key;
    files_.push_back(std::move(file));
  }
}

void Tailer::load_offsets() {
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

void Tailer::save_offsets() {
  if (options_.offset_file.empty()) return;
  // Write to a temporary file first, then replace the state file, so a crash
  // mid-write cannot destroy the previous checkpoint.
  const std::filesystem::path tmp = options_.offset_file.string() + ".tmp";
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

bool Tailer::ensure_open(FileState& file) {
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

void Tailer::handle_truncation(FileState& file) {
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
}

void Tailer::poll_file(FileState& file) {
  if (!ensure_open(file)) return;
  // One read block of the configured size per poll round (read.chunk_bytes).
  std::vector<char> buffer(static_cast<size_t>(options_.chunk_bytes));
  while (!stop_.load(std::memory_order_relaxed)) {
    // A short read at EOF sets eofbit|failbit; without clearing it the sentry
    // makes every later read a silent no-op and appended lines are lost.
    file.stream.clear();
    file.stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = file.stream.gcount();
    if (got <= 0) break;

    // Split what we read into complete lines. The trailing partial line
    // stays in `pending` and is NOT counted into the offset yet, so a crash
    // never loses the position of an unterminated line.
    file.pending.append(buffer.data(), static_cast<size_t>(got));
    size_t consumed = 0;
    size_t newline = file.pending.find('\n');
    while (newline != std::string::npos) {
      std::string text = file.pending.substr(0, newline);
      if (!text.empty() && text.back() == '\r') text.pop_back();  // tolerate CRLF
      if (!emit_line(file, text)) return;  // consumer went away: stop tailing
      consumed += newline + 1;
      file.pending.erase(0, newline + 1);
      newline = file.pending.find('\n');
    }
    file.offset += consumed;
    if (static_cast<size_t>(got) < sizeof(buffer)) break;  // drained for now
  }
}

bool Tailer::emit_line(const FileState& file, const std::string& text) {
  RawLine raw;
  raw.source = file.path.string();
  raw.ingested_ms = util::now_ms();  // time via the util module (team convention)
  raw.text = text;
  while (!stop_.load(std::memory_order_relaxed)) {
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

void Tailer::run() {
  util::log_info("tailer: started, " + std::to_string(files_.size()) + " input file(s)");
  const int64_t save_interval_ms =
      static_cast<int64_t>(std::max(options_.offset_save_sec, 1)) * 1000;
  int64_t last_save_ms = util::steady_now_ms();

  while (!stop_.load(std::memory_order_relaxed)) {
    for (auto& file : files_) {
      if (stop_.load(std::memory_order_relaxed)) break;
      handle_truncation(file);
      poll_file(file);
    }
    if (!options_.offset_file.empty() &&
        util::steady_now_ms() - last_save_ms >= save_interval_ms) {
      save_offsets();
      last_save_ms = util::steady_now_ms();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.poll_ms));
  }

  save_offsets();     // final checkpoint, best effort
  queue_.close();     // end-of-stream for the consumer
  util::log_info("tailer: stopped");
}

}  // namespace logpipe
