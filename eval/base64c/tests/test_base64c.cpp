// Minimal assertion-based test runner (no third-party dependencies).
#include "base64c/base64c.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_current = "";

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "FAIL [%s] line %d: %s\n", g_current,     \
                         __LINE__, #cond);                                 \
        }                                                                  \
    } while (0)

#define CHECK_THROWS(expr)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        bool threw_ = false;                                               \
        try { expr; }                                                      \
        catch (const base64c::decode_error&) { threw_ = true; }            \
        catch (...) { threw_ = true; }                                     \
        if (!threw_) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "FAIL [%s] line %d: expected throw: %s\n",\
                         g_current, __LINE__, #expr);                      \
        }                                                                  \
    } while (0)

void run(const char* name, void (*fn)()) {
    g_current = name;
    fn();
}

// ---- tests ---------------------------------------------------------------

void test_rfc4648_vectors() {
    struct { const char* raw; const char* enc; } v[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    for (const auto& t : v) {
        CHECK(base64c::encode(t.raw) == t.enc);
        CHECK(base64c::decode(t.enc) == t.raw);
    }
}

void test_empty_and_boundaries() {
    CHECK(base64c::encode("") == "");
    CHECK(base64c::decode("") == "");
    CHECK(base64c::encode("a").size() == 4);   // 1 byte -> 2 chars + "=="
    CHECK(base64c::encode("ab").size() == 4);  // 2 bytes -> 3 chars + "="
    CHECK(base64c::encode("abc").size() == 4); // 3 bytes -> 4 chars
    // All 256 byte values round-trip.
    std::string all;
    for (unsigned b = 0; b < 256; ++b) all += static_cast<char>(b);
    CHECK(base64c::decode(base64c::encode(all)) == all);
}

void test_binary_with_nul() {
    const std::string bin = std::string("a\0b", 3);
    CHECK(base64c::decode(base64c::encode(bin)) == bin);
    CHECK(base64c::encode(bin) == "YQBi");
    const std::string nul(1, '\0');
    CHECK(base64c::decode(base64c::encode(nul)) == nul);
}

void test_urlsafe() {
    // Bytes 0xFB 0xEF 0xBE map to "+/8=" in standard, "-_8" in URL-safe.
    const std::string raw = std::string("\xFB\xEF\xFC", 3);
    CHECK(base64c::encode(raw) == "++/8");
    CHECK(base64c::encode_urlsafe(raw) == "--_8");
    CHECK(base64c::decode_urlsafe("--_8") == raw);
    // Padding-free round trips at each tail length.
    for (const std::string& s : {"f", "fo", "foo", "foob", "fooba", "foobar"}) {
        CHECK(base64c::decode_urlsafe(base64c::encode_urlsafe(s)) == s);
    }
    // URL-safe decoder also accepts padded standard text via normalization.
    CHECK(base64c::decode_urlsafe("Zg==") == "f");
}

void test_streaming_matches_oneshot() {
    for (std::size_t total : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 64u, 1000u}) {
        const std::string data(static_cast<std::size_t>(total), '\xCD');
        for (std::size_t chunk : {1u, 2u, 3u, 5u, 7u}) {
            base64c::Base64Encoder enc;
            std::string out;
            for (std::size_t i = 0; i < data.size(); i += chunk)
                out += enc.update(data.substr(i, chunk));
            out += enc.final();
            CHECK(out == base64c::encode(data));
        }
    }
    // final() twice is a no-op the second time; reset() allows reuse.
    base64c::Base64Encoder enc;
    std::string out = enc.update("ab");
    out += enc.final();
    out += enc.final();
    CHECK(out == base64c::encode("ab"));
    enc.reset();
    out = enc.update("c");
    out += enc.final();
    CHECK(out == base64c::encode("c"));
}

