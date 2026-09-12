# logpipe

**logpipe** is a multi-source log collection pipeline: it tails (or reads) any
number of log sources, parses/lines them up, filters, rate-limits and enriches
them, and writes the result into rolling, optionally compressed and optionally
encrypted output files with per-line CRC-32 integrity stamps.

Everything is self-implemented standard C++17 — no third-party libraries, no
external tools at runtime. One executable, one config file.

```
sources (files / globs / stdin)                    outputs (rolling files)
  ┌──────────┐    ┌─────────┐    ┌──────────┐      ┌─────────────────────┐
  │ Tailer   │─┐  │         │    │ parse →  │      │ text / json_lines   │
  │ GlobSrc  │─┼─▶│ bounded │───▶│ filter → │─────▶│ + RLE per-line      │
  │ Stdin    │─┘  │  queue  │    │ rate →   │      │ + XOR container     │
  └──────────┘    └─────────┘    │ KV/tags  │      │ + CRC32 per line    │
   producer threads              └──────────┘      │ + versioned header  │
   (back-pressure)                main thread      └─────────────────────┘
```

## Feature overview

- **Multi-source input** — tail plain files (created later is picked up too),
  glob patterns (`glob:/var/log/app/*.log`) and standard input; every source
  can carry metadata tags (`source.<n>.tags`).
- **Resume & replay** — offsets are checkpointed to a state file so a
  restarted run continues where it stopped; optional time-stamp replay
  (`replay.since`) positions every file via a sidecar line index.
- **Parsing & levels** — `2026-08-30 12:00:00 [INFO] message` lines (also `T`
  separator, fractional seconds, custom registered levels); anything else
  passes through as a RAW record (RAW always clears every threshold).
- **Three filter stages** — optional DSL expression (`filter.expr`), minimum
  level (`filter.level`), case-insensitive keyword (`filter.keyword`).
- **Rate limiting** — fixed-window `rate.max_lines_per_sec`; overflow is
  dropped and counted separately.
- **KV extraction** — `extract.kv = true` pulls `key=value` pairs out of each
  line; fields feed the DSL (`kv("key")`), Top-N statistics and fan-out
  filters.
- **Fan-out outputs** — declare named outputs (`output.<N>.*`), each with its
  own file, format, compression, level threshold, DSL filter and ordered
  transform chain (`uppercase | trim | replace:pat->repl | tag:name=value`).
- **Rolling files** — size rotation with a shifting backup chain, daily
  rotation, per-source files (`per_source_files = true`), batched write
  buffering (`write.buffer_*`).
- **Integrity & confidentiality** — per-line CRC-32, self-implemented RLE
  container per line (`.rle`), XOR keystream container per file (`.enc`,
  applied after compression).
- **Versioned output header** — every freshly created output file starts with
  one `#logpipe v1.0.0 <format> created=<UTC>` line (see below).
- **Observability** — run summary on stdout, diag log (`diag.level/file`),
  optional periodic Prometheus text-format snapshots (`stats.interval_sec`).
- **Ops-friendly CLI** — `--self-test`, `--check-config`, `--dump-config`,
  `--no-env`.

## Building

Requires CMake ≥ 3.15 and a C++17 compiler (GCC ≥ 8, Clang ≥ 8, MSVC 2019+).

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
ctest                      # runs the full test suite (114 checks)
./src/logpipe --self-test  # library invariants without any config
```

Artifacts: `build/src/logpipe` (the pipeline binary). Threads are linked via
`find_package(Threads)`; GCC < 9 additionally links `stdc++fs` (handled by the
CMake files).

## Quick start

```sh
# 1. minimal config: tail two files into a rolling output file
cat > logpipe.conf <<'EOF'
input.files   = logs/app.log, logs/error.log
output.dir    = out
output.file   = logpipe_out.log
filter.level  = INFO
EOF

