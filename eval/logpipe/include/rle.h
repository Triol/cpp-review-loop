// rle.h - self-implemented run-length encoding (RLE) for the compressed
// output mode. No third-party libraries: the container format and both the
// encoder and the decoder live entirely in this module.
//
// Container format (all integers little-endian):
//   offset 0 : magic  "LRLE" (4 bytes) - identifies an RLE container
//   offset 4 : uint64 original (uncompressed) length in bytes
//   offset 12: token stream:
//                control byte, bit 7 = kind:
//                  0 = literal run : (len) bytes of raw data follow,
//                      len = (control & 0x7F) + 1, range 1..128
//                  1 = repeated run: one value byte follows, repeated
//                      count = (control & 0x7F) + 1 times, range 2..129
//   A literal token is used whenever a run is shorter than 2 repeats, so
//   incompressible data expands by at most ~1 byte per 128 bytes plus the
//   12-byte header. decode_rle() validates the magic, the length field and
//   every token boundary and returns false on any malformed input.

#pragma once

#include <cstdint>
#include <string>

namespace logpipe {
namespace rle {

// 4-byte magic at the start of every compressed file.
inline constexpr char kMagic[4] = {'L', 'R', 'L', 'E'};

// Size of the fixed header: magic + 64-bit original length.
inline constexpr size_t kHeaderSize = 12;

// True when `data` starts with the RLE magic (used by tests and by the
// decoder pre-check).
bool has_magic(const std::string& data);

// Compresses `raw` into a full RLE container (magic + length + tokens).
// Always succeeds; may be larger than the input for incompressible data.
std::string compress(const std::string& raw);

// Decompresses a container produced by compress(). Returns false (and leaves
// `out` empty) when the input is truncated, malformed or carries a wrong
// magic / declared length.
bool decompress(const std::string& packed, std::string& out);

// Convenience wrapper: decompress or return an empty string on failure.
std::string decompress_or_empty(const std::string& packed);

}  // namespace rle
}  // namespace logpipe
