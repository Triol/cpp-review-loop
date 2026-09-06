// xcrypt.cpp - implementation of the XOR keystream cipher (see xcrypt.h).

#include "xcrypt.h"

#include <cstring>

namespace logpipe {
namespace xcrypt {

uint64_t fnv1a_step(uint64_t hash, uint8_t byte) {
  // FNV-1a: XOR the byte into the low bits, then multiply by the 64-bit
  // FNV prime.
  hash ^= static_cast<uint64_t>(byte);
  return hash * 0x100000001b3ull;
}

uint64_t fnv1a(const std::string& data) {
  // FNV-1a offset basis.
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const char c : data) {
    hash = fnv1a_step(hash, static_cast<uint8_t>(c));
  }
  return hash;
}

uint64_t keystream_block(const std::string& password, uint64_t index) {
  // h_0 = FNV-1a(password); h_{i+1} = FNV-1a over the 8 LE bytes of h_i.
  uint64_t block = fnv1a(password);
  for (uint64_t i = 0; i < index; ++i) {
    uint8_t bytes[8];
    for (int lane = 0; lane < 8; ++lane) {
      bytes[lane] = static_cast<uint8_t>((block >> (8 * lane)) & 0xFFu);
    }
    // Re-hash the block bytes with the block index mixed in so equal blocks
    // cannot produce identical successor blocks through a degenerate cycle.
    uint64_t next = fnv1a_step(0xcbf29ce484222325ull, bytes[0]);
    for (int lane = 1; lane < 8; ++lane) {
      next = fnv1a_step(next, bytes[lane]);
    }
    next = fnv1a_step(next, static_cast<uint8_t>(index & 0xFFu));
    block = next * 0x100000001b3ull;
  }
  return block;
}

XorStream::XorStream(std::string password) : password_(std::move(password)) {}

void XorStream::apply(std::string& data) {
  for (char& c : data) {
    if (!block_valid_ || block_index_ != offset_ / 8) {
      block_index_ = offset_ / 8;
      block_ = keystream_block(password_, block_index_);
      block_valid_ = true;
    }
    const int lane = static_cast<int>(offset_ % 8);
    const uint8_t key_byte = static_cast<uint8_t>((block_ >> (8 * lane)) & 0xFFu);
    c = static_cast<char>(static_cast<uint8_t>(c) ^ key_byte);
    ++offset_;
  }
}

bool xcrypt_has_magic_impl(const std::string& data) {
  return data.size() >= 4 && std::memcmp(data.data(), kMagic, 4) == 0;
}

bool has_magic(const std::string& data) { return xcrypt_has_magic_impl(data); }

std::string encrypt_container(const std::string& password, const std::string& plain) {
  std::string blob;
  blob.reserve(kHeaderSize + plain.size());
  blob.append(kMagic, 4);
  blob.push_back(static_cast<char>(kVersion));
  blob += plain;
  XorStream stream(password);
  stream.apply(blob);  // header bytes are XORed too: keeps the format uniform
  return blob;
}

bool decrypt_container(const std::string& password, const std::string& blob,
                       std::string& out) {
  out.clear();
  if (blob.size() < kHeaderSize) return false;
  // The header is XORed together with the payload (the keystream starts at
  // offset 0 over the whole container), so decrypt first and then validate
  // magic and version on the decoded bytes.
  std::string decoded = blob;
  XorStream stream(password);
  stream.apply(decoded);
  if (!has_magic(decoded)) return false;
  if (static_cast<uint8_t>(decoded[4]) != kVersion) return false;
  out = decoded.substr(kHeaderSize);
  return true;
}

}  // namespace xcrypt
}  // namespace logpipe
