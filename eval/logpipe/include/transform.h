// transform.h - ordered record transformation chains for the fan-out outputs.
//
// A transform chain is configured per output with the `transform` key as a
// '|' separated list of steps applied in order, e.g.
//   transform = trim|uppercase|replace:foo->bar|tag:host=web-01
// Supported step kinds:
//   uppercase          - convert the message to ASCII upper case
//   trim               - strip leading/trailing whitespace from the message
//   replace:pat->repl  - replace every literal occurrence of pat with repl
//   tag:name=value     - attach the key/value pair to the record fields
//                        (visible to the DSL kv("name") syntax); the message
//                        itself is left untouched
//
// transform.position = before|after selects whether the chain runs before the
// output's filter judgement (the filter sees the transformed record) or after
// it (only accepted records are transformed before writing). Default: after.

#pragma once

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

#include "pipeline.h"  // LogRecord

namespace logpipe {

// Where the chain runs relative to the output's filter stage.
enum class TransformPosition { Before, After };

// Accepts "before" and "after" (case-insensitive). Returns false otherwise.
inline bool transform_position_from_string(const std::string& text,
                                           TransformPosition& out) {
  std::string lower;
  lower.reserve(text.size());
  for (char c : text) {
    lower.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "before") { out = TransformPosition::Before; return true; }
  if (lower == "after")  { out = TransformPosition::After;  return true; }
  return false;
}

inline const char* transform_position_name(TransformPosition position) {
  return position == TransformPosition::Before ? "before" : "after";
}

// One parsed transformation step. `kind` is the canonical name (uppercase,
// trim, replace, tag); pattern_/replacement_ carry the replace/tag arguments.
struct TransformStep {
  enum class Kind { Uppercase, Trim, Replace, Tag };
  Kind kind;
  std::string pattern_;      // replace: search text; tag: field name
  std::string replacement_;  // replace: replacement; tag: field value
};

// Parses a '|' separated chain specification. Throws std::runtime_error with
// a descriptive message on an empty step, an unknown kind or malformed
// replace/tag arguments. An empty (or whitespace-only) input yields an empty
// chain (no transformation).
std::vector<TransformStep> parse_transform_chain(const std::string& spec);

// True when the chain contains at least one step.
bool transforms_active(const std::vector<TransformStep>& chain);

// Applies the chain steps in order to a copy of `rec` and returns it.
// Uppercase/trim/replace rewrite rec.message; tag writes rec.fields.
LogRecord apply_transforms(const std::vector<TransformStep>& chain,
                           const LogRecord& rec);

}  // namespace logpipe
