// index_sidecar.cpp - implementation of the per-file line index sidecar:
// text persistence ("<file>.idx", offset<TAB>last_line_ts_ms per line),
// atomic replacement via tmp+rename and the binary searches used by the
// time-stamp replay mode. See index_sidecar.h.

#include "index_sidecar.h"

#include <algorithm>
#include <fstream>
#include <istream>
#include <system_error>

#include "util.h"

namespace logpipe {

LineIndexSidecar::LineIndexSidecar(std::filesystem::path target_path,
                                   uint64_t interval_bytes)
    : path_(sidecar_path_for(target_path)),
      tmp_path_(path_.string() + ".tmp"),
      interval_bytes_(interval_bytes) {}

std::filesystem::path LineIndexSidecar::sidecar_path_for(
    const std::filesystem::path& target) {
  return std::filesystem::path(target.string() + ".idx");
}

void LineIndexSidecar::load() {
  entries_.clear();
  dirty_ = false;
  std::ifstream in(path_);
  if (!in.is_open()) return;  // no index yet: stays empty, caller falls back
  parse_stream(in);
}

void LineIndexSidecar::parse_stream(std::istream& in) {
  // Format: "# ..." comment/header lines are skipped; every data line is
  // "<offset>\t<last_ts_ms>". Malformed lines are skipped (best effort);
  // the offset-ordered invariant is restored by only accepting entries whose
  // offset is strictly greater than the previous accepted one.
  std::string line;
  uint64_t last_offset = 0;
  bool first = true;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    try {
      const uint64_t offset = std::stoull(line.substr(0, tab));
      const int64_t ts = std::stoll(line.substr(tab + 1));
      if (!first && offset <= last_offset) continue;  // keep sorted invariant
      last_offset = offset;
      first = false;
      LineIndexEntry entry;
      entry.offset = offset;
      entry.last_ts_ms = ts;
      entries_.push_back(entry);
    } catch (const std::exception&) {
      util::log_warn("index: skipping malformed sidecar entry: " + line);
    }
  }
}

bool LineIndexSidecar::save() const {
  if (!dirty_) {
    // Nothing changed: keep an existing file untouched so a quiet tailer
    // does not rewrite its sidecar every interval.
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) return true;
  }
  // Tmp file first, then rename, so a crash mid-write cannot destroy the
  // previous checkpoint (same scheme as the offset state file).
  {
    std::ofstream out(tmp_path_, std::ios::trunc);
    if (!out.is_open()) {
      util::log_warn("index: cannot write sidecar index to '" + tmp_path_.string() + "'");
      return false;
    }
    out << "# logpipe line index (offset<TAB>last_line_ts_ms), interval="
        << interval_bytes_ << "\n";
    for (const LineIndexEntry& entry : entries_) {
      out << entry.offset << '\t' << entry.last_ts_ms << '\n';
    }
    out.flush();
    if (!out.good()) {
      util::log_warn("index: failed while writing sidecar index");
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::remove(path_, ec);  // rename() fails if target exists (Windows)
  ec.clear();
  std::filesystem::rename(tmp_path_, path_, ec);
  if (ec) {
    util::log_warn("index: cannot replace sidecar index: " + ec.message());
    return false;
  }
  dirty_ = false;
  return true;
}

void LineIndexSidecar::clear() {
  entries_.clear();
  dirty_ = true;
}

void LineIndexSidecar::record_mark(uint64_t offset, int64_t ts_ms) {
  // The engine walks the interval grid in increasing order; replacing the
  // trailing entry keeps re-recorded marks (after a resume that re-crosses a
  // known mark) idempotent. Anything older is dropped to preserve sorting.
  if (!entries_.empty()) {
    if (offset < entries_.back().offset) return;
    if (offset == entries_.back().offset) {
      entries_.back().last_ts_ms = ts_ms;
      dirty_ = true;
      return;
    }
  }
  LineIndexEntry entry;
  entry.offset = offset;
  entry.last_ts_ms = ts_ms;
  entries_.push_back(entry);
  dirty_ = true;
}

bool LineIndexSidecar::find_start_offset(int64_t since_ms, uint64_t& out_offset) const {
  if (entries_.empty() || entries_.front().last_ts_ms > since_ms) {
    // Empty index, or every indexed mark is already newer than the requested
    // time: the caller must rescan the file from the beginning.
    return false;
  }
  // Binary search over the timestamp-sorted entries: upper_bound gives the
  // first entry with ts > since_ms, so its predecessor is the last mark at
  // or before the requested time.
  const auto upper = std::upper_bound(
      entries_.begin(), entries_.end(), since_ms,
      [](int64_t value, const LineIndexEntry& entry) {
        return value < entry.last_ts_ms;
      });
  // `upper != begin()` is guaranteed by the front() guard above.
  out_offset = (upper - 1)->offset;
  return true;
}

bool LineIndexSidecar::find_first_at_or_after(int64_t since_ms,
                                              uint64_t& out_offset) const {
  if (entries_.empty()) return false;
  const auto lower = std::lower_bound(
      entries_.begin(), entries_.end(), since_ms,
      [](const LineIndexEntry& entry, int64_t value) {
        return entry.last_ts_ms < value;
      });
  if (lower == entries_.end()) return false;
  out_offset = lower->offset;
  return true;
}

}  // namespace logpipe
