// kv_extractor.h - key=value field extraction for logpipe.
//
// Enabled via the `extract.kv = true` configuration key. When active, every
// ingested line is additionally offered to the extractor: lines that look like
// a whitespace-separated sequence of "key=value" tokens contribute their pairs
// as structured fields attached to the LogRecord. Extracted fields:
//   - are visible to the filter DSL through the kv("key") = "value" comparison
//     syntax (see dsl.h);
//   - feed the Metrics field-value counters (Top-N counting, exported in the
//     Prometheus statistics);
//   - drive the KV extraction-rate diagnostic reported at shutdown.
//
// Grammar accepted per line (EBSG-ish):
//   line     := ws* ( pair ws* )*
//   pair     := key '=' value
//   key      := 1*key_char                  (key_char: anything but ws and '=')
//   value    := bare | quoted
//   bare     := 1*non_ws_char               (runs to the next whitespace)
//   quoted   := '"' *( any | '\"' | '\\' ) '"' | "'" *( any | "\'" | "\\" ) "'"
//
// Notes:
//   - Values in single quotes are also accepted and may contain double quotes
//     verbatim (and vice versa).
//   - Inside quoted values the escapes \" \' \\ are decoded; any other "\c"
//     sequence is passed through unchanged.
//   - A token without '=' (before any pair was read) makes the line malformed:
//     extraction stops there and the pairs collected so far are kept only if
//     at least one was already extracted.
//   - Duplicate keys: the last occurrence wins (std::map assignment).
//   - Empty values are valid ("k=" yields an empty string value).

#pragma once

#include <map>
#include <string>
#include <utility>

namespace logpipe {

// Stateless extractor: a single instance can be shared by any thread because
// extract() only reads its input and writes into the caller's map.
class KvExtractor {
 public:
  KvExtractor() = default;

  // Attempts to extract "k=v" pairs from `line`. On success fills `out` with
  // the extracted pairs (overwriting the previous content) and returns true
  // when at least one pair was extracted. Returns false for lines that do not
  // have the key=value shape at all (e.g. plain prose without any '=').
  bool extract(const std::string& line, std::map<std::string, std::string>& out) const;

  // Convenience overload returning the extracted fields; see above.
  std::map<std::string, std::string> extract(const std::string& line) const;

  // Tolerant variant for message parts that carry a free-form prefix before
  // the key=value payload ("evt user=bob host=web-01"): leading tokens that
  // are not "key=value" pairs are skipped until the first pair (or quoted
  // value) is found, and extraction proceeds from there. Returns false when
  // no "key=value" pair exists anywhere in the line.
  bool extract_tail(const std::string& line, std::map<std::string, std::string>& out) const;

  // Convenience overload returning the extracted fields; see above.
  std::map<std::string, std::string> extract_tail(const std::string& line) const;
};

}  // namespace logpipe
