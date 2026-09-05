// stdin_reader.cpp - implementation of the standard-input line source: one
// blocking getline per line, pushed onto the shared bounded queue.

#include "stdin_reader.h"

#include <iostream>
#include <string>

#include "util.h"

namespace logpipe {
namespace {

constexpr char kSourceName[] = "stdin";

}  // namespace

StdinReader::StdinReader(BlockingQueue<RawLine>& queue, bool close_queue_on_exit)
    : queue_(queue), close_queue_on_exit_(close_queue_on_exit) {}

void StdinReader::run() {
  util::log_info("stdin reader: started");
  std::string line;
  while (!stop_.load(std::memory_order_relaxed)) {
    if (!std::getline(std::cin, line)) {
      // End-of-stream (EOF, closed pipe or read error): this source is done.
      eof_.store(true, std::memory_order_relaxed);
      break;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF

    RawLine raw;
    raw.source = kSourceName;
    raw.ingested_ms = util::now_ms();  // time via the util module (team convention)
    raw.text = line;

    bool pushed = false;
    while (!stop_.load(std::memory_order_relaxed)) {
      if (queue_.push_for(raw, kPushTimeoutMs)) {
        pushed = true;
        break;
      }
      if (queue_.closed()) break;  // consumer shut down: stop feeding
      // Queue full: retry, matching the tailer's back-pressure behaviour.
    }
    if (!pushed) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      util::log_warn("stdin reader: dropped pending line during shutdown");
      break;
    }
  }

  if (close_queue_on_exit_) queue_.close();  // sole source: end-of-stream for the consumer
  util::log_info(std::string("stdin reader: stopped (") +
                 (eof_.load(std::memory_order_relaxed) ? "end of input" : "stop requested") +
                 ")");
}

}  // namespace logpipe
