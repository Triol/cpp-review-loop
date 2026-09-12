// test_integration.cpp - end-to-end integration tests for the final round:
//
//   I1  Multi-file source + global DSL filter + fan-out with two branches
//       (text + RLE and json_lines + XOR), full shutdown, then decode /
//       decrypt both outputs, parse every line back and verify the content
//       AND the CRC-32 stamps recomputed from the decoded bytes.
//   I2  Multi-source (file tailer + stdin) + rate limiting + KV field
//       extraction + per-source output files, all driven by one config file
//       and wired exactly like main().
//   I3  include + profile + environment three-layer configuration stacking,
//       verified on the effective values and their provenance, then by
//       RUNNING the merged configuration as a pipeline.
//   I4  Versioned output header (version.h): fresh files are stamped, every
//       rotation product is stamped, appended files are not re-stamped, and
//       RLE / encrypted outputs carry the header as the first decoded line.
//
// Uses the shared zero-dependency harness (test_framework.h).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "config.h"
#include "dsl.h"
#include "kv_extractor.h"
#include "pipeline.h"
#include "source.h"
#include "stdin_reader.h"
#include "tail_engine.h"
#include "tailer.h"
#include "test_framework.h"
#include "util.h"
#include "version.h"
#include "writer.h"

namespace fs = std::filesystem;
using logpipe::BlockingQueue;
using logpipe::LogRecord;
using logpipe::RawLine;

