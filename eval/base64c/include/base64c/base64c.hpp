// base64c.hpp - Base64 encode/decode library (RFC 4648), C++17.
#ifndef BASE64C_BASE64C_HPP
#define BASE64C_BASE64C_HPP

#include <cstddef>
#include <stdexcept>
#include <string>

namespace base64c {

// Thrown by decode functions on invalid input. `position` is the 0-based
// index into the input string where the error was detected.
class decode_error : public std::runtime_error {
public:
    decode_error(const std::string& msg, std::size_t position)
        : std::runtime_error(msg + " at position " + std::to_string(position)),
          position(position) {}

    std::size_t position;
};

// Encode `data` using the standard alphabet (A-Z a-z 0-9 + /) with '='
// padding. Accepts arbitrary binary content, including embedded NUL bytes.
std::string encode(const std::string& data);

// Encode using the URL-safe alphabet (- and _) with no padding.
std::string encode_urlsafe(const std::string& data);

// Encode like encode(), then insert "\r\n" every 76 output characters
// (RFC 2045 MIME line wrapping). The final line has no trailing "\r\n".
std::string encode_mime(const std::string& data);

// Strict decode of the standard alphabet. Rejects:
//   - characters outside the alphabet (whitespace included),
//   - '=' anywhere except as a properly formed final padding group,
//   - input length where len % 4 == 1.
std::string decode(const std::string& encoded);

// MIME decode: ignores all "\r\n" occurrences, then applies the standard
// strict rules (see decode()).
std::string decode_mime(const std::string& text);

// Lenient decode: skips all whitespace characters (space, \t, \r, \n)
// before applying the standard strict rules. Otherwise identical to
// decode(); input without whitespace yields the same result.
std::string decode_lenient(const std::string& text);

// Decode URL-safe input ('-' and '_'). Padding is optional: a missing
// final '=' or '==' is tolerated; a length of 1 (mod 4) is still an error.
std::string decode_urlsafe(const std::string& encoded);

// Incremental encoder for chunked (streaming) input. Feed arbitrarily
// sized pieces to update(); call final() to flush the tail and produce
// padding. The output of (update + final) equals one-shot encode() on the
// concatenation of all updates. final() may be called once; the object
// must not be reused afterwards unless reset() is called.
class Base64Encoder {
public:
    Base64Encoder() = default;

    // Encode a chunk; returns the Base64 text for the full groups present.
    std::string update(const std::string& data);

    // Flush remaining 1-2 buffered bytes, emitting proper '=' padding.
    std::string final();

    // Clear internal state so the encoder can be reused.
    void reset();

private:
    unsigned char buffer_[3] = {0, 0, 0};
    int buffered_ = 0;   // number of pending bytes (0..2)
    bool finished_ = false;
};

} // namespace base64c

#endif // BASE64C_BASE64C_HPP
