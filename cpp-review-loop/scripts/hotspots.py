#!/usr/bin/env python3
"""
hotspots.py — objective hotspot scanner for C/C++ code reviews.

Stdlib-only, cross-platform (Windows/Linux/macOS). Builds a cheap, objective
map of a codebase BEFORE deep reading: biggest files, longest functions,
include fan-out, TODO density, git churn, and counts of red-flag patterns.

    python hotspots.py <repo-root> [--top 15] [--json] [--all] [--no-churn]

Interpretation rules (do not skip):
  * Red-flag counts are CANDIDATES for review, not findings. Open the file and
    read context before reporting anything.
  * Third-party / build / generated dirs are skipped by default; pass --all to
    include them.
  * The function-length detector is a brace-counting heuristic: multi-line
    signatures, raw strings, and macro-heavy code can confuse it. It is for
    ranking hotspots, not for measuring exactness.
  * C-style-cast detection covers the pointer-cast form ("(T*)p") only;
    "(int)x" needs manual grep. The naming/style sections are candidate
    signals: they measure the DOMINANT convention so deviations can be
    flagged — they never define the convention themselves.
  * catch-by-value covers common class-type forms only; the self-header
    check applies only when a same-stem header exists in the scanned tree;
    "virtual without override" is a measure signal (a base declaration and
    a derived override missing its keyword look identical without class
    context — interpret, don't auto-flag).
  * Log-field-style and printf-spec signals run on RAW lines (string
    contents are needed); they are candidates — judge the output's audience
    and the cast intent before reporting anything (consistency.md §L).
  * Build-hardening signals measure flag PRESENCE in build files; missing
    flags are suggestions to weigh per toolchain (and vendored dirs may
    legitimately opt out), not findings by themselves (checklist.md §7).
  * Header-composition counts column-0 class/struct definitions per header
    (forward declarations excluded); multiple classes in one header is a
    CANDIDATE — co-located helper types serving the primary class are fine,
    relatedness is a human call (checklist.md §1).
  * Wrapper-bypass counts distinct sources per facility (time / rand / crc)
    as RAW candidates: wall-clock vs monotonic clocks serve different
    purposes — split by usage, then find the project's wrapper layer before
    calling anything a bypass (consistency.md §M).
"""

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

CODE_EXTS = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx",
             ".inl", ".ipp", ".tcc"}
HEADER_EXTS = {".h", ".hpp", ".hh", ".hxx", ".inl"}

SKIP_DIRS = {
    ".git", ".hg", ".svn", ".vs", ".vscode", ".idea", ".cache", ".github",
    "build", "builds", "out", "output", "bin", "obj", "libs",
    "cmake-build-debug", "cmake-build-release", "_deps", "buildroot",
    "third_party", "thirdparty", "3rdparty", "external", "vendor", "deps",
    "node_modules", "generated", "gen", "autogen",
}

RED_FLAGS = [
    ("raw new",            r"\bnew\s+[A-Za-z_]"),
    ("malloc/free family", r"\b(?:malloc|calloc|realloc|free)\s*\("),
    ("C string funcs",     r"\b(?:sprintf|strcpy|strcat|strncpy|gets|atoi|atol|alloca)\s*\("),
    ("catch-all (...)",    r"\bcatch\s*\(\s*\.\.\.\s*\)"),
    ("const_cast",         r"\bconst_cast\s*<"),
    ("reinterpret_cast",   r"\breinterpret_cast\s*<"),
    ("dynamic_cast",       r"\bdynamic_cast\s*<"),
    ("volatile",           r"\bvolatile\b"),
    ("mutex-like types",   r"\b(?:std::(?:recursive_)?mutex|pthread_mutex_t|CRITICAL_SECTION|QMutex)\b"),
    ("sleep calls",        r"\b(?:sleep_for|sleep_until)\s*\(|\b::sleep\s*\(|\bSleep\s*\("),
    ("auto_ptr (deprecated)", r"\bauto_ptr\b"),
    ("goto",               r"\bgoto\s+\w+"),
    ("void* usage",        r"\bvoid\s*\*"),
    ("C-style cast",       r"\(\s*(?:const\s+)?(?:unsigned\s+|signed\s+)?[A-Za-z_]\w*\s*\*+\s*\)\s*[A-Za-z_]"),
    ("typedef struct/union", r"\btypedef\s+(?:struct|union)\b"),
    ("typedef (prefer using)", r"\btypedef\b"),
    ("unscoped enum",      r"\benum\s+[A-Za-z_]\w*\s*[{:]"),
    ("memset/ZeroMemory",  r"\b(?:memset|ZeroMemory)\s*\("),
    ("printf family",      r"\b(?:printf|fprintf|snprintf|puts|scanf)\s*\("),
    ("NULL macro",         r"\bNULL\b"),
    ("exit() call",        r"\b(?:exit|_exit)\s*\("),
    ("setjmp/longjmp",     r"\b(?:setjmp|longjmp)\s*\("),
    ("this-> assignment",  r"this->\s*[A-Za-z_]\w*\s*=[^=]"),
    # Standards-adoption signals (references/standards.md F-I):
    ("default lambda capture", r"\[\s*(?:=|&)\s*\]|\[\s*=\s*,|\[\s*&\s*,"),
    ("catch by value",
     r"catch\s*\(\s*(?:const\s+)?(?:[A-Z][\w:]*|[\w:]+::[\w:]+)"
     r"(?:\s*<[^<>]*>)?(?:\s+const)?\s+(?!&)[A-Za-z_]\w*\s*\)"),
    ("signed/unsigned size() loop",
     r"\bfor\s*\(\s*(?:int|long|short)\s+\w+\s*=\s*0\s*;[^;\n]*\.size\s*\("),
    ("std::bind (prefer lambda)", r"\bstd::bind\s*\("),
    ("virtual + override together", r"\bvirtual\b[^;\n{}]*\boverride\b"),
    ("virtual without override",
     r"\bvirtual\b(?![^;\n]*\boverride\b)(?![^;\n]*=\s*0)"
     r"(?![^;\n]*\bfinal\b)(?![^;\n]*~)"),
]