namespace {

std::atomic<int> g_int_seq{0};

fs::path make_temp_dir(const char* prefix) {
  const fs::path dir =
      fs::temp_directory_path() /
      (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(g_int_seq.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

void write_text_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : text) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

// Env guard (POSIX): sets an override and removes it on destruction so a
// failing CHECK cannot leak the variable into later tests.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    ::setenv(name_.c_str(), value, 1);
  }
  ~ScopedEnv() { ::unsetenv(name_.c_str()); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string name_;
};

// Shared WriterOptions built from a Config exactly like main() does (the
// fan-out branches then override the per-output knobs on top of this).
logpipe::WriterOptions writer_options_from(const logpipe::Config& config) {
  logpipe::WriterOptions options;
  options.output_dir = config.output_dir;
  options.base_name = config.output_base;
  options.max_bytes_per_file = config.rotate_size_bytes;
  options.max_backups = config.rotate_backups;
  options.format = config.output_format;
  options.compress = config.output_compress;
  options.daily_rotation = config.rotate_daily;
  options.per_source_files = config.per_source_files;
  options.buffer_lines = config.write_buffer_lines;
  options.buffer_bytes = config.write_buffer_bytes;
  options.encrypt_password = config.encrypt_password;
  return options;
}

// Fan-out routes built from an explicit output.<N>.* group, mirroring the
// wiring in main(): shared options + per-output overrides.
std::vector<logpipe::OutputRoute> routes_from(const logpipe::Config& config,
                                              const logpipe::WriterOptions& shared) {
  std::vector<logpipe::OutputRoute> routes;
  routes.reserve(config.outputs.size());
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
  return routes;
}

// The main() ingestion path for one popped RawLine: parse, accounting,
// rate limit, tags/KV merge, filter, write. Returns true when the record was
// written, false when it was filtered (or dropped by the limiter: neither
// reaches the writer).
bool process_raw_line(const RawLine& raw, logpipe::LogParser& parser,
                      logpipe::LogFilter& filter, logpipe::RateLimiter& limiter,
                      logpipe::Metrics& metrics, bool kv_enabled,
                      const logpipe::KvExtractor& kv_extractor,
                      logpipe::OutputSink& writer) {
  LogRecord record = parser.parse(raw.source, raw.ingested_ms, raw.text);
  metrics.record_input(record.level);
  metrics.record_source_type_line(logpipe::source_type_name(raw.source_type));
  if (!limiter.allow(logpipe::util::now_ms())) {
    metrics.record_rate_dropped();
    return false;
  }
  metrics.record_source_line(record.source);
  std::map<std::string, std::string> merged_fields = raw.fields;
  if (kv_enabled) {
    std::map<std::string, std::string> fields;
    bool extracted = kv_extractor.extract(raw.text, fields);
    if (!extracted && record.parsed) {
      extracted = kv_extractor.extract(record.message, fields) ||
                  kv_extractor.extract_tail(record.message, fields);
    }
    if (extracted) {
      for (const auto& pair : fields) {
        merged_fields[pair.first] = pair.second;  // KV wins over tags
        metrics.record_kv_value(pair.first, pair.second);
      }
    }
    metrics.record_kv_line(extracted);
  }
  record.fields = std::move(merged_fields);
  if (!filter.passes(record)) {
    metrics.record_filtered();
    return false;
  }
  uint64_t bytes = 0;
  if (!writer.write(record, bytes)) return false;
  if (bytes > 0) metrics.record_written(bytes, record.source);
  return true;
}

// Parsed text-format output line:
//   <timestamp> [LEVEL] [source] message |crc=XXXXXXXX
struct ParsedTextLine {
  std::string timestamp;
  std::string level;
  std::string source;
  std::string message;
  uint32_t crc = 0;
};

// Parses one decoded text line and RECOMPUTES the CRC-32 over the exact
// prefix the writer stamped. Returns false when the shape is wrong or the
// CRC does not match (i.e. the round trip corrupted the line).
bool parse_and_verify_text_line(const std::string& line, ParsedTextLine& out) {
  const size_t stamp = line.rfind(" |crc=");
  if (stamp == std::string::npos) return false;
  const std::string body = line.substr(0, stamp);
  const std::string hex = line.substr(stamp + 6);
  if (hex.size() != 8) return false;
  out.crc = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
  if (logpipe::util::crc32(body) != out.crc) return false;
  const size_t level_open = body.find(" [");
  if (level_open == std::string::npos) return false;
  out.timestamp = body.substr(0, level_open);
  const size_t level_close = body.find(']', level_open);
  if (level_close == std::string::npos) return false;
  out.level = body.substr(level_open + 2, level_close - level_open - 2);
  const size_t source_open = body.find(" [", level_close);
  if (source_open == std::string::npos) return false;
  const size_t source_close = body.find(']', source_open);
  if (source_close == std::string::npos) return false;
  out.source = body.substr(source_open + 2, source_close - source_open - 2);
  if (source_close + 2 > body.size()) return false;
  out.message = body.substr(source_close + 2);
  return true;
}

// Parsed json_lines output line (ts, level, source, message, crc fields).
struct ParsedJsonLine {
  std::string ts;
  std::string level;
  std::string source;
  std::string message;
  uint32_t crc = 0;
};

// Extracts "\"key\":\"value\"" (test data is escape-free).
bool extract_json_string(const std::string& line, const char* key,
                         std::string& value) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t begin = line.find(needle);
  if (begin == std::string::npos) return false;
  const size_t value_begin = begin + needle.size();
  const size_t value_end = line.find('"', value_begin);
  if (value_end == std::string::npos) return false;
  value = line.substr(value_begin, value_end - value_begin);
  return true;
}

// Parses one decoded json_lines line and recomputes the CRC-32 over the
// serialized prefix before the "crc" field. Returns false on a shape or CRC
// mismatch.
bool parse_and_verify_json_line(const std::string& line, ParsedJsonLine& out) {
  const size_t crc_field = line.rfind(",\"crc\":");
  if (crc_field == std::string::npos) return false;
  if (line.empty() || line.back() != '}') return false;
  const std::string body = line.substr(0, crc_field);
  const std::string number = line.substr(crc_field + 7, line.size() - crc_field - 8);
  out.crc = static_cast<uint32_t>(std::strtoul(number.c_str(), nullptr, 10));
  if (logpipe::util::crc32(body) != out.crc) return false;
  if (!extract_json_string(line, "ts", out.ts)) return false;
  if (!extract_json_string(line, "level", out.level)) return false;
  if (!extract_json_string(line, "source", out.source)) return false;
  if (!extract_json_string(line, "message", out.message)) return false;
  return true;
}

bool has_line_with(const std::vector<std::string>& lines, const char* needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// ===========================================================================
// I1: multi-file source + DSL filter + fan-out (text+RLE / json_lines+XOR)
//     -> shutdown -> decode, decrypt, parse back, verify content + CRC.
// ===========================================================================
TEST(integration_fanout_two_branches_decode_and_verify_crc) {
  const fs::path dir = make_temp_dir("logpipe_int_fanout_");
  const fs::path app_log = dir / "app.log";
  const fs::path err_log = dir / "error.log";
  write_text_file(app_log,
                  "2026-09-06 12:00:00 [WARN] connect timeout to db\n"
                  "2026-09-06 12:00:01 [INFO] routine heartbeat ok\n"
                  "2026-09-06 12:00:02 [ERROR] disk failure imminent\n"
                  "plain unparsed noise line\n");
  write_text_file(err_log,
                  "2026-09-06 12:00:03 [ERROR] cache panic in shard 7\n"
                  "2026-09-06 12:00:04 [INFO] worker idle\n"
                  "2026-09-06 12:00:05 [WARN] retry storm subsiding\n");

  const fs::path cfg = dir / "fanout.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = " << app_log.string() << ", " << err_log.string() << "\n"
        << "output.dir = " << (dir / "out").string() << "\n"
        << "filter.level = DEBUG\n"
        // Global DSL stage: the INFO heartbeat is dropped, RAW lines pass.
        << "filter.expr = level >= \"INFO\" AND NOT msg CONTAINS \"heartbeat\"\n"
        << "output.1.name = plain\n"
        << "output.1.file = plain.log\n"
        << "output.1.format = text\n"
        << "output.1.compress = rle\n"
        << "output.2.name = safe\n"
        << "output.2.file = safe.log\n"
        << "output.2.format = json_lines\n"
        << "output.2.level = WARN\n";
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
  CHECK(config.filter_expr != nullptr);

  // Wiring identical to main(): DSL pre-filter + threshold/keyword stage,
  // fan-out writer from the explicit group. Branch 2 additionally gets the
  // XOR encryption here: the writer layer supports per-route encryption even
  // though the config key is currently global (the test exercises the writer
  // capability directly on the route options).
  const logpipe::WriterOptions shared = writer_options_from(config);
  std::vector<logpipe::OutputRoute> routes = routes_from(config, shared);
  CHECK_EQ_INT(static_cast<int>(routes.size()), 2);
  routes[1].options.encrypt_password = "branch2-passphrase";

  logpipe::Metrics metrics;
  logpipe::FanOutWriter writer(std::move(routes), &metrics);
  CHECK(writer.open());
  CHECK_EQ_INT(writer.route_count(), 2);

  logpipe::LogParser parser;
  const std::shared_ptr<const logpipe::dsl::FilterExpr> expr = config.filter_expr;
  logpipe::LogFilter filter(
      config.level_threshold, config.keyword,
      [expr](const LogRecord& rec) { return expr->passes(rec); });
  logpipe::RateLimiter unlimited(0);  // no rate limit on this scenario

  BlockingQueue<RawLine> queue(64);
  logpipe::TailOptions tail_options;
  tail_options.poll_ms = 10;  // offset_file stays empty (no persistence)
  std::vector<logpipe::FileSourceSpec> specs;
  for (const fs::path& path : config.input_files) {
    logpipe::FileSourceSpec spec;
    spec.path = path;
    specs.push_back(std::move(spec));
  }
  logpipe::Tailer tailer(specs, queue, tail_options);
  std::thread tailer_thread([&tailer] { tailer.run(); });

  const logpipe::KvExtractor kv_extractor;  // disabled branch (kv_enabled=false)
  int seen = 0;
  bool stop_requested = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    if (!stop_requested && seen == 7) {
      tailer.request_stop();  // all input lines observed: begin shutdown
      stop_requested = true;
    }
    RawLine raw;
    const auto popped = queue.pop_for(raw, 50);
    if (popped == BlockingQueue<RawLine>::PopResult::Got) {
      ++seen;
      process_raw_line(raw, parser, filter, unlimited, metrics,
                       /*kv_enabled=*/false, kv_extractor, writer);
    } else if (popped == BlockingQueue<RawLine>::PopResult::Closed) {
      break;  // tailer finished and the queue drained
    } else if (std::chrono::steady_clock::now() > deadline) {
      break;  // watchdog: never expected with 10s for 7 lines
    }
  }
  tailer_thread.join();
  writer.close();
  CHECK(!writer.failed());
  CHECK_EQ_INT(seen, 7);

  // Per-output fan-out accounting: 6 lines survive the DSL, 5 of them clear
  // the WARN threshold on the encrypted branch.
  const auto snap = metrics.snapshot();
  CHECK_EQ_INT(snap.output_written_lines.at("plain"), 6);
  CHECK_EQ_INT(snap.output_written_lines.at("safe"), 5);
  CHECK_EQ_INT(snap.filtered, 1);

  // ---- branch 1: text + RLE, decoded back and verified --------------------
  const fs::path plain_path = dir / "out" / "plain.log.rle";
  CHECK(fs::exists(plain_path));
  std::string plain_decoded;
  CHECK(logpipe::read_output_file(plain_path, /*password=*/"",
                                  /*compressed=*/true, plain_decoded));
  const std::vector<std::string> plain_lines = split_lines(plain_decoded);
  CHECK_EQ_INT(static_cast<int>(plain_lines.size()), 7);  // header + 6 records
  CHECK(logpipe::is_version_header(plain_lines[0]));
  CHECK(plain_lines[0].find("#logpipe v1.0.0 text created=") == 0);
  CHECK(plain_lines[0].find("created=") != std::string::npos &&
        plain_lines[0].back() == 'Z');  // UTC stamp
  for (size_t i = 1; i < plain_lines.size(); ++i) {
    ParsedTextLine parsed;
    // Every record line must re-verify its own CRC-32.
    CHECK(parse_and_verify_text_line(plain_lines[i], parsed));
    if (!parse_and_verify_text_line(plain_lines[i], parsed)) continue;
    CHECK(parsed.source == app_log.string() || parsed.source == err_log.string());
    // Branch 1 has no extra level gate: WARN, ERROR, RAW and the surviving
    // INFO line (worker idle) all land here - only the DSL-dropped heartbeat
    // is missing.
    CHECK(parsed.level == "WARN" || parsed.level == "ERROR" ||
          parsed.level == "RAW" || parsed.level == "INFO");
  }
  CHECK(has_line_with(plain_lines, "connect timeout to db"));
  CHECK(has_line_with(plain_lines, "disk failure imminent"));
  CHECK(has_line_with(plain_lines, "cache panic in shard 7"));
  CHECK(has_line_with(plain_lines, "plain unparsed noise line"));
  CHECK(!has_line_with(plain_lines, "routine heartbeat ok"));  // DSL-dropped

  // ---- branch 2: json_lines + XOR, decrypted back and verified ------------
  const fs::path safe_path = dir / "out" / "safe.log.enc";
  CHECK(fs::exists(safe_path));
  // The ciphertext on disk must not leak the header or any message.
  const std::string raw_cipher = read_text_file(safe_path);
  CHECK(raw_cipher.find("#logpipe") == std::string::npos);
  CHECK(raw_cipher.find("disk failure") == std::string::npos);
  std::string safe_decoded;
  CHECK(logpipe::read_output_file(safe_path, "branch2-passphrase",
                                  /*compressed=*/false, safe_decoded));
  const std::vector<std::string> safe_lines = split_lines(safe_decoded);
  CHECK_EQ_INT(static_cast<int>(safe_lines.size()), 6);  // header + 5 records
  CHECK(logpipe::is_version_header(safe_lines[0]));
  CHECK(safe_lines[0].find("#logpipe v1.0.0 json_lines created=") == 0);
  for (size_t i = 1; i < safe_lines.size(); ++i) {
    ParsedJsonLine parsed;
    CHECK(parse_and_verify_json_line(safe_lines[i], parsed));
    if (!parse_and_verify_json_line(safe_lines[i], parsed)) continue;
    CHECK(parsed.level == "WARN" || parsed.level == "ERROR" || parsed.level == "RAW");
  }
  CHECK(has_line_with(safe_lines, "connect timeout to db"));
  CHECK(has_line_with(safe_lines, "disk failure imminent"));
  CHECK(has_line_with(safe_lines, "cache panic in shard 7"));
  CHECK(has_line_with(safe_lines, "retry storm subsiding"));
  CHECK(has_line_with(safe_lines, "plain unparsed noise line"));  // RAW passes WARN
  CHECK(!has_line_with(safe_lines, "routine heartbeat ok"));
  CHECK(!has_line_with(safe_lines, "worker idle"));  // INFO below WARN branch

  // Wrong password must fail cleanly (container magic/payload check).
  std::string wrong;
  CHECK(!logpipe::read_output_file(safe_path, "wrong-password", false, wrong));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// I2: multi-source (file + stdin) + rate limiting + KV extraction +
//     per-source output files, driven by one shared config file.
// ===========================================================================
TEST(integration_multi_source_rate_limit_kv_per_source_files) {
  const fs::path dir = make_temp_dir("logpipe_int_multi_");
  const fs::path in_log = dir / "in.log";
  std::string file_body;
  for (int i = 0; i < 10; ++i) {
    file_body += "2026-09-06 12:00:0" + std::to_string(i % 10) +
                 " [INFO] user=alice action=login\n";
  }
  write_text_file(in_log, file_body);

  const fs::path cfg = dir / "multi.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = " << in_log.string() << ", stdin:\n"
        << "source.2.tags = channel=console\n"  // tags on the stdin entry
        << "output.dir = " << (dir / "out").string() << "\n"
        << "output.file = out.log\n"
        << "per_source_files = true\n"
        << "filter.level = DEBUG\n"
        << "rate.max_lines_per_sec = 12\n"  // 20 lines arrive: 8 must drop
        << "extract.kv = true\n";
  }

  logpipe::Config config;
  try {
    config = logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }
  CHECK(config.input_stdin);
  CHECK_EQ_INT(config.input_files.size(), 1);
  CHECK(config.per_source_files);
  CHECK_EQ_INT(config.max_lines_per_sec, 12);
  CHECK(config.extract_kv);
  CHECK_EQ_INT(config.stdin_tags.count("channel"), 1);

  // Wiring identical to main(): the file source closes the queue, stdin is a
  // secondary producer; PerSourceWriter splits the outputs by source name.
  logpipe::Metrics metrics;
  logpipe::PerSourceWriter writer(writer_options_from(config), &metrics);
  CHECK(writer.open());

  logpipe::LogParser parser;
  logpipe::LogFilter filter(config.level_threshold, config.keyword);
  logpipe::RateLimiter limiter(config.max_lines_per_sec);
  const logpipe::KvExtractor kv_extractor;

  BlockingQueue<RawLine> queue(64);
  logpipe::TailOptions tail_options;
  tail_options.poll_ms = 10;
  std::vector<logpipe::FileSourceSpec> specs;
  for (const fs::path& path : config.input_files) {
    const auto tags_it = config.file_tags.find(logpipe::normalize_file_key(path));
    logpipe::FileSourceSpec spec;
    spec.path = path;
    if (tags_it != config.file_tags.end()) spec.tags = tags_it->second;
    specs.push_back(std::move(spec));
  }
  logpipe::Tailer tailer(specs, queue, tail_options);
  tailer.load_offsets();
  std::thread tailer_thread([&tailer] { tailer.run(); });

  // Feed the stdin source from an in-process buffer (std::cin is redirected
  // before the source starts and restored after it joined).
  std::istringstream stdin_data(
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n"
      "job=sync status=done\n");
  std::streambuf* old_stdin = std::cin.rdbuf(stdin_data.rdbuf());
  logpipe::StdinReader stdin_reader(queue, /*close_queue_on_exit=*/false,
                                    config.stdin_tags);
  stdin_reader.start();

  int seen = 0;
  int stdin_records = 0;
  bool stop_requested = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    if (!stop_requested && seen == 20) {
      tailer.request_stop();
      stdin_reader.request_stop();
      stop_requested = true;
    }
    RawLine raw;
    const auto popped = queue.pop_for(raw, 50);
    if (popped == BlockingQueue<RawLine>::PopResult::Got) {
      ++seen;
      if (raw.source_type == logpipe::SourceType::Stdin) {
        ++stdin_records;
        // Source tags ride on the RawLine (config source.2.tags).
        CHECK(raw.fields.count("channel") == 1);
        CHECK(raw.fields.count("channel") == 1 &&
              raw.fields.at("channel") == "console");
      }
      process_raw_line(raw, parser, filter, limiter, metrics,
                       /*kv_enabled=*/true, kv_extractor, writer);
    } else if (popped == BlockingQueue<RawLine>::PopResult::Closed) {
      break;
    } else if (std::chrono::steady_clock::now() > deadline) {
      break;
    }
  }
  tailer_thread.join();
  stdin_reader.join();
  std::cin.rdbuf(old_stdin);  // restore before anything else touches std::cin
  writer.close();
  CHECK(!writer.failed());
  CHECK(stdin_reader.eof());  // the buffer hit end-of-stream
  CHECK_EQ_INT(seen, 20);
  CHECK_EQ_INT(stdin_records, 10);
  CHECK_EQ_INT(writer.sink_count(), 2);  // one output file per source