# 2. validate it, inspect it, then run until interrupted (Ctrl-C stops cleanly)
logpipe --check-config
logpipe --dump-config
logpipe
```

`Ctrl-C` (or `SIGTERM`, or the appearance of `run.stop_file`, or
`run.duration_sec` elapsing) triggers a graceful shutdown: sources stop, the
queue drains, buffered output is flushed, offsets are saved and a run summary
is printed.

```text
lines rate-dropped : 0
...
filter DSL      : enabled
```

## Versioned output header

Every **newly created** output file begins with exactly one self-describing
header line:

```
#logpipe v1.0.0 text created=2026-09-12T07:15:00Z
#logpipe v1.0.0 json_lines created=2026-09-12T07:15:00Z
```

- The stamp is true UTC (not local time), so headers from machines in
  different timezones are comparable.
- The header travels through the normal encoding path: RLE output carries it
  as the first decoded frame, encrypted output as the first payload chunk
  after the `LXEF` magic — after decoding, it is always line 1.
- Every rotation product (size rotation backups, daily rotation files,
  fallback timestamped names, per-source files, fan-out branches) is stamped,
  because each file is a fresh birth.
- Files that are **appended** to (restart resume onto a non-empty active
  file, a same-day daily-rotation file) are never stamped again.
- Consumers can detect and skip it: the line starts with
  `#logpipe v<version> <format> created=`.

## Input sources

Entries in the comma-separated `input.files` list (repeated lines append):

| entry form        | behaviour                                                              |
|-------------------|------------------------------------------------------------------------|
| `<path>`          | tail a regular file; missing files are reopened on every poll          |
| `stdin:`          | read standard input as an additional source                            |
| `glob:<pattern>`  | tail every match; new matches join, vanished files retire              |

- Exactly one source owns the end-of-stream: the first file-family source, or
  stdin when no file sources exist.
- `source.<n>.tags = k=v, k2` (1-based declaration index) attaches metadata to
  every line of that entry; bare words become `<word>="true"`.
- `glob.exclude = pattern, pattern` drops matched candidates (matched against
  the file name and the full path).
- Lines are only checkpointed at complete-line boundaries; a file that shrank
  below its recorded offset is re-read from the start.

## Output formats and how to read them back

**text** (default) — one line per record, CRC covers everything before
` |crc=`:

```
2026-09-06 12:00:00 [ERROR] [logs/app.log] disk failure imminent |crc=1A2B3C4D
```

**json_lines** — one single-line JSON object per record, CRC covers the exact
serialized prefix before the `"crc"` field:

```json
{"ts":"2026-09-06 12:00:00","level":"ERROR","source":"logs/app.log","message":"disk failure imminent","crc":439041805}
```

Encodings stack: `output.compress = rle` wraps every formatted line in its own
length-prefixed RLE frame (file gains `.rle`); `encrypt.password = ...` wraps
the whole stream in the XOR keystream container (file gains `.enc`, applied
after compression — readers reverse the order). The repository's
`tests/test_integration.cpp` demonstrates full decode → parse → CRC-verify
round trips, including wrong-password rejection.

## Configuration

Loading happens in three layers with increasing priority:

1. the config file plus every recursive `include =` target (cycle-safe);
   within one layer, later keys overwrite earlier ones;
2. the activated profile: `profile.<name>.<key>` groups apply only when
   `active_profile = <name>` selects them — otherwise selecting an unknown
   profile is a hard error;
3. environment variables `LOGPIPE_<KEY>` (dots become underscores, e.g.
   `LOGPIPE_FILTER_LEVEL`) override everything; disable with `--no-env`.

`--check-config` prints a per-key table of the effective value **and its
provenance** (`file`, `profile:<name>`, `env` or `default`).

### Key reference (canonical spellings)