void test_mime() {
    // MIME wraps at exactly 76 characters per line, last line unterminated.
    const std::string d57(57, '\xAB');   // -> 76 base64 chars, single line
    const std::string d54(54, '\xAB');   // -> 72 base64 chars, single line
    const std::string d58(58, '\xAB');   // -> 80 chars -> 76 + CRLF + 4
    const std::string d100(100, '\xAB'); // -> 136 chars -> 76 + CRLF + 60
    CHECK(base64c::encode_mime(d57) == base64c::encode(d57));
    CHECK(base64c::encode_mime(d57).find("\r\n") == std::string::npos);
    CHECK(base64c::encode_mime(d54) == base64c::encode(d54));
    CHECK(base64c::encode_mime(d58) == base64c::encode(d58).substr(0, 76) + "\r\n" + base64c::encode(d58).substr(76));
    CHECK(base64c::encode_mime(d100) == base64c::encode(d100).substr(0, 76) + "\r\n" + base64c::encode(d100).substr(76));
    CHECK(base64c::encode_mime(d100).size() == 136 + 2);
    CHECK(base64c::encode_mime("") == "");
    // Round trip through decode_mime, including multi-line input.
    CHECK(base64c::decode_mime(base64c::encode_mime(d100)) == d100);
    CHECK(base64c::decode_mime("Zm9v\r\nYmFy\r\n") == "foobar");
    CHECK(base64c::decode_mime("Zm9v\r\nYmFy") == "foobar");
    // Strict rules still apply after stripping line breaks.
    CHECK_THROWS(base64c::decode_mime("Zm9 v"));
    CHECK_THROWS(base64c::decode_mime("Z!\r\n9v"));
    // Error position refers to the original (unstripped) input string.
    // "Zm9v\r\nZm?9": '\r' at 4, '\n' at 5, so '?' is at original index 8.
    try { base64c::decode_mime("Zm9v\r\nZm?9"); CHECK(false); }
    catch (const base64c::decode_error& e) { CHECK(std::string(e.what()).find("position 8") != std::string::npos); }
}

void test_lenient() {
    // Without whitespace, decode_lenient matches decode exactly.
    for (const std::string& s : {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmFy"}) {
        CHECK(base64c::decode_lenient(s) == base64c::decode(s));
    }
    // Whitespace of every kind is skipped anywhere.
    CHECK(base64c::decode_lenient("Zm9 v") == "foo");
    CHECK(base64c::decode_lenient(" Zg ==\t") == "f");
    CHECK(base64c::decode_lenient("Zm9v\r\nYmFy\n") == "foobar");
    CHECK(base64c::decode_lenient("\tZm\n9v Y\rmFy ") == "foobar");
    // Non-whitespace invalid characters are still rejected.
    CHECK_THROWS(base64c::decode_lenient("Zm9v!"));
    CHECK_THROWS(base64c::decode_lenient("Zm9 v!"));
}

void test_decode_errors() {
    // Invalid characters (whitespace included).
    CHECK_THROWS(base64c::decode("Zm9v!"));
    CHECK_THROWS(base64c::decode("Zm9 v"));
    // '=' in the middle of data.
    CHECK_THROWS(base64c::decode("Z=9v"));
    CHECK_THROWS(base64c::decode("=m9v"));
    // Data after padding.
    CHECK_THROWS(base64c::decode("Zg==x"));
    // Truncated padding / bad tail lengths.
    CHECK_THROWS(base64c::decode("Zg="));   // incomplete padded group
    CHECK_THROWS(base64c::decode("Z"));     // len % 4 == 1
    CHECK_THROWS(base64c::decode("Zm9"));   // missing padding (no '=')
    // Non-zero bits in padded tail (should be zero).
    CHECK_THROWS(base64c::decode("Zh=="));  // 'h' has trailing bits set
    CHECK_THROWS(base64c::decode("Zm9="));
    // decode_error messages include the position.
    try { base64c::decode("Zm9v!"); CHECK(false); }
    catch (const base64c::decode_error& e) { CHECK(std::string(e.what()).find("position 4") != std::string::npos); }
    // URL-safe variant rejects length % 4 == 1 too.
    CHECK_THROWS(base64c::decode_urlsafe("A"));
    CHECK_THROWS(base64c::decode_urlsafe("++/8")); // '+' illegal in urlsafe set
}

} // namespace

int main() {
    run("rfc4648_vectors", test_rfc4648_vectors);
    run("empty_and_boundaries", test_empty_and_boundaries);
    run("binary_with_nul", test_binary_with_nul);
    run("urlsafe", test_urlsafe);
    run("streaming_matches_oneshot", test_streaming_matches_oneshot);
    run("mime", test_mime);
    run("lenient", test_lenient);
    run("decode_errors", test_decode_errors);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
