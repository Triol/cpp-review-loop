// test_features.cpp - tests for the iteration features: KV field extraction
// (extract.kv), Prometheus text-format statistics export (stats.interval_sec)
// and --dump-config behaviour. DSL kv() tests live in test_dsl.cpp; the
// framework is shared via test_framework.h.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "kv_extractor.h"
#include "pipeline.h"
#include "prom_stats.h"
#include "test_framework.h"

namespace fs = std::filesystem;

// Shared temp-dir helper (same pattern as test_main.cpp).
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

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// KV extractor: plain pairs, quoted values (with spaces and embedded quotes),
// single-quoted values, empty values and duplicate keys.
// ---------------------------------------------------------------------------
TEST(kv_extractor_plain_and_quoted_values) {
  logpipe::KvExtractor kv;
  std::map<std::string, std::string> fields;

  CHECK(kv.extract("k1=v1 k2=v2", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("k1"), "v1");
  CHECK_STR_EQ(fields.at("k2"), "v2");

  // Double-quoted value with spaces.
  CHECK(kv.extract("msg=\"hello world\" level=warn", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("msg"), "hello world");
  CHECK_STR_EQ(fields.at("level"), "warn");

  // Single-quoted value with spaces and a verbatim double quote inside.
  CHECK(kv.extract("path='/var/log/app' note=\"quoted \\\"inner\\\" text\"", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("path"), "/var/log/app");
  CHECK_STR_EQ(fields.at("note"), "quoted \"inner\" text");

  // Escaped backslash inside a quoted value.
  CHECK(kv.extract("win=\"C:\\\\temp\\\\log\"", fields));
  CHECK_STR_EQ(fields.at("win"), "C:\\temp\\log");

  // Empty value and duplicate keys (last one wins).
  CHECK(kv.extract("empty= dup=first dup=second", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("empty"), "");
  CHECK_STR_EQ(fields.at("dup"), "second");

  // Key with '=' appearing inside a quoted value must not split the pair.
  CHECK(kv.extract("expr=\"a=b\" tail=ok", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("expr"), "a=b");
  CHECK_STR_EQ(fields.at("tail"), "ok");
}

// ---------------------------------------------------------------------------
// KV extractor: lines without the key=value shape must be rejected cleanly.
// ---------------------------------------------------------------------------
TEST(kv_extractor_rejects_non_kv_lines) {
  logpipe::KvExtractor kv;
  std::map<std::string, std::string> fields;

  // No '=' anywhere.
  CHECK(!kv.extract("just a plain log line", fields));
  CHECK(fields.empty());
  CHECK(!kv.extract("", fields));
  CHECK(!kv.extract("   ", fields));

  // '=' but an empty key.
  CHECK(!kv.extract("=value", fields));
  CHECK(fields.empty());

  // Bare word before any pair: not a KV-shaped line.
  CHECK(!kv.extract("severity then key=value", fields));
  CHECK(fields.empty());

  // Leading whitespace is tolerated; a bare word after valid pairs simply
  // ends the scan and the pairs collected before it are kept.
  CHECK(kv.extract("   a=1 b=2 trailing-word", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("a"), "1");
  CHECK_STR_EQ(fields.at("b"), "2");

  // Single pair at end of line without trailing newline content.
  CHECK(kv.extract("only=pair", fields));
  CHECK_EQ_INT(fields.size(), 1);

  // Quoted value containing '=' then end of line.
  CHECK(kv.extract("q=\"x=y\"", fields));
  CHECK_STR_EQ(fields.at("q"), "x=y");
}

// ---------------------------------------------------------------------------
// extract_tail: message parts with a free-form prefix before the k=v payload.
// ---------------------------------------------------------------------------
TEST(kv_extractor_tail_mode_skips_prefix) {
  logpipe::KvExtractor kv;
  std::map<std::string, std::string> fields;

  CHECK(kv.extract_tail("evt user=bob host=web-01", fields));
  CHECK_EQ_INT(fields.size(), 2);
  CHECK_STR_EQ(fields.at("user"), "bob");
  CHECK_STR_EQ(fields.at("host"), "web-01");

  // Quoted values inside the payload survive the prefix skip.
  CHECK(kv.extract_tail("notice msg=\"hello world\" code=7", fields));
  CHECK_STR_EQ(fields.at("msg"), "hello world");
  CHECK_STR_EQ(fields.at("code"), "7");

  // No pair anywhere: rejected.
  CHECK(!kv.extract_tail("nothing to see here", fields));
  CHECK(fields.empty());

  // Strict extract() still rejects a leading bare word (tail mode is opt-in).
  CHECK(!kv.extract("evt user=bob", fields));
}

// ---------------------------------------------------------------------------
// Metrics KV counters: line attempts, extracted lines, field counts, Top-N
// snapshot trimming and the extraction-rate diagnostic in the summary.
// ---------------------------------------------------------------------------
TEST(metrics_kv_counters_and_top_n) {
  logpipe::Metrics metrics;
  CHECK_EQ_INT(metrics.kv_extraction_rate_percent(), 100);  // nothing attempted yet

  metrics.record_kv_line(false);  // a line without the KV shape
  metrics.record_kv_line(true);
  metrics.record_kv_value("user", "bob");
  metrics.record_kv_value("user", "bob");
  metrics.record_kv_value("user", "alice");
  metrics.record_kv_value("user", "carol");
  metrics.record_kv_value("level", "warn");

  CHECK_EQ_INT(metrics.kv_lines_attempted(), 2);
  CHECK_EQ_INT(metrics.kv_lines_extracted(), 1);
  CHECK_EQ_INT(metrics.kv_fields_extracted(), 5);
  CHECK_EQ_INT(metrics.kv_extraction_rate_percent(), 50);

  // Untrimmed snapshot keeps everything.
  const logpipe::Metrics::Snapshot full = metrics.snapshot();
  CHECK_EQ_INT(full.kv_top_values.at("user").size(), 3);
  CHECK_EQ_INT(full.kv_top_values.at("user").at("bob"), 2);

  // Top-2 snapshot keeps bob(2) and one of alice/carol (count 1, name order).
  const logpipe::Metrics::Snapshot top2 = metrics.snapshot(2);
  CHECK_EQ_INT(top2.kv_top_values.at("user").size(), 2);
  CHECK_EQ_INT(top2.kv_top_values.at("user").at("bob"), 2);
  CHECK(top2.kv_top_values.at("user").count("alice") == 1);
  CHECK(top2.kv_top_values.at("user").count("carol") == 0);
  CHECK_EQ_INT(top2.kv_top_values.at("level").at("warn"), 1);

  // The summary reports the extraction rate once lines were attempted.
  const std::string summary = metrics.summary(0);
  CHECK(summary.find("kv lines attempted : 2") != std::string::npos);
  CHECK(summary.find("kv extraction rate : 50%") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Prometheus render: HELP/TYPE headers, logpipe_ prefix, counter and gauge
// lines, labelled samples and label escaping.
// ---------------------------------------------------------------------------
TEST(prometheus_render_format) {
  logpipe::Metrics metrics;
  metrics.record_input(logpipe::Level::Info);
  metrics.record_input(logpipe::Level::Error);
  metrics.record_input(logpipe::Level::Raw);
  metrics.record_filtered();
  metrics.record_written(1234, "app.log");
  metrics.record_written(100, "stdin");
  metrics.record_source_line("app.log");
  metrics.record_source_line("stdin");
  metrics.record_rate_dropped();
  metrics.record_rotation(true);
  metrics.record_rotation(false);
  metrics.record_compression(500, 200);
  metrics.record_kv_line(true);
  metrics.record_kv_value("user", "bo\"b");
  metrics.record_kv_value("user", "bob");

  const std::string text =
      logpipe::PromStatsWriter::render(metrics.snapshot(/*kv_top_n=*/10));

  // HELP/TYPE headers with the logpipe_ prefix.
  CHECK(text.find("# HELP logpipe_lines_ingested_total ") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_lines_ingested_total counter") != std::string::npos);
  CHECK(text.find("# HELP logpipe_lines_filtered_total ") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_lines_written_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_output_bytes_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_lines_rate_dropped_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_compression_raw_bytes_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_output_rotations_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_kv_lines_attempted_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_kv_extraction_ratio gauge") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_source_lines_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_source_output_bytes_total counter") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_field_value_count counter") != std::string::npos);

  // Counter values: per-level samples plus the total under one HELP/TYPE pair.
  CHECK(text.find("logpipe_lines_ingested_total{level=\"INFO\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_lines_ingested_total{level=\"ERROR\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_lines_ingested_total{level=\"RAW\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_lines_ingested_total 3") != std::string::npos);
  CHECK(text.find("logpipe_lines_filtered_total 1") != std::string::npos);
  CHECK(text.find("logpipe_lines_written_total 2") != std::string::npos);
  CHECK(text.find("logpipe_output_bytes_total 1334") != std::string::npos);
  CHECK(text.find("logpipe_lines_rate_dropped_total 1") != std::string::npos);
  CHECK(text.find("logpipe_compression_raw_bytes_total 500") != std::string::npos);
  CHECK(text.find("logpipe_compression_packed_bytes_total 200") != std::string::npos);
  CHECK(text.find("logpipe_output_rotations_total{trigger=\"size\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_output_rotations_total{trigger=\"day\"} 1") != std::string::npos);

  // KV counters and the gauge (1/1 line extracted -> 100%).
  CHECK(text.find("logpipe_kv_lines_attempted_total 1") != std::string::npos);
  CHECK(text.find("logpipe_kv_lines_extracted_total 1") != std::string::npos);
  CHECK(text.find("logpipe_kv_fields_extracted_total 2") != std::string::npos);
  CHECK(text.find("logpipe_kv_extraction_ratio 100") != std::string::npos);

  // Per-source and field-value labelled samples; label escaping for the quote.
  CHECK(text.find("logpipe_source_lines_total{source=\"app.log\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_source_lines_total{source=\"stdin\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_source_output_bytes_total{source=\"app.log\"} 1234") != std::string::npos);
  CHECK(text.find("logpipe_field_value_count{key=\"user\",value=\"bo\\\"b\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_field_value_count{key=\"user\",value=\"bob\"} 1") != std::string::npos);

  // Exactly one HELP/TYPE pair per metric name (ingested has two sample forms).
  size_t occurrences = 0;
  for (size_t pos = text.find("# HELP logpipe_lines_ingested_total ");
       pos != std::string::npos;
       pos = text.find("# HELP logpipe_lines_ingested_total ", pos + 1)) {
    ++occurrences;
  }
  CHECK_EQ_INT(occurrences, 1);
}

// ---------------------------------------------------------------------------
// Prometheus label escaping helper.
// ---------------------------------------------------------------------------
TEST(prometheus_escape_label) {
  using logpipe::PromStatsWriter;
  CHECK_STR_EQ(PromStatsWriter::escape_label("plain"), "plain");
  CHECK_STR_EQ(PromStatsWriter::escape_label("back\\slash"), "back\\\\slash");
  CHECK_STR_EQ(PromStatsWriter::escape_label("qu\"ote"), "qu\\\"ote");
  CHECK_STR_EQ(PromStatsWriter::escape_label(std::string("new\nline")), "new\\nline");
  CHECK_STR_EQ(PromStatsWriter::escape_label(""), "");
}

// ---------------------------------------------------------------------------
// Export write + retention: timestamped .prom files are created in the stats
// directory and only the newest keep_files survive.
// ---------------------------------------------------------------------------
TEST(prometheus_export_files_and_retention) {
  const fs::path dir = make_temp_dir("logpipe_prom_");
  const fs::path stats_dir = dir / "stats";

  logpipe::PromStatsOptions options;
  options.dir = stats_dir;
  options.keep_files = 3;
  int64_t fake_now = 1788696000000;  // arbitrary fixed epoch ms
  options.clock = [&fake_now] { return fake_now; };

  logpipe::Metrics metrics;
  metrics.record_input(logpipe::Level::Info);
  metrics.record_kv_value("host", "alpha");

  logpipe::PromStatsWriter writer(options);
  std::vector<fs::path> written;
  for (int i = 0; i < 5; ++i) {
    fake_now += 61ull * 1000;  // advance past one second: new timestamped name
    const fs::path exported = writer.write(metrics.snapshot());
    CHECK(!exported.empty());
    written.push_back(exported);
    CHECK_EQ_INT(writer.kept_files(), std::min(static_cast<size_t>(i + 1),
                                               static_cast<size_t>(3)));
  }
  CHECK(!writer.failed());

  // Only the three newest files remain; older ones were pruned.
  CHECK(!fs::exists(written[0]));
  CHECK(!fs::exists(written[1]));
  CHECK(fs::exists(written[2]));
  CHECK(fs::exists(written[3]));
  CHECK(fs::exists(written[4]));

  // File names are timestamped .prom exports of the base name.
  const std::string name = written[4].filename().string();
  CHECK(name.find("logpipe_stats_") == 0);
  CHECK(name.find(".prom") != std::string::npos);

  // Content is the rendered snapshot (starts with the ingested HELP header).
  const std::string body = read_text_file(written[4]);
  CHECK(body.find("# HELP logpipe_lines_ingested_total") == 0);
  CHECK(body.find("logpipe_field_value_count{key=\"host\",value=\"alpha\"} 1") !=
        std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Config: the new keys parse, validate and show up in describe().
// ---------------------------------------------------------------------------
TEST(config_loads_stats_and_kv_keys) {
  const fs::path dir = make_temp_dir("logpipe_stcfg_");
  const fs::path cfg = dir / "stats.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "extract.kv = true\n"
        << "stats.interval_sec = 30\n"
        << "stats.dir = " << (dir / "stats").string() << "\n"
        << "stats.keep_files = 4\n";
  }
  const logpipe::Config config = logpipe::Config::load(cfg.string());
  CHECK(config.extract_kv);
  CHECK_EQ_INT(config.stats_interval_sec, 30);
  CHECK_EQ_INT(config.stats_keep_files, 4);
  CHECK(config.stats_dir == (dir / "stats"));

  const std::string desc = config.describe();
  CHECK(desc.find("extract_kv=on") != std::string::npos);
  CHECK(desc.find("stats=30s into " + (dir / "stats").string() + " keep 4") !=
        std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(config_defaults_and_validation_for_new_keys) {
  const fs::path dir = make_temp_dir("logpipe_stdef_");
  const fs::path cfg = dir / "plain.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n";
  }
  // Defaults: extraction off, statistics export disabled.
  const logpipe::Config config = logpipe::Config::load(cfg.string());
  CHECK(!config.extract_kv);
  CHECK_EQ_INT(config.stats_interval_sec, 0);
  CHECK_EQ_INT(config.stats_keep_files, 10);
  const std::string desc = config.describe();
  CHECK(desc.find("extract_kv=off") != std::string::npos);
  CHECK(desc.find("stats=off") != std::string::npos);

  // Invalid values are rejected with descriptive errors.
  const char* cases[][2] = {
      {"extract.kv = maybe\n", "extract.kv"},
      {"stats.interval_sec = -1\n", "stats.interval_sec"},
      {"stats.interval_sec = 100000\n", "stats.interval_sec"},
      {"stats.keep_files = 0\n", "stats.keep_files"},
  };
  for (const auto& c : cases) {
    const fs::path bad = dir / "bad.conf";
    {
      std::ofstream out(bad, std::ios::binary | std::ios::trunc);
      out << "input.files = dummy.log\n" << c[0];
    }
    bool threw = false;
    try {
      logpipe::Config::load(bad.string());
    } catch (const std::exception& e) {
      threw = true;
      CHECK(std::string(e.what()).find(c[1]) != std::string::npos);
    }
    CHECK(threw);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// --dump-config behaviour: dump_config() writes describe() plus a newline to
// the given stream and returns 0 (main routes --dump-config here, then exits 0
// without starting the pipeline).
// ---------------------------------------------------------------------------
TEST(dump_config_writes_describe_and_returns_zero) {
  const fs::path dir = make_temp_dir("logpipe_dump_");
  const fs::path cfg = dir / "dump.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "extract.kv = true\n"
        << "stats.interval_sec = 15\n";
  }
  const logpipe::Config config = logpipe::Config::load(cfg.string());

  std::FILE* sink = std::tmpfile();
  CHECK(sink != nullptr);
  CHECK_EQ_INT(logpipe::dump_config(sink, config), 0);
  std::rewind(sink);
  std::string captured;
  char buffer[512];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), sink)) > 0) {
    captured.append(buffer, got);
  }
  std::fclose(sink);

  // Exactly describe() plus one trailing newline, containing the new keys.
  CHECK_STR_EQ(captured, config.describe() + "\n");
  CHECK(captured.find("extract_kv=on") != std::string::npos);
  CHECK(captured.find("stats=15s into") != std::string::npos);
  CHECK(captured.find("inputs=[dummy.log]") != std::string::npos);

  // Null stream is rejected with a non-zero status.
  CHECK(logpipe::dump_config(nullptr, config) != 0);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
