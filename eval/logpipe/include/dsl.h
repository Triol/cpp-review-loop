// dsl.h - filter expression DSL for logpipe.
//
// A small hand-rolled expression language evaluated against a LogRecord.
// Grammar (precedence NOT > AND > OR, parentheses group):
//
//   expr        := or_expr
//   or_expr     := and_expr ( "OR" and_expr )*
//   and_expr    := unary   ( "AND" unary )*
//   unary       := "NOT" unary | primary
//   primary     := "(" expr ")" | comparison
//   comparison  := field op literal | "kv" "(" key ")" op literal
//   field       := "level" | "msg" | "src"
//   key         := double-quoted string naming an extracted KV field
//   op          := "=" | "!=" | ">=" | ">" | "<" | "<=" |
//                  "CONTAINS" | "STARTS_WITH" | "ENDS_WITH" | "MATCHES"
//   literal     := double-quoted string (backslash escapes: \" \\ \n \r \t;
//                  any other "\c" is passed through, so regex escapes like
//                  \b \d \. work inside MATCHES patterns)
//
// Semantics:
//   - "level" is compared by severity ordinal (DEBUG < INFO < WARN < ERROR);
//     ordering operators (>=, >, <, <=) only make sense there but are also
//     accepted for msg/src as lexicographic comparisons.
//   - "kv(key)" compares against the key=value fields extracted by the KV
//     extractor (extract.kv = true, see kv_extractor.h) and attached to the
//     LogRecord. When the record has no such key the comparison is false for
//     every operator, including "!=" (a missing field matches nothing).
//   - "=", "!=", CONTAINS, STARTS_WITH, ENDS_WITH are case-insensitive
//     substring/prefix/suffix/equality tests on msg and src; MATCHES applies
//     a std::regex (ECMAScript) search against the field value.
//   - AND / OR / NOT keywords are case-insensitive.
//
// Syntax errors, unknown fields and invalid regexes are reported at compile
// time as a dsl::Error carrying the 1-based character position in the
// expression text, so Config::load can reject bad filter.expr up front.

#pragma once

#include <memory>
#include <regex>
#include <stdexcept>
#include <string>

#include "pipeline.h"  // LogRecord, Level

namespace logpipe {
namespace dsl {

// Compile-time DSL failure. pos is the 1-based character position in the
// expression text where the problem was detected.
struct Error : std::runtime_error {
  Error(const std::string& message, int position)
      : std::runtime_error(message), pos(position) {}
  int pos;
};

// A compiled expression. Immutable and thread-safe once constructed (the
// std::regex member is only read during evaluation), so a single compiled
// instance can be shared by the filter stage across threads.
class FilterExpr {
 public:
  // Parses `text`; throws dsl::Error (with position) on any problem:
  // empty input, bad tokens, unterminated strings, unknown fields,
  // malformed MATCHES regexes or trailing garbage.
  static std::shared_ptr<const FilterExpr> compile(const std::string& text);

  // Evaluates the expression against `rec`.
  bool passes(const LogRecord& rec) const;

  // The expression text this instance was compiled from.
  const std::string& text() const { return text_; }

 private:
  FilterExpr(std::string text, std::unique_ptr<struct ExprNode> root);
  const std::string text_;
  std::unique_ptr<struct ExprNode> root_;
};

}  // namespace dsl
}  // namespace logpipe