HEADER_ONLY_FLAGS = [
    ("using namespace (in header)", r"^\s*using\s+namespace\s+\w+"),
]

MEMBER_RULES = [
    ("_member (leading _)",  re.compile(r"\b_[a-z]\w*\b")),
    ("member_ (trailing _)", re.compile(r"\b[a-z]\w*_\b")),
    ("m_member (m_ prefix)", re.compile(r"\bm_[A-Za-z]\w*\b")),
]

INCLUDE_PATH_RE = re.compile(r'\s*#\s*include\s*([<"])([^>"]+)[>"]')
GUARD_RE = re.compile(r"^\s*#\s*ifndef\s+\w+", re.MULTILINE)

TODO_RE = re.compile(r"\b(?:TODO|FIXME|HACK|XXX)\b")
INCLUDE_RE = re.compile(r"^\s*#\s*include\b")

# Log/output format signals (consistency.md §L) — measured on RAW lines.
QUOTED_RE = re.compile(r'"([^"]*)"')
LOG_KEY_RE = re.compile(r"(?<![A-Za-z0-9_%])([A-Za-z_][A-Za-z0-9_]+)\s*([=:])")
PRINTF_CALL_RE = re.compile(r"\b(?:std::)?(?:v?sn?printf|fprintf|printf)\s*\(")
SIZE_ARG_RE = re.compile(r"\bsizeof\s*\(|\.size\s*\(\s*\)|\.length\s*\(\s*\)")
INT_SPEC_RE = re.compile(r"%(?:[-+ #0']*[\d*]*(?:\.\d+)?(?:hh|h|ll|l|L|j|t)?)(z?)([diouxX])")

# Header composition (checklist.md §1): column-0 class/struct definitions.
HEADER_CLASS_RE = re.compile(
    r"^(?:template\s*<[^<>]*>\s*)?(?:class|struct)\s+([A-Z_]\w*)[^;{}]*\{")
HEADER_CLASS_NEXT_LINE_RE = re.compile(
    r"^(?:template\s*<[^<>]*>\s*)?(?:class|struct)\s+([A-Z_]\w*)[^;{}]*$")

# Wrapper-bypass candidates (consistency.md §M): multiple distinct sources for
# the same facility (time / randomness) or multiple same-purpose implementations
# (crc). Raw counts only — usage-level judgment is human work.
WRAPPER_TIME_RES = [
    ("time()/std::time", re.compile(r"(?<!::)\btime\s*\(\s*(?:NULL|nullptr|0)\s*\)|\bstd::time\s*\(")),
    ("chrono::system_clock", re.compile(r"\bchrono::system_clock\b")),
    ("chrono::steady_clock", re.compile(r"\bchrono::steady_clock\b")),
    ("gettimeofday*", re.compile(r"gettimeofday")),  # libc + butil::gettimeofday_us 等
    ("clock_gettime", re.compile(r"\bclock_gettime\b")),
    ("GetTickCount*", re.compile(r"\bGetTickCount(?:64)?\b")),
    ("GetSystemTime/LocalTime", re.compile(r"\bGet(?:System|Local)Time\b")),
    ("QueryPerformanceCounter", re.compile(r"\bQueryPerformanceCounter\b")),
    ("timeGetTime", re.compile(r"\btimeGetTime\b")),
    ("clock()", re.compile(r"\bclock\s*\(\s*\)")),
    ("spdk_get_ticks", re.compile(r"\bspdk_get_ticks\b")),
    ("boost::chrono", re.compile(r"boost::chrono\b")),
]
WRAPPER_RAND_RES = [
    ("rand()/srand()", re.compile(r"\b(?:srand|rand)\s*\(")),
    ("rand_r", re.compile(r"\brand_r\s*\(")),
    ("std::mt19937 family", re.compile(r"\bstd::(?:mt19937(?:_64)?|default_random_engine)\b")),
    ("std::random_device", re.compile(r"\bstd::random_device\b")),
]
CRC_CALL_RE = re.compile(r"\b([A-Za-z_]\w*[Cc][Rr][Cc]\w*)\s*\(")

# Magic numbers (checklist.md §1): two-plus-digit literals outside
# constant-definition lines. Strings/comments already stripped; hex, decimals
# and single-digit sentinels are not counted. Storage prefixes (inline/static/
# extern) before constexpr/const are allowed — namespace-scope named constants
# use them.
MAGIC_NUM_RE = re.compile(r"(?<![\w.\"'])(?<!0[xX])[0-9]{2,}(?![\w.])")
CONST_DECL_RE = re.compile(
    r"^\s*(?:#\s*define\b|enum\b|(?:(?:inline|static|extern)\s+)*(?:constexpr\b|const\b))")

# Build-file hardening signals (checklist.md §7) — measured on build files.
BUILD_FILE_SUFFIXES = (".cmake", ".vcxproj", ".mk")
BUILD_FILE_NAMES = {"cmakelists.txt", "makefile", "meson.build", "build.bazel",
                    "workspace"}
