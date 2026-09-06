// test_main.cpp - assertion tests for the logpipe pipeline (tailer, config,
// parser, rle, writer, DSL integration) plus the test runner main().
//
// The framework lives in test_framework.h; DSL unit tests live in
// test_dsl.cpp and register into the same registry.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "config.h"
#include "dsl.h"
#include "pipeline.h"
#include "rle.h"
#include "tailer.h"
#include "test_framework.h"
#include "writer.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// F1 regression: after a short read hits EOF, istream sets eofbit|failbit and
// the sentry makes every further read a no-op. poll_file() must clear the
// stream state inside its loop so lines appended to the file after the tailer
// reached EOF are still collected.
// ---------------------------------------------------------------------------
TEST(tailer_reads_appended_lines_after_eof) {
  static std::atomic<int> seq{0};
  const fs::path dir =
      fs::temp_directory_path() /
      ("logpipe_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(seq.fetch_add(1)));
  fs::create_directories(dir);
  const fs::path input = dir / "input.log";
  {
    std::ofstream out(input, std::ios::binary | std::ios::trunc);
    out << "line1\n";
  }

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::TailOptions options;
  options.poll_ms = 10;  // fast polling; offset_file stays empty (no persistence)
  logpipe::Tailer tailer({input}, queue, options);
  std::thread tailer_thread([&tailer] { tailer.run(); });

  // Pops with an overall deadline; returns the last line's text or "" on timeout.
  auto pop_text = [&](int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline && !queue.closed()) {
      logpipe::RawLine raw;
      if (queue.pop_for(raw, 25) ==
          logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
        return raw.text;
      }
    }
    return std::string();
  };

  CHECK_STR_EQ(pop_text(5000), "line1");

  {
    std::ofstream out(input, std::ios::binary | std::ios::app);
    out << "line2\n";
  }

  // Without the fix the tailer is stuck in EOF|fail state and times out here.
  CHECK_STR_EQ(pop_text(3000), "line2");

  tailer.request_stop();
  tailer_thread.join();

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// F7: all elapsed-time reads go through the util module. steady_now_ms()
// exposes the monotonic clock in milliseconds for interval measurement.
// ---------------------------------------------------------------------------
TEST(steady_now_ms_is_monotonic_ms) {
  const int64_t first = logpipe::util::steady_now_ms();
  const int64_t second = logpipe::util::steady_now_ms();
  CHECK(first > 0);        // millisecond magnitude since an arbitrary epoch
  CHECK(second >= first);  // monotonic clock never goes backwards
}

// ---------------------------------------------------------------------------
// F20: the LogParser raw-string regex was terminated early by the `)"` inside
// `(\d{1,6})?)`, so construction threw std::regex_error and every run aborted
// at startup. Construction must succeed and the pattern must parse the
// canonical "timestamp [LEVEL] message" line, falling back to RAW when the
// level word is unknown.
// ---------------------------------------------------------------------------
TEST(log_parser_parses_canonical_line) {
  std::optional<logpipe::LogParser> parser;
  try {
    parser.emplace();
  } catch (const std::exception& e) {
    // F20 red: construction itself fails before any line can be parsed.
    std::fprintf(stderr, "  LogParser() threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }

  const logpipe::LogRecord rec =
      parser->parse("in.log", 1234567, "2026-08-30 12:00:00 [INFO] hello world");
  CHECK(rec.parsed);
  CHECK(rec.level == logpipe::Level::Info);
  CHECK_STR_EQ(rec.timestamp_text, "2026-08-30 12:00:00");
  CHECK_STR_EQ(rec.message, "hello world");

  // Unknown level word: falls back to a verbatim RAW record.
  const logpipe::LogRecord raw =
      parser->parse("in.log", 1234567, "2026-08-30 12:00:00 [WIBBER] x");
  CHECK(!raw.parsed);
  CHECK(raw.level == logpipe::Level::Raw);
  CHECK_STR_EQ(raw.message, "2026-08-30 12:00:00 [WIBBER] x");
}

// ---------------------------------------------------------------------------
// F4: std::stoull fully consumes "-5" and wraps around to a huge value, so
// the rotate.size_bytes lower bound cannot reject it. Signed values must be
// rejected before parsing.
// ---------------------------------------------------------------------------
TEST(config_rejects_negative_rotate_size) {
  static std::atomic<int> cfg_seq{0};
  const fs::path dir =
      fs::temp_directory_path() /
      ("logpipe_cfg_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(cfg_seq.fetch_add(1)));
  fs::create_directories(dir);
  const fs::path cfg = dir / "negative.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "rotate.size_bytes = -5\n";
  }

  bool threw = false;
  try {
    logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    threw = true;
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
  }
  CHECK(threw);  // F4 red: without the fix the wrapped value passes validation

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// RLE compression: round-trip on highly compressible data. The container must
// carry the magic and the original length, decode to the exact input and beat
// the raw size for repetitive log-like content.
// ---------------------------------------------------------------------------
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

logpipe::LogRecord make_record(const std::string& source, const std::string& message) {
  logpipe::LogRecord rec;
  rec.source = source;
  rec.ingested_ms = 1757160000000;  // fixed so output lines are deterministic
  rec.timestamp_text = "2026-09-06 12:00:00";
  rec.level = logpipe::Level::Info;
  rec.message = message;
  rec.parsed = true;
  return rec;
}

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST(rle_roundtrip_compressible_data) {
  const std::string raw(4096, 'x');
  const std::string packed = logpipe::rle::compress(raw);
  CHECK(logpipe::rle::has_magic(packed));
  CHECK(packed.size() > logpipe::rle::kHeaderSize);
  CHECK(packed.size() < raw.size() / 4);  // 'x'*4096 must shrink massively
  CHECK_STR_EQ(logpipe::rle::decompress_or_empty(packed), raw);

  // Mixed log-like content: repeated tokens with varying runs.
  std::string mixed;
  for (int i = 0; i < 100; ++i) mixed += "2026-09-06 12:00:00 [INFO] tick aabbbbbb\n";
  const std::string packed_mixed = logpipe::rle::compress(mixed);
  CHECK_STR_EQ(logpipe::rle::decompress_or_empty(packed_mixed), mixed);
}

TEST(rle_roundtrip_incompressible_data) {
  // Deterministic pseudo-random bytes: no long runs, so the container may be
  // larger than the input but must still decode exactly.
  std::string raw;
  uint32_t state = 123456789u;
  for (int i = 0; i < 5000; ++i) {
    state = state * 1664525u + 1013904223u;
    raw.push_back(static_cast<char>((state >> 16) & 0xFFu));
  }
  const std::string packed = logpipe::rle::compress(raw);
  CHECK(logpipe::rle::has_magic(packed));
  CHECK_STR_EQ(logpipe::rle::decompress_or_empty(packed), raw);

  // Empty input round-trips to an empty output.
  const std::string empty_packed = logpipe::rle::compress("");
  CHECK(logpipe::rle::has_magic(empty_packed));
  CHECK_STR_EQ(logpipe::rle::decompress_or_empty(empty_packed), "");
}

TEST(rle_rejects_malformed_containers) {
  const std::string packed = logpipe::rle::compress("aaaaabbbb");
  std::string out;

  // Truncation: cut off the tail mid-token.
  CHECK(!logpipe::rle::decompress(packed.substr(0, packed.size() - 2), out));
  CHECK(out.empty());

  // Wrong magic.
  std::string bad_magic = packed;
  bad_magic[0] = 'X';
  CHECK(!logpipe::rle::decompress(bad_magic, out));

  // Declared original length too large (no data to back it).
  std::string inflated = packed;
  inflated[4] = '\x10';  // original length += 0x10000000000
  CHECK(!logpipe::rle::decompress(inflated, out));

  CHECK(!logpipe::rle::decompress("", out));
}

// ---------------------------------------------------------------------------
// Daily rotation: with an injected clock the writer must name the active file
// after the current date, close it when the date changes and start a new one,
// counting the transition as a by-day rotation.
// ---------------------------------------------------------------------------
TEST(daily_rotation_uses_date_names_and_rotates_on_day_change) {
  const fs::path dir = make_temp_dir("logpipe_day_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions opts;
  opts.output_dir = dir;
  opts.base_name = "out.log";
  opts.format = logpipe::OutputFormat::Text;
  opts.daily_rotation = true;
  int64_t fake_now = 1788696000000;  // 2026-09-06 12:00 UTC (20:00 in UTC+8)
  opts.clock = [&fake_now] { return fake_now; };

  // Derive the expected names with the same util conversion the writer uses,
  // so the test is timezone independent.
  const std::string day1_date =
      logpipe::util::timestamp_compact(fake_now).substr(0, 8);
  const std::string day2_date =
      logpipe::util::timestamp_compact(fake_now + 24ull * 3600 * 1000).substr(0, 8);
  CHECK(day1_date != day2_date);

  logpipe::RollingWriter writer(opts, &metrics);
  CHECK(writer.open());
  CHECK_STR_EQ(writer.active_file_name(), "out_" + day1_date + ".log");
  uint64_t bytes = 0;
  CHECK(writer.write(make_record("src", "day one line"), bytes));

  fake_now += 24ull * 3600 * 1000;  // next day
  CHECK(writer.write(make_record("src", "day two line"), bytes));
  CHECK_STR_EQ(writer.active_file_name(), "out_" + day2_date + ".log");

  writer.close();  // flush both files before reading them back
  CHECK(!writer.failed());
  CHECK(metrics.rotations_by_day() == 1);
  CHECK(metrics.rotations_by_size() == 0);

  const fs::path day1_file = dir / ("out_" + day1_date + ".log");
  const std::string day1 = read_text_file(day1_file);
  CHECK(day1.find("day one line") != std::string::npos);
  CHECK(day1.find("day two line") == std::string::npos);
  const std::string day2 = read_text_file(dir / ("out_" + day2_date + ".log"));
  CHECK(day2.find("day two line") != std::string::npos);
  CHECK(day2.find("day one line") == std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Size rotation still works alongside the daily trigger and is counted as a
// by-size rotation (kept separate from the by-day counter).
// ---------------------------------------------------------------------------
TEST(size_rotation_counts_separately_from_day_rotation) {
  const fs::path dir = make_temp_dir("logpipe_size_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions opts;
  opts.output_dir = dir;
  opts.base_name = "out.log";
  opts.max_bytes_per_file = 256;  // force a rotation after a few lines
  opts.max_backups = 3;
  opts.daily_rotation = true;  // date naming active but the day never changes
  int64_t fake_now = 1788696000000;  // fixed day, never changes in this test
  opts.clock = [&fake_now] { return fake_now; };

  const std::string day_date =
      logpipe::util::timestamp_compact(fake_now).substr(0, 8);
  logpipe::RollingWriter writer(opts, &metrics);
  CHECK(writer.open());
  for (int i = 0; i < 10; ++i) {
    uint64_t bytes = 0;
    CHECK(writer.write(make_record("src", "payload line number " + std::to_string(i)), bytes));
  }
  writer.close();
  CHECK(!writer.failed());
  CHECK(metrics.rotations_by_size() >= 1);
  CHECK(metrics.rotations_by_day() == 0);
  // The active file keeps the dated name; at least one backup exists.
  CHECK(fs::exists(dir / ("out_" + day_date + "_1.log")));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Compressed output: with output.compress = rle the writer produces frames of
// length-prefixed RLE containers; read_compressed_output() must reconstruct
// the exact line sequence, and Metrics must see raw vs packed byte counts.
// ---------------------------------------------------------------------------
TEST(compressed_output_roundtrip_and_metrics) {
  const fs::path dir = make_temp_dir("logpipe_rle_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions opts;
  opts.output_dir = dir;
  opts.base_name = "out.log";
  opts.compress = logpipe::CompressMode::Rle;
  logpipe::RollingWriter writer(opts, &metrics);
  CHECK(writer.open());
  CHECK_STR_EQ(writer.active_file_name(), "out.log.rle");

  uint64_t bytes = 0;
  CHECK(writer.write(make_record("src", std::string(200, 'z')), bytes));
  const uint64_t first_frame = bytes;
  CHECK(writer.write(make_record("src", "plain line"), bytes));
  writer.close();
  CHECK(!writer.failed());

  // The raw file is not plain text; the decoder restores the exact lines.
  const std::string packed = read_text_file(dir / "out.log.rle");
  CHECK(!packed.empty());
  CHECK(logpipe::rle::has_magic(packed.substr(4)));  // skip the frame length
  std::string decoded;
  CHECK(logpipe::read_compressed_output(dir / "out.log.rle", decoded));
  CHECK(decoded.find(std::string(200, 'z')) != std::string::npos);
  CHECK(decoded.find("plain line") != std::string::npos);
  CHECK(decoded.find("|crc=") != std::string::npos);  // CRC stamping preserved

  CHECK(metrics.compressed_raw_bytes() > 0);
  CHECK(metrics.compressed_packed_bytes() > 0);
  CHECK(metrics.compressed_packed_bytes() < metrics.compressed_raw_bytes());
  CHECK(first_frame > 0);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Per-source files: each input source must land in its own output file whose
// name embeds a sanitized form of the source, and Metrics must track the
// output bytes per source.
// ---------------------------------------------------------------------------
TEST(per_source_files_split_output_and_metrics) {
  const fs::path dir = make_temp_dir("logpipe_persrc_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions opts;
  opts.output_dir = dir;
  opts.base_name = "out.log";
  opts.per_source_files = true;
  logpipe::PerSourceWriter sink(opts, &metrics);
  CHECK(sink.open());

  uint64_t bytes_a = 0, bytes_b = 0;
  CHECK(sink.write(make_record("logs/app.log", "from app"), bytes_a));
  CHECK(sink.write(make_record("logs/error.log", "from error"), bytes_b));
  CHECK(sink.write(make_record("logs/app.log", "from app again"), bytes_a));
  sink.close();
  CHECK(!sink.failed());
  CHECK(sink.sink_count() == 2);
  // The main loop records the per-source output byte totals (the writer only
  // reports compression and rotation counters itself).
  metrics.record_written(bytes_a, "logs/app.log");
  metrics.record_written(bytes_b, "logs/error.log");

  const std::string app = read_text_file(dir / "out_app_log.log");
  const std::string err = read_text_file(dir / "out_error_log.log");
  CHECK(app.find("from app") != std::string::npos);
  CHECK(app.find("from app again") != std::string::npos);
  CHECK(app.find("from error") == std::string::npos);
  CHECK(err.find("from error") != std::string::npos);
  CHECK(err.find("from app") == std::string::npos);

  const std::string summary = metrics.summary(0);
  CHECK(summary.find("output bytes per source:") != std::string::npos);
  CHECK(summary.find("logs/app.log") != std::string::npos);
  CHECK(summary.find("logs/error.log") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Metrics extension: the summary must carry the new compression counters,
// the split rotation counters and the per-source output byte totals.
// ---------------------------------------------------------------------------
TEST(metrics_summary_reports_new_fields) {
  const fs::path dir = make_temp_dir("logpipe_metrics_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions opts;
  opts.output_dir = dir;
  opts.base_name = "out.log";
  opts.compress = logpipe::CompressMode::Rle;
  logpipe::RollingWriter writer(opts, &metrics);
  CHECK(writer.open());
  uint64_t bytes = 0;
  CHECK(writer.write(make_record("only_source", std::string(300, 'q')), bytes));
  writer.close();
  metrics.record_written(bytes, "only_source");
  metrics.record_rotation(true);
  metrics.record_rotation(false);

  const std::string summary = metrics.summary(0);
  CHECK(summary.find("compress raw bytes   : ") != std::string::npos);
  CHECK(summary.find("compress packed bytes: ") != std::string::npos);
  CHECK(summary.find("rotations by size : 1") != std::string::npos);
  CHECK(summary.find("rotations by day  : 1") != std::string::npos);
  CHECK(summary.find("output bytes per source:") != std::string::npos);
  CHECK(summary.find("only_source") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// filter.expr config integration: Config::load compiles the expression, the
// raw text shows up in describe(), and the compiled form drives the filter.
// ---------------------------------------------------------------------------
TEST(config_loads_filter_expr_and_drives_filter) {
  const fs::path dir = make_temp_dir("logpipe_fexpr_");
  const fs::path cfg = dir / "expr.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "filter.level = INFO\n"
        << "filter.expr = level >= \"WARN\" AND (msg CONTAINS \"timeout\" OR src ENDS_WITH \".log\")\n";
  }

  logpipe::Config config;
  try {
    config = logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }
  CHECK(config.filter_expr != nullptr);
  CHECK(config.filter_expr_text.find("WARN") != std::string::npos);
  CHECK(config.describe().find("filter.expr=") != std::string::npos);
  CHECK(config.describe().find("timeout") != std::string::npos);

  // Same wiring main() uses: DSL first, then threshold/keyword.
  const std::shared_ptr<const logpipe::dsl::FilterExpr> expr = config.filter_expr;
  logpipe::LogFilter filter(config.level_threshold, config.keyword,
                            [expr](const logpipe::LogRecord& r) { return expr->passes(r); });
  CHECK(filter.dsl_active());

  const auto mk = [&](const char* src, const std::string& msg, logpipe::Level level) {
    logpipe::LogRecord r = make_record(src, msg);
    r.level = level;
    return r;
  };
  // DSL passes, threshold passes.
  CHECK(filter.passes(mk("a.log", "connect timeout", logpipe::Level::Warn)));
  // DSL passes via src, but the INFO threshold drops it: DSL first, threshold second.
  CHECK(!filter.passes(mk("b.LOG", "all good", logpipe::Level::Info)));
  // DSL rejects (level gate ok but neither msg nor src arm matches).
  CHECK(!filter.passes(mk("c.txt", "all good", logpipe::Level::Error)));
  // DSL and threshold both reject.
  CHECK(!filter.passes(mk("c.txt", "all good", logpipe::Level::Debug)));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// filter.expr without threshold/keyword interplay: when the key is absent the
// filter must keep the plain threshold/keyword behaviour (no DSL stage).
// ---------------------------------------------------------------------------
TEST(config_without_filter_expr_keeps_plain_filter) {
  const fs::path dir = make_temp_dir("logpipe_noexpr_");
  const fs::path cfg = dir / "plain.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "filter.level = WARN\n"
        << "filter.keyword = disk\n";
  }

  const logpipe::Config config = logpipe::Config::load(cfg.string());
  CHECK(config.filter_expr == nullptr);
  CHECK(config.filter_expr_text.empty());
  CHECK(config.describe().find("filter.expr=<off>") != std::string::npos);

  logpipe::LogFilter filter(config.level_threshold, config.keyword);
  CHECK(!filter.dsl_active());

  logpipe::LogRecord pass = make_record("a.log", "hard DISK full");
  pass.level = logpipe::Level::Warn;
  CHECK(filter.passes(pass));

  logpipe::LogRecord low = make_record("a.log", "disk full");
  low.level = logpipe::Level::Info;
  CHECK(!filter.passes(low));  // threshold still applies

  logpipe::LogRecord no_keyword = make_record("a.log", "unrelated");
  no_keyword.level = logpipe::Level::Error;
  CHECK(!filter.passes(no_keyword));  // keyword still applies

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// A malformed filter.expr must abort Config::load with a dsl::Error that
// carries the position in the expression text.
// ---------------------------------------------------------------------------
TEST(config_rejects_malformed_filter_expr_with_position) {
  struct Case {
    const char* expr;
    int position;
  };
  const Case cases[] = {
      {"level >= ", 9},               // missing literal (End token position)
      {"msg CONTAINS \"oops", 19},    // unterminated string
      {R"(host = "h")", 1},            // unknown field
      {R"(level = "WARN" extra)", 16}, // trailing garbage
  };
  for (const Case& c : cases) {
    const fs::path dir = make_temp_dir("logpipe_badexpr_");
    const fs::path cfg = dir / "bad.conf";
    {
      std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
      out << "input.files = dummy.log\n"
          << "filter.expr = " << c.expr << "\n";
    }
    bool threw = false;
    try {
      logpipe::Config::load(cfg.string());
    } catch (const logpipe::dsl::Error& e) {
      threw = true;
      CHECK_EQ_INT(e.pos, c.position);
      CHECK(std::string(e.what()).find("filter.expr") != std::string::npos);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  wrong exception for '%s': %s\n", c.expr, e.what());
      ++testfw::failures();
    }
    CHECK(threw);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

// ---------------------------------------------------------------------------
// End to end: file tailer -> queue -> parser -> DSL-first filter -> rolling
// writer. Only records surviving the DSL expression may reach the output.
// ---------------------------------------------------------------------------
TEST(pipeline_end_to_end_dsl_first_filter) {
  const fs::path dir = make_temp_dir("logpipe_e2e_");
  const fs::path input = dir / "app.log";
  {
    std::ofstream out(input, std::ios::binary | std::ios::trunc);
    out << "2026-09-06 12:00:00 [WARN] connect timeout to db\n"
        << "2026-09-06 12:00:01 [INFO] routine heartbeat ok\n"
        << "2026-09-06 12:00:02 [ERROR] disk failure imminent\n"
        << "plain unparsed noise line\n";
  }

  const std::string expr_text = R"(level >= "WARN")";
  const auto expr = logpipe::dsl::FilterExpr::compile(expr_text);

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::TailOptions topts;
  topts.poll_ms = 10;  // offset_file stays empty (no persistence)
  logpipe::Tailer tailer({input}, queue, topts);
  std::thread tailer_thread([&tailer] { tailer.run(); });

  logpipe::LogParser parser;
  logpipe::LogFilter filter(logpipe::Level::Debug, "",
                            [expr](const logpipe::LogRecord& r) { return expr->passes(r); });
  CHECK(filter.dsl_active());

  logpipe::Metrics metrics;
  logpipe::WriterOptions wopts;
  wopts.output_dir = dir;
  wopts.base_name = "e2e_out.log";
  wopts.daily_rotation = true;
  int64_t fake_now = 1788696000000;
  wopts.clock = [&fake_now] { return fake_now; };
  logpipe::RollingWriter writer(wopts, &metrics);
  CHECK(writer.open());

  int written = 0, filtered = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (written + filtered < 4 && std::chrono::steady_clock::now() < deadline) {
    logpipe::RawLine raw;
    if (queue.pop_for(raw, 50) != logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
      continue;
    }
    const logpipe::LogRecord record = parser.parse(raw.source, raw.ingested_ms, raw.text);
    metrics.record_input(record.level);
    if (filter.passes(record)) {
      uint64_t bytes = 0;
      CHECK(writer.write(record, bytes));
      ++written;
    } else {
      ++filtered;
    }
  }

  tailer.request_stop();
  tailer_thread.join();
  writer.close();
  CHECK(!writer.failed());

  // WARN timeout, ERROR disk and the RAW noise (Raw ranks above every gate)
  // pass; the INFO heartbeat is dropped by the DSL even though a plain
  // DEBUG threshold would have kept it.
  CHECK_EQ_INT(written, 3);
  CHECK_EQ_INT(filtered, 1);

  const std::string day =
      logpipe::util::timestamp_compact(fake_now).substr(0, 8);
  const std::string out = read_text_file(dir / ("e2e_out_" + day + ".log"));
  CHECK(out.find("connect timeout to db") != std::string::npos);
  CHECK(out.find("disk failure imminent") != std::string::npos);
  CHECK(out.find("plain unparsed noise line") != std::string::npos);
  CHECK(out.find("routine heartbeat ok") == std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// End to end with a config file only (no hand-compiled expression): the same
// pipeline wiring but DSL text comes from filter.expr in the config, and the
// threshold/keyword stage composes after the DSL.
// ---------------------------------------------------------------------------
TEST(pipeline_end_to_end_config_expr_with_keyword_stage) {
  const fs::path dir = make_temp_dir("logpipe_e2ecfg_");
  const fs::path cfg = dir / "e2e.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = " << (dir / "in.log").string() << "\n"
        << "filter.level = INFO\n"
        << "filter.keyword = disk\n"
        << "filter.expr = msg CONTAINS \"disk\"\n";
  }
  const fs::path input = dir / "in.log";
  {
    std::ofstream out(input, std::ios::binary | std::ios::trunc);
    out << "2026-09-06 12:00:00 [ERROR] disk full\n"        // DSL ok, INFO ok, keyword ok
        << "2026-09-06 12:00:01 [WARN] memory pressure\n"   // DSL drops (no "disk")
        << "2026-09-06 12:00:02 [INFO] disk usage 42%\n"    // DSL ok, threshold ok, keyword ok
        << "2026-09-06 12:00:03 [DEBUG] disk spinup\n";     // DSL ok, threshold drops
  }

  const logpipe::Config config = logpipe::Config::load(cfg.string());
  CHECK(config.filter_expr != nullptr);

  const std::shared_ptr<const logpipe::dsl::FilterExpr> expr = config.filter_expr;
  logpipe::LogFilter filter(config.level_threshold, config.keyword,
                            [expr](const logpipe::LogRecord& r) { return expr->passes(r); });

  logpipe::LogParser parser;
  std::vector<std::string> kept;
  {
    std::ifstream in(input, std::ios::binary);
    std::string line;
    int64_t ms = 1757160000000;
    while (std::getline(in, line)) {
      const logpipe::LogRecord record = parser.parse("in.log", ms, line);
      ms += 1000;
      if (filter.passes(record)) kept.push_back(record.message);
    }
  }

  CHECK_EQ_INT(kept.size(), 2);
  CHECK(kept[0].find("disk full") != std::string::npos);
  CHECK(kept[1].find("disk usage 42%") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------

int main() {
  size_t failed_tests = 0;
  for (const auto& tc : testfw::registry()) {
    testfw::failures() = 0;
    std::fprintf(stderr, "[ RUN  ] %s\n", tc.name);
    tc.fn();
    if (testfw::failures() == 0) {
      std::fprintf(stderr, "[  OK  ] %s\n", tc.name);
    } else {
      std::fprintf(stderr, "[ FAIL ] %s (%d check(s) failed)\n", tc.name,
                   testfw::failures());
      ++failed_tests;
    }
  }
  std::fprintf(stderr, "%s: %zu test(s), %zu failed\n",
               failed_tests == 0 ? "PASSED" : "FAILED", testfw::registry().size(),
               failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
