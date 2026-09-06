// glob_source.cpp - implementation of the pattern-discovery tail source:
// directory scanning, glob matching, discovery/retirement of files and the
// producer loop that tails every currently matched file. See glob_source.h.

#include "glob_source.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

#include "util.h"

namespace logpipe {
namespace {

// Builds the TailOptions for the engine from the glob options; a distinct
// checkpoint temp suffix avoids clashes with a concurrently tailing Tailer.
TailOptions make_tail_options(const GlobOptions& options) {
  TailOptions tail;
  tail.poll_ms = options.poll_ms;
  tail.offset_file = options.offset_file;
  tail.offset_save_sec = options.offset_save_sec;
  tail.chunk_bytes = options.chunk_bytes;
  tail.tmp_suffix = ".glob.tmp";
  return tail;
}

}  // namespace

bool GlobSource::glob_match(const std::string& text, const std::string& pattern) {
  // Iterative wildcard match with backtracking: `star` remembers the last
  // '*' position and the matching position in `text`, so '*' can grow when a
  // later literal comparison fails.
  size_t t = 0, p = 0;
  size_t star = std::string::npos, mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++t;
      ++p;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;  // '*' matches the empty run first, grows on backtrack
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

void GlobSource::split_pattern(const std::string& pattern, std::filesystem::path& directory,
                               std::string& mask) {
  // The mask is the last path component; everything before it is the literal
  // directory. A pattern without any separator matches files in the current
  // directory. Windows and POSIX separators are both accepted.
  const auto slash = pattern.find_last_of("/\\");
  if (slash == std::string::npos) {
    directory = ".";
    mask = pattern;
  } else {
    directory = pattern.substr(0, slash);
    if (directory.empty() && slash == 0) directory = "/";  // pattern "/x.log"
    mask = pattern.substr(slash + 1);
  }
  if (mask.empty()) mask = "*";
}

GlobSource::GlobSource(std::string pattern, SourceTags tags, BlockingQueue<RawLine>& queue,
                       const GlobOptions& options, bool close_queue_on_exit)
    : pattern_(std::move(pattern)),
      tags_(std::move(tags)),
      engine_(queue, make_tail_options(options)),
      options_(options),
      close_queue_on_exit_(close_queue_on_exit) {
  // Tolerate the config-spelling prefix so callers can pass either form.
  if (pattern_.rfind("glob:", 0) == 0) pattern_.erase(0, 5);
  split_pattern(pattern_, directory_, mask_);
  engine_.set_source_type(SourceType::Glob);
}

void GlobSource::start() {
  // At most one producer thread per instance; a second start() is a no-op.
  if (started_.exchange(true, std::memory_order_relaxed)) return;
  thread_ = std::thread([this] { run(); });
}

void GlobSource::join() {
  if (thread_.joinable()) thread_.join();
}

bool GlobSource::matches(const std::filesystem::path& candidate) const {
  const std::string name = candidate.filename().generic_string();
  if (!glob_match(name, mask_)) return false;
  const std::string full = candidate.lexically_normal().generic_string();
  for (const std::string& pattern : options_.exclude) {
    // An exclude pattern matches against the file name and, when it contains
    // a separator, against the full normalized path as well.
    if (glob_match(name, pattern)) return false;
    if (pattern.find('/') != std::string::npos &&
        glob_match(full, pattern)) {
      return false;
    }
  }
  return true;
}

size_t GlobSource::discover() {
  size_t added = 0;
  std::error_code ec;
  std::filesystem::directory_iterator it(directory_, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    // Missing or unreadable directory: nothing to discover this round (the
    // scan is retried every poll, so a late-created directory is picked up).
    return 0;
  }
  for (const std::filesystem::directory_entry& entry : it) {
    if (stop_.load(std::memory_order_relaxed)) break;
    std::error_code file_ec;
    if (!entry.is_regular_file(file_ec) || file_ec) continue;
    if (!matches(entry.path())) continue;
    FileSourceSpec spec;
    spec.path = entry.path();
    spec.tags = tags_;
    if (engine_.add_file(spec)) {
      ++added;
      util::log_info("glob: discovered input file '" + entry.path().string() +
                     "' (pattern '" + pattern_ + "')");
    }
  }
  // Retire files that disappeared so their descriptors are released and the
  // next appearance of the same name starts fresh.
  for (const std::string& key : engine_.file_keys()) {
    std::error_code exists_ec;
    if (!std::filesystem::exists(std::filesystem::path(key), exists_ec)) {
      const size_t removed = engine_.remove_file(std::filesystem::path(key));
      if (removed > 0) {
        util::log_info("glob: input file '" + key +
                       "' disappeared, removed from the tail set");
      }
    }
  }
  return added;
}

void GlobSource::poll() {
  discover();
  engine_.poll_all(stop_);
}

void GlobSource::run() {
  util::log_info("glob: started, pattern '" + pattern_ + "'");
  engine_.load_offsets();  // resume discovered files where a previous run stopped
  const int64_t save_interval_ms =
      static_cast<int64_t>(std::max(options_.offset_save_sec, 1)) * 1000;
  int64_t last_save_ms = util::steady_now_ms();

  while (!stop_.load(std::memory_order_relaxed)) {
    poll();
    if (!options_.offset_file.empty() &&
        util::steady_now_ms() - last_save_ms >= save_interval_ms) {
      engine_.save_offsets();
      last_save_ms = util::steady_now_ms();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.poll_ms));
  }

  engine_.save_offsets();  // final checkpoint, best effort
  if (close_queue_on_exit_) engine_.close_queue();
  util::log_info("glob: stopped (" + std::to_string(engine_.file_count()) +
                 " file(s) still matched)");
  done_.store(true, std::memory_order_relaxed);
}

}  // namespace logpipe
