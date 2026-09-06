// Demo: round-trip encode/decode of representative samples.
#include "base64c/base64c.hpp"

#include <iostream>
#include <string>

namespace {

void roundtrip(const std::string& label, const std::string& raw) {
    const std::string enc = base64c::encode(raw);
    const std::string dec = base64c::decode(enc);
    std::cout << label << "\n"
              << "  raw    : " << raw << "\n"
              << "  base64 : " << enc << "\n"
              << "  urlsafe: " << base64c::encode_urlsafe(raw) << "\n"
              << "  decoded matches: " << (dec == raw ? "yes" : "NO") << "\n";
}

} // namespace

int main() {
    roundtrip("hello world", "hello world");
    roundtrip("empty string", "");
    roundtrip("one byte", "a");

    std::string binary;
    for (unsigned b = 0; b < 256; ++b) binary += static_cast<char>(b);
    roundtrip("binary bytes 0x00-0xFF (first 8 shown)", binary.substr(0, 8) + "...");

    // Streaming vs one-shot equivalence on a large chunked input.
    base64c::Base64Encoder enc;
    std::string streamed;
    const std::string chunk(1024, 'x');
    for (int i = 0; i < 17; ++i) streamed += enc.update(chunk);
    streamed += enc.final();
    std::cout << "streaming matches one-shot: "
              << (streamed == base64c::encode(std::string(17 * 1024, 'x')) ? "yes" : "NO") << "\n";
    return 0;
}
