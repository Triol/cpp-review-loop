// test_dsl.cpp - unit tests for the filter expression DSL (lexer, parser,
// evaluator and error positions). See dsl.h for the grammar under test.
//
// Every test works on hand-built LogRecord values, so no I/O is involved.

#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

#include "dsl.h"
#include "pipeline.h"
#include "test_framework.h"

using logpipe::Level;
using logpipe::LogRecord;
using logpipe::dsl::Error;
using logpipe::dsl::FilterExpr;

namespace {

// Builds a parsed INFO record; individual fields are overridden by callers.
LogRecord rec(const std::string& source, const std::string& message,
              Level level = Level::Info) {
  LogRecord r;
  r.source = source;
  r.ingested_ms = 1757160000000;
  r.timestamp_text = "2026-09-06 12:00:00";
  r.level = level;
  r.message = message;
  r.parsed = level != Level::Raw;
  return r;
}

// Compiles `text` and expects a dsl::Error whose position is exactly
// `position`; counts a failure (with the mismatch details) otherwise.
void expect_error_at(const std::string& text, int position,
                     const char* fragment = "") {
  try {
    FilterExpr::compile(text);
    ++testfw::failures();
    std::fprintf(stderr,
                 "  expect_error_at(%s): compile succeeded, expected "
                 "failure at position %d\n",
                 text.c_str(), position);
  } catch (const Error& e) {
    if (e.pos != position) {
      ++testfw::failures();
      std::fprintf(stderr,
                   "  expect_error_at(%s): error position %d, expected %d "
                   "(%s)\n",
                   text.c_str(), e.pos, position, e.what());
    }
    if (fragment && *fragment &&
        std::string(e.what()).find(fragment) == std::string::npos) {
      ++testfw::failures();
      std::fprintf(stderr,
                   "  expect_error_at(%s): message \"%s\" misses \"%s\"\n",
                   text.c_str(), e.what(), fragment);
    }
  } catch (const std::exception& e) {
    ++testfw::failures();
    std::fprintf(stderr,
                 "  expect_error_at(%s): wrong exception type: %s\n",
                 text.c_str(), e.what());
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Evaluator basics: equality is case-insensitive on msg/src and exact on the
// severity ordinal for level.
// ---------------------------------------------------------------------------
TEST(dsl_evaluator_equality_and_case) {
  const auto expr = FilterExpr::compile(R"(msg = "hello world")");
  CHECK(expr->passes(rec("a.log", "hello world")));
  CHECK(expr->passes(rec("a.log", "HELLO WORLD")));   // case-insensitive
  CHECK(!expr->passes(rec("a.log", "hello worl")));
  CHECK(!expr->passes(rec("a.log", "xhello world")));

  const auto ne = FilterExpr::compile(R"(src != "b.log")");
  CHECK(ne->passes(rec("a.log", "x")));
  CHECK(!ne->passes(rec("B.LOG", "x")));  // != is the negation of ci-equality
}

// ---------------------------------------------------------------------------
// Level comparisons use the severity ordinal (DEBUG < INFO < WARN < ERROR),
// and level names are accepted case-insensitively including aliases.
// ---------------------------------------------------------------------------
TEST(dsl_evaluator_level_ordering) {
  const auto warn_up = FilterExpr::compile(R"(level >= "WARN")");
  CHECK(!warn_up->passes(rec("a.log", "m", Level::Debug)));
  CHECK(!warn_up->passes(rec("a.log", "m", Level::Info)));
  CHECK(warn_up->passes(rec("a.log", "m", Level::Warn)));
  CHECK(warn_up->passes(rec("a.log", "m", Level::Error)));

  const auto alias = FilterExpr::compile(R"(level >= "warning")");  // alias
  CHECK(alias->passes(rec("a.log", "m", Level::Warn)));

  const auto below = FilterExpr::compile(R"(level < "ERROR")");
  CHECK(below->passes(rec("a.log", "m", Level::Warn)));
  CHECK(!below->passes(rec("a.log", "m", Level::Error)));

  const auto eq = FilterExpr::compile(R"(level = "error")");
  CHECK(eq->passes(rec("a.log", "m", Level::Error)));
  CHECK(!eq->passes(rec("a.log", "m", Level::Warn)));
}

// ---------------------------------------------------------------------------
// String operators: CONTAINS / STARTS_WITH / ENDS_WITH, all case-insensitive.
// ---------------------------------------------------------------------------
TEST(dsl_evaluator_string_operators) {
  const auto contains = FilterExpr::compile(R"(msg CONTAINS "TimeOut")");
  CHECK(contains->passes(rec("a.log", "connection TIMEOUT after 3s")));
  CHECK(!contains->passes(rec("a.log", "connection reset")));

  const auto starts = FilterExpr::compile(R"(msg STARTS_WITH "connect")");
  CHECK(starts->passes(rec("a.log", "Connection reset by peer")));
  CHECK(!starts->passes(rec("a.log", "reconnected")));

  const auto ends = FilterExpr::compile(R"(src ENDS_WITH ".log")");
  CHECK(ends->passes(rec("app.LOG", "m")));
  CHECK(!ends->passes(rec("app.log.1", "m")));
}

// ---------------------------------------------------------------------------
// MATCHES applies a std::regex (ECMAScript) search against the field value;
// an invalid pattern is a compile-time error with the literal's position.
// ---------------------------------------------------------------------------
TEST(dsl_evaluator_matches_regex) {
  const auto m = FilterExpr::compile(R"(msg MATCHES "\bdisk\b")");
  CHECK(m->passes(rec("a.log", "warning: disk full")));
  CHECK(!m->passes(rec("a.log", "DISK I/O error")));  // regex is case-sensitive
  CHECK(!m->passes(rec("a.log", "harddisk full")));  // \b blocks the prefix

  const auto capture = FilterExpr::compile(R"(src MATCHES "logs/[a-z]+\.log$")");
  CHECK(capture->passes(rec("logs/app.log", "m")));
  CHECK(!capture->passes(rec("logs/app.log.1", "m")));

  // Invalid regex must fail at compile time, not at evaluation time.
  expect_error_at("msg MATCHES \"([unclosed]\"", 26, "MATCHES");
}

// ---------------------------------------------------------------------------
// Boolean composition: NOT binds tighter than AND, AND tighter than OR;
// parentheses group explicitly. Truth table over a fixed record.
// ---------------------------------------------------------------------------
TEST(dsl_precedence_not_and_or) {
  // Record: level=WARN, msg="disk full", src="d.log".
  const LogRecord r = rec("d.log", "disk full", Level::Warn);

  // a AND b OR c: parsed as (a AND b) OR c.
  const auto p1 = FilterExpr::compile(
      R"(level = "DEBUG" AND msg CONTAINS "disk" OR src = "d.log")");
  CHECK(p1->passes(r));  // second OR arm fires

  // NOT over a comparison, then ANDed.
  const auto p2 = FilterExpr::compile(R"(NOT level = "ERROR" AND msg = "disk full")");
  CHECK(p2->passes(r));

  // Double negation and NOT of a parenthesised group.
  const auto p3 = FilterExpr::compile(R"(NOT NOT level = "WARN")");
  const auto p4 = FilterExpr::compile(R"(NOT (level = "WARN" OR level = "ERROR"))");
  CHECK(p3->passes(r));
  CHECK(!p4->passes(r));

  // Parentheses flip the grouping: c OR a AND b vs (c OR a) AND b.
  const auto p5 = FilterExpr::compile(
      R"((level = "DEBUG" OR src = "d.log") AND msg CONTAINS "full")");
  CHECK(p5->passes(r));
  const auto p6 = FilterExpr::compile(
      R"(level = "DEBUG" OR src = "d.log" AND msg CONTAINS "nomatch")");
  CHECK(!p6->passes(r));  // AND arm fails, OR arm (DEBUG) fails
}

// ---------------------------------------------------------------------------
// Keywords AND / OR / NOT are case-insensitive; whitespace is free-form.
// ---------------------------------------------------------------------------
TEST(dsl_keywords_case_insensitive) {
  const LogRecord r = rec("d.log", "disk full", Level::Warn);
  const auto lower = FilterExpr::compile(
      R"(not level = "error" and (msg contains "disk" or src ends_with ".log"))");
  CHECK(lower->passes(r));

  const auto mixed = FilterExpr::compile(R"(msg ContaIns "FULL" aNd level > "info")");
  CHECK(mixed->passes(r));

  // Whitespace-free around symbols, extra whitespace around keywords.
  const auto tight = FilterExpr::compile(R"( level>="WARN" AND   msg="disk full" )");
  CHECK(tight->passes(r));
}

// ---------------------------------------------------------------------------
// String literals support backslash escapes (\" \\ \n \r \t); the decoded
// value is what the operators see.
// ---------------------------------------------------------------------------
TEST(dsl_string_escapes) {
  const auto quoted = FilterExpr::compile(R"(msg CONTAINS "say \"hi\"")");
  CHECK(quoted->passes(rec("a.log", "he said say \"hi\" loudly")));

  const auto backslash = FilterExpr::compile(R"(msg CONTAINS "C:\\logs")");
  CHECK(backslash->passes(rec("a.log", "scanning C:\\logs\\app")));

  const auto tab = FilterExpr::compile(R"(msg CONTAINS "a\tb")");
  CHECK(tab->passes(rec("a.log", "col a\tb col")));
  CHECK(!tab->passes(rec("a.log", "literal a\\tb slash")));

  // Unknown escapes pass through as "\c", so regex escapes survive for
  // MATCHES patterns (\d = digit class here).
  const auto digits = FilterExpr::compile(R"(msg MATCHES "\d+ errors")");
  CHECK(digits->passes(rec("a.log", "saw 12 errors")));
  CHECK(!digits->passes(rec("a.log", "saw many errors")));
}

// ---------------------------------------------------------------------------
// Unknown fields are rejected at compile time with the field's position.
// ---------------------------------------------------------------------------
TEST(dsl_error_unknown_field_position) {
  expect_error_at(R"(user = "root")", 1, "field");
  expect_error_at(R"(level = "INFO" AND host = "h")", 20, "unknown field");
  // Lowercase keywords are fields too; "message" is not one of them.
  expect_error_at(R"(message CONTAINS "x")", 1, "unknown field");
}

// ---------------------------------------------------------------------------
// Lexer and parser errors all carry the 1-based position where the problem
// was detected: unterminated strings, stray characters, missing operators,
// trailing garbage and empty expressions.
// ---------------------------------------------------------------------------
TEST(dsl_error_positions_lexer_parser) {
  expect_error_at(R"(msg = "never closed)", 20, "unterminated string");
  expect_error_at(R"(msg @ "x")", 5, "unexpected character");
  expect_error_at(R"(msg CONTAINS !x)", 14, "'=' after '!'");
  expect_error_at("msg \"x\"", 5, "operator");          // missing operator
  expect_error_at(R"(msg = "x" extra)", 11, "end");     // trailing garbage
  expect_error_at(R"((msg = "x")", 11, "')'");          // unbalanced parens
  expect_error_at(R"(msg =)", 6, "string literal");     // missing literal
  expect_error_at("AND msg = \"x\"", 1, "field");       // no left operand
  try {
    FilterExpr::compile("");
    ++testfw::failures();
    std::fprintf(stderr, "  empty expression compiled successfully\n");
  } catch (const Error& e) {
    CHECK_EQ_INT(e.pos, 1);
  }
}

// ---------------------------------------------------------------------------
// Unknown level names and string operators on level are compile-time errors.
// ---------------------------------------------------------------------------
TEST(dsl_error_level_semantics) {
  expect_error_at(R"(level = "VERBOSE")", 18, "unknown level");
  // A plain number is not a quoted string literal either.
  expect_error_at(R"(level >= 3)", 10, "string literal");
  // Comparison against the reserved RAW level is rejected: RAW records are
  // forwarded verbatim and must not be selected by hand.
  expect_error_at(R"(level = "RAW")", 14, "unknown level");
}

// ---------------------------------------------------------------------------
// A compiled expression is reusable across many records (the pipeline calls
// it per line) and text() reports the original expression verbatim.
// ---------------------------------------------------------------------------
TEST(dsl_reuse_and_text_roundtrip) {
  const std::string text = R"(level >= "WARN" AND (msg CONTAINS "timeout" OR src ENDS_WITH ".log"))";
  const auto expr = FilterExpr::compile(text);
  CHECK_STR_EQ(expr->text(), text);

  CHECK(expr->passes(rec("a.log", "connect timeout", Level::Warn)));
  CHECK(expr->passes(rec("b.LOG", "anything", Level::Error)));
  CHECK(!expr->passes(rec("c.txt", "fine", Level::Info)));
  CHECK(!expr->passes(rec("a.log", "fine", Level::Info)));   // level too low
  CHECK(!expr->passes(rec("c.txt", "connect timeout", Level::Info)));
  // Twenty more evaluations on the shared instance must stay consistent.
  for (int i = 0; i < 20; ++i) {
    CHECK(expr->passes(rec("a.log", "connect timeout", Level::Warn)));
  }
}

// ---------------------------------------------------------------------------
// DSL records vs RAW: RAW records carry the whole line as msg, so string
// operators match against it, and ordering comparisons see Level::Raw (which
// is above ERROR and therefore passes any level >= gate).
// ---------------------------------------------------------------------------
TEST(dsl_on_raw_records) {
  const auto expr = FilterExpr::compile(R"(msg CONTAINS "[INFO]")");
  CHECK(expr->passes(rec("a.log", "echo: [INFO] forwarded verbatim", Level::Raw)));

  const auto gate = FilterExpr::compile(R"(level >= "ERROR")");
  CHECK(gate->passes(rec("a.log", "unparsed noise", Level::Raw)));
}

// ---------------------------------------------------------------------------
// Config integration is exercised end-to-end in test_main.cpp; here we check
// only that dsl::Error is a runtime_error so Config::load can propagate it.
// ---------------------------------------------------------------------------
TEST(dsl_error_is_runtime_error) {
  try {
    FilterExpr::compile("bogus = \"x\"");
    ++testfw::failures();
    std::fprintf(stderr, "  expected compile failure\n");
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()).find("filter.expr") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Ordering operators on msg/src behave as lexicographic string comparisons
// (documented in dsl.h), while level keeps its severity ordinal semantics.
// ---------------------------------------------------------------------------
TEST(dsl_evaluator_lexicographic_ordering_on_msg_src) {
  const auto after = FilterExpr::compile(R"(msg >= "m")");
  CHECK(after->passes(rec("a.log", "notice")));
  CHECK(!after->passes(rec("a.log", "alpha")));

  const auto src_range = FilterExpr::compile(R"(src > "a.log" AND src < "c.log")");
  CHECK(src_range->passes(rec("b.log", "x")));
  CHECK(!src_range->passes(rec("a.log", "x")));
  CHECK(!src_range->passes(rec("c.log", "x")));

  // msg != matches everything but the literal (case-insensitively).
  const auto ne = FilterExpr::compile(R"(msg != "keep out")");
  CHECK(ne->passes(rec("a.log", "come in")));
  CHECK(!ne->passes(rec("a.log", "KEEP Out")));
}

// ---------------------------------------------------------------------------
// Long chains: flat AND / OR lists short-circuit; NOT stacks stay unbounded.
// ---------------------------------------------------------------------------
TEST(dsl_long_chains_and_not_stacks) {
  const LogRecord r = rec("d.log", "disk full", Level::Warn);

  const auto chain = FilterExpr::compile(
      R"(level = "DEBUG" OR level = "INFO" OR level = "WARN" OR level = "ERROR")");
  CHECK(chain->passes(r));

  const auto dead_and = FilterExpr::compile(
      R"(level = "WARN" AND msg CONTAINS "disk" AND msg CONTAINS "never" AND src = "d.log")");
  CHECK(!dead_and->passes(r));  // must short-circuit on the "never" arm

  // Five NOTs negate five times: odd count flips the result.
  const auto stacked = FilterExpr::compile("NOT NOT NOT NOT NOT " + std::string(R"(level = "WARN")"));
  CHECK(!stacked->passes(r));

  // The OR arm rescues the over-negated left side.
  const auto rescued = FilterExpr::compile(
      "NOT NOT NOT NOT NOT " + std::string(R"(level = "WARN")") +
      " OR msg = \"disk full\"");
  CHECK(rescued->passes(r));
}

// ---------------------------------------------------------------------------
// STARTS_WITH / ENDS_WITH on msg and src are independent fields; whitespace
// inside and around a parenthesised group is free-form.
// ---------------------------------------------------------------------------
TEST(dsl_field_independence_and_whitespace) {
  const LogRecord r = rec("d.log", "disk full", Level::Warn);

  const auto both = FilterExpr::compile(
      R"(src STARTS_WITH "d" AND msg ENDS_WITH "full")");
  CHECK(both->passes(r));

  const auto swapped = FilterExpr::compile(
      R"(msg STARTS_WITH "d" AND src ENDS_WITH "full")");
  // msg starts with "disk" (true), src does not end with "full" (false).
  CHECK(!swapped->passes(r));

  const auto spaced = FilterExpr::compile(
      R"(  (   msg  =   "disk full"  )   )");
  CHECK(spaced->passes(r));

  const auto multiline = FilterExpr::compile(
      "level >= \"WARN\"\n  AND\n  src = \"d.log\"");
  CHECK(multiline->passes(r));
}

// ---------------------------------------------------------------------------
// kv("key") comparisons: extracted KV fields are addressable from the DSL.
// A missing key matches nothing (including "!="); the operators behave like
// the msg/src ones on the extracted value.
// ---------------------------------------------------------------------------
namespace {

// rec() with extracted fields attached (mimics the extract.kv pipeline stage).
LogRecord kv_rec(const std::map<std::string, std::string>& fields) {
  LogRecord r = rec("app.log", "payload");
  r.fields = fields;
  return r;
}

}  // namespace

TEST(dsl_kv_field_equality_and_missing_key) {
  const LogRecord r = kv_rec({{"user", "bob"}, {"env", "PROD"}});

  const auto eq = FilterExpr::compile(R"(kv("user") = "bob")");
  CHECK(eq->passes(r));

  // Case-insensitive value comparison, same as msg/src.
  const auto env = FilterExpr::compile(R"(kv("env") = "prod")");
  CHECK(env->passes(r));

  const auto ne = FilterExpr::compile(R"(kv("user") != "alice")");
  CHECK(ne->passes(r));

  // A missing key matches nothing, including the negated forms.
  const auto missing = FilterExpr::compile(R"(kv("nope") = "bob")");
  CHECK(!missing->passes(r));
  const auto missing_ne = FilterExpr::compile(R"(kv("nope") != "bob")");
  CHECK(!missing_ne->passes(r));
  const auto missing_contains = FilterExpr::compile(R"(kv("nope") CONTAINS "o")");
  CHECK(!missing_contains->passes(r));

  // A record without any extracted fields never matches.
  CHECK(!eq->passes(rec("app.log", "payload")));
}

TEST(dsl_kv_field_operators_and_combinators) {
  const LogRecord r = kv_rec({{"count", "42"}, {"host", "web-01"}});

  const auto contains = FilterExpr::compile(R"(kv("host") CONTAINS "eb-0")");
  CHECK(contains->passes(r));
  const auto starts = FilterExpr::compile(R"(kv("host") STARTS_WITH "web")");
  CHECK(starts->passes(r));
  const auto ends = FilterExpr::compile(R"(kv("host") ENDS_WITH "01")");
  CHECK(ends->passes(r));
  const auto ge = FilterExpr::compile(R"(kv("count") >= "10")");
  CHECK(ge->passes(r));
  const auto lt = FilterExpr::compile(R"(kv("count") < "5")");
  CHECK(lt->passes(r));
  const auto matches = FilterExpr::compile(R"(kv("host") MATCHES "^web-[0-9]+$")");
  CHECK(matches->passes(r));

  // Composes with the classic fields through AND / OR / NOT and groups.
  const auto combo = FilterExpr::compile(
      R"(level >= "INFO" AND kv("user") = "bob" OR NOT kv("ghost") = "x")");
  const LogRecord with_user = kv_rec({{"user", "bob"}});
  CHECK(combo->passes(with_user));
  const LogRecord without_user = kv_rec({{"other", "x"}});
  CHECK(combo->passes(without_user));  // the NOT kv("ghost") arm rescues it

  // kv() can be negated directly.
  const auto negated = FilterExpr::compile(R"(NOT kv("user") = "alice")");
  CHECK(negated->passes(with_user));
}

TEST(dsl_kv_syntax_errors_have_positions) {
  // Missing opening quote for the key.
  expect_error_at("kv(user) = \"bob\"", 4, "quoted key");
  // Missing closing parenthesis.
  expect_error_at("kv(\"user\" = \"bob\"", 11, "')'");
  // Empty key is rejected at compile time.
  expect_error_at("kv(\"\") = \"bob\"", 4, "empty");
  // Unclosed kv call.
  expect_error_at("kv(\"user\"", 10, "')'");
}