BUILD_FLAGS = [
    # (category, label, regex)
    ("warnings", "-Wall", re.compile(r"-Wall\b")),
    ("warnings", "-Wextra", re.compile(r"-Wextra\b")),
    ("warnings", "-Wpedantic/-pedantic", re.compile(r"-Wpedantic\b|-pedantic\b")),
    ("warnings", "MSVC /W4 /Wall", re.compile(r"/(?:W4|Wall)\b", re.I)),
    ("warnings", "MSVC /W3 (weak)", re.compile(r"/W3\b", re.I)),
    ("warnings-as-errors", "-Werror (all)", re.compile(r"-Werror(?![\w-]*=)")),
    ("warnings-as-errors", "-Werror=specific", re.compile(r"-Werror=")),
    ("warnings-as-errors", "MSVC /WX", re.compile(r"/WX\b", re.I)),
    ("strict", "-Wconversion", re.compile(r"-Wconversion\b")),
    ("strict", "-Wshadow", re.compile(r"-Wshadow\b")),
    ("strict", "MSVC /permissive-", re.compile(r"/permissive-", re.I)),
    ("hardening", "-fstack-protector*", re.compile(r"-fstack-protector")),
    ("hardening", "-fstack-clash-protection", re.compile(r"-fstack-clash-protection\b")),
    ("hardening", "-fcf-protection", re.compile(r"-fcf-protection\b")),
    ("hardening", "-D_FORTIFY_SOURCE", re.compile(r"(?:-D)?_FORTIFY_SOURCE")),
    ("hardening", "-D_GLIBCXX_ASSERTIONS", re.compile(r"(?:-D)?_GLIBCXX_ASSERTIONS")),
    ("hardening", "MSVC /sdl", re.compile(r"/sdl\b", re.I)),
    ("hardening", "MSVC /guard:cf", re.compile(r"/guard:cf\b", re.I)),
    ("sanitizers", "-fsanitize=", re.compile(r"-fsanitize(?:-recover)?=")),
    ("sanitizers", "MSVC /fsanitize", re.compile(r"/fsanitize\b", re.I)),
    ("global-state (§7 anti-pattern)", "add_compile_options/add_definitions",
     re.compile(r"\badd_compile_options\s*\(|\badd_definitions\s*\(")),
    ("global-state (§7 anti-pattern)", "set(CMAKE_CXX_FLAGS ...)",
     re.compile(r"set\s*\(\s*CMAKE_CXX_FLAGS")),
    ("standards", "CMAKE_CXX_EXTENSIONS OFF", re.compile(r"CMAKE_CXX_EXTENSIONS\s+OFF")),
]
CATEGORY_SUGGESTIONS = {
    "warnings": "-Wall -Wextra (-Wpedantic) or MSVC /W4",
    "warnings-as-errors": "-Werror / MSVC /WX (at least first-party targets & CI)",
    "hardening": "-fstack-protector-strong, -D_GLIBCXX_ASSERTIONS, /sdl, /guard:cf",
    "sanitizers": "-fsanitize=address,undefined in dev/CI builds",
}
# 用户要求：有运行时代价的旗标只告知风险，去留由用户决定（checklist.md §L 同款哲学：
# 测量与告知，不替用户做决定）。
CATEGORY_RUNTIME_COST = {
    "warnings": "compile-time only, no runtime cost",
    "warnings-as-errors": "compile-time only, no runtime cost",
    "strict": "compile-time only, no runtime cost",
    "hardening": "runtime ~0-3%, usually negligible (inform user; keep/remove is their call)",
    "sanitizers": "runtime HIGH: ASan ~2x CPU / 2-3x memory, dev/CI only — inform user; "
                  "removal decision is theirs, never propose removing as a fix",
    "global-state (§7 anti-pattern)": "compile-time only, no runtime cost",
    "standards": "compile-time only, no runtime cost",
}
CTRL_TOKENS = {"if", "for", "while", "switch", "catch", "return", "else", "do"}
NAME_BEFORE_PAREN_RE = re.compile(r"([A-Za-z_~][\w:]*)\s*$")


def strip_comments(text):
    """Blank out comments and string/char literal contents (state machine).

    Braces and keywords inside literals/comments thus stop affecting the
    function-length heuristic. Raw strings (R\"(...)\") are NOT understood and
    may leak characters — acceptable for a ranking heuristic.
    """
    out = []
    i, n = 0, len(text)
    state = "code"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"; out.append("  "); i += 2
            elif c == "/" and nxt == "*":
                state = "block"; out.append("  "); i += 2
            elif c == '"':
                state = "dq"; out.append(c); i += 1
            elif c == "'":
                state = "sq"; out.append(c); i += 1
            else:
                out.append(c); i += 1
        elif state == "line":
            if c == "\n":
                state = "code"; out.append(c)
            else:
                out.append(" ")
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"; out.append("  "); i += 2
            elif c == "\n":
                out.append(c); i += 1
            else:
                out.append(" "); i += 1
        else:  # dq or sq
            if c == "\\":
                out.append("  "); i += 2
            elif (state == "dq" and c == '"') or (state == "sq" and c == "'"):
                state = "code"; out.append(c); i += 1
            else:
                out.append("\n" if c == "\n" else " "); i += 1
    return "".join(out)


def looks_binary(chunk):
    return b"\x00" in chunk