| key                             | default              | description |
|---------------------------------|----------------------|-------------|
| `input.files`                   | *(required)*         | comma-separated entries: `<path>`, `stdin:`, `glob:<pattern>` |
| `source.<n>.tags`               | *(empty)*            | metadata k=v pairs for the n-th entry (1-based) |
| `glob.exclude`                  | *(empty)*            | comma-separated glob patterns excluded from glob sources |
| `output.dir`                    | `out`                | directory holding all output files |
| `output.file`                   | `logpipe_out.log`    | base name of the rolling active file |
| `rotate.size_bytes`             | 1048576              | rotate when the active file would exceed this (≥ 256) |
| `rotate.backups`                | 5                    | backup chain length (1..999) |
| `rotate.daily`                  | `false`              | also rotate on calendar-date change (date-stamped names) |
| `per_source_files`              | `false`              | one output file per input source (sanitized source in the name) |
| `output.format`                 | `text`               | `text` or `json_lines` |
| `output.compress`               | `none`               | `none` or `rle` (per-line RLE frame stream) |
| `encrypt.password`              | *(empty)*            | non-empty enables the XOR container (never logged/dumped) |
| `write.buffer_lines`            | 0                    | flush threshold in lines (0 = off) |
| `write.buffer_bytes`            | 0                    | flush threshold in bytes (0 = off; both 0 = unbuffered) |
| `filter.expr`                   | *(empty)*            | DSL expression, first filter stage (see below) |
| `filter.level`                  | `INFO`               | minimum level; RAW lines always pass |
| `filter.keyword`                | *(empty)*            | case-insensitive substring on the message |
| `rate.max_lines_per_sec`        | 0 (unlimited)        | fixed-window rate limit; overflow dropped and counted |
| `extract.kv`                    | `false`              | run the key=value extractor over admitted lines |
| `output.<N>.name`               | *(empty)*            | fan-out: unique branch name |
| `output.<N>.file`               | *(empty)*            | fan-out: branch base file inside `output.dir` |
| `output.<N>.format`             | `text`               | fan-out: branch format |
| `output.<N>.compress`           | `none`               | fan-out: branch compression |
| `output.<N>.level`              | `DEBUG`              | fan-out: branch level threshold |
| `output.<N>.filter.expr`        | *(empty)*            | fan-out: branch DSL filter |
| `output.<N>.transform`          | *(empty)*            | fan-out: `\|`-separated chain `uppercase\|trim\|replace:pat->repl\|tag:name=value` |
| `output.<N>.transform.position` | `after`              | `before` = filter sees (and writes) the transformed record |
| `stats.interval_sec`            | 0 (off)              | periodic Prometheus snapshot interval (≤ 86400) |
| `stats.dir`                     | `stats`              | snapshot directory (timestamped `.prom` files) |
| `stats.keep_files`              | 10                   | snapshot retention (oldest pruned) |
| `tail.poll_ms`                  | 500                  | poll interval between tail rounds (20..60000) |
| `state.offset_file`             | `logpipe_offsets.txt`| resume state file (empty disables persistence) |
| `replay.since`                  | *(empty)*            | `YYYY-MM-DD HH:MM:SS`: start near this timestamp via sidecar index |
| `replay.index_interval_bytes`   | 4096                 | sidecar index cadence (64..1073741824, 0 = off) |
| `read.chunk_bytes`              | 8192                 | read block size while tailing (128..1048576) |
| `queue.capacity`                | 1024                 | reader→main queue capacity in lines (1..1000000) |
| `run.duration_sec`              | 0 (run forever)      | stop automatically after N seconds (≤ one week) |
| `run.stop_file`                 | *(empty)*            | stop as soon as this file appears |
| `diag.level`                    | `info`               | `debug` / `info` / `warn` / `error` |
| `diag.file`                     | *(empty = stderr)*   | diagnostic log file (append mode) |
| `active_profile`                | *(empty)*            | selects the `profile.<name>.*` group to apply |
| `level.register.<NAME>`         | *(none)*             | register a custom level with severity 1..999 |
| `include`                       | *(none)*             | load another config file first (recursive, cycle-safe) |

Fan-out note: as soon as any `output.<N>.*` key is set, the single-output
settings above are replaced by the declared group (indices start at 1, gaps
allowed, order by index). Without fan-out keys the single-output settings map
onto one default output named `default`.

### Deprecated keys

Still accepted, each occurrence warns at load time and behaves exactly like
its canonical replacement — new configs should not use them:

| deprecated key        | canonical replacement   |
|-----------------------|-------------------------|
| `input.file`          | `input.files`           |
| `output_format`       | `output.format`         |
| `rotate_daily`        | `rotate.daily`          |
| `extract_kv`          | `extract.kv`            |
| `max_lines_per_sec`   | `rate.max_lines_per_sec`|
| `stop_file`           | `run.stop_file`         |

### Filter DSL

