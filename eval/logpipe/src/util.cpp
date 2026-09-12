// util.cpp - implementation of the shared time, diagnostics and CRC-32
// facilities. See util.h for the team conventions this module enforces.

#include "util.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>

namespace logpipe {
namespace util {
namespace {

std::tm to_local_tm(int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm value{};
#ifdef _WIN32
  localtime_s(&value, &seconds);
#else
  localtime_r(&seconds, &value);
#endif
  return value;
}

// ---- diagnostic sink state, guarded by diag_mutex ---------------------------
std::mutex diag_mutex;
DiagLevel diag_level = DiagLevel::Info;
std::unique_ptr<std::ofstream> diag_file_stream;  // non-null when logging to a file
std::ostream* diag_sink = &std::cerr;

const char* diag_tag(DiagLevel level) {
  switch (level) {
    case DiagLevel::Debug: return "DEBUG";
    case DiagLevel::Info:  return "INFO ";
    case DiagLevel::Warn:  return "WARN ";
    case DiagLevel::Error: return "ERROR";
  }
  return "??????";
}

// CRC-32 lookup table (polynomial 0xEDB88320). Function-local statics have
// thread-safe initialization, so the table is built exactly once on demand.
struct Crc32Table {
  uint32_t values[256];
  Crc32Table() {
    for (uint32_t byte = 0; byte < 256; ++byte) {
      uint32_t crc = byte;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
      }
      values[byte] = crc;
    }
  }
};

}  // namespace

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t steady_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string format_time_ms(int64_t epoch_ms) {
  int64_t millis = epoch_ms % 1000;
  if (millis < 0) millis += 1000;
  const std::tm tm_value = to_local_tm(epoch_ms);
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tm_value.tm_year + 1900, tm_value.tm_mon + 1, tm_value.tm_mday,
                tm_value.tm_hour, tm_value.tm_min, tm_value.tm_sec,
                static_cast<int>(millis));
  return buffer;
}

std::string timestamp_compact(int64_t epoch_ms) {
  const std::tm tm_value = to_local_tm(epoch_ms);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d_%02d%02d%02d",
                tm_value.tm_year + 1900, tm_value.tm_mon + 1, tm_value.tm_mday,
                tm_value.tm_hour, tm_value.tm_min, tm_value.tm_sec);
  return buffer;
}

bool parse_datetime_ms(const std::string& text, int64_t& out_ms) {
  // Strict shape check: "YYYY-MM-DD HH:MM:SS" (19 chars), tolerating 'T' as
  // the separator and an optional fractional part (".fff") afterwards. Every
  // digit position is verified explicitly so garbage like "2026-99-99..." is
  // rejected by the range checks below instead of being normalized away.
  if (text.size() < 19) return false;
  static const char* kShapes = "####-##-##X##:##:##";
  for (int i = 0; i < 19; ++i) {
    const char shape = kShapes[i];
    const char c = text[static_cast<size_t>(i)];
    if (shape == '#') {
      if (c < '0' || c > '9') return false;
    } else if (shape == 'X') {
      if (c != ' ' && c != 'T' && c != 't') return false;
    } else if (c != shape) {
      return false;
    }
  }
  auto digits = [&text](size_t from, size_t count) {
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
      value = value * 10 + (text[from + i] - '0');
    }
    return value;
  };
  const int year = digits(0, 4);
  const int month = digits(5, 2);
  const int day = digits(8, 2);
  const int hour = digits(11, 2);
  const int minute = digits(14, 2);
  const int second = digits(17, 2);
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour > 23 || minute > 59 || second > 60) return false;  // 60: leap second

  int millis = 0;
  if (text.size() > 19 && text[19] == '.') {
    size_t i = 20;
    int digits_seen = 0;
    int fraction = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits_seen < 3) {
      fraction = fraction * 10 + (text[i] - '0');
      ++i;
      ++digits_seen;
    }
    while (digits_seen < 3) {  // ".5" == 500ms
      fraction *= 10;
      ++digits_seen;
    }
    millis = fraction;
  }

  std::tm tm_value{};
  tm_value.tm_year = year - 1900;
  tm_value.tm_mon = month - 1;
  tm_value.tm_mday = day;
  tm_value.tm_hour = hour;
  tm_value.tm_min = minute;
  tm_value.tm_sec = second;
  tm_value.tm_isdst = -1;  // let mktime resolve daylight saving time
  const std::time_t seconds = std::mktime(&tm_value);
  if (seconds == static_cast<std::time_t>(-1)) return false;
  out_ms = static_cast<int64_t>(seconds) * 1000 + millis;
  return true;
}

std::string format_duration(int64_t ms) {
  if (ms < 0) ms = 0;
  std::ostringstream out;
  if (ms < 1000) {
    out << ms << "ms";
  } else if (ms < 60 * 1000) {
    out << (ms / 1000) << '.' << ((ms % 1000) / 100) << 's';
  } else {
    const int64_t total_seconds = ms / 1000;
    out << (total_seconds / 60) << 'm' << std::setw(2) << std::setfill('0')
        << (total_seconds % 60) << 's';
  }
  return out.str();
}

void init_diag(DiagLevel level, const std::string& file) {
  std::lock_guard<std::mutex> lock(diag_mutex);
  diag_level = level;
  diag_file_stream.reset();
  diag_sink = &std::cerr;
  if (file.empty()) return;
  auto stream = std::make_unique<std::ofstream>(file, std::ios::app);
  if (!stream->is_open()) {
    // Keep stderr and report the problem through it now that the sink is set.
    *diag_sink << "[" << format_time_ms(now_ms()) << "] [WARN ] cannot open diag file '"
               << file << "', falling back to stderr\n";
    return;
  }
  diag_file_stream = std::move(stream);
  diag_sink = diag_file_stream.get();
}

bool diag_level_from_string(const std::string& text, DiagLevel& out) {
  std::string upper;
  upper.reserve(text.size());
  for (char c : text) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (upper == "DEBUG") { out = DiagLevel::Debug; return true; }
  if (upper == "INFO")  { out = DiagLevel::Info;  return true; }
  if (upper == "WARN" || upper == "WARNING") { out = DiagLevel::Warn; return true; }
  if (upper == "ERROR") { out = DiagLevel::Error; return true; }
  return false;
}

void log_diag(DiagLevel level, const std::string& message) {
  std::lock_guard<std::mutex> lock(diag_mutex);
  if (static_cast<int>(level) < static_cast<int>(diag_level)) return;
  std::ostream& sink = *diag_sink;
  sink << '[' << format_time_ms(now_ms()) << "] [" << diag_tag(level) << "] " << message
       << '\n';
  sink.flush();
}

void log_debug(const std::string& message) { log_diag(DiagLevel::Debug, message); }
void log_info(const std::string& message)  { log_diag(DiagLevel::Info, message); }
void log_warn(const std::string& message)  { log_diag(DiagLevel::Warn, message); }
void log_error(const std::string& message) { log_diag(DiagLevel::Error, message); }

uint32_t crc32(const char* data, size_t size) {
  static const Crc32Table table;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t index = 0; index < size; ++index) {
    crc = table.values[(crc ^ static_cast<unsigned char>(data[index])) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32(const std::string& text) { return crc32(text.data(), text.size()); }

}  // namespace util
}  // namespace logpipe
