// xcrypt.h - lightweight self-implemented output encryption (no third-party
// libraries). The cipher is a XOR stream whose keystream is derived from a
// configured password through an iterated FNV-1a hash:
//
//   h_0 = FNV-1a-64(password bytes)
//   h_{i+1} = FNV-1a-64(the 8 little-endian bytes of h_i)
//
// The keystream is the concatenation of the 8-byte little-endian blocks
// h_0, h_1, h_2, ...; plaintext byte j is XORed with keystream byte j. The
// stream position is deterministic, so encryption and decryption are the same
// operation and a file can be produced (and reversed) incrementally, one
// chunk at a time, with every file starting at keystream offset 0.
//
// Container format produced by encrypt_container() / expected by
// decrypt_container() (all values little-endian):
//   offset 0: magic  "LXEF" (4 bytes) - identifies an encrypted container
//   offset 4: uint8  version (currently 1)
//   offset 5: XOR-encrypted payload
//
// This is deliberately lightweight obfuscation for log files at rest, not a
// cryptographic construction; the point of the module is that it is fully
// self-contained and testable round-trip.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace logpipe {
namespace xcrypt {

// 4-byte magic at the start of every encrypted container.
inline constexpr char kMagic[4] = {'L', 'X', 'E', 'F'};

// Container header size: magic (4) + version byte (1).
inline constexpr size_t kHeaderSize = 5;

// The only container format version produced and accepted right now.
inline constexpr uint8_t kVersion = 1;

// FNV-1a 64-bit over the raw bytes of `data` (offset basis + prime per
// byte), the hash the keystream derivation is built on.
uint64_t fnv1a(const std::string& data);

// One FNV-1a round over a single byte (exposed for the keystream generator
// and its tests).
uint64_t fnv1a_step(uint64_t hash, uint8_t byte);

// The 64-bit keystream block number `index` for `password` (block 0 is
// derived directly from the password, block i+1 from block i). Deterministic
// and independent of any call order.
uint64_t keystream_block(const std::string& password, uint64_t index);

// Incremental XOR stream over the keystream of `password`: every apply()
// continues where the previous one stopped, so a producer can encrypt a file
// chunk by chunk (line by line) and a consumer decrypt it the same way.
class XorStream {
 public:
  explicit XorStream(std::string password);

  // XORs `data` in place with the next data.size() keystream bytes.
  void apply(std::string& data);

  // Bytes of keystream consumed so far (test hook).
  uint64_t offset() const { return offset_; }

 private:
  std::string password_;
  uint64_t offset_ = 0;       // keystream bytes consumed so far
  uint64_t block_index_ = 0;  // keystream block currently cached in block_
  uint64_t block_ = 0;        // cached keystream_block(password_, block_index_)
  bool block_valid_ = false;  // block_ holds a valid cached block
};

// Builds a full encrypted container (magic + version + XOR payload) from
// `plain`. Always succeeds.
std::string encrypt_container(const std::string& password, const std::string& plain);

// Reverses encrypt_container(). Returns false (and leaves `out` empty) when
// `blob` is truncated, carries a wrong magic or an unknown version.
bool decrypt_container(const std::string& password, const std::string& blob,
                       std::string& out);

// True when `data` starts with the LXEF magic (used by tests and readers).
bool has_magic(const std::string& data);

}  // namespace xcrypt
}  // namespace logpipe
