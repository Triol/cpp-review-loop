// test_replay.cpp - tests for the sidecar line index and the time-stamp
// replay mode (replay.since):
//
//   1. LineIndexSidecar unit tests: record/save/load round-trip and the
//      binary searches used for the replay positioning;
//   2. tailer integration: the "<file>.idx" sidecar is written for tailed
//      files, reloaded on the next run and used to position the replay;
//   3. replay.since exact behaviour: the first line whose in-line timestamp
//      is >= since is always emitted (index hit and full-scan fallback);
//   4. Metrics replay counters: index hit/fallback and skipped bytes;
//   5. config parsing of replay.since / replay.index_interval_bytes.
//
// The framework is shared via test_framework.h; test_main.cpp runs every
// registered case.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "index_sidecar.h"
#include "pipeline.h"
#include "tail_engine.h"
#include "tailer.h"
#include "test_framework.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_dir_seq{0};

fs::path make_temp_dir(const char* prefix) {
  const fs::path dir =
      fs::temp_directory_path() /
      (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(g_dir_seq.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

void write_text_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

// Reads a whole file back (binary, exact bytes).
std::string read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// A timestamped log line: "YYYY-MM-DD HH:MM:SS [INFO] replay-line-NNN".
std::string make_line(int index, int year, int month, int day, int hour, int minute,
                      int second) {
  char buffer[80];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d [INFO] replay-line-%03d",
                year, month, day, hour, minute, second, index);
  return buffer;
}

// Writes `count` lines, one per second starting at the given local time.
std::string make_log_text(int count, int start_second) {
  std::string text;
  for (int i = 0; i < count; ++i) {
    text += make_line(i, 2026, 1, 1, 10, 0, start_second + i);
    text += '\n';
  }
  return text;
}

int64_t since_ms(int second) {
  int64_t ms = 0;
  if (!logpipe::util::parse_datetime_ms(make_line(0, 2026, 1, 1, 10, 0, second).substr(0, 19), ms)) {
    return 0;
  }
  return ms;
}

// Drains the queue until it closes or the deadline passes; returns all texts.
std::vector<std::string> drain_texts(logpipe::BlockingQueue<logpipe::RawLine>& queue,
                                     int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  std::vector<std::string> texts;
  while (std::chrono::steady_clock::now() < deadline && !queue.closed()) {
    logpipe::RawLine raw;
    if (queue.pop_for(raw, 25) ==
        logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
      texts.push_back(raw.text);
    }
  }
  return texts;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. LineIndexSidecar: write/load round-trip and the replay searches.
// ---------------------------------------------------------------------------
TEST(sidecar_index_save_load_roundtrip) {
  const fs::path dir = make_temp_dir("logpipe_idx_rt_");
  const fs::path target = dir / "app.log";
  write_text_file(target, "content\n");

  logpipe::LineIndexSidecar writer(target, 128);
  CHECK(writer.empty());
  writer.record_mark(128, 1000);
  writer.record_mark(256, 2500);
  writer.record_mark(384, 3000);
  // Re-recording the trailing mark is idempotent (resume may re-cross it).
  writer.record_mark(384, 3000);
  CHECK_EQ_INT(writer.size(), 3);
  CHECK(writer.dirty());

  CHECK(writer.save());
  CHECK(fs::exists(logpipe::LineIndexSidecar::sidecar_path_for(target)));
  // Second save with clean state must keep the file untouched (and succeed).
  CHECK(writer.save());

  // Round-trip: a fresh instance loads the same entries.
  logpipe::LineIndexSidecar reader(target, 128);
  CHECK(reader.empty());  // nothing loaded yet
  reader.load();
  CHECK_EQ_INT(reader.size(), 3);
  CHECK_EQ_INT(reader.entries()[0].offset, 128);
  CHECK_EQ_INT(reader.entries()[0].last_ts_ms, 1000);
  CHECK_EQ_INT(reader.entries()[1].offset, 256);
  CHECK_EQ_INT(reader.entries()[1].last_ts_ms, 2500);
  CHECK_EQ_INT(reader.entries()[2].offset, 384);
  CHECK_EQ_INT(reader.entries()[2].last_ts_ms, 3000);

  // Replay positioning: last entry with ts <= since.
  uint64_t offset = 0;
  CHECK(reader.find_start_offset(999, offset) == false);   // before everything
  CHECK(reader.find_start_offset(1000, offset));           // exact entry hit
  CHECK_EQ_INT(offset, 128);
  CHECK(reader.find_start_offset(2500, offset));           // between entries
  CHECK_EQ_INT(offset, 256);
  CHECK(reader.find_start_offset(2999, offset));
  CHECK_EQ_INT(offset, 256);
  CHECK(reader.find_start_offset(999999, offset));         // after everything
  CHECK_EQ_INT(offset, 384);

  // Upper bound helper: first entry at/after the requested time.
  CHECK(reader.find_first_at_or_after(2500, offset));
  CHECK_EQ_INT(offset, 256);
  CHECK(reader.find_first_at_or_after(999999, offset) == false);

  // clear() resets the entries and marks the sidecar dirty again.
  reader.clear();
  CHECK(reader.empty());
  CHECK(reader.dirty());
  fs::remove_all(dir);
}

TEST(sidecar_index_malformed_file_is_tolerated) {
  const fs::path dir = make_temp_dir("logpipe_idx_bad_");
  const fs::path target = dir / "app.log";
  write_text_file(logpipe::LineIndexSidecar::sidecar_path_for(target),
                  "# header\nnot an entry\n123\tx\n4096\t7000\n4096\t8000\n\n");
  logpipe::LineIndexSidecar sidecar(target, 4096);
  sidecar.load();
  // Malformed lines skipped; duplicate/out-of-order offsets collapse.
  CHECK_EQ_INT(sidecar.size(), 1);
  CHECK_EQ_INT(sidecar.entries()[0].offset, 4096u);
  CHECK_EQ_INT(sidecar.entries()[0].last_ts_ms, 7000);
  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 2. Tailer integration: the sidecar is written for tailed files and reloaded.
// ---------------------------------------------------------------------------
TEST(tailer_writes_and_reloads_sidecar_index) {
  const fs::path dir = make_temp_dir("logpipe_idx_tail_");
  const fs::path input = dir / "input.log";
  write_text_file(input, make_log_text(40, 0));

  logpipe::BlockingQueue<logpipe::RawLine> queue(256);
  logpipe::TailOptions options;
  options.poll_ms = 10;
  options.index_interval_bytes = 64;  // dense marks for a small test file
  logpipe::Tailer tailer(std::vector<fs::path>{input}, queue, options);
  tailer.start();
  drain_texts(queue, 2000);
  tailer.request_stop();
  tailer.join();

  const fs::path idx = logpipe::LineIndexSidecar::sidecar_path_for(input);
  CHECK(fs::exists(idx));  // shutdown checkpoint wrote the sidecar

  logpipe::LineIndexSidecar sidecar(input, 64);
  sidecar.load();
  CHECK(!sidecar.empty());
  // Entries must be strictly increasing in offset and carry a real timestamp
  // (extracted from the in-line timestamps of the emitted lines).
  uint64_t previous = 0;
  int64_t max_ts = 0;
  for (const logpipe::LineIndexEntry& entry : sidecar.entries()) {
    CHECK(entry.offset > previous);
    previous = entry.offset;
    if (entry.last_ts_ms > max_ts) max_ts = entry.last_ts_ms;
  }
  CHECK(max_ts >= since_ms(0));   // first line's timestamp
  CHECK(max_ts <= since_ms(39));  // last line's timestamp

  // Second run on the same file picks the index up and keeps appending marks.
  logpipe::BlockingQueue<logpipe::RawLine> queue2(256);
  logpipe::Tailer tailer2(std::vector<fs::path>{input}, queue2, options);
  tailer2.load_indexes();
  CHECK_EQ_INT(tailer2.file_count(), 1);
  tailer2.request_stop();
  tailer2.join();
  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 3. replay.since positioning: index hit (exact) and full-scan fallback.
// ---------------------------------------------------------------------------
TEST(replay_since_positions_via_index_hit) {
  const fs::path dir = make_temp_dir("logpipe_replay_hit_");
  const fs::path input = dir / "input.log";
  const int anchor_second = 20;  // replay start: 10:00:20
  write_text_file(input, make_log_text(40, 0));

  // Phase 1: plain tail run builds the sidecar index.
  logpipe::TailOptions options;
  options.poll_ms = 10;
  options.index_interval_bytes = 64;
  {
    logpipe::BlockingQueue<logpipe::RawLine> queue(256);
    logpipe::Tailer builder(std::vector<fs::path>{input}, queue, options);
    builder.start();
    drain_texts(queue, 2000);
    builder.request_stop();
    builder.join();
  }
  CHECK(fs::exists(logpipe::LineIndexSidecar::sidecar_path_for(input)));

  // Phase 2: replay run with since = the anchor line's timestamp.
  logpipe::BlockingQueue<logpipe::RawLine> queue(256);
  options.replay_since_ms = since_ms(anchor_second);
  logpipe::Tailer tailer(std::vector<fs::path>{input}, queue, options);
  tailer.start();
  const std::vector<std::string> texts = drain_texts(queue, 2000);
  tailer.request_stop();
  tailer.join();
  CHECK(texts.size() >= 20);  // anchor line plus the 19 lines after it

  // The anchor (first line with an in-line timestamp >= since) is included.
  const std::string anchor = make_line(anchor_second, 2026, 1, 1, 10, 0, anchor_second);
  bool saw_anchor = false;
  for (const std::string& text : texts) {
    if (text == anchor) saw_anchor = true;
    // Nothing older than the anchor may be replayed.
    CHECK(text >= anchor);
  }
  CHECK(saw_anchor);
  // Positioned via the sidecar index (no fallback).
  CHECK_EQ_INT(tailer.replay_index_hits(), 1);
  CHECK_EQ_INT(tailer.replay_index_fallbacks(), 0);
  // NOTE: replay_skipped_bytes() is intentionally NOT asserted here — with a
  // 64-byte index interval the binary search can land exactly on the anchor
  // line's start (optimal positioning), in which case the line-skip phase
  // legitimately runs zero times. The skip accounting is asserted
  // deterministically in the fallback test below.
  fs::remove_all(dir);
}

TEST(replay_since_falls_back_to_full_scan_without_index) {
  const fs::path dir = make_temp_dir("logpipe_replay_fb_");
  const fs::path input = dir / "input.log";
  const int anchor_second = 15;
  const std::string text = make_log_text(40, 0);
  write_text_file(input, text);
  // No sidecar at all: the replay must scan the whole file and still land
  // exactly on the first line whose timestamp is >= since.

  logpipe::TailOptions options;
  options.poll_ms = 10;
  options.index_interval_bytes = 64;
  options.replay_since_ms = since_ms(anchor_second);
  logpipe::BlockingQueue<logpipe::RawLine> queue(256);
  logpipe::Tailer tailer(std::vector<fs::path>{input}, queue, options);
  tailer.start();
  const std::vector<std::string> texts = drain_texts(queue, 2000);
  tailer.request_stop();
  tailer.join();
  CHECK(texts.size() >= 25);

  const std::string anchor = make_line(anchor_second, 2026, 1, 1, 10, 0, anchor_second);
  bool saw_anchor = false;
  bool first = true;
  for (const std::string& item : texts) {
    if (item == anchor) saw_anchor = true;
    CHECK(item >= anchor);
    if (first) {
      // Exact behaviour: the very first emitted line is the anchor.
      CHECK_STR_EQ(item, anchor);
      first = false;
    }
  }
  CHECK(saw_anchor);
  CHECK_EQ_INT(tailer.replay_index_hits(), 0);
  CHECK_EQ_INT(tailer.replay_index_fallbacks(), 1);

  // Skipped bytes = every pre-anchor line's byte count (text + newline).
  uint64_t expected_skipped = 0;
  size_t start = 0;
  for (int i = 0; i < anchor_second; ++i) {
    const size_t end = text.find('\n', start);
    expected_skipped += (end - start) + 1;
    start = end + 1;
  }
  CHECK_EQ_INT(tailer.replay_skipped_bytes(), static_cast<long long>(expected_skipped));
  fs::remove_all(dir);
}

TEST(replay_skip_counts_bytes_with_resume_and_raw_lines) {
  // RAW lines (no in-line timestamp) must not anchor the replay and count
  // into the skipped bytes when they appear before the anchor.
  const fs::path dir = make_temp_dir("logpipe_replay_raw_");
  const fs::path input = dir / "input.log";
  std::string text;
  text += "not a timestamped line at all\n";                    // RAW, skipped
  text += make_line(1, 2026, 1, 1, 10, 0, 5) + "\n";           // skipped
  text += make_line(2, 2026, 1, 1, 10, 0, 10) + "\n";          // anchor
  text += make_line(3, 2026, 1, 1, 10, 0, 20) + "\n";          // kept
  write_text_file(input, text);

  logpipe::TailOptions options;
  options.poll_ms = 10;
  options.index_interval_bytes = 0;  // index disabled: full-scan fallback
  options.replay_since_ms = since_ms(10);
  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::Tailer tailer(std::vector<fs::path>{input}, queue, options);
  tailer.start();
  const std::vector<std::string> texts = drain_texts(queue, 2000);
  tailer.request_stop();
  tailer.join();
  CHECK_EQ_INT(texts.size(), 2);
  CHECK_STR_EQ(texts[0], make_line(2, 2026, 1, 1, 10, 0, 10));
  CHECK_STR_EQ(texts[1], make_line(3, 2026, 1, 1, 10, 0, 20));
  CHECK_EQ_INT(tailer.replay_index_fallbacks(), 1);
  // RAW line + first line skipped, anchor onwards emitted.
  const uint64_t expected =
      std::string("not a timestamped line at all\n").size() +
      (make_line(1, 2026, 1, 1, 10, 0, 5) + "\n").size();
  CHECK_EQ_INT(tailer.replay_skipped_bytes(), static_cast<long long>(expected));
  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 4. Metrics: replay counters flow through the snapshot.
// ---------------------------------------------------------------------------
TEST(metrics_record_replay_counters) {
  logpipe::Metrics metrics;
  logpipe::Metrics::Snapshot snap = metrics.snapshot();
  CHECK_EQ_INT(snap.replay_index_hits, 0);
  CHECK_EQ_INT(snap.replay_index_fallbacks, 0);
  CHECK_EQ_INT(snap.replay_skipped_bytes, 0);

  metrics.record_replay_index(2, 1);
  metrics.record_replay_skipped_bytes(1234);
  snap = metrics.snapshot();
  CHECK_EQ_INT(snap.replay_index_hits, 2);
  CHECK_EQ_INT(snap.replay_index_fallbacks, 1);
  CHECK_EQ_INT(snap.replay_skipped_bytes, 1234);
  CHECK_EQ_INT(metrics.replay_index_hits(), 2);
  CHECK_EQ_INT(metrics.replay_skipped_bytes(), 1234);
  // The summary renders the replay section when replay was used.
  const std::string summary = metrics.summary(0);
  CHECK(summary.find("replay index hits") != std::string::npos);
  CHECK(summary.find("1234") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. Config: replay.since validation and the interval knob.
// ---------------------------------------------------------------------------
TEST(config_parses_replay_keys) {
  const fs::path dir = make_temp_dir("logpipe_replay_cfg_");
  const fs::path conf = dir / "logpipe.conf";
  // Minimal valid input/output sources keep the config-level validation (no
  // sources / no output) out of the way: this case is about the replay keys.
  write_text_file(conf,
                  "input.files = in.log\n"
                  "output.file = out.log\n"
                  "replay.since = 2026-03-05 07:08:09\n"
                  "replay.index_interval_bytes = 8192\n");
  const logpipe::Config config = logpipe::Config::load(conf.string());
  CHECK_STR_EQ(config.replay_since, "2026-03-05 07:08:09");
  CHECK(config.replay_since_ms > 0);
  CHECK_EQ_INT(config.replay_index_interval_bytes, 8192);
  // describe() carries the replay state for --dump-config.
  CHECK(config.describe().find("replay_since=2026-03-05 07:08:09") != std::string::npos);

  // Malformed timestamps abort the load.
  write_text_file(conf, "replay.since = not-a-date\n");
  bool threw = false;
  try {
    logpipe::Config::load(conf.string());
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  fs::remove_all(dir);
}

TEST(util_parse_datetime_ms) {
  int64_t ms = 0;
  CHECK(logpipe::util::parse_datetime_ms("2026-01-02 03:04:05", ms));
  CHECK_EQ_INT(ms % 1000, 0);
  // 'T' separator and fractional part are tolerated (parser-compatible).
  CHECK(logpipe::util::parse_datetime_ms("2026-01-02T03:04:05.500", ms));
  CHECK_EQ_INT(ms % 1000, 500);
  // Round-trip against the formatter at second granularity.
  CHECK(logpipe::util::parse_datetime_ms("2026-09-06 12:34:56", ms));
  CHECK_STR_EQ(logpipe::util::format_time_ms(ms).substr(0, 19), "2026-09-06 12:34:56");
  // Garbage is rejected.
  CHECK(!logpipe::util::parse_datetime_ms("2026-13-02 03:04:05", ms));
  CHECK(!logpipe::util::parse_datetime_ms("2026-01-02 99:04:05", ms));
  CHECK(!logpipe::util::parse_datetime_ms("short", ms));
  CHECK(!logpipe::util::parse_datetime_ms("", ms));
}