  const auto snap = metrics.snapshot();
  // Both sources ingested everything; the limiter dropped the overflow.
  CHECK_EQ_INT(static_cast<int>(snap.ingested), 20);
  CHECK(snap.rate_dropped > 0);
  CHECK_EQ_INT(static_cast<int>(snap.written_lines + snap.rate_dropped), 20);
  CHECK(snap.written_lines >= 4);  // each source still contributed
  CHECK_EQ_INT(static_cast<int>(snap.source_type_lines.at("file")), 10);
  CHECK_EQ_INT(static_cast<int>(snap.source_type_lines.at("stdin")), 10);
  // KV extraction ran on every admitted line; both lines carry two pairs.
  CHECK_EQ_INT(static_cast<int>(snap.kv_lines_attempted),
               static_cast<int>(snap.written_lines));
  CHECK_EQ_INT(static_cast<int>(snap.kv_lines_extracted),
               static_cast<int>(snap.kv_lines_attempted));
  CHECK_EQ_INT(static_cast<int>(snap.kv_fields_extracted),
               2 * static_cast<int>(snap.kv_lines_extracted));

  const std::string summary = metrics.summary(0);
  CHECK(summary.find("lines rate-dropped : ") != std::string::npos);
  CHECK(summary.find("kv lines attempted : ") != std::string::npos);
  CHECK(summary.find("lines per source type:") != std::string::npos);
  CHECK(summary.find("output bytes per source:") != std::string::npos);

