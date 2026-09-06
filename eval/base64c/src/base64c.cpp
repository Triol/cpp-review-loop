#include "base64c/base64c.hpp"

#include <cstdint>
#include <string>

namespace base64c {
namespace {

const char kStdAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kUrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Reverse lookup table: 0xFF means "not a valid character".
struct ReverseTable {
    std::uint8_t value[256];

    explicit ReverseTable(const char* alphabet) {
        for (int i = 0; i < 256; ++i) value[i] = 0xFF;
        for (int i = 0; alphabet[i] != '\0'; ++i)
            value[static_cast<unsigned char>(alphabet[i])] = static_cast<std::uint8_t>(i);
    }
};

const ReverseTable kStdReverse(kStdAlphabet);
const ReverseTable kUrlReverse(kUrlAlphabet);

// MIME (RFC 2045) line wrap width for encode_mime.
constexpr std::size_t kMimeLineLength = 76;

// Encode 1-3 raw bytes (left-justified in the 24-bit window) with `n` bytes
// of real data; the rest is zero-filled. Emits '=' padding for missing bytes.
std::string encode_group(const unsigned char* in, std::size_t n, const char* alphabet) {
    std::uint32_t v = (static_cast<std::uint32_t>(in[0]) << 16) |
                      (static_cast<std::uint32_t>(n > 1 ? in[1] : 0) << 8) |
                      static_cast<std::uint32_t>(n > 2 ? in[2] : 0);
    std::string out(4, '=');
    out[0] = alphabet[(v >> 18) & 0x3F];
    out[1] = alphabet[(v >> 12) & 0x3F];
    if (n > 1) out[2] = alphabet[(v >> 6) & 0x3F];
    if (n > 2) out[3] = alphabet[v & 0x3F];
    return out;
}

} // namespace

std::string encode(const std::string& data) {
    // Same encoding core as encode_mime below, with line wrapping disabled.
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    for (; i + 3 <= data.size(); i += 3)
        out += encode_group(p + i, 3, kStdAlphabet);
    const std::size_t rem = data.size() - i;
    if (rem != 0)
        out += encode_group(p + i, rem, kStdAlphabet);
    return out;
}

std::string encode_mime(const std::string& data) {
    // Encode with the standard alphabet, inserting "\r\n" after every 76
    // output characters (the final line is never terminated).
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4 + (data.size() + 2) / 3 * 4 / kMimeLineLength * 2);
    std::size_t line_len = 0;
    std::size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    for (; i + 3 <= data.size(); i += 3) {
        if (line_len == kMimeLineLength) { // flush the completed line before starting a new one
            out += "\r\n";
            line_len = 0;
        }
        out += encode_group(p + i, 3, kStdAlphabet);
        line_len += 4;
    }
    const std::size_t rem = data.size() - i;
    if (rem != 0) {
        if (line_len == kMimeLineLength) { // wrap before the final short group if the line is full
            out += "\r\n";
        }
        out += encode_group(p + i, rem, kStdAlphabet);
    }
    return out;
}

