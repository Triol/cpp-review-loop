// transform.cpp - parsing and application of the ordered transform chains
// attached to the fan-out outputs (see transform.h).

#include "transform.h"

#include <cctype>

namespace logpipe {
namespace {

std::string trim_ws(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

std::string to_upper_ascii(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

// Replaces every literal (non-regex) occurrence of `pattern` with `repl`.
std::string replace_all_literal(const std::string& text,
                                const std::string& pattern,
                                const std::string& repl) {
  if (pattern.empty()) return text;  // nothing to search for
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  for (;;) {
    const size_t hit = text.find(pattern, pos);
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, hit - pos);
    out += repl;
    pos = hit + pattern.size();
  }
  return out;
}

}  // namespace

std::vector<TransformStep> parse_transform_chain(const std::string& spec) {
  std::vector<TransformStep> chain;
  if (trim_ws(spec).empty()) return chain;  // no transform configured

  // Manual split so a leading or trailing '|' yields an empty step (rejected
  // below) instead of silently vanishing the way std::getline would.
  size_t pos = 0;
  for (;;) {
    const size_t sep = spec.find('|', pos);
    const std::string token =
        sep == std::string::npos ? spec.substr(pos) : spec.substr(pos, sep - pos);
    const std::string step = trim_ws(token);
    if (step.empty()) {
      throw std::runtime_error("transform: empty step in chain '" + spec + "'");
    }
    // Kind is everything before the first ':' (a replace/tag body may itself
    // contain further colons; they belong to the arguments).
    const auto colon = step.find(':');
    const std::string kind_raw =
        colon == std::string::npos ? step : step.substr(0, colon);
    std::string kind;
    kind.reserve(kind_raw.size());
    for (char c : kind_raw) {
      kind.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(c))));
    }

    TransformStep parsed;
    if (kind == "uppercase") {
      if (colon != std::string::npos) {
        throw std::runtime_error("transform: 'uppercase' takes no arguments: '" +
                                 step + "'");
      }
      parsed.kind = TransformStep::Kind::Uppercase;
    } else if (kind == "trim") {
      if (colon != std::string::npos) {
        throw std::runtime_error("transform: 'trim' takes no arguments: '" +
                                 step + "'");
      }
      parsed.kind = TransformStep::Kind::Trim;
    } else if (kind == "replace") {
      if (colon == std::string::npos) {
        throw std::runtime_error(
            "transform: 'replace' needs 'replace:pat->repl': '" + step + "'");
      }
      const std::string body = step.substr(colon + 1);
      const auto arrow = body.find("->");
      if (arrow == std::string::npos) {
        throw std::runtime_error(
            "transform: 'replace' needs 'replace:pat->repl': '" + step + "'");
      }
      parsed.kind = TransformStep::Kind::Replace;
      parsed.pattern_ = body.substr(0, arrow);
      parsed.replacement_ = body.substr(arrow + 2);
      if (parsed.pattern_.empty()) {
        throw std::runtime_error(
            "transform: 'replace' pattern must not be empty: '" + step + "'");
      }
    } else if (kind == "tag") {
      if (colon == std::string::npos) {
        throw std::runtime_error(
            "transform: 'tag' needs 'tag:name=value': '" + step + "'");
      }
      const std::string body = step.substr(colon + 1);
      const auto eq = body.find('=');
      if (eq == std::string::npos || eq == 0) {
        throw std::runtime_error(
            "transform: 'tag' needs 'tag:name=value': '" + step + "'");
      }
      parsed.kind = TransformStep::Kind::Tag;
      parsed.pattern_ = body.substr(0, eq);
      parsed.replacement_ = body.substr(eq + 1);
    } else {
      throw std::runtime_error("transform: unknown step kind '" + kind_raw +
                               "' in '" + step + "'");
    }
    chain.push_back(std::move(parsed));
    if (sep == std::string::npos) break;
    pos = sep + 1;
  }
  return chain;
}

bool transforms_active(const std::vector<TransformStep>& chain) {
  return !chain.empty();
}

LogRecord apply_transforms(const std::vector<TransformStep>& chain,
                           const LogRecord& rec) {
  LogRecord out = rec;  // fields/message copied; steps below mutate as needed
  for (const TransformStep& step : chain) {
    switch (step.kind) {
      case TransformStep::Kind::Uppercase:
        out.message = to_upper_ascii(out.message);
        break;
      case TransformStep::Kind::Trim:
        out.message = trim_ws(out.message);
        break;
      case TransformStep::Kind::Replace:
        out.message = replace_all_literal(out.message, step.pattern_,
                                          step.replacement_);
        break;
      case TransformStep::Kind::Tag:
        out.fields[step.pattern_] = step.replacement_;
        break;
    }
  }
  return out;
}

}  // namespace logpipe