  // Per-source output files: each holds only its own source's lines and
  // starts with the versioned output header.
  const fs::path file_out = dir / "out" / "out_in_log.log";
  const fs::path stdin_out = dir / "out" / "out_stdin.log";
  CHECK(fs::exists(file_out));
  CHECK(fs::exists(stdin_out));
  const std::vector<std::string> file_lines = split_lines(read_text_file(file_out));
  const std::vector<std::string> stdin_lines = split_lines(read_text_file(stdin_out));
  CHECK(logpipe::is_version_header(file_lines[0]));
  CHECK(logpipe::is_version_header(stdin_lines[0]));
  CHECK(has_line_with(file_lines, "user=alice action=login"));
  CHECK(!has_line_with(file_lines, "job=sync"));
  CHECK(has_line_with(stdin_lines, "job=sync status=done"));
  CHECK(!has_line_with(stdin_lines, "user=alice"));
  CHECK(file_lines.size() > 1);
  CHECK(stdin_lines.size() > 1);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// I3: include + profile + environment three-layer configuration stacking,
//     verified on effective values AND by running the merged pipeline.
// ===========================================================================
TEST(integration_config_layering_include_profile_env_pipeline) {
  const fs::path dir = make_temp_dir("logpipe_int_layers_");
  const fs::path in_log = dir / "in3.log";
  write_text_file(in_log,
                  "2026-09-06 12:00:00 [DEBUG] cache warm envkw done\n"
                  "2026-09-06 12:00:01 [INFO] routine sweep finished\n");

  // Layer 1 (file) is split across two files: common.conf is pulled in via
  // include and defines the profile groups used by the profile layer.
  write_text_file(dir / "common.conf",
                  "# shared profile groups (loaded via include)\n"
                  "profile.dev.filter.level = DEBUG\n"
                  "profile.dev.tail.poll_ms = 40\n"
                  "profile.prod.filter.level = ERROR\n");
  const fs::path cfg = dir / "logpipe.conf";
  write_text_file(cfg,
                  "include = common.conf\n"
                  "input.files = " + in_log.string() + "\n"
                  "output.dir = " + (dir / "out").string() + "\n"
                  "output.file = layered.log\n"
                  "filter.level = INFO\n"     // file layer: overridden by profile
                  "tail.poll_ms = 999\n"      // file layer: overridden by env
                  "active_profile = dev\n");  // profile layer switch

  // Layer 3 (environment): beats file AND profile values.
  ScopedEnv env_poll("LOGPIPE_TAIL_POLL_MS", "25");
  ScopedEnv env_keyword("LOGPIPE_FILTER_KEYWORD", "envkw");
  ScopedEnv env_rate("LOGPIPE_RATE_MAX_LINES_PER_SEC", "500");

  logpipe::Config config;
  try {
    config = logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }

  // Provenance: every layer left its trace on the winning key.
  CHECK_EQ_INT(config.tail_poll_ms, 25);  // env beats file (999) and profile (40)
  CHECK_STR_EQ(config.effective.at("tail.poll_ms").source, "env");
  CHECK(config.level_threshold == logpipe::Level::Debug);  // profile.dev (via include)
  CHECK_STR_EQ(config.effective.at("filter.level").source, "profile:dev");
  CHECK_STR_EQ(config.keyword, "envkw");  // env beats the (unset) file value
  CHECK_STR_EQ(config.effective.at("filter.keyword").source, "env");
  CHECK_EQ_INT(config.max_lines_per_sec, 500);  // env-only key
  CHECK_STR_EQ(config.effective.at("rate.max_lines_per_sec").source, "env");
  CHECK_STR_EQ(config.effective.at("output.file").source, "file");
  CHECK(config.effective.at("filter.level").value == "DEBUG");

  // The --check-config report renders all three provenance tags.
  const fs::path report_path = dir / "report.txt";
  {
    std::FILE* report = std::fopen(report_path.string().c_str(), "w");
    CHECK(report != nullptr);
    CHECK_EQ_INT(logpipe::check_config_report(report, cfg.string(), config), 0);
    std::fclose(report);
  }
  const std::string report_text = read_text_file(report_path);
  CHECK(report_text.find("logpipe configuration report") != std::string::npos);
  CHECK(report_text.find("profile:dev") != std::string::npos);
  CHECK(report_text.find("env") != std::string::npos);
  CHECK(report_text.find("tail.poll_ms") != std::string::npos);
  CHECK(report_text.find("filter.keyword") != std::string::npos);
  CHECK(report_text.find("rate.max_lines_per_sec") != std::string::npos);
  CHECK(report_text.find("default") != std::string::npos);  // unset keys listed too

  // RUN the merged configuration: the DEBUG line survives only because the
  // threshold came from the profile layer (DEBUG) - the file layer's INFO
  // would have dropped it; the second line fails the env-layer keyword.
  logpipe::Metrics metrics;
  logpipe::RollingWriter writer(writer_options_from(config), &metrics);
  CHECK(writer.open());
  logpipe::LogParser parser;
  logpipe::LogFilter filter(config.level_threshold, config.keyword);
  logpipe::RateLimiter unlimited(0);  // this scenario has no rate limit

  BlockingQueue<RawLine> queue(16);
  logpipe::TailOptions tail_options;
  tail_options.poll_ms = config.tail_poll_ms;
  logpipe::Tailer tailer({in_log}, queue, tail_options);
  std::thread tailer_thread([&tailer] { tailer.run(); });

  const logpipe::KvExtractor kv_extractor;
  int seen = 0;
  int written = 0;
  bool stop_requested = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    if (!stop_requested && seen == 2) {
      tailer.request_stop();
      stop_requested = true;
    }
    RawLine raw;
    const auto popped = queue.pop_for(raw, 50);
    if (popped == BlockingQueue<RawLine>::PopResult::Got) {
      ++seen;
      if (process_raw_line(raw, parser, filter, unlimited, metrics,
                           /*kv_enabled=*/false, kv_extractor, writer)) {
        ++written;
      }
    } else if (popped == BlockingQueue<RawLine>::PopResult::Closed) {
      break;
    } else if (std::chrono::steady_clock::now() > deadline) {
      break;
    }
  }
  tailer_thread.join();
  writer.close();
  CHECK(!writer.failed());
  CHECK_EQ_INT(seen, 2);
  CHECK_EQ_INT(written, 1);  // only the DEBUG+envkw line

