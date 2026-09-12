// selftest.cpp - `logpipe --self-test`: runs the library's internal
// invariants (checksum vectors, compression/encryption round trips, time
// formatting, level registry consistency) and reports one line per check.
// Exit code 0 when every invariant holds, 1 otherwise. This is a deployment
// sanity probe: it validates the build against known-answer values without
// touching any configuration file or input source.

#include "pipeline.h"
#include "rle.h"
#include "util.h"
#include "xcrypt.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace logpipe {
namespace {

int g_checks = 0;
int g_failures = 0;

void report(const char* name, bool ok) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::cout << "[self-test] " << (ok ? "OK  " : "FAIL") << "  " << name << "\n";
}

std::string to_hex(uint32_t value) {
  const char* digits = "0123456789ABCDEF";
  std::string out;
  for (int shift = 28; shift >= 0; shift -= 4) {
    out += digits[(value >> shift) & 0xFu];
  }
  return out;
}

// 1. CRC-32 known-answer vector (IEEE 802.3 check value of "123456789").
void check_crc32_known_answer() {
  report("crc32 known answer (123456789 -> CBF43926)",
         to_hex(util::crc32("123456789")) == "CBF43926");
}

// 2. CRC-32 must not change with a different overload path.
void check_crc32_overload_consistency() {
  const std::string text = "logpipe self-test payload \0 binary";
  report("crc32 overload consistency",
         util::crc32(text) == util::crc32(text.data(), text.size()));
}

// 3. RLE round trip on compressible input.
void check_rle_roundtrip_compressible() {
  const std::string raw(512, 'a');
  std::string out;
  report("rle roundtrip (compressible)", rle::decompress(rle::compress(raw), out) && out == raw);
}

// 4. RLE round trip on incompressible pseudo-random input.
void check_rle_roundtrip_incompressible() {
  std::string raw;
  uint32_t state = 0x9E3779B9u;
  for (int i = 0; i < 4096; ++i) {
    state = state * 1664525u + 1013904223u;
    raw += static_cast<char>((state >> 24) & 0xFFu);
  }
  std::string out;
  report("rle roundtrip (incompressible)",
         rle::decompress(rle::compress(raw), out) && out == raw);
}

// 5. RLE must refuse garbage instead of producing silent nonsense.
void check_rle_rejects_bad_magic() {
  std::string out;
  report("rle rejects bad magic", !rle::decompress("NOTRLE", out));
}

// 6. XOR container round trip with a password.
void check_xcrypt_roundtrip() {
  const std::string plain = "sensitive log line\nwith two lines\n";
  const std::string encrypted = xcrypt::encrypt_container("self-test-password", plain);
  std::string decrypted;
  report("xcrypt roundtrip",
         xcrypt::decrypt_container("self-test-password", encrypted, decrypted) &&
             decrypted == plain);
}

// 7. XOR container must refuse a wrong password (magic or payload check).
void check_xcrypt_rejects_wrong_password() {
  const std::string encrypted = xcrypt::encrypt_container("right", "payload");
  std::string decrypted;
  report("xcrypt rejects wrong password",
         !xcrypt::decrypt_container("wrong", encrypted, decrypted));
}

// 8. Time formatting round trip: parse(format(t)) == t (millisecond grid).
void check_time_format_roundtrip() {
  const int64_t t = 1798700000123LL;  // arbitrary epoch ms
  int64_t parsed = 0;
  report("time format roundtrip",
         util::parse_datetime_ms(util::format_time_ms(t), parsed) && parsed == t);
}

// 9. format_duration sanity on the documented buckets.
void check_format_duration_buckets() {
  report("format_duration buckets",
         util::format_duration(850) == "850ms" &&
             util::format_duration(12300) == "12.3s" &&
             util::format_duration(62000) == "1m02s");
}

// 10. Level model: RAW must rank above every named level (threshold gate).
void check_level_ordering() {
  report("level ordering (RAW highest)",
         static_cast<int>(Level::Raw) > static_cast<int>(Level::Error) &&
             static_cast<int>(Level::Error) > static_cast<int>(Level::Info));
}

}  // namespace

int run_self_test() {
  check_crc32_known_answer();
  check_crc32_overload_consistency();
  check_rle_roundtrip_compressible();
  check_rle_roundtrip_incompressible();
  check_rle_rejects_bad_magic();
  check_xcrypt_roundtrip();
  check_xcrypt_rejects_wrong_password();
  check_time_format_roundtrip();
  check_format_duration_buckets();
  check_level_ordering();
  std::cout << "[self-test] " << g_checks << " check(s), " << g_failures
            << " failure(s)\n";
  return g_failures == 0 ? 0 : 1;
}

}  // namespace logpipe
