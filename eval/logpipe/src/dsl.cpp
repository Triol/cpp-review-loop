// dsl.cpp - lexer, recursive-descent parser and evaluator for the filter
// expression DSL (see dsl.h for the grammar and semantics).

#include "dsl.h"

#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace logpipe {
namespace dsl {
namespace {

std::string to_lower(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// Case-insensitive helpers shared by the string operators.
bool ci_equals(const std::string& a, const std::string& b) {
  return to_lower(a) == to_lower(b);
}
bool ci_contains(const std::string& text, const std::string& needle) {
  return to_lower(text).find(to_lower(needle)) != std::string::npos;
}
bool ci_starts_with(const std::string& text, const std::string& prefix) {
  const std::string t = to_lower(text);
  const std::string p = to_lower(prefix);
  return t.size() >= p.size() && t.compare(0, p.size(), p) == 0;
}
bool ci_ends_with(const std::string& text, const std::string& suffix) {
  const std::string t = to_lower(text);
  const std::string s = to_lower(suffix);
  return t.size() >= s.size() &&
         t.compare(t.size() - s.size(), s.size(), s) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// AST (namespace scope: dsl.h forward-declares dsl::ExprNode)
// ---------------------------------------------------------------------------

enum class Field { Level, Msg, Src, Kv };
enum class Op { Eq, Ne, Ge, Gt, Le, Lt, Contains, StartsWith, EndsWith, Matches };

struct ExprNode {
  virtual ~ExprNode() = default;
  virtual bool eval(const LogRecord& rec) const = 0;
};

// AND / OR over one or more children (kept in a flat list; short-circuits).
struct LogicNode : ExprNode {
  bool is_and = true;
  std::vector<std::unique_ptr<ExprNode>> children;

  bool eval(const LogRecord& rec) const override {
    if (is_and) {
      for (const auto& child : children) {
        if (!child->eval(rec)) return false;
      }
      return true;
    }
    for (const auto& child : children) {
      if (child->eval(rec)) return true;
    }
    return false;
  }
};

// NOT with a single child.
struct NotNode : ExprNode {
  std::unique_ptr<ExprNode> child;
  bool eval(const LogRecord& rec) const override { return !child->eval(rec); }
};

// field op "literal" (or bare boolean for level-less forms; all comparisons
// here take a quoted string literal per the grammar). `field` selects the
// record member; Field::Kv instead looks the key up in the record's extracted
// fields (kv("key") syntax, see dsl.h).
struct CompareNode : ExprNode {
  Field field = Field::Msg;
  std::string kv_key;  // only for Field::Kv
  Op op = Op::Eq;
  std::string literal;
  std::shared_ptr<const std::regex> matcher;  // only for Op::Matches

  bool eval(const LogRecord& rec) const override {
    if (field == Field::Level) return eval_level(rec);
    if (field == Field::Kv) {
      const auto it = rec.fields.find(kv_key);
      if (it == rec.fields.end()) return false;  // missing key: matches nothing
      return eval_value(it->second);
    }
    return eval_value(field == Field::Msg ? rec.message : rec.source);
  }

  bool eval_value(const std::string& value) const {
    switch (op) {
      case Op::Eq:        return ci_equals(value, literal);
      case Op::Ne:        return !ci_equals(value, literal);
      case Op::Ge:        return value >= literal;
      case Op::Gt:        return value > literal;
      case Op::Le:        return value <= literal;
      case Op::Lt:        return value < literal;
      case Op::Contains:  return ci_contains(value, literal);
      case Op::StartsWith: return ci_starts_with(value, literal);
      case Op::EndsWith:  return ci_ends_with(value, literal);
      case Op::Matches:   return matcher != nullptr &&
                                 std::regex_search(value, *matcher);
    }
    return false;
  }

 private:
  // Level comparisons use severity ordinals; a literal that is not a known
  // level name (built-in or registered custom level) is rejected at compile
  // time (see parser), so this is total.
  bool eval_level(const LogRecord& rec) const {
    int want = 0;
    if (!level_token_to_int(literal, want)) return false;
    const int have = static_cast<int>(rec.level);
    switch (op) {
      case Op::Eq: return have == want;
      case Op::Ne: return have != want;
      case Op::Ge: return have >= want;
      case Op::Gt: return have > want;
      case Op::Le: return have <= want;
      case Op::Lt: return have < want;
      default:     return false;  // string ops on level are compile errors
    }
  }
};

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

namespace {

enum class TokKind {
  End,           // end of input
  Ident,         // level / msg / src / AND / OR / NOT / CONTAINS / ...
  String,        // "quoted literal"
  LParen,        // (
  RParen,        // )
  Eq,            // =
  Ne,            // !=
  Ge,            // >=
  Gt,            // >
  Le,            // <=
  Lt             // <
};

struct Token {
  TokKind kind = TokKind::End;
  std::string text;  // identifier name or decoded string value
  int pos = 0;       // 1-based position of the first character
};

class Lexer {
 public:
  explicit Lexer(const std::string& text) : text_(text) {}

  // Tokenizes the whole input; throws dsl::Error on unterminated strings,
  // illegal escape sequences or stray characters.
  std::vector<Token> tokenize() {
    std::vector<Token> tokens;
    for (;;) {
      skip_ws();
      Token tok;
      tok.pos = pos_ + 1;  // report 1-based positions
      if (pos_ >= text_.size()) {
        tok.kind = TokKind::End;
        tokens.push_back(tok);
        return tokens;
      }
      const char c = text_[pos_];
      switch (c) {
        case '(': tok.kind = TokKind::LParen; ++pos_; break;
        case ')': tok.kind = TokKind::RParen; ++pos_; break;
        case '=': tok.kind = TokKind::Eq; ++pos_; break;
        case '>':
          tok.kind = TokKind::Gt;
          ++pos_;
          if (peek() == '=') { tok.kind = TokKind::Ge; ++pos_; }
          break;
        case '<':
          tok.kind = TokKind::Lt;
          ++pos_;
          if (peek() == '=') { tok.kind = TokKind::Le; ++pos_; }
          break;
        case '!':
          ++pos_;
          if (peek() != '=') {
            fail(tok.pos, "expected '=' after '!' (use '!=')");
          }
          tok.kind = TokKind::Ne;
          ++pos_;
          break;
        case '"':
          tok.kind = TokKind::String;
          tok.text = read_string();
          break;
        default:
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            tok.kind = TokKind::Ident;
            tok.text = read_ident();
          } else {
            fail(tok.pos, std::string("unexpected character '") + c + "'");
          }
          break;
      }
      tokens.push_back(std::move(tok));
    }
  }

 private:
  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

  void skip_ws() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  std::string read_ident() {
    const size_t start = pos_;
    while (pos_ < text_.size() &&
           (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
            text_[pos_] == '_')) {
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }

  // Reads a double-quoted string with backslash escapes: \" \\ \n \r \t;
  // any other escaped character is passed through as "\c", which lets
  // MATCHES regexes carry escapes like \b \d \. unchanged.
  std::string read_string() {
    ++pos_;  // consume the opening quote
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return out;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) {
          fail(pos_ + 1, "unterminated escape sequence in string literal");
        }
        switch (text_[pos_]) {
          case '"':  out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case 'n':  out.push_back('\n'); break;
          case 'r':  out.push_back('\r'); break;
          case 't':  out.push_back('\t'); break;
          default:
            // Unknown escapes pass through as "\c" so regex escapes such as
            // \b, \d, \. survive for MATCHES patterns.
            out.push_back('\\');
            out.push_back(text_[pos_]);
            break;
        }
        ++pos_;
        continue;
      }
      out.push_back(c);
      ++pos_;
    }
    fail(pos_ + 1, "unterminated string literal (missing closing '\"')");
    return "";  // unreachable
  }

  [[noreturn]] void fail(int position, const std::string& message) const {
    throw Error("filter.expr: " + message, position);
  }

  const std::string& text_;
  size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Parser (recursive descent)
// ---------------------------------------------------------------------------

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  std::unique_ptr<ExprNode> parse() {
    auto root = parse_or();
    expect(TokKind::End, "end of expression");
    return root;
  }

 private:
  const Token& current() const { return tokens_[index_]; }

  void advance() {
    if (current().kind != TokKind::End) ++index_;
  }

  bool is_ident(const char* keyword) const {
    return current().kind == TokKind::Ident &&
           ci_equals(current().text, keyword);
  }

  void expect(TokKind kind, const char* what) {
    if (current().kind != kind) {
      fail("expected " + std::string(what) + describe(current()));
    }
  }

  static std::string describe(const Token& tok) {
    switch (tok.kind) {
      case TokKind::End:    return ", but reached end of expression";
      case TokKind::String: return ", got string \"" + tok.text + "\"";
      default:              return ", got '" + tok.text + "'";
    }
  }

  [[noreturn]] void fail(const std::string& message) const {
    throw Error("filter.expr: " + message + " (at position " +
                    std::to_string(current().pos) + ")",
                current().pos);
  }

  // or_expr := and_expr ( "OR" and_expr )*
  std::unique_ptr<ExprNode> parse_or() {
    auto left = parse_and();
    if (!is_ident("OR")) return left;
    auto node = std::make_unique<LogicNode>();
    node->is_and = false;
    node->children.push_back(std::move(left));
    while (is_ident("OR")) {
      advance();
      node->children.push_back(parse_and());
    }
    return std::unique_ptr<ExprNode>(node.release());
  }

  // and_expr := unary ( "AND" unary )*
  std::unique_ptr<ExprNode> parse_and() {
    auto left = parse_unary();
    if (!is_ident("AND")) return left;
    auto node = std::make_unique<LogicNode>();
    node->is_and = true;
    node->children.push_back(std::move(left));
    while (is_ident("AND")) {
      advance();
      node->children.push_back(parse_unary());
    }
    return std::unique_ptr<ExprNode>(node.release());
  }

  // unary := "NOT" unary | primary
  std::unique_ptr<ExprNode> parse_unary() {
    if (is_ident("NOT")) {
      advance();
      auto node = std::make_unique<NotNode>();
      node->child = parse_unary();
      return node;
    }
    return parse_primary();
  }

  // primary := "(" expr ")" | comparison
  std::unique_ptr<ExprNode> parse_primary() {
    if (current().kind == TokKind::LParen) {
      advance();
      auto inner = parse_or();
      expect(TokKind::RParen, "')'");
      advance();
      return inner;
    }
    return parse_comparison();
  }

  // comparison := field op string_literal
  //             | "kv" "(" string_literal ")" op string_literal
  //             | "field" "(" string_literal ")" op string_literal
  std::unique_ptr<ExprNode> parse_comparison() {
    static const std::map<std::string, Field> kFields = {
        {"level", Field::Level}, {"msg", Field::Msg}, {"src", Field::Src}};

    auto node = std::make_unique<CompareNode>();

    // kv("key") / field("key") = "value" form: the pseudo-field is the call
    // itself. Both read the record's field map (extracted KV pairs and the
    // injected source tags share it); field(...) exists so source-level
    // metadata reads naturally (field("tag")).
    if (current().kind == TokKind::Ident &&
        (ci_equals(current().text, "kv") || ci_equals(current().text, "field")) &&
        index_ + 1 < tokens_.size() &&
        tokens_[index_ + 1].kind == TokKind::LParen) {
      const bool is_field_form = ci_equals(current().text, "field");
      advance();  // kv / field
      advance();  // (
      if (current().kind != TokKind::String) {
        fail(std::string("expected a quoted key inside ") +
             (is_field_form ? "field(...)" : "kv(...)") + describe(current()));
      }
      if (current().text.empty()) {
        fail(std::string("the key inside ") + (is_field_form ? "field(...)" : "kv(...)") +
             " must not be empty");
      }
      node->field = Field::Kv;
      node->kv_key = current().text;
      advance();
      expect(TokKind::RParen, is_field_form ? "')' after the field key"
                                            : "')' after the kv key");
      advance();
      parse_operator_and_literal(*node);
      return node;
    }

    if (current().kind != TokKind::Ident || kFields.find(to_lower(current().text)) ==
                                                 kFields.end()) {
      fail("expected a field name (level, msg, src, kv(...) or field(...))" +
           (current().kind == TokKind::Ident
                ? ", got unknown field '" + current().text + "'"
                : describe(current())));
    }
    node->field = kFields.at(to_lower(current().text));
    advance();
    parse_operator_and_literal(*node);
    return node;
  }

  // Shared tail of a comparison: the operator, the quoted literal and the
  // compile-time semantic checks (level names, MATCHES regex compilation).
  void parse_operator_and_literal(CompareNode& node) {
    // Operator: symbols or keyword operators (case-insensitive).
    switch (current().kind) {
      case TokKind::Eq: node.op = Op::Eq; break;
      case TokKind::Ne: node.op = Op::Ne; break;
      case TokKind::Ge: node.op = Op::Ge; break;
      case TokKind::Gt: node.op = Op::Gt; break;
      case TokKind::Le: node.op = Op::Le; break;
      case TokKind::Lt: node.op = Op::Lt; break;
      case TokKind::Ident:
        if (is_ident("CONTAINS")) node.op = Op::Contains;
        else if (is_ident("STARTS_WITH")) node.op = Op::StartsWith;
        else if (is_ident("ENDS_WITH")) node.op = Op::EndsWith;
        else if (is_ident("MATCHES")) node.op = Op::Matches;
        else fail("expected a comparison operator after field");
        break;
      default:
        fail("expected a comparison operator after field" + describe(current()));
    }
    advance();

    if (current().kind != TokKind::String) {
      fail("expected a quoted string literal" + describe(current()));
    }
    node.literal = current().text;
    advance();

    // Semantic checks that need the literal (compile-time, with position of
    // the literal approximated by the current position).
    if (node.field == Field::Level) {
      int parsed_level_value = 0;
      // Built-in RAW is not accepted in expressions (it marks unparsed lines);
      // registered custom levels participate like built-in names.
      Level builtin;
      const bool is_builtin_raw =
          level_from_string(node.literal, builtin) && builtin == Level::Raw;
      if (is_builtin_raw || !level_token_to_int(node.literal, parsed_level_value)) {
        fail("unknown level name \"" + node.literal +
             "\" (expected DEBUG, INFO, WARN, ERROR or a registered custom level)");
      }
    }
    if (node.op == Op::Matches) {
      try {
        node.matcher =
            std::make_shared<const std::regex>(node.literal,
                                               std::regex::ECMAScript);
      } catch (const std::regex_error& error) {
        fail("invalid MATCHES regular expression \"" + node.literal + "\": " +
             error.what());
      }
    }
  }

  std::vector<Token> tokens_;
  size_t index_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// FilterExpr
// ---------------------------------------------------------------------------

FilterExpr::FilterExpr(std::string text, std::unique_ptr<ExprNode> root)
    : text_(std::move(text)), root_(std::move(root)) {}

std::shared_ptr<const FilterExpr> FilterExpr::compile(const std::string& text) {
  if (text.empty()) {
    throw Error("filter.expr: expression is empty", 1);
  }
  std::vector<Token> tokens = Lexer(text).tokenize();
  std::unique_ptr<ExprNode> root = Parser(std::move(tokens)).parse();
  return std::shared_ptr<const FilterExpr>(
      new FilterExpr(text, std::move(root)));
}

bool FilterExpr::passes(const LogRecord& rec) const {
  return root_ != nullptr && root_->eval(rec);
}

}  // namespace dsl
}  // namespace logpipe