`filter.expr` compiles at load time (a malformed expression aborts startup
with a positioned error). Operators (case-insensitive): `AND`, `OR`, `NOT`,
`=`, `!=`, `>=`, `>`, `<=`, `<`, `CONTAINS`, `STARTS_WITH`, `ENDS_WITH`,
`MATCHES` (ECMAScript regex). Fields: `level` (severity-ordered), `msg`,
`src`, plus `kv("key")` / `field("key")` for extracted fields and source tags.
Example:

```
filter.expr = level >= "WARN" AND (msg CONTAINS "timeout" OR kv("host") STARTS_WITH "web-")
```

### Profiles

```
active_profile = dev
profile.dev.filter.level  = DEBUG
profile.dev.tail.poll_ms  = 40
profile.prod.filter.level = ERROR
```

`LOGPIPE_ACTIVE_PROFILE` can select the profile from the environment layer.

## Command line

```
logpipe [config-file] [--dump-config] [--check-config] [--no-env] [--self-test]
```

| switch          | effect |
|-----------------|--------|
| *(none)*        | load config and run the pipeline |
| `--self-test`   | run the library invariants (CRC known-answer vectors, RLE/XOR round trips, time formatting, level ordering) and exit; exit 0 = all hold, 1 = broken; no config is loaded |
| `--check-config`| load + validate the config (including DSL compilation), print the per-key effective-value/provenance report and exit 0 without touching input or output files |
| `--dump-config` | load the config, print the one-line dump of every effective key to stdout and exit 0 |
| `--no-env`      | disable the `LOGPIPE_<KEY>` environment layer (config file and profile layers still apply) |
| `config-file`   | path to the config file (default: `logpipe.conf`); must come first if multiple positional arguments are given |

Flags may appear before or after the config path. Exit codes: `0` success,
`1` self-test failure, `2` usage error or config load failure, `3` output
file not writable, `4` output write failure during the run (data possibly
lost), `5` stdout write failure in `--dump-config`/`--check-config`.

## Example: fan-out with two encodings

```
input.files          = logs/app.log
output.dir           = out
filter.level         = DEBUG
filter.expr          = level >= "INFO" AND NOT msg CONTAINS "heartbeat"

output.1.name        = plain
output.1.file        = plain.log
output.1.format      = text
output.1.compress    = rle

output.2.name        = safe
output.2.file        = safe.log
output.2.format      = json_lines
output.2.level       = WARN

encrypt.password     = s3cret
```

`encrypt.password` is a global key, so both fan-out branches get the XOR
container on top of their own settings: branch `plain` keeps every surviving
record as CRC-stamped text wrapped in RLE frames (`out/plain.log.rle.enc`),
branch `safe` keeps only WARN and above as JSON lines (`out/safe.log.enc`).
After decrypting/decompressing, each file starts with the versioned header
line, followed by lines whose CRC-32 can be recomputed from the decoded
bytes.

## Metrics and statistics export

The stdout run summary covers ingested/filtered/rate-dropped lines, per-level
counts, per-source and per-source-type totals, output bytes, rotation counts,
KV extraction rates and (in replay mode) sidecar index hit/fallback counts.
With `stats.interval_sec > 0` the same counters are exported as Prometheus
text-format snapshots into `stats.dir`, pruned to `stats.keep_files`, with a
final snapshot at shutdown.

## Project layout

```
include/          public headers (one module per file, self-documenting)
  version.h         project identity + the versioned output header builder
  config.h/.cpp     layered key=value configuration, profiles, env layer
  pipeline.h        queue, parser, levels, filter, rate limiter, metrics
  source.h          ISource producer contract (files/globs/stdin implement it)
  tailer.h tail_engine.h index_sidecar.h   tailing, resume, replay index
  glob_source.h stdin_reader.h             the other sources
  dsl.h             filter expression compiler
  kv_extractor.h transform.h              field extraction and record rewrites
  rle.h xcrypt.h writer.h                 encodings and the output sinks
  util.h            time, diagnostics, CRC-32 (the only clock/stderr user)
  prom_stats.h      Prometheus text-format export
src/              implementation
tests/            zero-dependency test harness + suites (114 groups)
logpipe.conf      fully commented canonical reference configuration
```

Single-threaded writer/DSL/config code, multi-threaded sources; the bounded
queue applies back-pressure to producers. The util module is the single point
of truth for time reads and diagnostic output.