  const std::vector<std::string> out_lines =
      split_lines(read_text_file(dir / "out" / "layered.log"));
  CHECK_EQ_INT(static_cast<int>(out_lines.size()), 2);  // header + 1 record
  CHECK(logpipe::is_version_header(out_lines[0]));
  CHECK(has_line_with(out_lines, "cache warm envkw done"));
  CHECK(!has_line_with(out_lines, "routine sweep finished"));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// I4: versioned output header - fresh files, rotation products, append-only
//     files, RLE and encrypted outputs. The injected clock makes the header
//     bytes exactly predictable.
// ===========================================================================
TEST(integration_version_header_fresh_rotation_append_and_encodings) {
  const fs::path dir = make_temp_dir("logpipe_int_header_");
  const int64_t fixed_ms = 1788696000000;  // one fixed instant, UTC-stamped
  const std::string expected_text =
      logpipe::output_version_header("text", fixed_ms);
  const std::string expected_json =
      logpipe::output_version_header("json_lines", fixed_ms);
  CHECK(logpipe::is_version_header(expected_text));
  CHECK(!logpipe::is_version_header("logpipe v1.0.0 text created=x"));  // no '#'
  CHECK(!logpipe::is_version_header("#logpipe v1.0.0 text"));           // no stamp
  CHECK(!logpipe::is_version_header("#other v1.0.0 text created=x"));

  auto record = [&](const std::string& message) {
    LogRecord rec;
    rec.source = "src";
    rec.ingested_ms = fixed_ms;
    rec.timestamp_text = "2026-09-06 12:00:00";
    rec.level = logpipe::Level::Info;
    rec.message = message;
    rec.parsed = true;
    return rec;
  };

  // (a) Fresh text file: exact header bytes as the very first line.
  {
    const fs::path out = dir / "a";
    fs::create_directories(out);
    logpipe::WriterOptions options;
    options.output_dir = out;
    options.base_name = "fresh.log";
    options.clock = [&fixed_ms] { return fixed_ms; };
    logpipe::RollingWriter writer(options);
    CHECK(writer.open());
    uint64_t bytes = 0;
    CHECK(writer.write(record("payload one"), bytes));
    writer.close();
    CHECK(!writer.failed());
    const std::vector<std::string> lines = split_lines(read_text_file(out / "fresh.log"));
    CHECK_STR_EQ(lines[0], expected_text);
    CHECK_EQ_INT(static_cast<int>(lines.size()), 2);
    CHECK(lines[1].find("payload one") != std::string::npos);
  }

  // (b) Size rotation: the active file AND every backup product start with
  // the header - each rotated file is a fresh birth, stamped exactly once.
  {
    const fs::path out = dir / "b";
    fs::create_directories(out);
    logpipe::WriterOptions options;
    options.output_dir = out;
    options.base_name = "rot.log";
    options.max_bytes_per_file = 160;
    options.max_backups = 4;
    options.clock = [&fixed_ms] { return fixed_ms; };
    logpipe::RollingWriter writer(options);
    CHECK(writer.open());
    for (int i = 0; i < 12; ++i) {
      uint64_t bytes = 0;
      CHECK(writer.write(record("rotation payload line number " + std::to_string(i)),
                         bytes));
    }
    writer.close();
    CHECK(!writer.failed());
    const std::vector<std::string> active =
        split_lines(read_text_file(out / "rot.log"));
    const std::vector<std::string> backup1 =
        split_lines(read_text_file(out / "rot_1.log"));
    const std::vector<std::string> backup2 =
        split_lines(read_text_file(out / "rot_2.log"));
    CHECK_STR_EQ(active[0], expected_text);
    CHECK_STR_EQ(backup1[0], expected_text);
    CHECK_STR_EQ(backup2[0], expected_text);
    CHECK(active.size() > 1 && backup1.size() > 1 && backup2.size() > 1);
    // Exactly one header per file, always the first line.
    int header_count = 0;
    for (const std::string& line : active) {
      if (logpipe::is_version_header(line)) ++header_count;
    }
    for (const std::string& line : backup1) {
      if (logpipe::is_version_header(line)) ++header_count;
    }
    for (const std::string& line : backup2) {
      if (logpipe::is_version_header(line)) ++header_count;
    }
    CHECK_EQ_INT(header_count, 3);
  }

  // (c) Append (daily rotation, same day restart): the existing dated file
  // is continued and NOT stamped again.
  {
    const fs::path out = dir / "c";
    fs::create_directories(out);
    const std::string date =
        logpipe::util::timestamp_compact(fixed_ms).substr(0, 8);
    write_text_file(out / ("app_" + date + ".log"), "old line from yesterday run\n");
    logpipe::WriterOptions options;
    options.output_dir = out;
    options.base_name = "app.log";
    options.daily_rotation = true;
    options.clock = [&fixed_ms] { return fixed_ms; };
    logpipe::RollingWriter writer(options);
    CHECK(writer.open());
    CHECK_STR_EQ(writer.active_file_name(), "app_" + date + ".log");
    uint64_t bytes = 0;
    CHECK(writer.write(record("new line from this run"), bytes));
    writer.close();
    CHECK(!writer.failed());
    const std::vector<std::string> lines =
        split_lines(read_text_file(out / ("app_" + date + ".log")));
    CHECK_STR_EQ(lines[0], "old line from yesterday run");  // untouched head
    CHECK_EQ_INT(static_cast<int>(lines.size()), 2);
    CHECK(lines[1].find("new line from this run") != std::string::npos);
    for (const std::string& line : lines) {
      CHECK(!logpipe::is_version_header(line));  // no header anywhere
    }
  }

  // (d) RLE output: the header travels as the first decoded frame.
  {
    const fs::path out = dir / "d";
    fs::create_directories(out);
    logpipe::WriterOptions options;
    options.output_dir = out;
    options.base_name = "packed.log";
    options.compress = logpipe::CompressMode::Rle;
    options.clock = [&fixed_ms] { return fixed_ms; };
    logpipe::RollingWriter writer(options);
    CHECK(writer.open());
    uint64_t bytes = 0;
    CHECK(writer.write(record("compressed payload"), bytes));
    writer.close();
    CHECK(!writer.failed());
    std::string decoded;
    CHECK(logpipe::read_output_file(out / "packed.log.rle", "", true, decoded));
    const std::vector<std::string> lines = split_lines(decoded);
    CHECK_STR_EQ(lines[0], expected_text);  // format name stays "text"
    CHECK(lines.size() == 2 && lines[1].find("compressed payload") != std::string::npos);
  }

  // (e) Encrypted json_lines output: the header is the first decrypted chunk
  // after the LXEF magic and never appears in plaintext on disk.
  {
    const fs::path out = dir / "e";
    fs::create_directories(out);
    logpipe::WriterOptions options;
    options.output_dir = out;
    options.base_name = "secret.log";
    options.format = logpipe::OutputFormat::JsonLines;
    options.encrypt_password = "header-pass";
    options.clock = [&fixed_ms] { return fixed_ms; };
    logpipe::RollingWriter writer(options);
    CHECK(writer.open());
    uint64_t bytes = 0;
    CHECK(writer.write(record("encrypted payload"), bytes));
    writer.close();
    CHECK(!writer.failed());
    const std::string raw = read_text_file(out / "secret.log.enc");
    CHECK(raw.find("#logpipe") == std::string::npos);
    std::string decoded;
    CHECK(logpipe::read_output_file(out / "secret.log.enc", "header-pass", false,
                                    decoded));
    const std::vector<std::string> lines = split_lines(decoded);
    CHECK_STR_EQ(lines[0], expected_json);
    CHECK(lines.size() == 2 && lines[1].find("encrypted payload") != std::string::npos);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}