std::string encode_urlsafe(const std::string& data) {
    // Same layout as standard encode, then remap the two differing symbols
    // and drop '=' padding.
    std::string out = encode(data);
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

// Core strict decoder. The `mode` controls which characters are silently
// skipped before the strict rules apply: `strict` validates every character,
// `skip_ws` skips whitespace (space, \t, \r, \n), and `skip_crlf` skips only
// CR/LF (used for MIME, where "\r\n" pairs are removed). Error positions
// refer to the original input string in all modes.
enum class DecodeMode { strict, skip_ws, skip_crlf };

std::string decode_impl(const std::string& encoded, DecodeMode mode) {
    std::string out;
    out.reserve(encoded.size() / 4 * 3);

    std::uint32_t acc = 0;   // accumulates 4 sextets
    int sextets = 0;         // how many accumulated in the current group
    bool padding = false;    // once '=' is seen, only more '=' (to group end) is legal
    int pad_count = 0;       // '=' sextets in the current group

    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(encoded[i]);

        if (c == '=') {
            if (!padding) padding = true;
            acc <<= 6; // padding sextets contribute zero bits, keep alignment
            ++sextets;
            ++pad_count;
            if (sextets == 4) {
                // "ABC=" carries 2 bytes (last must be zero); "AB==" carries
                // 1 byte (lower 12 bits must be zero).
                const std::uint32_t mask = pad_count == 2 ? 0xFFFFu : 0x00FFu;
                if (acc & mask)
                    throw decode_error("non-zero bits in padded group", i);
                out += static_cast<char>((acc >> 16) & 0xFF);
                if (pad_count == 1) out += static_cast<char>((acc >> 8) & 0xFF);
                acc = 0;
                sextets = 0;
                pad_count = 0;
            }
            continue;
        }

        if (mode == DecodeMode::skip_ws && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            continue;
        if (mode == DecodeMode::skip_crlf && (c == '\r' || c == '\n'))
            continue;

        if (padding)
            throw decode_error("data character after padding", i);

        const std::uint8_t v = kStdReverse.value[c];
        if (v == 0xFF)
            throw decode_error("invalid character", i);

        acc = (acc << 6) | v;
        if (++sextets == 4) {
            out += static_cast<char>((acc >> 16) & 0xFF);
            out += static_cast<char>((acc >> 8) & 0xFF);
            out += static_cast<char>(acc & 0xFF);
            acc = 0;
            sextets = 0;
        }
    }

    if (sextets == 1)
        throw decode_error("truncated input (length % 4 == 1)", encoded.size() - 1);
    if (sextets != 0)
        // Either a 2-3 char tail without padding, or an incomplete padded
        // group (e.g. "AB=") — both are malformed in strict mode.
        throw decode_error(padding ? "incomplete padding group" : "truncated input (missing padding)",
                           encoded.size() - 1);
    return out;
}

std::string decode(const std::string& encoded) {
    // Strict mode: no characters are skipped.
    return decode_impl(encoded, DecodeMode::strict);
}

std::string decode_mime(const std::string& text) {
    // Skip "\r\n" line breaks in place (positions stay in the original input),
    // then apply the strict rules to the rest.
    return decode_impl(text, DecodeMode::skip_crlf);
}

std::string decode_lenient(const std::string& text) {
    // Lenient mode: whitespace anywhere is ignored before strict decoding.
    return decode_impl(text, DecodeMode::skip_ws);
}

std::string decode_urlsafe(const std::string& encoded) {
    std::string normalized;
    normalized.reserve(encoded.size() + 3);
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c == '-') normalized += '+';
        else if (c == '_') normalized += '/';
        else if (c == '+' || c == '/')
            throw decode_error("standard-alphabet character in URL-safe input", i);
        else normalized += c;
    }
    switch (normalized.size() % 4) {
        case 2: normalized += "=="; break;
        case 3: normalized += '='; break;
        case 1: throw decode_error("truncated input (length % 4 == 1)", encoded.size());
        default: break; // 0: nothing to pad
    }
    return decode(normalized);
}

// ---- Base64Encoder (streaming) -------------------------------------------

std::string Base64Encoder::update(const std::string& data) {
    std::string out;
    out.reserve((data.size() + buffered_ + 2) / 3 * 4);
    std::size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());

    // Top up the 3-byte window first, if partially filled.
    while (buffered_ < 3 && i < data.size()) buffer_[buffered_++] = p[i++];
    if (buffered_ < 3) return out; // not enough for a full group yet

    out += encode_group(buffer_, 3, kStdAlphabet);
    buffered_ = 0;

    for (; i + 3 <= data.size(); i += 3)
        out += encode_group(p + i, 3, kStdAlphabet);

    const std::size_t rem = data.size() - i;
    for (std::size_t k = 0; k < rem; ++k) buffer_[buffered_++] = p[i + k];
    return out;
}

std::string Base64Encoder::final() {
    if (finished_) return "";
    finished_ = true;
    if (buffered_ == 0) return "";
    std::string out = encode_group(buffer_, static_cast<std::size_t>(buffered_), kStdAlphabet);
    buffered_ = 0;
    return out;
}

void Base64Encoder::reset() {
    buffer_[0] = buffer_[1] = buffer_[2] = 0;
    buffered_ = 0;
    finished_ = false;
}

} // namespace base64c