def find_function_start(line):
    """Return (name) if this stripped line opens a function-like block, else None.

    Heuristic: line contains '(', ends with '{', first token is not a control
    keyword, no '=' appears before the first '(' (rules out lambdas and
    assignments), and an identifier precedes that '('.
    """
    if not line.rstrip().endswith("{") or "(" not in line:
        return None
    first_paren = line.index("(")
    before = line[:first_paren]
    if "=" in before:
        return None
    first_word = re.match(r"\s*([A-Za-z_~][\w:]*)", before)
    if not first_word or first_word.group(1) in CTRL_TOKENS:
        return None
    m = NAME_BEFORE_PAREN_RE.search(before)
    if not m:
        return None
    name = m.group(1).split("::")[-1]
    if name in CTRL_TOKENS:
        return None
    return name


def function_spans(stripped_lines):
    """Yield (start_line_1based, name, length) for function-like blocks."""
    results = []
    i = 0
    n = len(stripped_lines)
    while i < n:
        name = find_function_start(stripped_lines[i])
        if name is None:
            i += 1
            continue
        balance = stripped_lines[i].count("{") - stripped_lines[i].count("}")
        start = i
        while balance > 0 and i + 1 < n:
            i += 1
            balance += stripped_lines[i].count("{") - stripped_lines[i].count("}")
        length = i - start + 1
        if length >= 3:  # one-liners are noise
            results.append((start + 1, name, length))
        i += 1
    return results


def red_flag_scan(stripped_lines, raw_lines, is_header):
    """Return {flag_name: [(line_no, raw_snippet), ...]} for this file.

    Matching runs on comment/string-stripped lines so literals and comments
    don't trigger flags; snippets come from the raw lines for readability.
    """
    hits = {}
    rules = list(RED_FLAGS)
    if is_header:
        rules += HEADER_ONLY_FLAGS
    for flag, pattern in rules:
        rx = re.compile(pattern)
        found = []
        for idx, line in enumerate(stripped_lines, 1):
            if rx.search(line):
                snippet = raw_lines[idx - 1].strip()[:100] if idx <= len(raw_lines) else line.strip()[:100]
                found.append((idx, snippet))
        if found:
            hits[flag] = found
    return hits


def func_style(name):
    """Classify an identifier's naming style (heuristic)."""
    n = name[1:] if name.startswith("~") else name
    if "_" in n:
        return "snake_case" if n.islower() else "mixed_case"
    if n.isupper():
        return "UPPER_CASE"
    if any(c.isupper() for c in n):
        return "PascalCase" if n[0].isupper() else "camelCase"
    return "lowercase"


def header_hygiene(text, raw_lines, s_lines):
    """Return [(flag_name, sample_or_None)] for one header file.

    Covers include-guard style, include placement, and include sorting.
    Sorting is judged only within same-bracket-type runs to avoid flagging
    legitimate group ordering between <> and "" includes.
    """
    issues = []
    has_pragma_once = "#pragma once" in text
    has_guard = bool(GUARD_RE.search(text))
    if not has_pragma_once and not has_guard:
        issues.append(("header missing include guard", None))
    elif has_guard and not has_pragma_once:
        issues.append(("#ifdef guard (no #pragma once)", None))

    inc_idx = [i for i, l in enumerate(raw_lines) if INCLUDE_RE.match(l)]
    if not inc_idx:
        return issues

    first_inc = inc_idx[0]
    for i in range(first_inc):
        s = s_lines[i].strip()
        if s and not s.startswith("#"):
            issues.append(("code before first include",
                           f"line {i+1}: {raw_lines[i].strip()[:80]}"))
            break

    runs = []
    for i in inc_idx:
        if runs and i == runs[-1][-1] + 1:
            runs[-1].append(i)
        else:
            runs.append([i])
    for run in runs:
        prev = None
        for i in run:
            m = INCLUDE_PATH_RE.match(raw_lines[i])
            if not m:
                prev = None
                continue
            key = (m.group(1), m.group(2).lower())
            if prev and key[0] == prev[0] and key[1] < prev[1]:
                issues.append(("unsorted includes",
                               f"line {i+1}: {raw_lines[i].strip()[:80]}"))
                break
            prev = key
    return issues


def log_field_scan(raw_lines):
    """Key/separator statistics from string literals on output-ish lines.

    Consistency.md §L measurement: mixed "=" vs ":" separators and mixed key
    naming styles across log/print statements are the drift candidates.
    Output-ish lines also open a 3-line window so multi-line printf/snprintf
    calls (format literal on the next line) are still measured.
    """
    seps, styles, keys = Counter(), Counter(), Counter()
    window = 0
    for raw in raw_lines:
        low = raw.lower()
        if "printf" in low or "<<" in raw or ("log" in low and '"' in raw):
            window = 3
        elif window > 0:
            window -= 1
        else:
            continue
        for chunk in QUOTED_RE.findall(raw):
            for m in LOG_KEY_RE.finditer(chunk):
                key, sep = m.group(1), m.group(2)
                if sep == ":" and chunk[m.end():m.end() + 2] == "//":
                    continue  # URL scheme, not a key
                seps[sep] += 1
                styles[func_style(key)] += 1
                keys[key] += 1
    return seps, styles, keys


def _call_text(line, open_idx):
    depth = 0
    for j in range(open_idx, len(line)):
        if line[j] == "(":
            depth += 1
        elif line[j] == ")":
            depth -= 1
            if depth == 0:
                return line[open_idx:j + 1]
    return ""


