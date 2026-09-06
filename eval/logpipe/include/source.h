// source.h - the input source abstraction: ISource is the common interface
// every producer of RawLine values implements (start/stop/poll semantics).
//
// The pipeline consumes lines through a shared BlockingQueue<RawLine> (see
// pipeline.h). Implementations own their producer thread (start()/join()),
// honour request_stop() promptly, and may additionally expose poll() for
// periodic non-blocking work driven by an external scheduler (the glob source
// uses discovery work inside its own loop, but poll() keeps the contract
// uniform and is exercised by tests).
//
// Implementations:
//   Tailer      - tail mode over a fixed set of files (tailer.h)
//   StdinReader - standard input (stdin_reader.h)
//   GlobSource  - pattern-discovered files, tail mode (glob_source.h)

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "pipeline.h"  // RawLine, SourceType, BlockingQueue

namespace logpipe {

class ISource {
 public:
  virtual ~ISource() = default;

  // Kind of this source (per-type Metrics columns).
  virtual SourceType type() const = 0;

  // Human-readable identity used in diagnostics and per-source metrics
  // (a file path, "stdin", or "glob:<pattern>").
  virtual std::string name() const = 0;

  // Starts producing: spawns the producer thread (or otherwise begins the
  // producing work). Must be called at most once per instance.
  virtual void start() = 0;

  // Requests a prompt, cooperative stop. Safe to call more than once and
  // before start() (a source that never started must not report dropped
  // lines and must not touch the queue on destruction).
  virtual void request_stop() = 0;

  // One round of non-blocking periodic work (e.g. glob discovery). Sources
  // that drive everything from their own thread implement it as a no-op.
  virtual void poll() {}

  // Waits for the producer thread spawned by start() (no-op when the source
  // runs without a thread or was never started).
  virtual void join() = 0;

  // True once the producer finished (EOF reached or stop processed).
  virtual bool finished() const = 0;

  // Lines given up on while the queue stayed full during shutdown.
  virtual uint64_t dropped_lines() const = 0;
};

}  // namespace logpipe
