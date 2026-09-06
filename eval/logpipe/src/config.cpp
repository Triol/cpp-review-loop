// config.cpp - key=value config file parsing and validation. Unknown keys
// are reported through util diagnostics; hard errors throw runtime_error.

#include "config.h"

#include <cctype>
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

}  // namespace

Config Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("config: cannot open '" + path + "'");
  }

  Config config;
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
    const std::string key = to_lower(trim(line.substr(0, eq)));
    const std::string value = trim(line.substr(eq + 1));

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
      for (OutputConfig& entry : config.outputs) {
        if (entry.index == fanout_index) return entry;
      }
      config.outputs.emplace_back();
      config.outputs.back().index = fanout_index;
      return config.outputs.back();
    };

    if (fanout_index > 0) {
      OutputConfig& entry = output_slot();
      if (fanout_attr == "name") {
        entry.name = value;
      } else if (fanout_attr == "file") {
        entry.file = value;
      } else if (fanout_attr == "format") {
        if (!output_format_from_string(value, entry.format)) {
          throw std::runtime_error(
              "config: 'output." + std::to_string(fanout_index) +
              ".format' expects text or json_lines, got '" + value + "'");
        }
      } else if (fanout_attr == "compress") {
        if (!compress_mode_from_string(value, entry.compress)) {
          throw std::runtime_error(
              "config: 'output." + std::to_string(fanout_index) +
              ".compress' expects none or rle, got '" + value + "'");
        }
      } else if (fanout_attr == "level") {
        if (!level_from_string(value, entry.level) || entry.level == Level::Raw) {
          throw std::runtime_error(
              "config: 'output." + std::to_string(fanout_index) +
              ".level' expects DEBUG, INFO, WARN or ERROR, got '" + value + "'");
        }
      } else if (fanout_attr == "filter.expr") {
        entry.filter_expr_text = value;
      } else if (fanout_attr == "transform") {
        entry.transform_text = value;
      } else if (fanout_attr == "transform.position") {
        if (!transform_position_from_string(value, entry.transform_position)) {
          throw std::runtime_error(
              "config: 'output." + std::to_string(fanout_index) +
              ".transform.position' expects before or after, got '" + value + "'");
        }
      } else {
        util::log_warn("config: " + path + ":" + std::to_string(line_no) +
                       ": unknown key '" + key + "' ignored");
      }
    } else if (key == "input.files" || key == "input.file") {
      for (const auto& item : split_list(value)) {
        // "stdin:" selects standard input as an extra source; anything else
        // is treated as a file path.
        if (to_lower(item) == "stdin:") {
          config.input_stdin = true;
        } else {
          config.input_files.emplace_back(item);
        }
      }
    } else if (key == "output.dir") {
      config.output_dir = value;
    } else if (key == "output.file") {
      config.output_base = value;
    } else if (key == "rotate.size_bytes") {
      config.rotate_size_bytes = parse_u64(key, value);
      if (config.rotate_size_bytes < 256) {
        throw std::runtime_error("config: 'rotate.size_bytes' must be at least 256");
      }
    } else if (key == "rotate.backups") {
      config.rotate_backups = parse_int(key, value, 1, 999);
    } else if (key == "output_format") {
      if (!output_format_from_string(value, config.output_format)) {
        throw std::runtime_error(
            "config: 'output_format' expects text or json_lines, got '" + value + "'");
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
    } else if (key == "max_lines_per_sec") {
      config.max_lines_per_sec = parse_int(key, value, 0, 100000000);
    } else if (key == "extract.kv" || key == "extract_kv") {
      if (!parse_bool(value, config.extract_kv)) {
        throw std::runtime_error("config: 'extract.kv' expects true or false, got '" +
                                 value + "'");
      }
    } else if (key == "stats.interval_sec") {
      config.stats_interval_sec = parse_int(key, value, 0, 86400);
    } else if (key == "stats.dir") {
      config.stats_dir = value;
    } else if (key == "stats.keep_files") {
      config.stats_keep_files = parse_int(key, value, 1, 10000);
    } else if (key == "filter.level") {
      if (!level_from_string(value, config.level_threshold) ||
          config.level_threshold == Level::Raw) {
        throw std::runtime_error("config: 'filter.level' expects DEBUG, INFO, WARN or ERROR, got '" +
                                 value + "'");
      }
    } else if (key == "filter.keyword") {
      config.keyword = value;
    } else if (key == "filter.expr") {
      // Keep the raw text; the compiled form is built once after the whole
      // file is consumed so dsl errors surface before anything else runs.
      config.filter_expr_text = value;
    } else if (key == "tail.poll_ms") {
      config.tail_poll_ms = parse_int(key, value, 20, 60000);
    } else if (key == "state.offset_file") {
      config.offset_file = value;
    } else if (key == "run.duration_sec") {
      config.run_duration_sec = parse_int(key, value, 0, 7 * 24 * 3600);
    } else if (key == "stop_file") {
      config.stop_file = value;
    } else if (key == "diag.level") {
      if (!util::diag_level_from_string(value, config.diag_level)) {
        throw std::runtime_error("config: 'diag.level' expects debug, info, warn or error, got '" +
                                 value + "'");
      }
    } else if (key == "diag.file") {
      config.diag_file = value;
    } else {
      util::log_warn("config: " + path + ":" + std::to_string(line_no) +
                     ": unknown key '" + key + "' ignored");
    }
  }

  if (config.input_files.empty() && !config.input_stdin) {
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

  // Fan-out finalization (requirement 3): a config without output.<N>.* keys
  // maps its legacy single-output settings onto exactly one default output so
  // consumers can always iterate Config::outputs with identical semantics.
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
    out << (input_files.empty() ? "" : ", ") << "<stdin>";
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

}  // namespace logpipe