def printf_spec_scan(raw_lines):
    """High-precision candidates: size-t-ish argument printed without %z.

    Only inspects argument text AFTER the format literal, so snprintf's
    destination-buffer sizeof() does not trigger. Calls whose argument text
    contains an explicit static_cast are skipped (the cast states intent).
    Returns [(line_no, snippet)], at most one per line.
    """
    hits = {}
    for idx, raw in enumerate(raw_lines, 1):
        for m in PRINTF_CALL_RE.finditer(raw):
            call = _call_text(raw, m.start())
            quoted = QUOTED_RE.findall(call)
            if not quoted:
                continue
            fmt_start = call.rfind('"' + quoted[-1] + '"')
            args_after = call[fmt_start + len(quoted[-1]) + 2:]
            if "static_cast<" in args_after:
                continue
            if not SIZE_ARG_RE.search(args_after):
                continue
            if any(z != "z" for z, _conv in INT_SPEC_RE.findall(quoted[-1])):
                hits[idx] = raw.strip()[:100]
                break
    return sorted(hits.items())


def build_hardening_scan(root, include_all):
    """Scan build files for warning/hardening compile flags (checklist.md §7).

    Presence is measured; ABSENCE is only a suggestion — judge it against the
    project's toolchains (vendored/third-party dirs may legitimately opt out).
    Returns (files_scanned, {label: count}).
    """
    found = Counter()
    nfiles = 0
    for p in root.rglob("*"):
        try:
            if not p.is_file():
                continue
            name = p.name.lower()
            if not (name in BUILD_FILE_NAMES or name.endswith(BUILD_FILE_SUFFIXES)):
                continue
            if not include_all:
                parts = p.relative_to(root).parts
                if any(part in SKIP_DIRS for part in parts[:-1]):
                    continue
            raw = p.read_bytes()
        except OSError:
            continue
        if looks_binary(raw[:8192]):
            continue
        nfiles += 1
        text = raw.decode("utf-8", errors="replace")
        for _cat, label, rx in BUILD_FLAGS:
            found[label] += len(rx.findall(text))
    return nfiles, found


def header_class_scan(s_lines):
    """Column-0 class/struct definition names in one header (candidates).

    Forward declarations (no body on/after the line) do not count. Handles
    brace-on-same-line and brace-on-next-line forms plus one-line template
    prefixes. Nested/indented classes are excluded on purpose (they serve
    their enclosing class). Returns [name, ...] in file order.
    """
    names = []
    n = len(s_lines)
    for i, line in enumerate(s_lines):
        if not line or line[:1].isspace():
            continue
        m = HEADER_CLASS_RE.match(line)
        if m:
            names.append(m.group(1))
        elif (i + 1 < n and s_lines[i + 1].lstrip().startswith("{")):
            m2 = HEADER_CLASS_NEXT_LINE_RE.match(line)
            if m2:
                names.append(m2.group(1))
    return names


def wrapper_bypass_scan(s_lines):
    """Distinct sources per wrapped facility (consistency.md §M).

    Returns (time_counter, rand_counter, crc_counter). Raw candidate counts:
    wall-clock vs monotonic clocks serve different purposes, so usage-level
    judgment stays with the reviewer.
    """
    time_c, rand_c, crc_c = Counter(), Counter(), Counter()
    for line in s_lines:
        for name, rx in WRAPPER_TIME_RES:
            n = len(rx.findall(line))
            if n:
                time_c[name] += n
        for name, rx in WRAPPER_RAND_RES:
            n = len(rx.findall(line))
            if n:
                rand_c[name] += n
        for m in CRC_CALL_RE.finditer(line):
            crc_c[m.group(1)] += 1
    return time_c, rand_c, crc_c


def magic_number_scan(s_lines):
    """Unnamed 2+-digit literals outside constant definitions (checklist §1).

    Returns (total_count, [(line_no, snippet), ...]) — samples capped by the
    caller. Candidate counts only: unit/contract judgment is human work.
    """
    total = 0
    samples = []
    for idx, line in enumerate(s_lines, 1):
        ls = line.strip()
        if not ls or ls.startswith("#") or CONST_DECL_RE.match(ls):
            continue
        found = MAGIC_NUM_RE.findall(line)
        if found:
            total += len(found)
            if len(samples) < 3:
                samples.append((idx, ls[:100]))
    return total, samples


def collect_files(root, include_all):
    root = Path(root).resolve()
    files = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        if not include_all:
            rel_parts = p.relative_to(root).parts
            if any(part in SKIP_DIRS for part in rel_parts[:-1]):
                continue
        if p.suffix.lower() not in CODE_EXTS:
            continue
        files.append(p)
    return root, files


