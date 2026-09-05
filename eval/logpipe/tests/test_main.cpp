// test_main.cpp - minimal zero-dependency assertion runner for logpipe.
//
// No third-party framework: tests register themselves via the TEST macro,
// main() runs them in order and exits non-zero when any CHECK failed.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "config.h"
#include "pipeline.h"
#include "tailer.h"

namespace fs = std::filesystem;

namespace testfw {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> registry;
  return registry;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int& failures() {
  static int count = 0;
  return count;
}

}  // namespace testfw

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++testfw::failures();                                                      \
      std::fprintf(stderr, "  CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,  \
                   #cond);                                                       \
    }                                                                            \
  } while (0)

// Equality check for values printable with c_str() (std::string and const char*).
#define CHECK_STR_EQ(actual, expected)                                           \
  do {                                                                           \
    const std::string& a_ = (actual);                                            \
    const std::string& e_ = (expected);                                          \
    if (a_ != e_) {                                                              \
      ++testfw::failures();                                                      \
      std::fprintf(stderr, "  CHECK_STR_EQ failed at %s:%d\n    actual  : \"%s\"\n" \
                           "    expected: \"%s\"\n",                             \
                   __FILE__, __LINE__, a_.c_str(), e_.c_str());                  \
    }                                                                            \
  } while (0)

#define TEST(name)                                                     \
  static void name();                                                  \
  static const testfw::Registrar registrar_##name(#name, &name);       \
  static void name()

// ---------------------------------------------------------------------------
// F1 regression: after a short read hits EOF, istream sets eofbit|failbit and
// the sentry makes every further read a no-op. poll_file() must clear the
// stream state inside its loop so lines appended to the file after the tailer
// reached EOF are still collected.
// ---------------------------------------------------------------------------
TEST(tailer_reads_appended_lines_after_eof) {
  static std::atomic<int> seq{0};
  const fs::path dir =
      fs::temp_directory_path() /
      ("logpipe_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(seq.fetch_add(1)));
  fs::create_directories(dir);
  const fs::path input = dir / "input.log";
  {
    std::ofstream out(input, std::ios::binary | std::ios::trunc);
    out << "line1\n";
  }

  logpipe::BlockingQueue<logpipe::RawLine> queue(64);
  logpipe::TailOptions options;
  options.poll_ms = 10;  // fast polling; offset_file stays empty (no persistence)
  logpipe::Tailer tailer({input}, queue, options);
  std::thread tailer_thread([&tailer] { tailer.run(); });

  // Pops with an overall deadline; returns the last line's text or "" on timeout.
  auto pop_text = [&](int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline && !queue.closed()) {
      logpipe::RawLine raw;
      if (queue.pop_for(raw, 25) ==
          logpipe::BlockingQueue<logpipe::RawLine>::PopResult::Got) {
        return raw.text;
      }
    }
    return std::string();
  };

  CHECK_STR_EQ(pop_text(5000), "line1");

  {
    std::ofstream out(input, std::ios::binary | std::ios::app);
    out << "line2\n";
  }

  // Without the fix the tailer is stuck in EOF|fail state and times out here.
  CHECK_STR_EQ(pop_text(3000), "line2");

  tailer.request_stop();
  tailer_thread.join();

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// F7: all elapsed-time reads go through the util module. steady_now_ms()
// exposes the monotonic clock in milliseconds for interval measurement.
// ---------------------------------------------------------------------------
TEST(steady_now_ms_is_monotonic_ms) {
  const int64_t first = logpipe::util::steady_now_ms();
  const int64_t second = logpipe::util::steady_now_ms();
  CHECK(first > 0);        // millisecond magnitude since an arbitrary epoch
  CHECK(second >= first);  // monotonic clock never goes backwards
}

// ---------------------------------------------------------------------------
// F20: the LogParser raw-string regex was terminated early by the `)"` inside
// `(\d{1,6})?)`, so construction threw std::regex_error and every run aborted
// at startup. Construction must succeed and the pattern must parse the
// canonical "timestamp [LEVEL] message" line, falling back to RAW when the
// level word is unknown.
// ---------------------------------------------------------------------------
TEST(log_parser_parses_canonical_line) {
  std::optional<logpipe::LogParser> parser;
  try {
    parser.emplace();
  } catch (const std::exception& e) {
    // F20 red: construction itself fails before any line can be parsed.
    std::fprintf(stderr, "  LogParser() threw: %s\n", e.what());
    ++testfw::failures();
    return;
  }

  const logpipe::LogRecord rec =
      parser->parse("in.log", 1234567, "2026-08-30 12:00:00 [INFO] hello world");
  CHECK(rec.parsed);
  CHECK(rec.level == logpipe::Level::Info);
  CHECK_STR_EQ(rec.timestamp_text, "2026-08-30 12:00:00");
  CHECK_STR_EQ(rec.message, "hello world");

  // Unknown level word: falls back to a verbatim RAW record.
  const logpipe::LogRecord raw =
      parser->parse("in.log", 1234567, "2026-08-30 12:00:00 [WIBBER] x");
  CHECK(!raw.parsed);
  CHECK(raw.level == logpipe::Level::Raw);
  CHECK_STR_EQ(raw.message, "2026-08-30 12:00:00 [WIBBER] x");
}

// ---------------------------------------------------------------------------
// F4: std::stoull fully consumes "-5" and wraps around to a huge value, so
// the rotate.size_bytes lower bound cannot reject it. Signed values must be
// rejected before parsing.
// ---------------------------------------------------------------------------
TEST(config_rejects_negative_rotate_size) {
  static std::atomic<int> cfg_seq{0};
  const fs::path dir =
      fs::temp_directory_path() /
      ("logpipe_cfg_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(cfg_seq.fetch_add(1)));
  fs::create_directories(dir);
  const fs::path cfg = dir / "negative.conf";
  {
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << "input.files = dummy.log\n"
        << "output.file = out.log\n"
        << "rotate.size_bytes = -5\n";
  }

  bool threw = false;
  try {
    logpipe::Config::load(cfg.string());
  } catch (const std::exception& e) {
    threw = true;
    std::fprintf(stderr, "  Config::load threw: %s\n", e.what());
  }
  CHECK(threw);  // F4 red: without the fix the wrapped value passes validation

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------

int main() {
  size_t failed_tests = 0;
  for (const auto& tc : testfw::registry()) {
    testfw::failures() = 0;
    std::fprintf(stderr, "[ RUN  ] %s\n", tc.name);
    tc.fn();
    if (testfw::failures() == 0) {
      std::fprintf(stderr, "[  OK  ] %s\n", tc.name);
    } else {
      std::fprintf(stderr, "[ FAIL ] %s (%d check(s) failed)\n", tc.name,
                   testfw::failures());
      ++failed_tests;
    }
  }
  std::fprintf(stderr, "%s: %zu test(s), %zu failed\n",
               failed_tests == 0 ? "PASSED" : "FAILED", testfw::registry().size(),
               failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
