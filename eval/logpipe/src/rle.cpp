// rle.cpp - run-length encoder/decoder implementation. See rle.h for the
// exact container format this module implements.

#include "rle.h"

#include <cstring>

namespace logpipe {
namespace rle {

namespace {

constexpr uint8_t kRunFlag = 0x80;     // control bit: repeated run
constexpr size_t kMaxTokenLength = 128;  // literal bytes / repeats per token
constexpr size_t kMinRun = 2;            // repeats needed to emit a run token

void append_u64_le(std::string& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

bool read_u64_le(const std::string& data, size_t offset, uint64_t& value) {
  if (data.size() < offset + 8) return false;
  value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) |
            static_cast<uint64_t>(static_cast<unsigned char>(data[offset + index]));
  }
  return true;
}

}  // namespace

bool has_magic(const std::string& data) {
  return data.size() >= 4 && std::memcmp(data.data(), kMagic, 4) == 0;
}

std::string compress(const std::string& raw) {
  std::string packed;
  packed.reserve(raw.size() + raw.size() / 8 + kHeaderSize + 16);
  packed.append(kMagic, 4);
  append_u64_le(packed, raw.size());

  size_t index = 0;
  while (index < raw.size()) {
    // Measure the run of identical bytes starting at `index`.
    const char value = raw[index];
    size_t run = 1;
    while (run < kMaxTokenLength && index + run < raw.size() &&
           raw[index + run] == value) {
      ++run;
    }
    if (run >= kMinRun) {
      // Repeated run: control byte (flag | count-1) + one value byte.
      packed.push_back(static_cast<char>(kRunFlag | static_cast<uint8_t>(run - 1)));
      packed.push_back(value);
      index += run;
    } else {
      // Literal run: accumulate until the next run of >= kMinRun repeats or
      // the token capacity is reached.
      const size_t literal_start = index;
      size_t literal_length = 0;
      while (index < raw.size() && literal_length < kMaxTokenLength) {
        // Stop the literal right before a new qualifying run begins.
        size_t ahead = 1;
        while (index + ahead < raw.size() && ahead < kMinRun &&
               raw[index + ahead] == raw[index]) {
          ++ahead;
        }
        if (ahead >= kMinRun && literal_length > 0) break;
        ++index;
        ++literal_length;
      }
      packed.push_back(static_cast<char>(static_cast<uint8_t>(literal_length - 1)));
      packed.append(raw, literal_start, literal_length);
    }
  }
  return packed;
}

bool decompress(const std::string& packed, std::string& out) {
  std::string decoded;
  if (packed.size() < kHeaderSize || !has_magic(packed)) return false;
  uint64_t expected = 0;
  if (!read_u64_le(packed, 4, expected)) return false;

  decoded.reserve(static_cast<size_t>(expected));
  size_t index = kHeaderSize;
  while (index < packed.size()) {
    const uint8_t control = static_cast<uint8_t>(packed[index++]);
    if (control & kRunFlag) {
      const size_t count = static_cast<size_t>(control & 0x7Fu) + 1;
      if (index >= packed.size()) return false;  // truncated run
      decoded.append(count, packed[index++]);
    } else {
      const size_t count = static_cast<size_t>(control & 0x7Fu) + 1;
      if (packed.size() - index < count) return false;  // truncated literal
      decoded.append(packed, index, count);
      index += count;
    }
    if (decoded.size() > expected) return false;  // declared length overrun
  }
  if (decoded.size() != expected) return false;
  out = std::move(decoded);
  return true;
}

std::string decompress_or_empty(const std::string& packed) {
  std::string out;
  decompress(packed, out);
  return out;
}

}  // namespace rle
}  // namespace logpipe