def churn_counts(root):
    """File -> commit count over the last 12 months. {} if not a git repo."""
    try:
        res = subprocess.run(
            ["git", "-C", str(root), "log", "--since=12 months ago",
             "--name-only", "--pretty=format:"],
            capture_output=True, text=True, timeout=60,
        )
        if res.returncode != 0:
            return {}
    except Exception:
        return {}
    counts = Counter()
    for line in res.stdout.splitlines():
        line = line.strip()
        if line:
            counts[line] += 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("root", help="repository / directory to scan")
    ap.add_argument("--top", type=int, default=15, help="entries per section")
    ap.add_argument("--json", action="store_true", dest="as_json")
    ap.add_argument("--all", action="store_true",
                    help="include build/third-party/generated dirs")
    ap.add_argument("--no-churn", action="store_true", help="skip git churn")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    root, files = collect_files(args.root, args.all)
    if not files:
        print(f"No C/C++ files found under {root}")
        return 1

    # For the self-header check: basenames of all scanned headers.
    header_stems = {p.stem.lower() for p in files
                    if p.suffix.lower() in HEADER_EXTS}
    header_multi = []  # (class_count, rel, names) for >=2-class headers

    file_stats = {}
    flag_totals = {name: {"count": 0, "files": set(), "samples": []}
                   for name, _ in RED_FLAGS + HEADER_ONLY_FLAGS}
    hygiene_totals = {}
    func_style_counter = Counter()
    func_style_samples = []  # (style, rel, line, name)
    member_totals = {label: 0 for label, _ in MEMBER_RULES}
    member_samples = {label: [] for label, _ in MEMBER_RULES}
    log_sep_totals, log_style_totals, log_key_totals = Counter(), Counter(), Counter()
    wrapper_totals = {"time": Counter(), "rand": Counter(), "crc": Counter()}
    magic_files = []  # (count, rel, samples) for files with magic literals

    for path in files:
        try:
            raw = path.read_bytes()
        except OSError:
            continue
        if looks_binary(raw[:8192]):
            continue
        text = raw.decode("utf-8", errors="replace")
        stripped = strip_comments(text)
        s_lines = stripped.splitlines()
        raw_lines = text.splitlines()
        rel = path.relative_to(root).as_posix()
        is_header = path.suffix.lower() in HEADER_EXTS

        todos = [(i, l.strip()[:100]) for i, l in enumerate(text.splitlines(), 1)
                 if TODO_RE.search(l)]
        stats = {
            "loc": len(s_lines),
            "includes": sum(1 for l in s_lines if INCLUDE_RE.match(l)),
            "todos": todos,
            "funcs": function_spans(s_lines) if not is_header else [],
            "flags": red_flag_scan(s_lines, raw_lines, is_header),
        }
        file_stats[rel] = stats

        if is_header:
            for flag, sample in header_hygiene(text, raw_lines, s_lines):
                agg = hygiene_totals.setdefault(
                    flag, {"count": 0, "files": set(), "samples": []})
                agg["count"] += 1
                agg["files"].add(rel)
                agg["samples"].append(f"{rel}: {sample}" if sample else rel)
            names = header_class_scan(s_lines)
            if len(names) >= 2:
                header_multi.append((len(names), rel, names))
        elif path.stem.lower() in header_stems:
            # self-header-first (Google include rule #1): a .cpp whose
            # same-stem header exists should include it FIRST.
            for line in raw_lines:
                if not INCLUDE_RE.match(line):
                    continue
                m = INCLUDE_PATH_RE.match(line)
                if m:
                    inc = m.group(2).replace("\\", "/").rsplit("/", 1)[-1]
                    if inc.rsplit(".", 1)[0].lower() != path.stem.lower():
                        agg = hygiene_totals.setdefault(
                            "self-header not first include",
                            {"count": 0, "files": set(), "samples": []})
                        agg["count"] += 1
                        agg["files"].add(rel)
                        agg["samples"].append(
                            f"{rel}: first include is {m.group(2)}")
                break

        for label, rx in MEMBER_RULES:
            for idx, line in enumerate(s_lines, 1):
                found = rx.findall(line)
                if found:
                    member_totals[label] += len(found)
                    if len(member_samples[label]) < 3:
                        member_samples[label].append(f"{rel}:{idx}  {found[0]}")

        file_seps, file_styles, file_keys = log_field_scan(raw_lines)
        log_sep_totals.update(file_seps)
        log_style_totals.update(file_styles)
        log_key_totals.update(file_keys)

        w_time, w_rand, w_crc = wrapper_bypass_scan(s_lines)
        wrapper_totals["time"].update(w_time)
        wrapper_totals["rand"].update(w_rand)
        wrapper_totals["crc"].update(w_crc)

        m_total, m_samples = magic_number_scan(s_lines)
        if m_total:
            magic_files.append((m_total, rel, m_samples))

        for line_no, snippet in printf_spec_scan(raw_lines):
            agg = flag_totals.setdefault(
                "printf: size_t arg w/o %z (verify -Wformat)",
                {"count": 0, "files": set(), "samples": []})
            agg["count"] += 1
            agg["files"].add(rel)
            if len(agg["samples"]) < 3:
                agg["samples"].append(f"{rel}:{line_no}  {snippet}")

        for start, name, _length in stats["funcs"]:
            style = func_style(name)
            func_style_counter[style] += 1
            func_style_samples.append((style, rel, start, name))

        for flag, hits in stats["flags"].items():
            agg = flag_totals[flag]
            agg["count"] += len(hits)
            agg["files"].add(rel)
            for line_no, snippet in hits[:3]:
                agg["samples"].append(f"{rel}:{line_no}  {snippet}")

        # header cross-include signal: ../ reach-outs (must check RAW lines:
        # the include path is a string literal and is blanked by stripping)
        if is_header:
            reach = [(i + 1, l) for i, l in enumerate(raw_lines)
                     if INCLUDE_RE.match(l) and '"../' in l]
            if reach:
                agg = flag_totals.setdefault(
                    "../ includes from header",
                    {"count": 0, "files": set(), "samples": []})
                agg["count"] += len(reach)
                agg["files"].add(rel)
                for line_no, line in reach[:3]:
                    agg["samples"].append(
                        f"{rel}:{line_no}  {line.strip()[:100]}")

    churn = {} if args.no_churn else churn_counts(root)
    build_nfiles, build_flag_counts = build_hardening_scan(root, args.all)
    build_detected = {label: build_flag_counts[label]
                      for _cat, label, _rx in BUILD_FLAGS
                      if build_flag_counts[label]}
    cats_with = {cat for cat, label, _rx in BUILD_FLAGS
                 if build_flag_counts[label]}
    build_missing = sorted(cat for cat in CATEGORY_SUGGESTIONS
                           if cat not in cats_with)

    total_loc = sum(s["loc"] for s in file_stats.values())
    top_files = sorted(file_stats.items(),
                       key=lambda kv: kv[1]["loc"], reverse=True)[:args.top]
    long_funcs = []
    for rel, s in file_stats.items():
        for start, name, length in s["funcs"]:
            long_funcs.append((length, rel, start, name))
    long_funcs.sort(reverse=True)
    long_funcs = long_funcs[:args.top]
    inc_heavy = sorted(file_stats.items(),
                       key=lambda kv: kv[1]["includes"], reverse=True)
    inc_heavy = [(rel, s["includes"]) for rel, s in inc_heavy
                 if rel.endswith(tuple(HEADER_EXTS))][:args.top]
    todo_files = sorted(((rel, len(s["todos"])) for rel, s in file_stats.items()
                         if s["todos"]), key=lambda kv: kv[1], reverse=True)
    todo_files = todo_files[:args.top]

    churn_risk = []
    for rel, s in file_stats.items():
        c = churn.get(rel, 0)
        if c:
            churn_risk.append((c * min(s["loc"], 3000), rel, c, s["loc"]))
    churn_risk.sort(reverse=True)
    churn_risk = churn_risk[:args.top]

    header_multi.sort(key=lambda t: (-t[0], t[1]))
    magic_files.sort(key=lambda t: (-t[0], t[1]))

    report = {
        "root": str(root),
        "scanned_files": len(file_stats),
        "total_loc": total_loc,
        "top_files_by_size": [(r, s["loc"]) for r, s in top_files],
        "longest_functions": [(r, ln, name, length)
                              for length, r, ln, name in long_funcs],
        "include_heavy_headers": inc_heavy,
        "todo_hotspots": todo_files,
        "churn_x_size": [(r, c, loc) for _, r, c, loc in churn_risk],
        "red_flags": {
            flag: {"count": a["count"], "files": len(a["files"]),
                   "samples": a["samples"][:3]}
            for flag, a in sorted(flag_totals.items(),
                                  key=lambda kv: kv[1]["count"], reverse=True)
            if a["count"] > 0
        },
        "header_hygiene": {
            flag: {"count": a["count"], "files": len(a["files"]),
                   "samples": a["samples"][:3]}
            for flag, a in hygiene_totals.items()
        },
        "function_naming": {
            "distribution": dict(func_style_counter),
            "samples": [f"{r}:{ln}  {name} ({st})"
                        for st, r, ln, name in func_style_samples[:8]],
        },
        "member_naming": {label: {"count": member_totals[label],
                                  "samples": member_samples[label]}
                          for label in member_totals},
        "log_field_style": {
            "separators": dict(log_sep_totals),
            "key_styles": dict(log_style_totals),
            "top_keys": dict(log_key_totals.most_common(12)),
        },
        "build_hardening": {
            "files_scanned": build_nfiles,
            "detected": build_detected,
            "categories_missing": build_missing,
            "runtime_cost": dict(CATEGORY_RUNTIME_COST),
        },
        "header_composition": {
            "multi_class_headers": [{"file": rel, "count": n,
                                     "classes": names[:8]}
                                    for n, rel, names in header_multi[:10]],
        },
        "wrapper_bypass": {key: dict(counter)
                           for key, counter in wrapper_totals.items()},
        "magic_numbers": {
            "total": sum(n for n, _rel, _s in magic_files),
            "files": [[rel, n] for n, rel, _s in magic_files[:10]],
            "samples": [f"{rel}:{idx}  {s}" for _n, rel, ss in magic_files[:4]
                        for idx, s in ss],
        },
    }

    if args.as_json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return 0

    out = [f"=== C++ hotspot report: {report['root']} ===",
           f"scanned: {report['scanned_files']} files, "
           f"{total_loc} LOC\n"]

    out.append("-- Top files by size --")
    for r, loc in report["top_files_by_size"]:
        out.append(f"{loc:>7}  {r}")

    out.append("\n-- Longest functions (heuristic) --")
    for r, ln, name, length in report["longest_functions"]:
        out.append(f"{length:>5} lines  {name}  ({r}:{ln})")

    out.append("\n-- Include-heaviest headers --")
    for r, n in report["include_heavy_headers"]:
        out.append(f"{n:>5} includes  {r}")

    out.append("\n-- TODO/FIXME/HACK density --")
    if report["todo_hotspots"]:
        for r, n in report["todo_hotspots"]:
            out.append(f"{n:>5}  {r}")
    else:
        out.append("(none)")

    out.append("\n-- Churn (12mo) x size, highest-risk files --")
    if report["churn_x_size"]:
        for r, c, loc in report["churn_x_size"]:
            out.append(f"{c:>5} commits / {loc:>6} LOC  {r}")
    else:
        out.append("(not a git repo, or no churn)")

    out.append("\n-- Red-flag pattern counts (CANDIDATES, verify in context) --")
    for flag, a in report["red_flags"].items():
        out.append(f"{a['count']:>5} in {a['files']:>3} files  {flag}")
        for sample in a["samples"]:
            out.append(f"            e.g. {sample}")

    out.append("\n-- Header hygiene (uniformity candidates) --")
    if report["header_hygiene"]:
        for flag, a in report["header_hygiene"].items():
            out.append(f"{a['count']:>5}  {flag}")
            for sample in a["samples"][:2]:
                out.append(f"            e.g. {sample}")
    else:
        out.append("(headers look uniform)")

    out.append("\n-- Header composition (classes per header; CANDIDATES — judge relatedness) --")
    if header_multi:
        for count, rel, names in header_multi[:8]:
            out.append(f"{count:>5}  {rel}: "
                       f"{', '.join(names[:5])}{' ...' if len(names) > 5 else ''}")
        out.append("        single-class headers are the healthy default; co-located")
        out.append("        helpers serving one primary class are fine")
    else:
        out.append("(no multi-class headers)")

    out.append("\n-- Wrapper-bypass candidates (consistency.md §M; measure first) --")
    fam_lines = []
    if len(wrapper_totals["time"]) >= 2:
        items = ", ".join(f"{k}({v})" for k, v in wrapper_totals["time"].most_common())
        fam_lines.append(f"  时间获取 {len(wrapper_totals['time'])} 种来源并存: {items}")
        fam_lines.append("          (墙钟 vs 单调钟用途不同——分用途后再判断是否绕过)")
    if len(wrapper_totals["rand"]) >= 2:
        items = ", ".join(f"{k}({v})" for k, v in wrapper_totals["rand"].most_common())
        fam_lines.append(f"  随机数 {len(wrapper_totals['rand'])} 种来源并存: {items}")
    if len(wrapper_totals["crc"]) >= 2:
        items = ", ".join(f"{k}({v})" for k, v in wrapper_totals["crc"].most_common())
        fam_lines.append(f"  CRC 实现 {len(wrapper_totals['crc'])} 个不同名: {items}")
    if fam_lines:
        out.extend(fam_lines)
        out.append("          (若项目已有统一封装，直调标准库的散点即候选偏离；"
                   "先 grep 封装名定位封装层。注意：封装模块自身实现"
                   "（如 util.cpp）里的时钟调用也计入总数——判断前先剔除)")
    else:
        out.append("(各设施来源单一，未见绕过候选)")

    out.append("\n-- Magic-number candidates (unnamed 2+ digit literals; judge units/contracts) --")
    if magic_files:
        out.append(f"{sum(n for n, _r, _s in magic_files):>5}  literals in "
                   f"{len(magic_files)} files (constant-definition lines excluded)")
        for n, rel, samples in magic_files[:5]:
            out.append(f"{n:>5}  {rel}")
            for idx, s in samples[:1]:
                out.append(f"            e.g. {rel}:{idx}  {s}")
        out.append("        (0/1/-1、枚举值、具名常量定义行已排除；单位/契约不明即候选)")
    else:
        out.append("(no magic-number candidates)")

    out.append("\n-- Function naming style (heuristic; ctors/dtors counted) --")
    if report["function_naming"]["distribution"]:
        dist = report["function_naming"]["distribution"]
        peak = max(dist.values())
        for st, c in sorted(dist.items(), key=lambda kv: kv[1], reverse=True):
            out.append(f"{c:>5}  {st}")
        minority = [st for st, c in dist.items() if 0 < c <= 0.25 * peak]
        if minority:
            out.append("   minority-style examples (flag these, not the majority):")
            shown = 0
            for st, r, ln, name in func_style_samples:
                if st in minority and shown < 4:
                    out.append(f"            e.g. {r}:{ln}  {name} ({st})")
                    shown += 1
    else:
        out.append("(no functions detected)")

    out.append("\n-- Member naming signals (candidates; verify they are members) --")
    if any(member_totals.values()):
        for label, _ in MEMBER_RULES:
            out.append(f"{member_totals[label]:>5}  {label}")
            for s in member_samples[label][:2]:
                out.append(f"            e.g. {s}")
    else:
        out.append("(no member-naming signals found)")

    out.append("\n-- Log/output field style signals (CANDIDATES; judge by audience) --")
    if log_sep_totals or log_key_totals:
        out.append(f"{log_sep_totals.get('=', 0):>5}  key=value   "
                   f"{log_sep_totals.get(':', 0):>5}  key:value")
        for st, c in log_style_totals.most_common(4):
            out.append(f"{c:>5}  key style {st}")
        top = ", ".join(f"{k}({n})" for k, n in log_key_totals.most_common(12))
        out.append(f"        distinct keys: {top}")
        out.append("        watch: same field two names (*_ms vs *_us), mixed separators")
    else:
        out.append("(no log key/value signals found)")

    out.append("\n-- Build hardening signals (checklist.md §7; presence measured) --")
    if build_nfiles:
        out.append(f"{build_nfiles} build file(s) scanned")
        if build_detected:
            cur = None
            for cat, label, _rx in BUILD_FLAGS:
                if label not in build_detected:
                    continue
                if cat != cur:
                    note = CATEGORY_RUNTIME_COST.get(cat, "")
                    suffix = f"  -- runtime: {note}" if note else ""
                    out.append(f"  [{cat}]{suffix}")
                    cur = cat
                out.append(f"{build_detected[label]:>5}  {label}")
        else:
            out.append("(no warning/hardening flags detected at all)")
        for cat in build_missing:
            out.append(f"        missing [{cat}]: consider {CATEGORY_SUGGESTIONS[cat]}")
        out.append("        (absence is a suggestion — judge per toolchain; "
                   "vendored dirs may legitimately opt out)")
    else:
        out.append("(no build files found)")

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
