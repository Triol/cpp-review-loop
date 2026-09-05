// util.h - shared facilities for logpipe: time access, diagnostics, CRC-32.
//
// Team convention: all time reads and all diagnostic output in the project
// MUST go through this module; other components never call clock/date
// functions or write to stderr directly. Time is needed for rotation
// fallback naming, RAW record stamping, diagnostics and run statistics.

#pragma once

#include <cstddef>   // size_t
#include <cstdint>   // int64_t, uint32_t
#include <string>

namespace logpipe {
namespace util {

// ------------------------------ time ----------------------------------------

// Milliseconds since the Unix epoch (wall clock).
int64_t now_ms();

// Monotonic clock in milliseconds; only for measuring elapsed intervals
// (differences between two calls), never as a wall-clock timestamp.
int64_t steady_now_ms();

// Local time as "YYYY-MM-DD HH:MM:SS.mmm".
std::string format_time_ms(int64_t epoch_ms);

// Local time as "YYYYMMDD_HHMMSS"; used by the writer when a rotation target
// name is taken and for run bookkeeping.
std::string timestamp_compact(int64_t epoch_ms);

// Human readable span, e.g. "850ms", "12.3s", "1m02s".
std::string format_duration(int64_t ms);

// --------------------------- diagnostics ------------------------------------

enum class DiagLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Installs the diagnostic sink: stderr when `file` is empty, otherwise the
// named file opened in append mode (falls back to stderr on failure). Safe
// to call again, e.g. once the configuration has been loaded.
void init_diag(DiagLevel level, const std::string& file);

// Parses "debug" | "info" | "warn" | "error" (case-insensitive).
bool diag_level_from_string(const std::string& text, DiagLevel& out);

// Thread-safe log helpers; messages are timestamped inside this module, e.g.
//   [2026-09-05 12:00:00.123] [INFO ] tailer started
void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);
void log_diag(DiagLevel level, const std::string& message);

// ---------------------------- checksums -------------------------------------

// CRC-32 (polynomial 0xEDB88320) used to stamp every rolling-output line.
uint32_t crc32(const char* data, size_t size);
uint32_t crc32(const std::string& text);

}  // namespace util
}  // namespace logpipe
