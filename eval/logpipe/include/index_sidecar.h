// index_sidecar.h - per-file line index sidecar for the tail sources.
//
// For every tailed input file the engine maintains a sidecar index file
// "<file>.idx" next to the data file. The index holds one entry per fixed
// byte interval (config: replay.index_interval_bytes = N): the file offset
// of the mark together with the timestamp of the most recent complete line
// at or before that offset:
//
//   # logpipe line index (offset<TAB>last_line_ts_ms)
//   4096   1767715200000
//   8192   1767715201500
//   ...
//
// The sidecar serves the time-stamp replay mode (config: replay.since): at
// startup the engine binary-searches the entries for the last mark whose
// line timestamp is <= the requested "since" and starts reading from that
// offset instead of scanning the whole file. Entries are appended strictly
// in offset order, so the stored sequence is sorted and the timestamps are
// monotonically non-decreasing - both preconditions of the binary search.
//
// Writes go through the same tmp-file + rename dance as the offset state
// file, so a crash mid-write never destroys the previous checkpoint. The
// class is deliberately free of engine dependencies so it can be exercised
// (write/load round-trip, search) directly from the tests.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace logpipe {

// One index entry: a byte offset inside the tailed file and the timestamp
// (epoch ms) of the most recent complete line at or before that offset.
struct LineIndexEntry {
  uint64_t offset = 0;    // byte mark inside the tailed file
  int64_t last_ts_ms = 0; // timestamp of the last line at/before the mark
};

class LineIndexSidecar {
 public:
  // `target_path` is the tailed data file; the sidecar lives at
  // "<target_path>.idx". `interval_bytes` is the entry cadence (the N from
  // replay.index_interval_bytes); it is only recorded for diagnostics - the
  // engine drives when to call record_mark().
  LineIndexSidecar(std::filesystem::path target_path, uint64_t interval_bytes);

  // Sidecar file path for a tailed data file: "<file>.idx".
  static std::filesystem::path sidecar_path_for(const std::filesystem::path& target);

  // ---- persistence ---------------------------------------------------------

  // Reads "<file>.idx" (best effort: a missing or malformed file leaves the
  // entry list empty; malformed trailing entries are skipped with a warning).
  void load();

  // Atomically writes the entries: tmp file first, then rename over the
  // target. Only writes when the in-memory state is dirty (a mark was
  // recorded or the index was cleared) or the file does not exist yet.
  // Returns false when the write itself failed (already reported via diag).
  bool save() const;

  // Drops every entry (used when the tailed file was truncated: the old
  // offsets no longer refer to the same content).
  void clear();

  // ---- entries -------------------------------------------------------------

  // Records (or replaces) the entry for byte mark `offset` with the last
  // known line timestamp `ts_ms`. Marks are expected to arrive in increasing
  // order (the engine walks the interval grid); an out-of-order mark smaller
  // than the largest stored one is ignored (keeps the vector sorted).
  void record_mark(uint64_t offset, int64_t ts_ms);

  const std::vector<LineIndexEntry>& entries() const { return entries_; }
  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }
  bool dirty() const { return dirty_; }
  uint64_t interval_bytes() const { return interval_bytes_; }
  const std::filesystem::path& path() const { return path_; }

  // ---- replay positioning --------------------------------------------------

  // Binary-searches the entries for the LAST entry whose line timestamp is
  // <= `since_ms` (the closest mark at or before the requested time). Returns
  // true and fills `out_offset` with that entry's file offset. Returns false
  // when the index is empty or every entry is newer than `since_ms` (the
  // caller falls back to a full-file scan, because the first line with a
  // timestamp >= since may sit before the earliest indexed mark).
  bool find_start_offset(int64_t since_ms, uint64_t& out_offset) const;

  // Convenience: first entry offset whose timestamp is >= `since_ms`, i.e.
  // the upper bound of the search above. Returns false when no such entry
  // exists (used by tests and diagnostics).
  bool find_first_at_or_after(int64_t since_ms, uint64_t& out_offset) const;

 private:
  // Loads entries from an open text stream (shared by load() and tests).
  void parse_stream(std::istream& in);

  std::filesystem::path path_;     // "<file>.idx"
  std::filesystem::path tmp_path_; // "<file>.idx<tmp_suffix>" scratch file
  uint64_t interval_bytes_ = 0;
  std::vector<LineIndexEntry> entries_;  // sorted by offset, ts non-decreasing
  mutable bool dirty_ = false;     // unsaved in-memory changes
};

}  // namespace logpipe
