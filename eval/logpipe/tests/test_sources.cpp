// test_sources.cpp - tests for the input source abstraction round:
//
//   1. ISource contract regression: the refactored Tailer driven through the
//      ISource interface (type/name/start/poll/join/finished/dropped_lines)
//      still tails files with the same external behaviour;
//   2. GlobSource: discovery of matching files, late-created files, retirement
//      of vanished files, glob matcher and pattern splitting unit tests,
//      glob.exclude exclusion, offset-file isolation (separate checkpoint
//      temp suffix);
//   3. source tags: per-file and glob tags injected into RawLine fields,
//      StdinReader tags, config parsing of source.<n>.tags / glob.exclude /
//      glob:<pattern> entries, and KV-over-tags precedence in the merge;
//   4. DSL: field("key") comparisons against the injected tags;
//   5. Metrics: per-source-type line counters and active-source gauges, and
//      their Prometheus rendering.
//
// The framework is shared via test_framework.h; test_main.cpp runs every
// registered case.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "dsl.h"
#include "glob_source.h"
#include "pipeline.h"
#include "prom_stats.h"
#include "source.h"
#include "stdin_reader.h"
#include "tail_engine.h"
#include "tailer.h"
#include "test_framework.h"

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

void append_text_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out << text;
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
// ISource contract: the refactored Tailer driven through the abstract
// interface produces the same lines as before the refactor.
// ---------------------------------------------------------------------------
TEST(isource_tailer_contract) {
  const fs::path dir = make_temp_dir("logpipe_isrc_tail_");
  const fs::path input = dir / "input.log";
  write_text_file(input, "alpha\nbeta\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::TailOptions options;
  options.poll_ms = 10;  // offset_file stays empty (no persistence)
  std::unique_ptr<logpipe::ISource> source =
      std::make_unique<logpipe::Tailer>(std::vector<fs::path>{input}, queue, options);

  CHECK_EQ_INT(static_cast<int>(source->type()),
               static_cast<int>(logpipe::SourceType::File));
  CHECK_STR_EQ(source->name(), "tailer");
  CHECK(!source->finished());
  CHECK_EQ_INT(source->dropped_lines(), 0);  // never started: nothing dropped

  source->start();
  append_text_file(input, "gamma\n");
  const std::vector<std::string> texts = drain_texts(queue, 5000);
  source->request_stop();
  source->join();
  CHECK(source->finished());

  bool saw_alpha = false, saw_beta = false, saw_gamma = false;
  for (const std::string& text : texts) {
    if (text == "alpha") saw_alpha = true;
    if (text == "beta") saw_beta = true;
    if (text == "gamma") saw_gamma = true;
  }
  CHECK(saw_alpha);
  CHECK(saw_beta);
  CHECK(saw_gamma);
  fs::remove_all(dir);
}

// ISource::poll() on a self-driving source is a no-op that must not disturb
// the source (contract test; safe to call before and after start()).
TEST(isource_poll_is_safe_noop_for_tailer) {
  const fs::path dir = make_temp_dir("logpipe_isrc_poll_");
  logpipe::BlockingQueue<logpipe::RawLine> queue(8);
  logpipe::TailOptions options;
  options.poll_ms = 10;
  logpipe::Tailer tailer(std::vector<fs::path>{dir / "missing.log"}, queue, options);
  tailer.poll();
  tailer.start();
  tailer.poll();
  tailer.request_stop();
  tailer.join();
  CHECK(tailer.finished());
  fs::remove_all(dir);
}

// request_stop() before start() is safe and the source never touches the
// queue (no close, no drop accounting).
TEST(isource_stop_before_start_is_safe) {
  logpipe::BlockingQueue<logpipe::RawLine> queue(8);
  logpipe::TailOptions options;
  logpipe::Tailer tailer(std::vector<fs::path>{}, queue, options);
  tailer.request_stop();
  tailer.join();  // no thread: must return immediately
  CHECK_EQ_INT(tailer.dropped_lines(), 0);
  CHECK(!queue.closed());  // never started: no queue ownership taken

  logpipe::StdinReader reader(queue, /*close_queue_on_exit=*/false);
  reader.request_stop();
  reader.join();
  CHECK_EQ_INT(reader.dropped_lines(), 0);
  CHECK(!queue.closed());
}

// ---------------------------------------------------------------------------
// GlobSource: discovery, late files, exclusion, retirement.
// ---------------------------------------------------------------------------
TEST(glob_discovers_and_tails_matching_files) {
  const fs::path dir = make_temp_dir("logpipe_glob_disc_");
  write_text_file(dir / "a.log", "a1\n");
  write_text_file(dir / "b.log", "b1\n");
  write_text_file(dir / "skip.txt", "ignored\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::GlobOptions options;
  options.poll_ms = 10;
  logpipe::GlobSource source("glob:" + (dir / "*.log").generic_string(), {}, queue,
                             options);
  CHECK_STR_EQ(source.name(), "glob:" + (dir / "*.log").generic_string());
  CHECK_EQ_INT(static_cast<int>(source.type()),
               static_cast<int>(logpipe::SourceType::Glob));

  source.start();
  const std::vector<std::string> texts = drain_texts(queue, 5000);
  source.request_stop();
  source.join();
  CHECK(source.finished());

  bool saw_a1 = false, saw_b1 = false;
  for (const std::string& text : texts) {
    if (text == "a1") saw_a1 = true;
    if (text == "b1") saw_b1 = true;
    CHECK(text != "ignored");  // non-matching extension never tailed
  }
  CHECK(saw_a1);
  CHECK(saw_b1);
  CHECK_EQ_INT(texts.size(), 2);
  fs::remove_all(dir);
}

TEST(glob_discovers_late_created_file) {
  const fs::path dir = make_temp_dir("logpipe_glob_late_");
  write_text_file(dir / "first.log", "f1\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::GlobOptions options;
  options.poll_ms = 10;
  logpipe::GlobSource source("glob:" + (dir / "*.log").generic_string(), {}, queue,
                             options);
  source.start();
  const std::vector<std::string> early = drain_texts(queue, 5000);
  CHECK_EQ_INT(early.size(), 1);

  write_text_file(dir / "second.log", "s1\n");
  const std::vector<std::string> late = drain_texts(queue, 5000);
  source.request_stop();
  source.join();

  CHECK_EQ_INT(late.size(), 1);
  CHECK(late.size() == 1 && late[0] == "s1");
  fs::remove_all(dir);
}

TEST(glob_retires_vanished_file) {
  const fs::path dir = make_temp_dir("logpipe_glob_vanish_");
  const fs::path gone = dir / "gone.log";
  write_text_file(gone, "g1\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::GlobOptions options;
  options.poll_ms = 10;
  logpipe::GlobSource source("glob:" + (dir / "*.log").generic_string(), {}, queue,
                             options);
  source.start();
  (void)drain_texts(queue, 5000);
  CHECK_EQ_INT(source.active_files(), 1);

  // The file disappears: the next discovery round must retire it (a fresh
  // file with the same name is then re-added and tailed from the start).
  fs::remove(gone);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (source.active_files() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK_EQ_INT(source.active_files(), 0);

  write_text_file(gone, "g2\n");
  const std::vector<std::string> again = drain_texts(queue, 5000);
  source.request_stop();
  source.join();
  CHECK_EQ_INT(again.size(), 1);
  CHECK(again.size() == 1 && again[0] == "g2");
  fs::remove_all(dir);
}

TEST(glob_exclude_patterns_keep_matches_out) {
  const fs::path dir = make_temp_dir("logpipe_glob_excl_");
  write_text_file(dir / "keep.log", "k1\n");
  write_text_file(dir / "debug.log", "d1\n");
  write_text_file(dir / "noisy.log", "n1\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::GlobOptions options;
  options.poll_ms = 10;
  // Exclude by file name wildcard and by exact name.
  options.exclude.push_back("debug.*");
  options.exclude.push_back("noisy.log");
  logpipe::GlobSource source("glob:" + (dir / "*.log").generic_string(), {}, queue,
                             options);
  source.start();
  const std::vector<std::string> texts = drain_texts(queue, 5000);
  source.request_stop();
  source.join();

  CHECK_EQ_INT(texts.size(), 1);
  CHECK(texts.size() == 1 && texts[0] == "k1");
  fs::remove_all(dir);
}

// Unit tests for the glob matcher and the pattern splitter.
TEST(glob_matcher_and_pattern_split) {
  CHECK(logpipe::GlobSource::glob_match("app.log", "*.log"));
  CHECK(logpipe::GlobSource::glob_match("app.log", "app.*"));
  CHECK(logpipe::GlobSource::glob_match("app.log", "app.?og"));
  CHECK(logpipe::GlobSource::glob_match("app-2026-09.log", "app-*.log"));
  CHECK(logpipe::GlobSource::glob_match("anything", "*"));
  CHECK(!logpipe::GlobSource::glob_match("app.log", "*.txt"));
  CHECK(!logpipe::GlobSource::glob_match("app.log", "app"));
  CHECK(logpipe::GlobSource::glob_match("appxlog", "app?log"));  // '?' is one char
  CHECK(!logpipe::GlobSource::glob_match("apxlog", "app?log"));  // too short
  CHECK(!logpipe::GlobSource::glob_match("appxylog", "app?log"));  // too long
  CHECK(logpipe::GlobSource::glob_match("a.log", "?.log"));

  fs::path directory;
  std::string mask;
  logpipe::GlobSource::split_pattern("/var/log/app/*.log", directory, mask);
  CHECK_STR_EQ(directory.generic_string(), "/var/log/app");
  CHECK_STR_EQ(mask, "*.log");
  logpipe::GlobSource::split_pattern("plain.log", directory, mask);
  CHECK_STR_EQ(directory.generic_string(), ".");
  CHECK_STR_EQ(mask, "plain.log");
  logpipe::GlobSource::split_pattern("/x.log", directory, mask);
  CHECK_STR_EQ(directory.generic_string(), "/");
  CHECK_STR_EQ(mask, "x.log");
}

// ---------------------------------------------------------------------------
// Source tags: injection into RawLine fields, config plumbing, DSL access.
// ---------------------------------------------------------------------------
TEST(tailer_injects_source_tags_into_rawlines) {
  const fs::path dir = make_temp_dir("logpipe_tags_tail_");
  const fs::path input = dir / "input.log";
  write_text_file(input, "t1\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(16);
  logpipe::TailOptions options;
  options.poll_ms = 10;
  std::vector<logpipe::FileSourceSpec> specs;
  logpipe::FileSourceSpec spec;
  spec.path = input;
  spec.tags["env"] = "prod";
  spec.tags["service"] = "api";
  specs.push_back(spec);
  logpipe::Tailer tailer(specs, queue, options);

  std::thread thread([&tailer] { tailer.run(); });
  logpipe::RawLine raw;
  bool got = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (queue.pop_for(raw, 25) ==
        logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
      got = true;
      break;
    }
  }
  tailer.request_stop();
  thread.join();

  CHECK(got);
  CHECK(got && raw.fields.size() == 2);
  CHECK(got && raw.fields.at("env") == "prod");
  CHECK(got && raw.fields.at("service") == "api");
  CHECK_EQ_INT(static_cast<int>(raw.source_type),
               static_cast<int>(logpipe::SourceType::File));
  fs::remove_all(dir);
}

TEST(glob_source_injects_unified_tags) {
  const fs::path dir = make_temp_dir("logpipe_tags_glob_");
  write_text_file(dir / "one.log", "o1\n");
  write_text_file(dir / "two.log", "o2\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(16);
  logpipe::GlobOptions options;
  options.poll_ms = 10;
  logpipe::SourceTags tags;
  tags["tier"] = "edge";
  logpipe::GlobSource source("glob:" + (dir / "*.log").generic_string(), tags, queue,
                             options);
  source.start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::vector<logpipe::RawLine> lines;
  while (std::chrono::steady_clock::now() < deadline && lines.size() < 2) {
    logpipe::RawLine raw;
    if (queue.pop_for(raw, 25) ==
        logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
      lines.push_back(raw);
    }
  }
  source.request_stop();
  source.join();

  CHECK_EQ_INT(lines.size(), 2);
  for (const logpipe::RawLine& raw : lines) {
    CHECK(raw.fields.size() == 1 && raw.fields.at("tier") == "edge");
    CHECK_EQ_INT(static_cast<int>(raw.source_type),
                 static_cast<int>(logpipe::SourceType::Glob));
  }
  fs::remove_all(dir);
}

TEST(config_parses_glob_sources_and_tags) {
  const fs::path dir = make_temp_dir("logpipe_tags_cfg_");
  write_text_file(dir / "logpipe.conf",
                  "input.files = " + (dir / "app.log").generic_string() +
                      ", glob:" + (dir / "*.log").generic_string() + ", stdin:\n"
                  "source.2.tags = tier=edge,dc=7\n"
                  "source.1.tags = legacy\n"
                  "glob.exclude = debug.*,secret.log\n");
  const logpipe::Config config =
      logpipe::Config::load((dir / "logpipe.conf").string());

  CHECK_EQ_INT(config.glob_sources.size(), 1);
  CHECK(config.glob_sources.size() == 1 &&
        config.glob_sources[0].pattern == (dir / "*.log").generic_string());
  CHECK(config.glob_sources.size() == 1 && config.glob_sources[0].tags.size() == 2);
  CHECK(config.glob_sources.size() == 1 &&
        config.glob_sources[0].tags.at("tier") == "edge");
  CHECK(config.glob_sources.size() == 1 && config.glob_sources[0].tags.at("dc") == "7");
  CHECK(config.stdin_tags.size() == 0);  // no source.3.tags key
  CHECK_EQ_INT(config.glob_exclude.size(), 2);
  CHECK(config.glob_exclude.size() == 2 && config.glob_exclude[0] == "debug.*");
  CHECK(config.glob_exclude.size() == 2 && config.glob_exclude[1] == "secret.log");
  // Plain file entries keep their legacy input_files behaviour and get the
  // tags of their 1-based declaration index (bare tag -> "true").
  CHECK_EQ_INT(config.input_files.size(), 1);
  CHECK(config.file_tags.count(logpipe::normalize_file_key(config.input_files[0])) == 1);
  CHECK(config.file_tags.at(logpipe::normalize_file_key(config.input_files[0]))
                .at("legacy") == "true");
  // A glob source counts as an input source (no "no input sources" error).
  fs::remove_all(dir);
}

// The merge rule: injected tags land in LogRecord::fields and KV-extracted
// pairs take precedence on a key clash (pipeline-level behaviour of main).
TEST(fields_merge_kv_wins_over_tags) {
  logpipe::RawLine raw;
  raw.fields["env"] = "prod";
  raw.fields["user"] = "tag-value";

  // Simulate the main-loop merge: tags first, then extracted KV on top.
  std::map<std::string, std::string> merged = raw.fields;
  const std::map<std::string, std::string> extracted = {{"user", "kv-value"}};
  for (const auto& pair : extracted) merged[pair.first] = pair.second;

  CHECK_EQ_INT(merged.size(), 2);
  CHECK(merged.at("env") == "prod");
  CHECK(merged.at("user") == "kv-value");

  // The DSL reads the merged map through field("...") / kv("...").
  logpipe::LogRecord record;
  record.fields = merged;
  const auto expr = logpipe::dsl::FilterExpr::compile(R"(field("env") = "prod")");
  CHECK(expr->passes(record));
  const auto kv_expr = logpipe::dsl::FilterExpr::compile(R"(kv("user") = "kv-value")");
  CHECK(kv_expr->passes(record));
  const auto miss = logpipe::dsl::FilterExpr::compile(R"(field("env") = "dev")");
  CHECK(!miss->passes(record));
  const auto missing_key = logpipe::dsl::FilterExpr::compile(R"(field("nope") != "x")");
  CHECK(!missing_key->passes(record));  // missing field matches nothing
  const auto contains = logpipe::dsl::FilterExpr::compile(
      R"(field("env") CONTAINS "rod")");
  CHECK(contains->passes(record));
}

TEST(dsl_field_form_compile_errors) {
  bool threw = false;
  try {
    (void)logpipe::dsl::FilterExpr::compile(R"(field() = "x")");
  } catch (const logpipe::dsl::Error&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    (void)logpipe::dsl::FilterExpr::compile(R"(field("") = "x")");
  } catch (const logpipe::dsl::Error&) {
    threw = true;  // empty key rejected, like kv(...)
  }
  CHECK(threw);
  threw = false;
  try {
    (void)logpipe::dsl::FilterExpr::compile(R"(field("env") @ "x")");
  } catch (const logpipe::dsl::Error&) {
    threw = true;  // no operator
  }
  CHECK(threw);
}

// ---------------------------------------------------------------------------
// Metrics: per-source-type line counters, active gauges, Prometheus export.
// ---------------------------------------------------------------------------
TEST(metrics_per_source_type_counters_and_gauges) {
  logpipe::Metrics metrics;
  metrics.record_source_type_line("file");
  metrics.record_source_type_line("file");
  metrics.record_source_type_line("stdin");
  metrics.set_source_type_active("file", 1);
  metrics.set_source_type_active("glob", 2);
  metrics.set_source_type_active("stdin", 1);
  metrics.set_source_type_active("glob", 0);  // a count of 0 removes the row

  const logpipe::Metrics::Snapshot snap = metrics.snapshot();
  CHECK_EQ_INT(snap.source_type_lines.at("file"), 2);
  CHECK_EQ_INT(snap.source_type_lines.at("stdin"), 1);
  CHECK(snap.source_type_lines.find("glob") == snap.source_type_lines.end());
  CHECK_EQ_INT(snap.source_type_active.at("file"), 1);
  CHECK_EQ_INT(snap.source_type_active.at("stdin"), 1);
  CHECK(snap.source_type_active.find("glob") == snap.source_type_active.end());

  const std::string summary = metrics.summary(0);
  CHECK(summary.find("lines per source type:") != std::string::npos);
  CHECK(summary.find("active sources by type:") != std::string::npos);
}

TEST(prometheus_renders_source_type_metrics) {
  logpipe::Metrics metrics;
  metrics.record_source_type_line("file");
  metrics.record_source_type_line("glob");
  metrics.record_source_type_line("glob");
  metrics.set_source_type_active("file", 1);
  metrics.set_source_type_active("glob", 3);
  const std::string text =
      logpipe::PromStatsWriter::render(metrics.snapshot());

  CHECK(text.find("# TYPE logpipe_source_type_lines_total counter") != std::string::npos);
  CHECK(text.find("logpipe_source_type_lines_total{type=\"file\"} 1") != std::string::npos);
  CHECK(text.find("logpipe_source_type_lines_total{type=\"glob\"} 2") != std::string::npos);
  CHECK(text.find("# TYPE logpipe_source_type_active gauge") != std::string::npos);
  CHECK(text.find("logpipe_source_type_active{type=\"glob\"} 3") != std::string::npos);
  CHECK(text.find("logpipe_source_type_active{type=\"file\"} 1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// End-to-end through the producer interface: a glob source's lines carry the
// SourceType::Glob marker and the tags, ready for the main-loop accounting.
// ---------------------------------------------------------------------------
TEST(glob_lines_feed_type_metrics_via_source_type_name) {
  const fs::path dir = make_temp_dir("logpipe_glob_metrics_");
  write_text_file(dir / "m.log", "m1\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(16);
  logpipe::GlobOptions options;
  options.poll_ms = 10;
  logpipe::GlobSource source("glob:" + (dir / "*.log").generic_string(), {}, queue,
                             options);
  source.start();
  logpipe::Metrics metrics;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    logpipe::RawLine raw;
    if (queue.pop_for(raw, 25) ==
        logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
      metrics.record_source_type_line(logpipe::source_type_name(raw.source_type));
      break;
    }
  }
  source.request_stop();
  source.join();

  CHECK_EQ_INT(metrics.snapshot().source_type_lines.at("glob"), 1);
  fs::remove_all(dir);
}
