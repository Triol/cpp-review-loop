// config.cpp - key=value config file parsing and validation. Unknown keys
// are reported through util diagnostics; hard errors throw runtime_error.
//
// Loading happens in layers with increasing priority:
//   1. the root file plus every `include = other.conf` it pulls in
//      (relative to the including file's directory, recursive, cycles are
//      detected and rejected); within a file, later keys overwrite earlier
//      ones; included files are applied at the position of their include
//      directive, so a later include overrides an earlier one;
//   2. the activated profile: `profile.<name>.*` keys are pre-defined groups
//      that only apply when `active_profile = <name>` selects them; they
//      override the plain file keys;
//   3. environment variables LOGPIPE_<KEY> (key uppercased, dots become
//      underscores, e.g. LOGPIPE_FILTER_LEVEL) override everything; they can
//      be disabled with --no-env (LoadOptions::use_env = false) and every
//      applied override is logged through the diagnostics.
//
// `level.register.<NAME> = <1..999>` entries are applied before everything
// else so a custom level name can be used by any later key (thresholds, DSL).
// After loading, --check-config (check_config_report) prints a per-key table
// of effective values and their provenance.

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "util.h"

namespace logpipe {
namespace {

std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

std::string to_lower(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string to_upper(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

// Strictly parses a non-negative integer. Signed input is rejected up front:
// std::stoull would fully consume "-5" and wrap around to a huge value that
// downstream range checks accept.
uint64_t parse_u64(const std::string& key, const std::string& value) {
  const std::string trimmed = trim(value);
  if (!trimmed.empty() && (trimmed[0] == '-' || trimmed[0] == '+')) {
    throw std::runtime_error("config: '" + key + "' must be a non-negative integer, got '" +
                             value + "'");
  }
  try {
    size_t consumed = 0;
    const uint64_t number = std::stoull(value, &consumed);
    if (trim(value.substr(consumed)).empty()) return number;
  } catch (const std::exception&) {
    // fall through to the error below
  }
  throw std::runtime_error("config: '" + key + "' expects an integer, got '" + value + "'");
}

int parse_int(const std::string& key, const std::string& value, int low, int high) {
  const uint64_t number = parse_u64(key, value);
  if (number < static_cast<uint64_t>(low) || number > static_cast<uint64_t>(high)) {
    throw std::runtime_error("config: '" + key + "' must be between " +
                             std::to_string(low) + " and " + std::to_string(high));
  }
  return static_cast<int>(number);
}

std::vector<std::string> split_list(const std::string& value) {
  std::vector<std::string> items;
  std::istringstream in(value);
  std::string item;
  while (std::getline(in, item, ',')) {
    const std::string trimmed = trim(item);
    if (!trimmed.empty()) items.push_back(trimmed);
  }
  return items;
}

// Accepts the usual boolean spellings; false is returned (with ok=false) for
// anything else so the caller can raise a descriptive error.
bool parse_bool(const std::string& value, bool& out) {
  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    out = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    out = false;
    return true;
  }
  return false;
}

// Returns true when `text` is a non-empty all-digit run (fan-out indices).
bool is_number(const std::string& text) {
  if (text.empty()) return false;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// Environment variable name for a configuration key: LOGPIPE_ prefix, the
// key uppercased with dots replaced by underscores (filter.level ->
// LOGPIPE_FILTER_LEVEL).
std::string env_name_for_key(const std::string& key) {
  std::string out = "LOGPIPE_";
  for (const char c : key) {
    out.push_back(c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

// Reads `name` from the environment; returns false when unset or empty.
bool read_env(const std::string& name, std::string& value) {
  const char* raw = std::getenv(name.c_str());
  if (raw == nullptr) return false;
  value = raw;
  return true;
}

// The known configuration keys, used for (a) the environment-override layer
// and (b) the --check-config report table. Both spellings are listed for the
// keys that accept dotted and underscored aliases; the report deduplicates
// them by displayed name.
const std::vector<std::string>& known_keys() {
  static const std::vector<std::string> keys = {
      "input.files",        "input.file",         "output.dir",
      "output.file",        "rotate.size_bytes",  "rotate.backups",
      "output_format",      "output.compress",    "rotate.daily",
      "rotate_daily",       "per_source_files",   "max_lines_per_sec",
      "extract.kv",         "extract_kv",         "stats.interval_sec",
      "stats.dir",          "stats.keep_files",   "filter.level",
      "filter.keyword",     "filter.expr",        "tail.poll_ms",
      "state.offset_file",  "run.duration_sec",   "stop_file",
      "run.stop_file",      "rate.max_lines_per_sec",
      "replay.since",       "replay.index_interval_bytes",
      "diag.level",         "diag.file",          "active_profile",
      "write.buffer_lines", "write.buffer_bytes", "encrypt.password",
      "read.chunk_bytes",   "queue.capacity",     "glob.exclude",
  };
  return keys;
}

// Deprecated spellings → canonical keys. A hit is warned about (once per
// occurrence, with the config origin) and the value is parsed by the canonical
// branch below — the map only changes the message, never the behaviour. Add a
// pair here when renaming a key so migrations stay visible.
const std::map<std::string, std::string>& deprecated_keys() {
  static const std::map<std::string, std::string> keys = {
      {"input.file", "input.files"},
      {"rotate_daily", "rotate.daily"},
      {"extract_kv", "extract.kv"},
      {"output_format", "output.format"},
      {"max_lines_per_sec", "rate.max_lines_per_sec"},
      {"stop_file", "run.stop_file"},
  };
  return keys;
}

// Validation bounds (named so the contract is greppable and the error
// messages cannot drift away from the checks they belong to).
constexpr int kMinRotateSizeBytes = 256;
constexpr int kMaxRotateBackups = 999;
constexpr int kMaxLinesPerSec = 100000000;
constexpr int kMinTailPollMs = 20;
constexpr int kMaxTailPollMs = 60000;
constexpr int kMaxRunDurationSec = 7 * 24 * 3600;  // one week
constexpr int kMinReadChunkBytes = 128;
constexpr int kMaxReadChunkBytes = 1048576;
constexpr int kMinQueueCapacity = 1;
constexpr int kMaxQueueCapacity = 1000000;
constexpr int kMaxStatsIntervalSec = 86400;
constexpr int kMinStatsKeepFiles = 1;
constexpr int kMaxStatsKeepFiles = 10000;
constexpr int kMinIndexIntervalBytes = 64;
constexpr int kMaxIndexIntervalBytes = 1073741824;

// The attributes accepted on the fan-out keys output.<N>.<attr>; used to
// derive the environment-variable candidates and the report rows.
const std::vector<std::string>& fanout_attrs() {
  static const std::vector<std::string> attrs = {
      "name", "file", "format", "compress", "level",
      "filter.expr", "transform", "transform.position",
  };
  return attrs;
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

// Local spelling of the diagnostic level for the report table (util.h only
// exposes the parser direction).
const char* diag_level_name(util::DiagLevel level) {
  switch (level) {
    case util::DiagLevel::Debug: return "debug";
    case util::DiagLevel::Info:  return "info";
    case util::DiagLevel::Warn:  return "warn";
    case util::DiagLevel::Error: return "error";
  }
  return "info";
}

// One raw key=value assignment gathered before application. `source` is the
// provenance tag ("file", "profile:<name>" or "env") and `origin` the
// human-readable location (file:line) used in warnings and errors.
struct RawEntry {
  std::string key;
  std::string value;
  std::string source;
  std::string origin;
};

// Reads `path` and appends every assignment to `entries`, following
// `include = file` directives recursively. `visited` holds the canonical
// paths currently being loaded so include cycles are detected and rejected;
// include targets are resolved relative to the including file's directory.
void load_file_entries(const std::string& path,
                       std::vector<std::string>& visited,
                       std::vector<RawEntry>& entries) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) canonical = std::filesystem::path(path);
  const std::string canonical_text = canonical.string();
  for (const std::string& open : visited) {
    if (open == canonical_text) {
      throw std::runtime_error("config: include cycle detected at '" + path + "'");
    }
  }
  visited.push_back(canonical_text);

  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("config: cannot open '" + path + "'");
  }
  const std::filesystem::path base_dir = canonical.parent_path();

  std::string raw_line;
  int line_no = 0;
  while (std::getline(in, raw_line)) {
    ++line_no;
    const std::string line = trim(raw_line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      util::log_warn("config: " + path + ":" + std::to_string(line_no) +
                     ": ignoring line without '='");
      continue;
    }
    RawEntry entry;
    entry.key = to_lower(trim(line.substr(0, eq)));
    entry.value = trim(line.substr(eq + 1));
    entry.source = "file";
    entry.origin = path + ":" + std::to_string(line_no);

    // include = other.conf (comma-separated lists are accepted too):
    // resolved relative to this file's directory and loaded recursively.
    if (entry.key == "include" || entry.key == "include.file") {
      for (const std::string& target : split_list(entry.value)) {
        std::filesystem::path full = std::filesystem::path(target);
        if (full.is_relative()) full = base_dir / full;
        load_file_entries(full.string(), visited, entries);
      }
      continue;
    }
    entries.push_back(std::move(entry));
  }
  visited.pop_back();
}

// Splits `profile.<name>.<rest>` keys. Returns false when the key does not
// have the profile prefix; throws when the prefix is there but malformed.
bool split_profile_key(const RawEntry& entry, std::string& name, std::string& rest) {
  if (entry.key.rfind("profile.", 0) != 0) return false;
  const std::string body = entry.key.substr(8);
  const auto dot = body.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= body.size()) {
    throw std::runtime_error("config: " + entry.origin +
                             ": malformed profile key '" + entry.key +
                             "' (expected profile.<name>.<key>)");
  }
  name = body.substr(0, dot);
  rest = body.substr(dot + 1);
  return true;
}

// Applies a filter.level / output.<N>.level value: built-in names (except
// RAW, which would let everything through) and registered custom levels are
// both accepted; the numeric value drives the threshold comparison.
Level parse_level_value(const std::string& key, const std::string& value) {
  Level builtin;
  if (level_from_string(value, builtin)) {
    if (builtin == Level::Raw) {
      throw std::runtime_error("config: '" + key + "' expects DEBUG, INFO, WARN or ERROR, got '" +
                               value + "'");
    }
    return builtin;
  }
  int custom_value = 0;
  if (LevelRegistry::instance().lookup(value, custom_value)) {
    return static_cast<Level>(custom_value);
  }
  throw std::runtime_error(
      "config: '" + key + "' expects DEBUG, INFO, WARN, ERROR or a registered custom level, got '" +
      value + "'");
}

// Applies one raw assignment (file, profile or env) to `config` and records
// the effective value + source in config.effective. `inputs_reset` lets the
// list-valued input.files key replace (instead of append) when a
// higher-priority layer (profile/env) overrides it.
void apply_entry(Config& config, const RawEntry& entry, bool& inputs_reset) {
  const std::string& key = entry.key;
  const std::string& value = entry.value;
  const bool from_override = entry.source != "file";

  // Fan-out output group: output.<N>.<attr>. The numeric second segment
  // distinguishes these keys from the legacy output.dir / output.file /
  // output.compress spellings.
  int fanout_index = -1;
  std::string fanout_attr;
  if (key.rfind("output.", 0) == 0) {
    const std::string rest = key.substr(7);
    const auto dot = rest.find('.');
    if (dot != std::string::npos && is_number(rest.substr(0, dot))) {
      fanout_index = parse_int("output.<N> index", rest.substr(0, dot), 1, 9999);
      fanout_attr = to_lower(rest.substr(dot + 1));
      config.outputs_explicit = true;
    }
  }
  auto output_slot = [&, fanout_index]() -> OutputConfig& {
    for (OutputConfig& slot : config.outputs) {
      if (slot.index == fanout_index) return slot;
    }
    config.outputs.emplace_back();
    config.outputs.back().index = fanout_index;
    return config.outputs.back();
  };

  auto record = [&](const std::string& shown_value) {
    config.effective[key] = ConfigEntry{shown_value, entry.source};
  };

  if (fanout_index > 0) {
    OutputConfig& slot = output_slot();
    if (fanout_attr == "name") {
      slot.name = value;
    } else if (fanout_attr == "file") {
      slot.file = value;
    } else if (fanout_attr == "format") {
      if (!output_format_from_string(value, slot.format)) {
        throw std::runtime_error(
            "config: 'output." + std::to_string(fanout_index) +
            ".format' expects text or json_lines, got '" + value + "'");
      }
    } else if (fanout_attr == "compress") {
      if (!compress_mode_from_string(value, slot.compress)) {
        throw std::runtime_error(
            "config: 'output." + std::to_string(fanout_index) +
            ".compress' expects none or rle, got '" + value + "'");
      }
    } else if (fanout_attr == "level") {
      slot.level = parse_level_value("output." + std::to_string(fanout_index) + ".level", value);
    } else if (fanout_attr == "filter.expr") {
      slot.filter_expr_text = value;
    } else if (fanout_attr == "transform") {
      slot.transform_text = value;
    } else if (fanout_attr == "transform.position") {
      if (!transform_position_from_string(value, slot.transform_position)) {
        throw std::runtime_error(
            "config: 'output." + std::to_string(fanout_index) +
            ".transform.position' expects before or after, got '" + value + "'");
      }
    } else {
      util::log_warn("config: " + entry.origin + ": unknown key '" + key + "' ignored");
      return;
    }
    record(value);
    return;
  }

  // Deprecated spellings: warn with the config origin (the value is parsed by
  // the canonical branch below — the map only changes the message).
  const auto deprecated = deprecated_keys().find(key);
  if (deprecated != deprecated_keys().end()) {
    util::log_warn("config: " + entry.origin + ": deprecated key '" + key +
                   "' — use '" + deprecated->second + "' instead");
  }

  if (key == "input.files" || key == "input.file") {
    // A profile or environment override replaces the whole input list instead
    // of appending to whatever the file layer already produced.
    if (from_override && !inputs_reset) {
      config.input_files.clear();
      config.input_stdin = false;
      config.glob_sources.clear();
      config.input_entry_specs.clear();
      inputs_reset = true;
    }
    for (const auto& item : split_list(value)) {
      // "stdin:" selects standard input as an extra source; "glob:<pattern>"
      // declares a pattern-discovery directory source; anything else is
      // treated as a plain file path. Every entry gets a 1-based declaration
      // index so its source.<n>.tags can be attached later.
      if (to_lower(item) == "stdin:") {
        config.input_stdin = true;
        config.input_entry_specs.push_back("stdin");
      } else if (item.rfind("glob:", 0) == 0 && item.size() > 5) {
        GlobSourceConfig glob;
        glob.pattern = item.substr(5);
        config.glob_sources.push_back(glob);
        config.input_entry_specs.push_back(item);
      } else {
        config.input_files.emplace_back(item);
        config.input_entry_specs.push_back("file:" + item);
      }
    }
    std::string shown;
    for (const std::string& spec : config.input_entry_specs) {
      shown += (shown.empty() ? "" : ", ") +
               (spec == "stdin" ? std::string("stdin:") : spec);
    }
    record(shown);
    return;
  }
  if (key == "glob.exclude") {
    // Append (repeated keys accumulate); a profile/env override appends too,
    // mirroring how list-valued knobs behave elsewhere.
    for (const std::string& pattern : split_list(value)) {
      config.glob_exclude.push_back(pattern);
    }
    std::string shown;
    for (const std::string& pattern : config.glob_exclude) {
      shown += (shown.empty() ? "" : ", ") + pattern;
    }
    record(shown);
    return;
  }
  // source.<N>.tags: source-level metadata for the N-th input entry
  // (1-based, in input.files declaration order). Only the "tags" attribute
  // exists today; anything else falls through to the unknown-key warning.
  if (key.rfind("source.", 0) == 0) {
    const std::string rest = key.substr(7);
    const auto dot = rest.find('.');
    if (dot != std::string::npos && is_number(rest.substr(0, dot)) &&
        rest.substr(dot + 1) == "tags") {
      const int index = parse_int("source.<N> index", rest.substr(0, dot), 1, 9999);
      // Comma-separated k=v pairs; a bare word becomes <word>="true".
      SourceTags tags;
      for (const std::string& item : split_list(value)) {
        const auto eq = item.find('=');
        if (eq != std::string::npos) {
          tags[item.substr(0, eq)] = item.substr(eq + 1);
        } else if (!item.empty()) {
          tags[item] = "true";
        }
      }
      config.source_tags[index] = tags;
      record(value);
      return;
    }
  }
  if (key == "output.dir") {
    config.output_dir = value;
  } else if (key == "output.file") {
    config.output_base = value;
  } else if (key == "rotate.size_bytes") {
    config.rotate_size_bytes = parse_u64(key, value);
    if (config.rotate_size_bytes < kMinRotateSizeBytes) {
      throw std::runtime_error("config: 'rotate.size_bytes' must be at least " +
                               std::to_string(kMinRotateSizeBytes));
    }
  } else if (key == "rotate.backups") {
    config.rotate_backups = parse_int(key, value, 1, kMaxRotateBackups);
  } else if (key == "output_format" || key == "output.format") {
    if (!output_format_from_string(value, config.output_format)) {
      throw std::runtime_error(
          "config: '" + key + "' expects text or json_lines, got '" + value + "'");
    }
  } else if (key == "output.compress") {
    if (!compress_mode_from_string(value, config.output_compress)) {
      throw std::runtime_error(
          "config: 'output.compress' expects none or rle, got '" + value + "'");
    }
  } else if (key == "rotate.daily" || key == "rotate_daily") {
    if (!parse_bool(value, config.rotate_daily)) {
      throw std::runtime_error("config: 'rotate.daily' expects true or false, got '" +
                               value + "'");
    }
  } else if (key == "per_source_files") {
    if (!parse_bool(value, config.per_source_files)) {
      throw std::runtime_error("config: 'per_source_files' expects true or false, got '" +
                               value + "'");
    }
  } else if (key == "write.buffer_lines") {
    config.write_buffer_lines = parse_u64(key, value);
    if (config.write_buffer_lines > 100000000ull) {
      throw std::runtime_error("config: 'write.buffer_lines' must be at most 100000000");
    }
  } else if (key == "write.buffer_bytes") {
    config.write_buffer_bytes = parse_u64(key, value);
    if (config.write_buffer_bytes > (1ull << 30)) {
      throw std::runtime_error("config: 'write.buffer_bytes' must be at most 1073741824");
    }
  } else if (key == "encrypt.password") {
    // The password is stored for the writer but never echoed: the effective
    // map only ever carries the masked form.
    config.encrypt_password = value;
    record("***");
    return;
  } else if (key == "read.chunk_bytes") {
    config.read_chunk_bytes = parse_int(key, value, kMinReadChunkBytes, kMaxReadChunkBytes);
  } else if (key == "queue.capacity") {
    config.queue_capacity = parse_int(key, value, kMinQueueCapacity, kMaxQueueCapacity);
  } else if (key == "max_lines_per_sec" || key == "rate.max_lines_per_sec") {
    config.max_lines_per_sec = parse_int(key, value, 0, kMaxLinesPerSec);
  } else if (key == "extract.kv" || key == "extract_kv") {
    if (!parse_bool(value, config.extract_kv)) {
      throw std::runtime_error("config: 'extract.kv' expects true or false, got '" +
                               value + "'");
    }
  } else if (key == "stats.interval_sec") {
    config.stats_interval_sec = parse_int(key, value, 0, kMaxStatsIntervalSec);
  } else if (key == "stats.dir") {
    config.stats_dir = value;
  } else if (key == "stats.keep_files") {
    config.stats_keep_files = parse_int(key, value, kMinStatsKeepFiles, kMaxStatsKeepFiles);
  } else if (key == "filter.level") {
    config.level_threshold = parse_level_value(key, value);
  } else if (key == "filter.keyword") {
    config.keyword = value;
  } else if (key == "filter.expr") {
    // Keep the raw text; the compiled form is built once after the whole
    // file is consumed so dsl errors surface before anything else runs.
    config.filter_expr_text = value;
  } else if (key == "tail.poll_ms") {
    config.tail_poll_ms = parse_int(key, value, kMinTailPollMs, kMaxTailPollMs);
  } else if (key == "state.offset_file") {
    config.offset_file = value;
  } else if (key == "replay.since") {
    // Time-stamp replay start: strictly validated here so a malformed value
    // aborts startup instead of silently disabling the replay mode.
    int64_t since_ms = 0;
    if (!util::parse_datetime_ms(value, since_ms)) {
      throw std::runtime_error(
          "config: 'replay.since' expects \"YYYY-MM-DD HH:MM:SS\", got '" + value + "'");
    }
    config.replay_since = value;
    config.replay_since_ms = since_ms;
  } else if (key == "replay.index_interval_bytes") {
    config.replay_index_interval_bytes =
        parse_int(key, value, kMinIndexIntervalBytes, kMaxIndexIntervalBytes);
  } else if (key == "run.duration_sec") {
    config.run_duration_sec = parse_int(key, value, 0, kMaxRunDurationSec);
  } else if (key == "stop_file" || key == "run.stop_file") {
    config.stop_file = value;
  } else if (key == "diag.level") {
    if (!util::diag_level_from_string(value, config.diag_level)) {
      throw std::runtime_error("config: 'diag.level' expects debug, info, warn or error, got '" +
                               value + "'");
    }
  } else if (key == "diag.file") {
    config.diag_file = value;
  } else if (key == "active_profile") {
    // Handled during the profile phase; recorded for the report only.
  } else {
    util::log_warn("config: " + entry.origin + ": unknown key '" + key + "' ignored");
    return;
  }
  record(value);
}

// Formats the effective value of `key` from the (fully loaded) config for
// the --check-config report. Fan-out keys are handled by the caller.
std::string effective_value_of(const Config& config, const std::string& key) {
  if (key == "input.files" || key == "input.file") {
    std::string shown;
    for (size_t i = 0; i < config.input_files.size(); ++i) {
      shown += (i != 0 ? ", " : "") + config.input_files[i].string();
    }
    if (config.input_stdin) shown += (shown.empty() ? "" : ", ") + std::string("stdin:");
    return shown.empty() ? "<none>" : shown;
  }
  if (key == "output.dir") return config.output_dir.string();
  if (key == "output.file") return config.output_base;
  if (key == "rotate.size_bytes") return std::to_string(config.rotate_size_bytes);
  if (key == "rotate.backups") return std::to_string(config.rotate_backups);
  if (key == "output_format") return output_format_name(config.output_format);
  if (key == "output.compress") return compress_mode_name(config.output_compress);
  if (key == "rotate.daily" || key == "rotate_daily") return bool_text(config.rotate_daily);
  if (key == "per_source_files") return bool_text(config.per_source_files);
  if (key == "write.buffer_lines") return std::to_string(config.write_buffer_lines);
  if (key == "write.buffer_bytes") return std::to_string(config.write_buffer_bytes);
  // Secret: only the masked form ever reaches a report or a dump.
  if (key == "encrypt.password") return config.encrypt_password.empty() ? "<off>" : "***";
  if (key == "read.chunk_bytes") return std::to_string(config.read_chunk_bytes);
  if (key == "queue.capacity") return std::to_string(config.queue_capacity);
  if (key == "max_lines_per_sec") return std::to_string(config.max_lines_per_sec);
  if (key == "extract.kv" || key == "extract_kv") return bool_text(config.extract_kv);
  if (key == "stats.interval_sec") return std::to_string(config.stats_interval_sec);
  if (key == "stats.dir") return config.stats_dir.string();
  if (key == "stats.keep_files") return std::to_string(config.stats_keep_files);
  if (key == "filter.level") return level_name(config.level_threshold);
  if (key == "filter.keyword") return config.keyword.empty() ? "<none>" : config.keyword;
  if (key == "filter.expr") {
    return config.filter_expr ? config.filter_expr->text() : "<off>";
  }
  if (key == "tail.poll_ms") return std::to_string(config.tail_poll_ms);
  if (key == "state.offset_file") {
    return config.offset_file.empty() ? "<off>" : config.offset_file.string();
  }
  if (key == "replay.since") {
    return config.replay_since.empty() ? "<off>" : config.replay_since;
  }
  if (key == "replay.index_interval_bytes") {
    return std::to_string(config.replay_index_interval_bytes);
  }
  if (key == "glob.exclude") {
    if (config.glob_exclude.empty()) return "<none>";
    std::string shown;
    for (const std::string& pattern : config.glob_exclude) {
      shown += (shown.empty() ? "" : ", ") + pattern;
    }
    return shown;
  }
  if (key == "run.duration_sec") return std::to_string(config.run_duration_sec);
  if (key == "stop_file") return config.stop_file.empty() ? "<off>" : config.stop_file.string();
  if (key == "diag.level") return diag_level_name(config.diag_level);
  if (key == "diag.file") return config.diag_file.empty() ? "<stderr>" : config.diag_file;
  if (key == "active_profile") return "<none>";
  return "";
}

}  // namespace

Config Config::load(const std::string& path, const LoadOptions& options) {
  // ---- layer 1: the root file and its (recursive) includes -----------------
  std::vector<std::string> visited;
  std::vector<RawEntry> entries;
  load_file_entries(path, visited, entries);

  // ---- profiles: collect, then splice the activated group ------------------
  // profile.<name>.<key> assignments are parked per profile and only applied
  // when active_profile selects that name; inactive profiles are ignored
  // silently (no unknown-key warnings).
  std::vector<std::pair<std::string, std::vector<RawEntry>>> profiles;
  std::string active_profile;
  std::vector<RawEntry> flat;  // file entries without profile.* / active_profile
  flat.reserve(entries.size());
  for (RawEntry& entry : entries) {
    std::string name;
    std::string rest;
    if (split_profile_key(entry, name, rest)) {
      RawEntry inner;
      inner.key = to_lower(rest);
      inner.value = entry.value;
      inner.source = "profile:" + name;
      inner.origin = entry.origin;
      bool stored = false;
      for (auto& slot : profiles) {
        if (slot.first == name) {
          slot.second.push_back(std::move(inner));
          stored = true;
          break;
        }
      }
      if (!stored) profiles.emplace_back(name, std::vector<RawEntry>{std::move(inner)});
      continue;
    }
    if (entry.key == "active_profile") {
      // Last definition inside the files wins; may still be overridden by
      // the environment below.
      active_profile = entry.value;
      RawEntry marker;
      marker.key = "active_profile";
      marker.value = entry.value;
      marker.source = "file";
      marker.origin = entry.origin;
      flat.push_back(std::move(marker));
      continue;
    }
    flat.push_back(std::move(entry));
  }

  // ---- environment overrides: build the entries before splicing profiles
  // so that LOGPIPE_ACTIVE_PROFILE can still select (or switch) the profile.
  std::vector<RawEntry> env_entries;
  if (options.use_env) {
    std::vector<std::string> seen_env_names;
    auto consider = [&](const std::string& key) {
      const std::string env_name = env_name_for_key(key);
      bool already = false;
      for (const std::string& seen : seen_env_names) {
        if (seen == env_name) {
          already = true;
          break;
        }
      }
      if (already) return;
      seen_env_names.push_back(env_name);
      std::string value;
      if (!read_env(env_name, value)) return;
      RawEntry entry;
      entry.key = key;
      entry.value = value;
      entry.source = "env";
      entry.origin = "environment variable " + env_name;
      env_entries.push_back(std::move(entry));
    };
    for (const std::string& key : known_keys()) consider(key);
    // Fan-out keys: output.<N>.<attr> for a bounded index range keeps the
    // env lookup deterministic (no environ iteration needed).
    for (int n = 1; n <= 64; ++n) {
      for (const std::string& attr : fanout_attrs()) {
        consider("output." + std::to_string(n) + "." + attr);
      }
      consider("source." + std::to_string(n) + ".tags");
    }
  }
  for (const RawEntry& entry : env_entries) {
    if (entry.key == "active_profile") active_profile = entry.value;
  }

  // Splice: file entries, then the activated profile (higher priority), then
  // the environment (highest priority).
  std::vector<RawEntry> ordered = std::move(flat);
  if (!active_profile.empty()) {
    bool found = false;
    for (const auto& slot : profiles) {
      if (slot.first == active_profile) {
        for (const RawEntry& inner : slot.second) ordered.push_back(inner);
        found = true;
        break;
      }
    }
    if (!found) {
      throw std::runtime_error("config: active_profile '" + active_profile +
                               "' is not defined by any profile.<name>.* keys");
    }
  }
  for (const RawEntry& entry : env_entries) {
    if (entry.key == "active_profile") continue;  // report-only, already handled
    ordered.push_back(entry);
  }

  // ---- apply ----------------------------------------------------------------
  Config config;
  bool inputs_reset = false;

  // Pass 1: custom level registrations, so any later entry (thresholds, DSL
  // literals, parser) can reference the registered names.
  for (const RawEntry& entry : ordered) {
    if (entry.key.rfind("level.register.", 0) != 0) continue;
    const std::string name = entry.key.substr(15);
    if (name.empty()) {
      throw std::runtime_error("config: " + entry.origin +
                               ": 'level.register.<NAME>' needs a level name");
    }
    const int value = parse_int(entry.key, entry.value,
                                LevelRegistry::kMinCustomValue, LevelRegistry::kMaxCustomValue);
    std::string error;
    if (!LevelRegistry::instance().register_level(name, value, error)) {
      throw std::runtime_error("config: " + entry.origin + ": cannot register custom level '" +
                               name + "': " + error);
    }
    config.effective[entry.key] = ConfigEntry{entry.value, entry.source};
    util::log_info("config: " + entry.origin + ": registered custom level '" +
                   to_upper(name) + "' = " + std::to_string(value));
  }

  // Pass 2: everything else, in order, so later layers overwrite earlier ones.
  for (const RawEntry& entry : ordered) {
    if (entry.key.rfind("level.register.", 0) == 0) continue;
    if (entry.source == "env") {
      // The password is a secret: its value is masked even in the override log.
      const bool secret = entry.key == "encrypt.password";
      util::log_info("config: " + entry.origin + " overrides '" + entry.key + "' = '" +
                     (secret ? std::string("***") : entry.value) + "'");
    }
    apply_entry(config, entry, inputs_reset);
  }

  // ---- validation and finalization ------------------------------------------
  // Resolve the source tags onto the input entries: source.<n>.tags was
  // parsed independently of input.files, so the attachment happens once both
  // sides are known (order of the two keys in the file does not matter).
  {
    size_t next_glob = 0;  // the i-th "glob:" spec created the i-th glob slot
    for (size_t i = 0; i < config.input_entry_specs.size(); ++i) {
      const int index = static_cast<int>(i) + 1;
      const auto tags_it = config.source_tags.find(index);
      SourceTags tags;
      if (tags_it != config.source_tags.end()) tags = tags_it->second;
      const std::string& spec = config.input_entry_specs[i];
      if (spec == "stdin") {
        config.stdin_tags = tags;
      } else if (spec.rfind("glob:", 0) == 0) {
        if (next_glob < config.glob_sources.size()) {
          config.glob_sources[next_glob].tags = tags;
          ++next_glob;
        }
      } else if (spec.rfind("file:", 0) == 0) {
        const std::filesystem::path path(spec.substr(5));
        config.file_tags[normalize_file_key(path)] = tags;
      }
    }
  }
  if (config.input_files.empty() && !config.input_stdin && config.glob_sources.empty()) {
    throw std::runtime_error("config: no input sources given (set 'input.files = a.log, b.log'"
                             " or add 'stdin:' to input.files)");
  }
  if (config.output_base.empty()) {
    throw std::runtime_error("config: 'output.file' must not be empty");
  }
  // Compile filter.expr now: dsl::Error carries the position in the
  // expression and derives from runtime_error, so it propagates verbatim and
  // a bad expression aborts the run before any thread starts.
  if (!config.filter_expr_text.empty()) {
    config.filter_expr = dsl::FilterExpr::compile(config.filter_expr_text);
  }

  // Fan-out finalization: a config without output.<N>.* keys maps its legacy
  // single-output settings onto exactly one default output so consumers can
  // always iterate Config::outputs with identical semantics.
  if (!config.outputs_explicit) {
    OutputConfig legacy;
    legacy.index = 1;
    legacy.name = "default";
    legacy.file = config.output_base;
    legacy.format = config.output_format;
    legacy.compress = config.output_compress;
    legacy.level = config.level_threshold;
    legacy.filter_expr_text = config.filter_expr_text;
    legacy.filter_expr = config.filter_expr;
    config.outputs.push_back(std::move(legacy));
  } else {
    // Declared group: sort by index, fill defaults, check name uniqueness.
    std::sort(config.outputs.begin(), config.outputs.end(),
              [](const OutputConfig& a, const OutputConfig& b) {
                return a.index < b.index;
              });
    std::vector<std::string> seen;
    for (OutputConfig& entry : config.outputs) {
      if (entry.name.empty()) entry.name = "out" + std::to_string(entry.index);
      if (entry.file.empty()) entry.file = config.output_base;
      for (const std::string& taken : seen) {
        if (taken == entry.name) {
          throw std::runtime_error("config: duplicate output name '" +
                                   entry.name + "'");
        }
      }
      seen.push_back(entry.name);
    }
  }
  for (OutputConfig& entry : config.outputs) {
    if (entry.file.empty()) {
      throw std::runtime_error("config: output '" + entry.name +
                               "' has an empty file name");
    }
    if (!entry.filter_expr_text.empty()) {
      entry.filter_expr = dsl::FilterExpr::compile(entry.filter_expr_text);
    }
    try {
      entry.transform = parse_transform_chain(entry.transform_text);
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("config: output '" + entry.name +
                                           "': ") + error.what());
    }
  }
  return config;
}

std::string Config::describe() const {
  std::ostringstream out;
  out << "inputs=[";
  for (size_t i = 0; i < input_files.size(); ++i) {
    out << (i != 0 ? ", " : "") << input_files[i].string();
  }
  if (input_stdin) {
    out << (input_files.empty() && glob_sources.empty() ? "" : ", ") << "<stdin>";
  }
  for (size_t g = 0; g < glob_sources.size(); ++g) {
    out << (input_files.empty() && !input_stdin && g == 0 ? "" : ", ")
        << "glob:" << glob_sources[g].pattern;
  }
  out << "] output_dir=" << output_dir.string() << " output_file=" << output_base
      << " format=" << output_format_name(output_format)
      << " compress=" << compress_mode_name(output_compress)
      << " daily_rotation=" << (rotate_daily ? "on" : "off")
      << " per_source_files=" << (per_source_files ? "on" : "off")
      << " rotate=" << rotate_size_bytes << "B x" << rotate_backups
      << " level>=" << level_name(level_threshold)
      << " keyword=" << (keyword.empty() ? "<none>" : keyword)
      << " filter.expr=" << (filter_expr ? filter_expr->text() : "<off>")
      << " max_lines_per_sec=" << max_lines_per_sec
      << " extract_kv=" << (extract_kv ? "on" : "off")
      << " stats=" << (stats_interval_sec > 0
                           ? (std::to_string(stats_interval_sec) + "s into " +
                              stats_dir.string() + " keep " +
                              std::to_string(stats_keep_files))
                           : std::string("off"))
      << " poll_ms=" << tail_poll_ms << " offsets="
      << (offset_file.empty() ? "<off>" : offset_file.string())
      << " replay_since=" << (replay_since.empty() ? "<off>" : replay_since)
      << " index_interval_bytes=" << replay_index_interval_bytes
      << " write_buffer=[lines=" << write_buffer_lines
      << " bytes=" << write_buffer_bytes << "]"
      << " read_chunk_bytes=" << read_chunk_bytes
      << " queue_capacity=" << queue_capacity
      << " encrypt_password=" << (encrypt_password.empty() ? "<off>" : "***")
      << " stop_file=" << (stop_file.empty() ? "<off>" : stop_file.string())
      << " duration_sec=" << run_duration_sec;
  // Fan-out group dump: one bracketed entry per configured output.
  out << " outputs=[";
  for (size_t i = 0; i < outputs.size(); ++i) {
    const OutputConfig& entry = outputs[i];
    if (i != 0) out << ", ";
    out << entry.name << "(" << entry.file
        << " fmt=" << output_format_name(entry.format)
        << " compress=" << compress_mode_name(entry.compress)
        << " level>=" << level_name(entry.level)
        << " expr=" << (entry.filter_expr ? entry.filter_expr->text() : "<off>")
        << " transform=" << (entry.transform.empty()
                                 ? "<none>"
                                 : (entry.transform_text.empty()
                                        ? "<chain>"
                                        : entry.transform_text))
        << " pos=" << transform_position_name(entry.transform_position)
        << ")";
  }
  out << "]";
  return out.str();
}

int dump_config(std::FILE* out, const Config& config) {
  if (out == nullptr) return 1;
  const std::string text = config.describe();
  std::fwrite(text.data(), 1, text.size(), out);
  std::fputc('\n', out);
  return std::fflush(out) == 0 ? 0 : 1;
}

int check_config_report(std::FILE* out, const std::string& path, const Config& config) {
  if (out == nullptr) return 1;
  const int key_width = 28;
  const int value_width = 34;
  std::fprintf(out, "logpipe configuration report: %s\n", path.c_str());
  std::fprintf(out, "%-*s %-*s %s\n", key_width, "KEY", value_width, "VALUE", "SOURCE");
  std::fprintf(out, "%-*s %-*s %s\n", key_width, std::string(key_width, '-').c_str(),
               value_width, std::string(value_width, '-').c_str(),
               std::string(12, '-').c_str());

  auto row = [&](const std::string& key, const std::string& value) {
    const auto it = config.effective.find(key);
    const std::string source = it != config.effective.end() ? it->second.source : "default";
    std::fprintf(out, "%-*s %-*s %s\n", key_width, key.c_str(), value_width, value.c_str(),
                 source.c_str());
  };

  // Report each spelling family once (the dotted canonical form).
  std::vector<std::string> reported;
  auto reported_already = [&](const std::string& key) {
    for (const std::string& seen : reported) {
      if (seen == key) return true;
    }
    return false;
  };
  for (const std::string& key : known_keys()) {
    const std::string canonical =
        (key == "input.file" ? "input.files"
         : key == "rotate_daily" ? "rotate.daily"
         : key == "extract_kv" ? "extract.kv"
         : key);
    if (reported_already(canonical)) continue;
    reported.push_back(canonical);
    row(canonical, effective_value_of(config, key));
  }
  if (config.outputs_explicit) {
    for (const OutputConfig& slot : config.outputs) {
      const std::string prefix = "output." + std::to_string(slot.index) + ".";
      row(prefix + "name", slot.name);
      row(prefix + "file", slot.file);
      row(prefix + "format", output_format_name(slot.format));
      row(prefix + "compress", compress_mode_name(slot.compress));
      row(prefix + "level", level_name(slot.level));
      row(prefix + "filter.expr",
          slot.filter_expr ? slot.filter_expr->text() : "<off>");
      row(prefix + "transform",
          slot.transform_text.empty() ? "<none>" : slot.transform_text);
      row(prefix + "transform.position",
          transform_position_name(slot.transform_position));
    }
  }
  // Custom level registrations appear at the end of the report.
  for (const auto& item : config.effective) {
    if (item.first.rfind("level.register.", 0) == 0) {
      row(item.first, item.second.value);
    }
  }
  // Per-source tag assignments too (index-keyed, so not in known_keys()).
  for (const auto& item : config.effective) {
    if (item.first.rfind("source.", 0) == 0 &&
        item.first.find(".tags") != std::string::npos) {
      row(item.first, item.second.value);
    }
  }
  return std::fflush(out) == 0 ? 0 : 1;
}

}  // namespace logpipe
