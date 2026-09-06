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

    if (key == "input.files" || key == "input.file") {
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
      << " poll_ms=" << tail_poll_ms << " offsets="
      << (offset_file.empty() ? "<off>" : offset_file.string())
      << " stop_file=" << (stop_file.empty() ? "<off>" : stop_file.string())
      << " duration_sec=" << run_duration_sec;
  return out.str();
}

}  // namespace logpipe
