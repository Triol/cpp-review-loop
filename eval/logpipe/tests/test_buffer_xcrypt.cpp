// test_buffer_xcrypt.cpp - tests for this iteration's features:
//
//   1. batched write buffer (write.buffer_lines / write.buffer_bytes):
//      threshold-triggered flushes, the shutdown flush, rotation flushing,
//      and the Metrics buffer high-water marks;
//   2. configurable tail read block (read.chunk_bytes) via a small-chunk
//      tailing round trip;
//   3. the lightweight XOR keystream encryption (xcrypt.h): FNV-1a vectors,
//      keystream determinism, incremental == whole-blob, container round
//      trips (empty data, key longer than data, larger payloads), wrong
//      password / truncated container rejection, and the compress+encrypt
//      stacking round trip through RollingWriter + read_output_file();
//   4. secret masking: the encrypt.password value never appears in
//      describe() (--dump-config), the --check-config report or the env
//      override diagnostics, always "***" instead;
//   5. config parsing of the new keys including their bounds validation.
//
// The framework is shared via test_framework.h (TEST/CHECK macros live
// there); test_main.cpp runs every registered case.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "pipeline.h"
#include "tailer.h"
#include "test_framework.h"
#include "writer.h"
#include "xcrypt.h"

namespace fs = std::filesystem;

