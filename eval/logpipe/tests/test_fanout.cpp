// test_fanout.cpp - tests for the multi-output fan-out (named outputs with
// independent filter/format/compress settings), the ordered transform chains
// (uppercase|trim|replace|tag with before/after positioning) and the
// per-output Metrics columns. Uses the shared zero-dependency harness.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "config.h"
#include "dsl.h"
#include "pipeline.h"
#include "prom_stats.h"
#include "test_framework.h"
#include "transform.h"
#include "writer.h"

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_fanout_dir_seq{0};

fs::path make_fanout_temp_dir(const char* prefix) {
  const fs::path dir =
      fs::temp_directory_path() /
      (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(g_fanout_dir_seq.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

logpipe::LogRecord make_fanout_record(const std::string& source,
                                      const std::string& message,
                                      logpipe::Level level = logpipe::Level::Info) {
  logpipe::LogRecord rec;
  rec.source = source;
  rec.ingested_ms = 1757160000000;  // fixed so output lines are deterministic
  rec.timestamp_text = "2026-09-06 12:00:00";
  rec.level = level;
  rec.message = message;
  rec.parsed = true;
  return rec;
}

std::string read_fanout_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

logpipe::OutputRoute make_route(const std::string& name, const std::string& file,
                                const fs::path& dir) {
  logpipe::OutputRoute route;
  route.name = name;
  route.options.output_dir = dir;
  route.options.base_name = file;
  return route;
}

}  // namespace

// ---------------------------------------------------------------------------
// Requirement 3 (backward compatibility): a config without output.<N>.* keys
// maps the legacy single-output settings onto exactly one default output
// named "default"; describe() carries the fan-out dump.
// ---------------------------------------------------------------------------
TEST(config_maps_legacy_output_to_default_fanout) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_legacy_");
  const fs::path cfg = dir / "legacy.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "output_format = json_lines\n"
        << "output.compress = rle\n"
        << "filter.level = WARN\n"
        << "filter.expr = msg CONTAINS \"disk\"\n";
  }

  logpipe::Config config;
  try {
    config = logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }
  CHECK(config.outputs.size() == 1);
  CHECK(!config.outputs_explicit);
  const logpipe::OutputConfig& entry = config.outputs.front();
  CHECK_STR_EQ(entry.name, "default");
  CHECK_STR_EQ(entry.file, "out.log");
  CHECK(entry.format == logpipe::OutputFormat::JsonLines);
  CHECK(entry.compress == logpipe::CompressMode::Rle);
  CHECK(entry.level == logpipe::Level::Warn);
  CHECK(entry.filter_expr != nullptr);
  CHECK(entry.transform.empty());
  CHECK(entry.transform_position == logpipe::TransformPosition::After);
  CHECK(config.describe().find("outputs=[default(out.log") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Requirement 1: output.<N>.* keys build a named output group. Missing names
// default to out<N>, missing files default to the legacy output.file, and
// duplicate names are rejected.
// ---------------------------------------------------------------------------
TEST(config_parses_multi_output_group) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_group_");
  const fs::path cfg = dir / "group.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = fallback.log\n"
        << "output.2.name = errors\n"
        << "output.2.file = errors.log\n"
        << "output.2.format = json_lines\n"
        << "output.2.compress = rle\n"
        << "output.2.level = ERROR\n"
        << "output.2.filter.expr = level >= \"ERROR\"\n"
        << "output.2.transform = uppercase|tag:route=err\n"
        << "output.2.transform.position = before\n"
        << "output.1.name = all\n";  // file missing -> fallback.log
  }

  logpipe::Config config;
  try {
    config = logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }
  CHECK(config.outputs_explicit);
  CHECK(config.outputs.size() == 2);
  // Ordered by index: output.1 then output.2.
  CHECK_STR_EQ(config.outputs[0].name, "all");
  CHECK_STR_EQ(config.outputs[0].file, "fallback.log");
  CHECK(config.outputs[0].format == logpipe::OutputFormat::Text);
  CHECK(config.outputs[0].level == logpipe::Level::Debug);
  CHECK(config.outputs[0].filter_expr == nullptr);
  CHECK_STR_EQ(config.outputs[1].name, "errors");
  CHECK_STR_EQ(config.outputs[1].file, "errors.log");
  CHECK(config.outputs[1].format == logpipe::OutputFormat::JsonLines);
  CHECK(config.outputs[1].compress == logpipe::CompressMode::Rle);
  CHECK(config.outputs[1].level == logpipe::Level::Error);
  CHECK(config.outputs[1].filter_expr != nullptr);
  CHECK(config.outputs[1].transform.size() == 2);
  CHECK(config.outputs[1].transform_position == logpipe::TransformPosition::Before);

  // Duplicate output names must abort the load.
  const fs::path dup = dir / "dup.conf";
  {
    std::ofstream out(dup, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.1.name = same\n"
        << "output.2.name = same\n";
  }
  bool threw = false;
  try {
    logpipe::Config::load(dup.string());
  } catch (const std::exception& e) {
    threw = true;
    CHECK(std::string(e.what()).find("duplicate output name") != std::string::npos);
  }
  CHECK(threw);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Requirement 2: transform chain parsing. Chains parse into ordered steps;
// malformed chains (unknown kind, replace without '->', tag without '=',
// empty step) abort with a descriptive error; an empty spec means no chain.
// ---------------------------------------------------------------------------
TEST(transform_chain_parse_and_apply_in_order) {
  // Empty / whitespace spec: no transform.
  CHECK(logpipe::parse_transform_chain("").empty());
  CHECK(logpipe::parse_transform_chain("   ").empty());

  const auto chain = logpipe::parse_transform_chain(
      "trim|uppercase|replace:foo->bar|tag:host=web-01");
  CHECK_EQ_INT(chain.size(), 4);
  CHECK(chain[0].kind == logpipe::TransformStep::Kind::Trim);
  CHECK(chain[1].kind == logpipe::TransformStep::Kind::Uppercase);
  CHECK(chain[2].kind == logpipe::TransformStep::Kind::Replace);
  CHECK_STR_EQ(chain[2].pattern_, "foo");
  CHECK_STR_EQ(chain[2].replacement_, "bar");
  CHECK(chain[3].kind == logpipe::TransformStep::Kind::Tag);
  CHECK_STR_EQ(chain[3].pattern_, "host");
  CHECK_STR_EQ(chain[3].replacement_, "web-01");

  // Steps apply in order: the second replace sees the first one's output.
  auto order = logpipe::parse_transform_chain("replace:ab->X|replace:X->Y");
  logpipe::LogRecord rec = make_fanout_record("s", "ab");
  const logpipe::LogRecord applied = logpipe::apply_transforms(order, rec);
  CHECK_STR_EQ(applied.message, "Y");  // ab -> X -> Y (order matters)

  // trim then uppercase: trailing space is gone before uppercasing.
  auto combo = logpipe::parse_transform_chain("trim|uppercase");
  const logpipe::LogRecord trimmed =
      logpipe::apply_transforms(combo, make_fanout_record("s", "  hello world  "));
  CHECK_STR_EQ(trimmed.message, "HELLO WORLD");

  // replace rewrites every occurrence; tag attaches a field, message intact.
  auto rep = logpipe::parse_transform_chain("replace:a->b");
  CHECK_STR_EQ(logpipe::apply_transforms(rep, rec).message, "bb");
  auto tag = logpipe::parse_transform_chain("tag:env=prod");
  const logpipe::LogRecord tagged = logpipe::apply_transforms(tag, rec);
  CHECK_STR_EQ(tagged.message, "ab");
  CHECK_STR_EQ(tagged.fields.at("env"), "prod");

  // Malformed chains throw with the offending text in the message.
  const char* bad[] = {
      "uppercase|",           // empty step
      "shout",                // unknown kind
      "replace:foo-bar",      // replace without '->'
      "replace:->x",          // ... still needs a pattern
      "tag:envprod",          // tag without '='
      "tag:=prod",            // tag with an empty name
  };
  for (const char* spec : bad) {
    bool threw = false;
    try {
      logpipe::parse_transform_chain(spec);
    } catch (const std::exception& e) {
      threw = true;
      CHECK(std::string(e.what()).find("transform:") == 0);
    }
    CHECK(threw);
  }
}

// ---------------------------------------------------------------------------
// Requirement 1: the same record is offered to every branch and lands only in
// the outputs whose per-output filter accepts it (disjoint subsets here).
// ---------------------------------------------------------------------------
TEST(fanout_routes_records_to_matching_outputs) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_route_");

  auto disk_expr = logpipe::dsl::FilterExpr::compile(R"(msg CONTAINS "disk")");

  logpipe::Metrics metrics;
  std::vector<logpipe::OutputRoute> routes;
  routes.push_back(make_route("all", "all.log", dir));
  logpipe::OutputRoute errors = make_route("errors", "errors.log", dir);
  errors.level = logpipe::Level::Error;
  routes.push_back(std::move(errors));
  logpipe::OutputRoute disk = make_route("disk", "disk.log", dir);
  disk.filter_expr = disk_expr;
  routes.push_back(std::move(disk));

  logpipe::FanOutWriter writer(std::move(routes), &metrics);
  CHECK(writer.open());
  CHECK(writer.route_count() == 3);

  uint64_t bytes = 0;
  CHECK(writer.write(make_fanout_record("app", "disk full", logpipe::Level::Error), bytes));
  CHECK(writer.write(make_fanout_record("app", "disk usage fine", logpipe::Level::Info), bytes));
  CHECK(writer.write(make_fanout_record("app", "unrelated event", logpipe::Level::Warn), bytes));
  writer.close();
  CHECK(!writer.failed());

  // "all": everything; "errors": only the ERROR line; "disk": both disk lines.
  const std::string all = read_fanout_file(dir / "all.log");
  const std::string err = read_fanout_file(dir / "errors.log");
  const std::string disk_out = read_fanout_file(dir / "disk.log");
  CHECK(all.find("disk full") != std::string::npos);
  CHECK(all.find("disk usage fine") != std::string::npos);
  CHECK(all.find("unrelated event") != std::string::npos);
  CHECK(err.find("disk full") != std::string::npos);
  CHECK(err.find("disk usage fine") == std::string::npos);
  CHECK(err.find("unrelated event") == std::string::npos);
  CHECK(disk_out.find("disk full") != std::string::npos);
  CHECK(disk_out.find("disk usage fine") != std::string::npos);
  CHECK(disk_out.find("unrelated event") == std::string::npos);
  // Per-output format independence: json_lines output carries JSON fields.
  CHECK(disk_out.find("|crc=") != std::string::npos);  // text format default

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Requirement 2: per-output format/compress independence inside the fan-out
// (one branch json_lines + rle, the other plain text).
// ---------------------------------------------------------------------------
TEST(fanout_output_format_and_compress_independence) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_fmt_");

  logpipe::OutputRoute text = make_route("plain", "plain.log", dir);
  logpipe::OutputRoute packed = make_route("packed", "packed.log", dir);
  packed.options.format = logpipe::OutputFormat::JsonLines;
  packed.options.compress = logpipe::CompressMode::Rle;

  logpipe::FanOutWriter writer({std::move(text), std::move(packed)});
  CHECK(writer.open());
  uint64_t bytes = 0;
  CHECK(writer.write(make_fanout_record("app", "hello branch"), bytes));
  writer.close();
  CHECK(!writer.failed());

  const std::string plain = read_fanout_file(dir / "plain.log");
  CHECK(plain.find("[INFO] [app] hello branch |crc=") != std::string::npos);

  const std::string packed_raw = read_fanout_file(dir / "packed.log.rle");
  CHECK(!packed_raw.empty());
  std::string decoded;
  CHECK(logpipe::read_compressed_output(dir / "packed.log.rle", decoded));
  CHECK(decoded.find("\"message\":\"hello branch\"") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Requirement 2: transform.position = before makes the filter judge the
// TRANSFORMED record; with the default "after" the filter sees the original
// message and the transform is applied only to the written line.
// ---------------------------------------------------------------------------
TEST(fanout_transform_position_changes_filter_outcome) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_pos_");

  // Branch A: replace:foo->KEY runs BEFORE the filter, so "foo bar" becomes
  // "KEY bar" and passes msg CONTAINS "KEY".
  logpipe::OutputRoute before = make_route("before", "before.log", dir);
  before.transform = logpipe::parse_transform_chain("replace:foo->KEY");
  before.transform_position = logpipe::TransformPosition::Before;
  before.filter_expr = logpipe::dsl::FilterExpr::compile(R"(msg CONTAINS "KEY")");

  // Branch B: same chain but AFTER the filter, so the original "foo bar"
  // fails msg CONTAINS "KEY" and nothing is written.
  logpipe::OutputRoute after = make_route("after", "after.log", dir);
  after.transform = logpipe::parse_transform_chain("replace:foo->KEY");
  after.transform_position = logpipe::TransformPosition::After;
  after.filter_expr = logpipe::dsl::FilterExpr::compile(R"(msg CONTAINS "KEY")");

  // Branch C: uppercase AFTER the filter — filter matched the original
  // lowercase message, output carries the uppercase rewrite.
  logpipe::OutputRoute upper = make_route("upper", "upper.log", dir);
  upper.transform = logpipe::parse_transform_chain("uppercase");
  upper.transform_position = logpipe::TransformPosition::After;
  upper.filter_expr =
      logpipe::dsl::FilterExpr::compile(R"(msg CONTAINS "disk full")");

  logpipe::FanOutWriter writer(
      {std::move(before), std::move(after), std::move(upper)});
  CHECK(writer.open());

  uint64_t bytes = 0;
  CHECK(writer.write(make_fanout_record("app", "foo bar"), bytes));
  CHECK(writer.write(make_fanout_record("app", "disk full now"), bytes));
  writer.close();
  CHECK(!writer.failed());

  const std::string before_out = read_fanout_file(dir / "before.log");
  const std::string after_out = read_fanout_file(dir / "after.log");
  const std::string upper_out = read_fanout_file(dir / "upper.log");
  // "before": the transformed record passed and the transformed text is written.
  CHECK(before_out.find("KEY bar") != std::string::npos);
  // "after": the original record failed the filter, so nothing landed.
  CHECK(after_out.find("KEY bar") == std::string::npos);
  CHECK(after_out.find("foo bar") == std::string::npos);
  // "upper": filter saw the original message, output is the rewritten one.
  CHECK(upper_out.find("DISK FULL NOW") != std::string::npos);
  CHECK(upper_out.find("disk full now") == std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Requirement 2: the tag transform runs BEFORE a per-output DSL filter and
// the attached field is visible to the kv("name") syntax there.
// ---------------------------------------------------------------------------
TEST(fanout_tag_transform_feeds_kv_filter) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_tag_");

  logpipe::OutputRoute tagged = make_route("tagged", "tagged.log", dir);
  tagged.transform = logpipe::parse_transform_chain("tag:env=prod");
  tagged.transform_position = logpipe::TransformPosition::Before;
  tagged.filter_expr =
      logpipe::dsl::FilterExpr::compile(R"(kv("env") = "prod")");

  logpipe::FanOutWriter writer({std::move(tagged)});
  CHECK(writer.open());
  uint64_t bytes = 0;
  // No extracted fields at all: only the tag supplies kv("env").
  CHECK(writer.write(make_fanout_record("app", "deploy finished"), bytes));
  writer.close();
  CHECK(!writer.failed());

  const std::string out = read_fanout_file(dir / "tagged.log");
  CHECK(out.find("deploy finished") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Requirement 1: Metrics keep independent per-output columns (lines and bytes
// per output name); the summary lists them and the Prometheus renderer
// exposes them as labelled counters.
// ---------------------------------------------------------------------------
TEST(fanout_metrics_per_output_columns) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_metrics_");

  logpipe::Metrics metrics;
  std::vector<logpipe::OutputRoute> routes;
  routes.push_back(make_route("alpha", "alpha.log", dir));
  logpipe::OutputRoute beta = make_route("beta", "beta.log", dir);
  beta.filter_expr = logpipe::dsl::FilterExpr::compile(R"(msg CONTAINS "beta")");
  routes.push_back(std::move(beta));

  logpipe::FanOutWriter writer(std::move(routes), &metrics);
  CHECK(writer.open());
  uint64_t bytes = 0;
  CHECK(writer.write(make_fanout_record("app", "alpha only"), bytes));    // -> alpha
  metrics.record_written(bytes, "app");  // mirrors main()'s aggregate call
  CHECK(writer.write(make_fanout_record("app", "beta payload"), bytes));  // -> both
  metrics.record_written(bytes, "app");
  writer.close();
  CHECK(!writer.failed());

  const auto snap = metrics.snapshot();
  CHECK_EQ_INT(snap.output_written_lines.at("alpha"), 2);
  CHECK_EQ_INT(snap.output_written_lines.at("beta"), 1);
  CHECK(snap.output_written_bytes.at("alpha") > snap.output_written_bytes.at("beta"));
  CHECK(snap.output_written_bytes.at("beta") > 0);
  // The aggregate counters still count every physical line (2 records).
  CHECK_EQ_INT(snap.written_lines, 2);

  const std::string summary = metrics.summary(0);
  CHECK(summary.find("lines per output:") != std::string::npos);
  CHECK(summary.find("alpha: 2") != std::string::npos);
  CHECK(summary.find("beta: 1") != std::string::npos);
  CHECK(summary.find("output bytes per output:") != std::string::npos);

  // Prometheus exposition: per-output labelled counters appear.
  const std::string prom = logpipe::PromStatsWriter({}).render(snap);
  CHECK(prom.find("logpipe_output_lines_written_total{output=\"alpha\"} 2") !=
        std::string::npos);
  CHECK(prom.find("logpipe_output_lines_written_total{output=\"beta\"} 1") !=
        std::string::npos);
  CHECK(prom.find("logpipe_output_bytes_written_total{output=\"alpha\"}") !=
        std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// End to end through Config::load: an explicit output group drives the
// fan-out routes exactly as main() would build them (per-output file,
// format, level, DSL and transform settings all honoured).
// ---------------------------------------------------------------------------
TEST(fanout_end_to_end_from_config_group) {
  const fs::path dir = make_fanout_temp_dir("logpipe_fo_e2e_");
  const fs::path cfg = dir / "fanout.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.dir = " << dir.string() << "\n"
        << "output.1.name = audit\n"
        << "output.1.file = audit.log\n"
        << "output.1.level = INFO\n"
        << "output.1.transform = tag:audited=yes\n"
        << "output.1.transform.position = before\n"
        << "output.1.filter.expr = kv(\"audited\") = \"yes\"\n"
        << "output.2.name = jsonl\n"
        << "output.2.file = jsonl.log\n"
        << "output.2.format = json_lines\n"
        << "output.2.level = WARN\n"
        << "output.2.transform = uppercase\n"
        << "output.2.transform.position = after\n";
  }

  logpipe::Config config;
  try {
    config = logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }
  CHECK(config.outputs_explicit);
  CHECK_EQ_INT(config.outputs.size(), 2);

  // Wiring identical to main(): shared WriterOptions overridden per route.
  logpipe::WriterOptions shared;
  shared.output_dir = config.output_dir;
  shared.max_bytes_per_file = config.rotate_size_bytes;
  shared.max_backups = config.rotate_backups;
  shared.daily_rotation = config.rotate_daily;
  shared.per_source_files = config.per_source_files;

  logpipe::Metrics metrics;
  std::vector<logpipe::OutputRoute> routes;
  for (const logpipe::OutputConfig& entry : config.outputs) {
    logpipe::OutputRoute route;
    route.name = entry.name;
    route.options = shared;
    route.options.base_name = entry.file;
    route.options.format = entry.format;
    route.options.compress = entry.compress;
    route.level = entry.level;
    route.filter_expr = entry.filter_expr;
    route.transform = entry.transform;
    route.transform_position = entry.transform_position;
    routes.push_back(std::move(route));
  }
  logpipe::FanOutWriter writer(std::move(routes), &metrics);
  CHECK(writer.open());

  uint64_t bytes = 0;
  CHECK(writer.write(make_fanout_record("app", "audit me", logpipe::Level::Info), bytes));
  CHECK(writer.write(make_fanout_record("app", "boom failure", logpipe::Level::Error), bytes));
  writer.close();
  CHECK(!writer.failed());

  // "audit": every record gains the audited tag before the filter, so all
  // lines land there.
  const std::string audit = read_fanout_file(dir / "audit.log");
  CHECK(audit.find("audit me") != std::string::npos);
  CHECK(audit.find("boom failure") != std::string::npos);
  // "jsonl": WARN+ only, json_lines format, uppercase applied after filtering.
  const std::string jsonl = read_fanout_file(dir / "jsonl.log");
  CHECK(jsonl.find("\"message\":\"BOOM FAILURE\"") != std::string::npos);
  CHECK(jsonl.find("audit me") == std::string::npos);
  CHECK_EQ_INT(metrics.snapshot().output_written_lines.at("audit"), 2);
  CHECK_EQ_INT(metrics.snapshot().output_written_lines.at("jsonl"), 1);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
