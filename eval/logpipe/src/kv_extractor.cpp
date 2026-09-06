// kv_extractor.cpp - implementation of the key=value field extractor
// (see kv_extractor.h for the accepted line grammar and semantics).

#include "kv_extractor.h"

namespace logpipe {
namespace {

// True for the characters that may appear inside a bare (unquoted) key.
bool is_key_char(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u != ' ' && u != '\t' && u != '\r' && u != '\n' && u != '\v' &&
         u != '\f' && c != '=';
}

bool is_ws(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u == ' ' || u == '\t' || u == '\r' || u == '\n' || u == '\v' ||
         u == '\f';
}

// Scanner over one line. Keeps a cursor and exposes the token-level primitives
// the pair loop needs; all positions are byte offsets (the extractor is
// byte-oriented and never mutates the input).
class LineScanner {
 public:
  explicit LineScanner(const std::string& text) : text_(text) {}

  // Skips whitespace; false when the cursor reached the end.
  bool skip_ws() {
    while (pos_ < text_.size() && is_ws(text_[pos_])) ++pos_;
    return pos_ < text_.size();
  }

  // Reads a key: a maximal run of key characters. Returns false (leaving the
  // cursor untouched) when there is no key at the cursor, e.g. because the
  // next character is '=' (empty key) or the end of the line.
  bool read_key(std::string& key) {
    const size_t start = pos_;
    while (pos_ < text_.size() && is_key_char(text_[pos_])) ++pos_;
    if (pos_ == start) return false;
    key.assign(text_, start, pos_ - start);
    return true;
  }

  // Consumes the '=' separating key and value; false when the next character
  // is anything else.
  bool expect_equals() {
    if (pos_ < text_.size() && text_[pos_] == '=') {
      ++pos_;
      return true;
    }
    return false;
  }

  // Reads a value: either a quoted string (single or double quotes, with the
  // quote-escaped escapes decoded) or a bare run of non-whitespace characters.
  // Returns false only when the value is empty AND malformed; "k=" (nothing
  // before the next whitespace) is a valid empty value.
  bool read_value(std::string& value) {
    if (pos_ >= text_.size()) {
      value.clear();
      return true;  // "k=" at end of line: empty value
    }
    const char quote = text_[pos_];
    if (quote == '"' || quote == '\'') {
      return read_quoted(quote, value);
    }
    const size_t start = pos_;
    while (pos_ < text_.size() && !is_ws(text_[pos_])) ++pos_;
    value.assign(text_, start, pos_ - start);
    return true;
  }

  size_t pos() const { return pos_; }

  // Consumes a single character (used to step over stray characters such as
  // an '=' without a key).
  void advance_one() { if (pos_ < text_.size()) ++pos_; }

 private:
  // Reads a quoted value starting at the opening quote. Unterminated quotes
  // consume the rest of the line (tolerant behaviour: better to keep the data
  // than to reject an almost-CSV line).
  bool read_quoted(char quote, std::string& value) {
    ++pos_;  // consume the opening quote
    value.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == quote) {
        ++pos_;
        return true;
      }
      if (c == '\\' && pos_ + 1 < text_.size()) {
        const char next = text_[pos_ + 1];
        if (next == quote || next == '\\') {
          value.push_back(next);
          pos_ += 2;
          continue;
        }
        // Any other escape passes through verbatim ("\c" stays "\c").
        value.push_back(c);
        value.push_back(next);
        pos_ += 2;
        continue;
      }
      value.push_back(c);
      ++pos_;
    }
    return true;  // unterminated: consumed the rest of the line as the value
  }

  const std::string& text_;
  size_t pos_ = 0;
};

}  // namespace

bool KvExtractor::extract(const std::string& line,
                          std::map<std::string, std::string>& out) const {
  out.clear();
  LineScanner scanner(line);

  // A line qualifies when the scanner reaches at least one well-formed
  // "key=value" pair. The first malformed token ends the scan; whatever was
  // collected before it is kept (if anything).
  while (scanner.skip_ws()) {
    const size_t token_start = scanner.pos();
    std::string key;
    if (!scanner.read_key(key)) {
      // Either an empty key ("=v") or a stray character: stop here.
      (void)token_start;
      break;
    }
    if (!scanner.expect_equals()) {
      break;  // bare word without '=': not a KV-shaped line (or end of pairs)
    }
    std::string value;
    scanner.read_value(value);
    out[std::move(key)] = std::move(value);  // duplicates: last one wins
  }
  return !out.empty();
}

std::map<std::string, std::string> KvExtractor::extract(
    const std::string& line) const {
  std::map<std::string, std::string> out;
  extract(line, out);
  return out;
}

bool KvExtractor::extract_tail(
    const std::string& line, std::map<std::string, std::string>& out) const {
  out.clear();
  LineScanner scanner(line);

  // Walk the tokens until one starts a well-formed "key=value" pair; leading
  // free-form tokens are skipped so only the payload is extracted.
  while (scanner.skip_ws()) {
    const size_t token_start = scanner.pos();
    std::string key;
    if (scanner.read_key(key) && scanner.expect_equals()) {
      // First pair found: extract the rest of the line from here.
      return extract(line.substr(token_start), out);
    }
    // Not a pair start: consume the remainder of this token (a bare value,
    // a quoted string or a stray character) and keep looking.
    std::string dummy;
    if (!scanner.read_value(dummy)) {
      scanner.advance_one();
    }
  }
  return false;
}

std::map<std::string, std::string> KvExtractor::extract_tail(
    const std::string& line) const {
  std::map<std::string, std::string> out;
  extract_tail(line, out);
  return out;
}

}  // namespace logpipe