// Shared temp-dir helper (same pattern as the other test translation units).
namespace {

std::atomic<int> g_dir_seq{0};

fs::path make_temp_dir(const char* prefix) {
  const fs::path dir =
      fs::temp_directory_path() /
      (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(g_dir_seq.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void write_text_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

// Builds one plain text record with a unique message.
logpipe::LogRecord make_record(const std::string& message) {
  logpipe::LogRecord rec;
  rec.source = "test.log";
  rec.ingested_ms = 1700000000000ll;
  rec.timestamp_text = "2026-09-06 12:00:00";
  rec.level = logpipe::Level::Info;
  rec.message = message;
  rec.parsed = true;
  return rec;
}

// Reads the full --check-config report into a string via a real temp file
// (portable across the platforms the test binary targets).
std::string render_check_config(const std::string& conf_path,
                                const logpipe::Config& config) {
  const fs::path out_path = make_temp_dir("logpipe_report_") / "report.txt";
  std::FILE* out = std::fopen(out_path.string().c_str(), "wb");
  CHECK(out != nullptr);
  if (out == nullptr) return "";
  const int rc = logpipe::check_config_report(out, conf_path, config);
  std::fclose(out);
  CHECK_EQ_INT(rc, 0);
  const std::string text = read_text_file(out_path);
  std::error_code ec;
  fs::remove(out_path, ec);
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// xcrypt: hash and keystream primitives.
// ---------------------------------------------------------------------------
TEST(xcrypt_fnv1a_known_vectors) {
  // Standard FNV-1a 64-bit test vectors: the offset basis for the empty
  // string and the well-known value for "a".
  CHECK_EQ_INT(logpipe::xcrypt::fnv1a(""), 0xcbf29ce484222325ull);
  CHECK_EQ_INT(logpipe::xcrypt::fnv1a("a"), 0xaf63dc4c8601ec8cull);
  // Order matters (a real hash, not a sum).
  CHECK(logpipe::xcrypt::fnv1a("ab") != logpipe::xcrypt::fnv1a("ba"));
}

TEST(xcrypt_keystream_blocks_deterministic) {
  const std::string password = "round-trip-passphrase";
  const uint64_t block0 = logpipe::xcrypt::keystream_block(password, 0);
  const uint64_t block1 = logpipe::xcrypt::keystream_block(password, 1);
  const uint64_t block2 = logpipe::xcrypt::keystream_block(password, 2);
  // Block 0 is the plain password hash; successor blocks differ from their
  // predecessors and every call is independent of call order.
  CHECK_EQ_INT(block0, logpipe::xcrypt::fnv1a(password));
  CHECK(block0 != block1);
  CHECK(block1 != block2);
  CHECK_EQ_INT(block1, logpipe::xcrypt::keystream_block(password, 1));
  // A different password yields a different keystream.
  CHECK(block0 != logpipe::xcrypt::keystream_block("other", 0));
}

TEST(xcrypt_stream_incremental_matches_whole) {
  const std::string password = "chunked";
  std::string whole = "The quick brown fox jumps over the lazy dog, twice over.";
  std::string chunked = whole;
  logpipe::xcrypt::XorStream whole_stream(password);
  whole_stream.apply(whole);

  // Applying the same keystream in uneven chunks must produce exactly the
  // same bytes (the stream position carries across apply() calls).
  logpipe::xcrypt::XorStream chunk_stream(password);
  size_t offset = 0;
  size_t take = 1;
  while (offset < chunked.size()) {
    const size_t count = std::min(take, chunked.size() - offset);
    std::string piece = chunked.substr(offset, count);
    chunk_stream.apply(piece);
    for (size_t i = 0; i < count; ++i) chunked[offset + i] = piece[i];
    offset += count;
    take = take * 2 + 3;
  }
  CHECK_STR_EQ(chunked, whole);
  CHECK_EQ_INT(chunk_stream.offset(), whole_stream.offset());
}

TEST(xcrypt_container_round_trip) {
  const std::string password = "s3cret-passphrase";
  // (a) empty payload;
  {
    const std::string blob = logpipe::xcrypt::encrypt_container(password, "");
    CHECK_EQ_INT(blob.size(), logpipe::xcrypt::kHeaderSize);
    std::string back;
    CHECK(logpipe::xcrypt::decrypt_container(password, blob, back));
    CHECK(back.empty());
  }
  // (b) key material longer than the payload (password much longer than the
  // data exercises the single-block path);
  {
    const std::string plain = "hi";
    const std::string blob = logpipe::xcrypt::encrypt_container(password, plain);
    std::string back;
    CHECK(logpipe::xcrypt::decrypt_container(password, blob, back));
    CHECK_STR_EQ(back, plain);
  }
  // (c) multi-block payload with binary-hostile bytes.
  {
    std::string plain;
    for (int i = 0; i < 500; ++i) {
      plain.push_back(static_cast<char>((i * 7 + 1) & 0xFF));
    }
    const std::string blob = logpipe::xcrypt::encrypt_container(password, plain);
    CHECK(logpipe::xcrypt::has_magic(blob) == false);  // header is encrypted
    std::string back;
    CHECK(logpipe::xcrypt::decrypt_container(password, blob, back));
    CHECK_STR_EQ(back, plain);
  }
}

TEST(xcrypt_container_rejects_bad_input) {
  const std::string password = "right";
  const std::string blob = logpipe::xcrypt::encrypt_container(password, "payload");
  std::string out;
  // Wrong password: magic check fails after decryption.
  CHECK(!logpipe::xcrypt::decrypt_container("wrong", blob, out));
  CHECK(out.empty());
  // Truncated container (below header size).
  CHECK(!logpipe::xcrypt::decrypt_container(password, "LX", out));
  // Garbage input.
  CHECK(!logpipe::xcrypt::decrypt_container(password, "not a container at all", out));
}

// ---------------------------------------------------------------------------
// RollingWriter: buffer thresholds, shutdown flush, watermarks.
// ---------------------------------------------------------------------------
TEST(writer_buffer_lines_threshold_and_close_flush) {
  const fs::path dir = make_temp_dir("logpipe_buf_lines_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions options;
  options.output_dir = dir;
  options.base_name = "out.log";
  options.buffer_lines = 3;  // flush every third line

  logpipe::RollingWriter writer(options, &metrics);
  CHECK(writer.open());

  // Two lines stay purely in memory: the file must not exist yet.
  uint64_t bytes = 0;
  CHECK(writer.write(make_record("buffered-one"), bytes));
  CHECK(writer.write(make_record("buffered-two"), bytes));
  const fs::path active = dir / "out.log";
  CHECK(!fs::exists(active) || read_text_file(active).empty());

  // Third line crosses the lines threshold: all three spill at once.
  CHECK(writer.write(make_record("flushing-three"), bytes));
  const std::string after_threshold = read_text_file(active);
  CHECK(after_threshold.find("buffered-one") != std::string::npos);
  CHECK(after_threshold.find("buffered-two") != std::string::npos);
  CHECK(after_threshold.find("flushing-three") != std::string::npos);

  // One more line stays buffered until close() flushes it.
  CHECK(writer.write(make_record("pending-four"), bytes));
  CHECK(read_text_file(active).find("pending-four") == std::string::npos);
  writer.close();
  CHECK(read_text_file(active).find("pending-four") != std::string::npos);
  CHECK(!writer.failed());

  // High-water marks: the buffer peaked at 3 lines (watermark is sampled
  // before the threshold flush) and at whatever those lines weighed.
  CHECK_EQ_INT(metrics.buffer_lines_high_water(), 3);
  CHECK(metrics.buffer_bytes_high_water() > 0);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(writer_buffer_bytes_threshold_flushes_every_line) {
  const fs::path dir = make_temp_dir("logpipe_buf_bytes_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions options;
  options.output_dir = dir;
  options.base_name = "out.log";
  options.buffer_bytes = 1;  // any line crosses the byte threshold

  logpipe::RollingWriter writer(options, &metrics);
  CHECK(writer.open());
  uint64_t bytes = 0;
  CHECK(writer.write(make_record("immediate-one"), bytes));
  const fs::path active = dir / "out.log";
  // Bytes threshold crossed: the first line is already on disk.
  CHECK(read_text_file(active).find("immediate-one") != std::string::npos);
  CHECK(writer.write(make_record("immediate-two"), bytes));
  CHECK(read_text_file(active).find("immediate-two") != std::string::npos);
  writer.close();
  CHECK(!writer.failed());
  // Watermark: one line's worth of bytes (flush happens before the next
  // line can stack up).
  CHECK_EQ_INT(metrics.buffer_lines_high_water(), 1);
  CHECK(metrics.buffer_bytes_high_water() >= 10);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(writer_unbuffered_watermark_stays_at_line_count) {
  const fs::path dir = make_temp_dir("logpipe_buf_none_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions options;
  options.output_dir = dir;
  options.base_name = "out.log";
  // Both thresholds 0 = unbuffered; the watermark still reports what sat in
  // the (immediately flushed) buffer.
  logpipe::RollingWriter writer(options, &metrics);
  CHECK(writer.open());
  uint64_t bytes = 0;
  for (int i = 0; i < 5; ++i) {
    CHECK(writer.write(make_record("line-" + std::to_string(i)), bytes));
  }
  writer.close();
  CHECK_EQ_INT(metrics.buffer_lines_high_water(), 1);
  const std::string text = read_text_file(dir / "out.log");
  CHECK(text.find("line-4") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(writer_rotation_flushes_pending_buffer) {
  const fs::path dir = make_temp_dir("logpipe_buf_rotate_");
  logpipe::Metrics metrics;
  logpipe::WriterOptions options;
  options.output_dir = dir;
  options.base_name = "out.log";
  options.max_bytes_per_file = 256;  // force a size rotation quickly
  options.buffer_lines = 100;        // big line buffer: rotation must flush it

  logpipe::RollingWriter writer(options, &metrics);
  CHECK(writer.open());
  uint64_t bytes = 0;
  for (int i = 0; i < 4; ++i) {
    std::ostringstream message;
    message << "rotation-payload-" << i << "-" << std::string(60, 'x');
    CHECK(writer.write(make_record(message.str()), bytes));
  }
  writer.close();
  CHECK(!writer.failed());
  // The lines that were still buffered when the size trigger fired must all
  // be somewhere: active file or backup chain.
  std::string everything = read_text_file(dir / "out.log");
  for (int index = 1; index <= 3; ++index) {
    everything += read_text_file(dir / ("out_" + std::to_string(index) + ".log"));
  }
  for (int i = 0; i < 4; ++i) {
    CHECK(everything.find("rotation-payload-" + std::to_string(i)) != std::string::npos);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// RollingWriter: encryption and compress+encrypt stacking.
// ---------------------------------------------------------------------------
TEST(writer_encrypt_round_trip) {
  const fs::path dir = make_temp_dir("logpipe_enc_");
  const std::string password = "round-trip-password";
  logpipe::WriterOptions options;
  options.output_dir = dir;
  options.base_name = "out.log";
  options.encrypt_password = password;

  logpipe::RollingWriter writer(options);
  CHECK(writer.open());
  uint64_t bytes = 0;
  CHECK(writer.write(make_record("secret-one"), bytes));
  CHECK(writer.write(make_record("secret-two"), bytes));
  writer.close();
  CHECK(!writer.failed());

  const fs::path active = dir / "out.log.enc";
  CHECK(fs::exists(active));
  const std::string raw = read_text_file(active);
  // The container header is encrypted: no plaintext magic or plaintext
  // messages on disk.
  CHECK(raw.find("secret-one") == std::string::npos);
  CHECK(raw.find("secret-two") == std::string::npos);

  std::string back;
  CHECK(logpipe::read_output_file(active, password, /*compressed=*/false, back));
  CHECK(back.find("secret-one") != std::string::npos);
  CHECK(back.find("secret-two") != std::string::npos);

  // Wrong password must fail cleanly.
  std::string wrong;
  CHECK(!logpipe::read_output_file(active, "not-the-password", false, wrong));
  CHECK(wrong.empty());

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(writer_compress_encrypt_stacked_round_trip) {
  const fs::path dir = make_temp_dir("logpipe_enc_rle_");
  const std::string password = "stacked-secret";
  logpipe::WriterOptions options;
  options.output_dir = dir;
  options.base_name = "out.log";
  options.compress = logpipe::CompressMode::Rle;  // applied FIRST...
  options.encrypt_password = password;            // ...then encryption

  logpipe::RollingWriter writer(options);
  CHECK(writer.open());
  uint64_t bytes = 0;
  CHECK(writer.write(make_record("aaa bbb ccc ddd eee"), bytes));
  CHECK(writer.write(make_record("zzz yyy xxx www vvv"), bytes));
  writer.close();
  CHECK(!writer.failed());

  const fs::path active = dir / "out.log.rle.enc";
  CHECK(fs::exists(active));
  // Neither the plaintext nor the RLE containers appear verbatim.
  const std::string raw = read_text_file(active);
  CHECK(raw.find("aaa bbb ccc") == std::string::npos);

  // read_output_file reverses in the inverse order: decrypt, then decode.
  std::string back;
  CHECK(logpipe::read_output_file(active, password, /*compressed=*/true, back));
  CHECK(back.find("aaa bbb ccc ddd eee") != std::string::npos);
  CHECK(back.find("zzz yyy xxx www vvv") != std::string::npos);
  // With the compressed flag cleared the RLE frames look like garbage: the
  // decode order matters.
  std::string undecoded;
  CHECK(!logpipe::read_output_file(active, password, false, undecoded) ||
        undecoded.find("aaa bbb ccc") == std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Metrics watermark bookkeeping.
// ---------------------------------------------------------------------------
TEST(metrics_buffer_watermark_is_monotonic) {
  logpipe::Metrics metrics;
  metrics.record_buffer_watermark(5, 400);
  // Smaller values never lower the high-water marks.
  metrics.record_buffer_watermark(3, 100);
  CHECK_EQ_INT(metrics.buffer_lines_high_water(), 5);
  CHECK_EQ_INT(metrics.buffer_bytes_high_water(), 400);
  metrics.record_buffer_watermark(7, 350);
  CHECK_EQ_INT(metrics.buffer_lines_high_water(), 7);
  CHECK_EQ_INT(metrics.buffer_bytes_high_water(), 400);
}

// ---------------------------------------------------------------------------
// Config: new keys, bounds validation and secret masking.
// ---------------------------------------------------------------------------
namespace {

// Writes a minimal valid config with the given extra body.
std::string write_conf(const fs::path& dir, const std::string& body) {
  const fs::path conf = dir / "logpipe.conf";
  write_text_file(conf,
                  "input.files = in.log\n"
                  "output.dir = out\n"
                  + body + "\n");
  return conf.string();
}

}  // namespace

TEST(config_buffer_read_queue_keys_parse) {
  const fs::path dir = make_temp_dir("logpipe_cfg_keys_");
  const std::string conf = write_conf(dir,
      "write.buffer_lines = 25\n"
      "write.buffer_bytes = 65536\n"
      "read.chunk_bytes = 4096\n"
      "queue.capacity = 64\n");
  const logpipe::Config config = logpipe::Config::load(conf);
  CHECK_EQ_INT(config.write_buffer_lines, 25);
  CHECK_EQ_INT(config.write_buffer_bytes, 65536);
  CHECK_EQ_INT(config.read_chunk_bytes, 4096);
  CHECK_EQ_INT(config.queue_capacity, 64);
  CHECK(config.encrypt_password.empty());  // not set here

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(config_bounds_rejected) {
  const fs::path dir = make_temp_dir("logpipe_cfg_bounds_");
  struct Case {
    const char* body;
  };
  const char* bodies[] = {
      "read.chunk_bytes = 1\n",        // below the 128-byte floor
      "read.chunk_bytes = 2000000\n",  // above the 1 MiB ceiling
      "read.chunk_bytes = abc\n",      // not a number
      "queue.capacity = 0\n",          // below the 1-line floor
      "queue.capacity = 2000000\n",    // above the ceiling
      "write.buffer_lines = 999999999\n",
      "write.buffer_lines = -3\n",
      "write.buffer_bytes = 999999999999\n",
  };
  for (const char* body : bodies) {
    bool threw = false;
    try {
      const std::string conf = write_conf(dir, body);
      logpipe::Config::load(conf);
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);  // every out-of-range value must abort the load
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(config_password_masked_in_describe_and_report) {
  const fs::path dir = make_temp_dir("logpipe_cfg_mask_");
  const std::string conf = write_conf(dir,
      "write.buffer_lines = 5\n"
      "encrypt.password = super-secret-value\n");
  const logpipe::Config config = logpipe::Config::load(conf);
  CHECK_STR_EQ(config.encrypt_password, "super-secret-value");

  // --dump-config path: describe() carries the mask, never the secret.
  const std::string dumped = config.describe();
  CHECK(dumped.find("***") != std::string::npos);
  CHECK(dumped.find("super-secret-value") == std::string::npos);

  // --check-config path: the report row for encrypt.password is masked too.
  const std::string report = render_check_config(conf, config);
  CHECK(report.find("encrypt.password") != std::string::npos);
  CHECK(report.find("***") != std::string::npos);
  CHECK(report.find("super-secret-value") == std::string::npos);

  // The dump_config helper (the actual --dump-config switch) also masks.
  const fs::path dump_path = dir / "dump.txt";
  std::FILE* dump_file = std::fopen(dump_path.string().c_str(), "wb");
  CHECK(dump_file != nullptr);
  CHECK_EQ_INT(logpipe::dump_config(dump_file, config), 0);
  std::fclose(dump_file);
  const std::string dump_text = read_text_file(dump_path);
  CHECK(dump_text.find("super-secret-value") == std::string::npos);
  CHECK(dump_text.find("***") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(config_password_env_override_masked) {
  const fs::path dir = make_temp_dir("logpipe_cfg_env_");
  const std::string conf = write_conf(dir, "encrypt.password = file-secret\n");
  // Environment override layer: the override log line must not leak the
  // env-supplied secret either.
#if defined(_WIN32)
  _putenv("LOGPIPE_ENCRYPT_PASSWORD=env-secret-value");
#else
  ::setenv("LOGPIPE_ENCRYPT_PASSWORD", "env-secret-value", 1);
#endif
  std::string describe_text;
  std::string report_text;
  {
    const logpipe::Config config = logpipe::Config::load(conf);  // env on by default
    CHECK_STR_EQ(config.encrypt_password, "env-secret-value");
    describe_text = config.describe();
    report_text = render_check_config(conf, config);
  }
#if defined(_WIN32)
  _putenv("LOGPIPE_ENCRYPT_PASSWORD=");
#else
  ::unsetenv("LOGPIPE_ENCRYPT_PASSWORD");
#endif
  CHECK(describe_text.find("env-secret-value") == std::string::npos);
  CHECK(describe_text.find("***") != std::string::npos);
  CHECK(report_text.find("env-secret-value") == std::string::npos);
  // The report source column still credits the env layer.
  CHECK(report_text.find("env") != std::string::npos);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Tailer: read.chunk_bytes actually drives the read block size (small chunks
// must still assemble complete lines).
// ---------------------------------------------------------------------------
TEST(tailer_small_chunk_bytes_still_emits_lines) {
  const fs::path dir = make_temp_dir("logpipe_tail_chunk_");
  const fs::path input = dir / "in.log";
  write_text_file(input, "first-line-longer-than-chunk\nsecond-line-longer\nthird\n");

  logpipe::BlockingQueue<logpipe::RawLine> queue(16);
  logpipe::TailOptions options;
  options.poll_ms = 10;
  options.chunk_bytes = 4;  // tiny read blocks on purpose
  logpipe::Tailer tailer({input}, queue, options);

  std::thread reader([&tailer] { tailer.run(); });
  std::vector<std::string> lines;
  for (;;) {
    logpipe::RawLine raw;
    const auto popped = queue.pop_for(raw, 2000);
    if (popped != logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) break;
    lines.push_back(raw.text);
  }
  tailer.request_stop();
  reader.join();

  CHECK_EQ_INT(lines.size(), 3);
  CHECK(lines.size() == 3 && lines[0] == "first-line-longer-than-chunk");
  CHECK(lines.size() > 1 && lines[1] == "second-line-longer");
  CHECK(lines.size() > 2 && lines[2] == "third");

  std::error_code ec;
  fs::remove_all(dir, ec);
}
