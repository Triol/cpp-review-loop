// version.h - project identity constants and the versioned output header.
//
// Every freshly created output file (the very first active file and every
// rotation product: size rotation, day rotation, fallback names, per-source
// files, fan-out branches) starts with one self-describing header line:
//
//   #logpipe v1.0.0 <format> created=<UTC>
//
// e.g. "#logpipe v1.0.0 json_lines created=2026-09-12T07:15:00Z". The header
// is part of the encoded output stream: compressed output carries it as the
// first RLE frame, encrypted output as the first payload chunk after the LXEF
// magic, so a consumer that reverses the encodings always sees it as line 1.
// Files that are APPENDED to (a resumed run reusing a non-empty active file,
// a same-day daily-rotation file) are not stamped again - the header marks a
// file's birth, not each write session.
//
// The created= stamp is true UTC (util::format_utc_time_sec), not local time,
// so headers written on machines in different timezones stay comparable.

#pragma once

#include <cstdint>
#include <string>

#include "util.h"  // now_ms, format_utc_time_sec

namespace logpipe {

// Project identity. kProjectVersion mirrors project(VERSION 1.0.0) in
// CMakeLists.txt - keep the two in sync when bumping the release number.
inline constexpr char kProjectName[] = "logpipe";
inline constexpr char kProjectVersion[] = "1.0.0";

// Builds the versioned output header line for `format` ("text" or
// "json_lines"), stamped with `epoch_ms` converted to UTC. Exposed so writers
// and tests can predict the exact bytes a fresh output file starts with.
inline std::string output_version_header(const std::string& format,
                                         int64_t epoch_ms) {
  return std::string("#") + kProjectName + " v" + kProjectVersion + " " +
         format + " created=" + util::format_utc_time_sec(epoch_ms);
}

// As above, stamped with the current wall clock (via the util module).
inline std::string output_version_header(const std::string& format) {
  return output_version_header(format, util::now_ms());
}

// True when `line` looks like a versioned output header produced by
// output_version_header(): starts with "#logpipe v<version> " and carries a
// "created=<UTC>" stamp. Consumers can use this to skip the header line; the
// writer itself only ever emits it as the first line of a fresh file.
inline bool is_version_header(const std::string& line) {
  const std::string prefix =
      std::string("#") + kProjectName + " v" + kProjectVersion + " ";
  if (line.rfind(prefix, 0) != 0) return false;
  return line.find(" created=") != std::string::npos;
}

}  // namespace logpipe
