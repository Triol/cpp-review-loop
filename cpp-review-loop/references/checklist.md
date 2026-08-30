# Review Checklist — Large-Scale C++

Concrete red flags per dimension. Every item is a **candidate** to investigate: open the file and read context before reporting. Each entry says why it matters so you can judge severity in context rather than pattern-matching.

At the end: the "what NOT to flag" list (anti-noise) and the severity rubric.

---

## 1. Architecture & module boundaries

- **Dependency direction violations** — low-level modules including high-level ones (`grep -rn '#include "'` across module dirs; compare against the intended layering in the build files). Why: in large codebases this is how a "utils" library ends up depending on the UI, making both untestable and unbisectable.
- **Circular include dependencies between modules** — module A's headers include module B's and vice versa. Heuristic: cross-directory `#include` pairs in both directions. Fix class: invert with an interface header or move shared types to a leaf module.
- **Paths reaching out of the module** — `#include "../other_module/..."` or absolute-ish deep paths. Sign the module's boundary is not real.
- **God objects / god files** — files >2–3k lines or a class with >50 members / mixed responsibilities (parsing + IO + policy). Check the hotspot list first. Drift pattern is the same as god functions: a file absorbs one feature per iteration; judge by responsibilities mixed, not line count alone — a 2k-line single-cohesive-parser file can be healthier than an 800-line file doing five unrelated jobs. Fix class: split by responsibility, not by size.
- **Kitchen-sink header — many classes in one header** — a header declaring (or worse, implementing) several *unrelated* classes forces every includer to depend on all of them: compile-time coupling, needless recompilation, merge-conflict magnet, and the extraction cost grows each iteration. Judge relatedness, not count: small helper types that only serve the header's primary class are fine co-located; two subsystems' public classes sharing one header are not. A header should also not carry out-of-line-worthy implementation bodies (inline bloat → compile time; non-inline definitions → ODR/link errors). hotspots.py counts class definitions per header (*Header composition* signal) — candidates only, relatedness is a human call. Fix class: one primary class per header plus its private helpers; split by consumer, not by size.
- **Singletons & global mutable state** — `GetInstance()`, `static` non-const objects with non-trivial constructors. Why: static init/destruction order fiascos, hidden coupling, untestable code. Distinguish "config-style read-only" (tolerable) from mutable service locators (report).
- **Public headers exposing implementation** — private impl types, wide headers that drag in half the codebase. Pimpl candidates: headers with many `#include` that could be forward declarations.
- **Layer mixing inside one file** — protocol parsing, business rules, and OS calls in one function.
- **God functions — one body, several jobs** — hotspots.py's longest-functions ranking is the entry point, but line count alone is not the finding: the smell is mixed abstraction levels (parsing + orchestration + IO + formatting in one body), `// ---- step 1 ----` section comments inside a function (implicit sub-logic begging to be extracted), or a name as vague as `process`/`handle`. Real drift pattern: functions grow one feature per iteration (measured case: `main` 139→218 lines over 5 feature rounds, `runOne` 62→86) until nobody can change them safely. Fix class: extract sub-logic at the section boundaries. Minor for internal helpers; Major when the function is a module's integration point and effectively untestable.
- **Copy-paste-adapted logic without a shared abstraction** — the same field list / format string / table rendering maintained in N places (one serializer per output format, a trace format spelled out at every call site). Why: adding one field means editing N sites; miss one and outputs silently disagree — invisible until a consumer parses the stale one. Signal: near-identical printf/serialization blocks in different functions, the same enum→string mapping written twice, a field list enumerated in both a writer and its parser. Exact-line dedupe tooling is too brittle (one renamed field breaks it) — judge structurally. Fix class: single source of truth (one shared field-list constant or one render function parameterized by format).
- **Magic numbers — unnamed literals in logic** — `if (retries == 3)`, `usleep(86400)`, `buf[512]`: the numeral hides a unit or contract (seconds? ms? who knows), changes require hunting every copy, and grepping the intent is impossible. hotspots.py counts two-plus-digit literals outside constant-definition lines (*magic-number* signal, per-file counts + samples) — candidates; single-digit sentinels (0/1/-1), enum values, and values in named-constant definitions are fine. Fix class: named `constexpr` constants following the project's naming convention (`kFoo` / `FOO`), unit in the name (`kTimeoutMs`).

## 2. Headers & API design

- **`using namespace ...` in headers** (`grep -n "using namespace" **/*.h`) — pollutes every includer's namespace at scale; never acceptable in a header.
- **Include order, placement, and guards** — project-uniformity checks (sorting within groups, code before includes, `#pragma once` vs `#ifdef` guards) live in references/consistency.md §B; flag deviations from the project's own convention, not from your taste.
- **Log/output format uniformity** — `key=value` vs `key: value` styles, field renames across iterations, struct fields placed after free-text fields, status-value set drift: measure-then-flag per references/consistency.md §L (scanner signal: *Log field style signals*).
- **Standard-library wrapper bypass** — when the project wraps a facility (time / rand / CRC / logging), scattered direct std/OS calls defeat the testability seam and drift apart; measure-then-flag per references/consistency.md §M (scanner signal: *Wrapper-bypass candidates*).
- **Headers that include what they could forward-declare** — include-what-you-use violations in the *public* surface. Compiles-slow is a real large-scale cost.
- **Polymorphic base with non-virtual destructor** — class has `virtual` functions but no `virtual ~Base()`; delete via base pointer is UB. High-severity, verify inheritance actually happens.
- **Missing `explicit` on single-argument constructors** — silent implicit conversions.
- **Public raw-owning pointers in interfaces** — `Widget* create()`, `set_thing(Widget*)` with no ownership documented. This is where ownership bugs are born.
- **Returning references/pointers to internals** — leaks representation, freezes implementation, and risks dangling on rehash/resize.
- **Macros as API** — `#define` in public headers instead of constants/inline functions (no scope, no type, collides at scale).
- **Deprecated / legacy constructs** — `std::auto_ptr`, `std::bind` where a lambda is clearer, `rand()`, `sprintf`, `strlen(x)==0` patterns, `NULL` vs `nullptr` inconsistency in new code.
- **ABI hazards (only if the project cares — look for explicit versioning/exports)** — STL types or default arguments in exported interfaces, inline functions changing layout, missing export macros.
- **`const` correctness of the API** — accessors that should be `const`/`noexcept` but aren't; `const&` parameters for non-trivially-copyable types.
- **`virtual` and `override` on the same declaration** — C.128: a declaration carries exactly one of `virtual`/`override`/`final`. hotspots.py `virtual + override together` is a true violation; its `virtual without override` count is only a *measure* — separate base declarations (fine) from derived overrides missing the keyword (report) by reading the class hierarchy (references/standards.md §G).
- **Default arguments on virtual functions** — the default arg binds statically, the function dynamically: calling through a base pointer and through the derived type picks different arguments. Report on sight.

## 3. Ownership & resource management

- **Raw owning `new`** — `new T` whose result is stored in a raw pointer for longer than a function scope. Check whether a smart pointer takes it later. Aggregated pattern: count per module (see hotspots.py `raw new` flag).
- **`new[]` with `delete`** (or `malloc` with `delete`) — mismatched pairing is UB.
- **Manual `lock()`/`unlock()` without a guard** — every early return / exception path leaks the lock.
- **`shared_ptr` as default** — where `unique_ptr` (or a value) suffices; `shared_ptr` in hot-path signatures (atomic refcount cost); `const shared_ptr<T>&` parameters (the `const&` buys nothing).
- **`shared_ptr` cycles** — parent/child or observer pairs both holding `shared_ptr`; needs `weak_ptr` somewhere. Verify the graph shape before reporting.
- **`enable_shared_from_this` misuse** — `shared_from_this()` called from a constructor (the control block doesn't exist yet → throws/UB).
- **Non-memory resources without RAII** — file handles, sockets, OS handles, GPU/DB handles managed by hand. One wrapper class per resource type is the expected state in a mature codebase.
- **Ownership ambiguity across module boundaries** — an interface takes a pointer and neither side's docs say who deletes. Any one ambiguous interface is worth flagging; count the pattern.
- **`delete this` / self-owning objects, `this` captured without lifetime guarantees** — lambdas/threads capturing raw `this` that can outlive the owner. Cross-check with the concurrency section.
- **`std::string_view` / spans stored as members or escaping calls** — they own nothing; a member or async callback holding one dangles when the source string dies. grep and verify each lifetime (references/standards.md §F).
- **Two-phase initialization** (`Foo f; f.init();`) — half-built objects that are usable by mistake and forgettable. Widespread → systemic pattern, not per-site nits.
- **`unique_ptr<T>(new T)` instead of `make_unique`** — only worth aggregating when the project already uses `make_unique` broadly (anti-noise: no drive-by modernization).

## 4. Concurrency

- **Fields "guarded by mutex" that aren't** — grep each mutable member of a mutex-guarded class; find any access outside the lock (const methods, callbacks, operators). This grep-and-verify is the single highest-yield concurrency check.
- **`mutable` used to mutate shared state in const methods** — sometimes legitimate (lazy cache), often a race.
- **Lock held across callbacks / I/O / logging with user hooks** — deadlock or latency bomb. Check whether any function called under the lock re-enters the same class.
- **Multiple locks without documented ordering** — two locks acquired in different orders on different paths.
- **`volatile` used for thread synchronization** — it is not a sync primitive in C++. Usually indicates hand-rolled synchronization that needs an atomic.
- **Plain (non-atomic) shared data across threads** — shutdown flags, stats counters, cached pointers touched by multiple threads.
- **Threads/async lifetimes** — `detach()`ed threads outliving their owner; `jthread`/join missing on destruction; callbacks fired after the listener died (dangling `this`); futures whose shared state keeps objects alive unexpectedly.
- **Atomics** — `memory_order_relaxed` used where read-modify-write ordering matters; hand-rolled double-checked locking; `atomic<shared_ptr>` vs `shared_ptr` copy confusion.
- **Condition-variable waits without a predicate loop** (`while` not `if`), missed-wakeup patterns.
- **`thread_local` with non-trivial destructors in hot paths or DLL/SO unload paths.**
- **Default lambda captures `[=]`/`[&]`** — invisible capture sets; `[=]` copies `this`, `[&]` dangles in anything async (hotspots.py `default lambda capture`). Cross-check with the lifetime items in §3.

## 5. Error handling

- **Mixed error styles in the same layer** — some functions throw, siblings return error codes or `bool` or `optional`. Pick the dominant project convention; flag deviations, not the convention itself.
- **`catch (...)` that swallows** — especially with no log. Also empty `catch (const std::exception&) {}`.
- **Exceptions thrown from destructors** — `noexcept` violation, terminates during stack unwinding.
- **Throw by value / catch by value** — object slicing; catch should be by const reference.
- **Exceptions across module/ABI boundaries** — throwing through `extern "C"` or across DLL boundaries compiled with different flags.
- **Unchecked error returns** — `(void)f()` casts, ignored `std::error_code`/out-params, unchecked `fopen`/`system` results in non-test code.
- **`noexcept` correctness** — move constructors that allocate/throw (kills `vector` reallocation performance → actually a perf bug); `noexcept` functions that can throw.
- **printf-family format-specifier / argument-type mismatches** — `%d` against `size_t`, `%lu` vs `%llu` drift after a type change mid-iteration. Variadic args have no type checking: a mismatch is UB. Prefer compiling with `-Wformat` and quoting it; otherwise verify hotspots.py `printf: size_t arg w/o %z` hits by hand (references/consistency.md §L).
- **Error messages without context** — bare `return -1;` with no logging, at scale this makes production debugging impossible. Weight by how central the module is.
- **`catch` by value** — object slicing: `catch (std::exception e)` truncates the derived exception. Catch by `const&` (hotspots.py `catch by value`, common class-type forms).
- **Signed/unsigned comparisons and silent narrowing** — `for (int i = 0; i < v.size(); ++i)`, `-1` compared against unsigned, int→short / double→float in arguments (CERT INT). Correctness, not style (references/standards.md §I).

## 6. Performance (mark suspicions "needs measurement" unless profiling evidence exists)

- **Allocation in hot loops** — `std::string` concatenation, `push_back` without `reserve`, `map::operator[]` creating accidental entries, temporary containers per iteration.
- **Copies that should be moves** — pass-by-value of containers/strings in call paths; members updated via `s = s + x` instead of `+=`.
- **Wrong container for the pattern** — `std::map` for pure lookup (vs `unordered_map`), linear `find` in a loop, `vector<unique_ptr<T>>` of small uniform objects (vs vector of values).
- **String handling at scale** — `std::to_string`/`stringstream` in formatting hot paths, repeated parsing of the same input.
- **Virtual dispatch / `dynamic_cast` in tight loops** — only flag inside code identified as hot (profiler output, obvious per-packet/per-sample paths).
- **False sharing / layout** — only with measurement context; otherwise skip.
- Do NOT flag: returning big objects by value (RVO/move handles it), `auto` copies you misread as deep copies, anything you can't point to as "per-call/per-packet".

## 7. Build & dependency hygiene

- **Global state in the build** — CMake: `include_directories(...)` / `add_definitions(...)` at top level instead of per-target `target_include_directories` / `target_compile_definitions`. Why: at scale, global includes make every dependency implicit and unremovable.
- **Warnings & warnings-as-errors** — warning level low (`/W3` vs `/W4`, no `-Wall -Wextra -Wpedantic`); `-Werror` / `/WX` absent (at minimum for first-party targets and CI; narrow `-Werror=specific-check` is an acceptable middle ground). hotspots.py measures which flags are present (Build hardening signals); absence is a suggestion to weigh per toolchain, not a finding by itself.
- **Robustness compile options absent** — none of the cheap hardening switches are on: `-fstack-protector-strong`, `-D_GLIBCXX_ASSERTIONS`, `-D_FORTIFY_SOURCE=2` (release), `-fsanitize=address,undefined` in dev/CI builds; MSVC: `/sdl`, `/guard:cf`, `/permissive-`. Suggest per toolchain via `check_cxx_compiler_flag`-style guards, never as a blanket list — fortify/sanitizer changes affect performance and ABI, so frame as recommendation and verify applicability.
- **Hardening/sanitizer flags present — warn, don't fix** — when options with runtime cost are detected, tell the user the concrete risk and leave keep-or-remove entirely to them; never propose removal as a suggested fix. Costs to quote: sanitizers are dev/CI-only (ASan ≈2x CPU / 2-3x memory; never ship in a release build); hardening switches (`-fstack-protector*`, `_GLIBCXX_ASSERTIONS`, `/sdl`, `/guard:cf`, `_FORTIFY_SOURCE`) are typically ~0-3% and usually negligible on non-hot paths. Warnings / `-Werror` / `-Wconversion`-level flags cost nothing at runtime — no risk warning needed for those.
- **Vendored duplicates** — two versions of the same third-party lib (one vendored, one via package manager); locally patched copies with no note.
- **Non-hermetic includes** — code that compiles only because of include order or a transitive include from a pch; especially `#include <bits/...>` or `<windows.h>` leakage into public headers (macro pollution: `min`/`max`/`TRUE`).
- **PCH/unity-build landmines** — headers relying on PCH contents; files that only build in unity mode.
- **Missing/absent dependency pinning** — `git submodule` without pins, `FetchContent` with floating tags/master.

## 8. Testing & CI signals

- Which modules have tests adjacent to them, and which have none — report the *distribution*, not the absence in general.
- Tests that depend on singletons/global state, execution order, real network/filesystem, or wall-clock sleeps.
- Whether tests can run at all: find the test runner target; note if the suite appears unbuildable/stale (as a coverage note, not a code finding).

---

## What NOT to flag (anti-noise list)

These are the false positives that make reviews untrustworthy. Suppress them.

- Returning large objects **by value** — RVO/named-RVO/moves make this correct and usually optimal.
- `auto x = vec[i]` "should be `auto&`" — check the type first; for `map` value copies it's real, for cheap types it's a nit at best.
- `shared_ptr` *somewhere* without evidence of cost — needs a hot path or a cycle to be a finding.
- Style the project does **consistently** but differs from your taste (naming, brace style, header order). The project's `.clang-format`/`.clang-tidy` wins.
- Consistency is the exception, not a contradiction: deviations from the project's *own dominant* convention (naming, include order, guards, init style) are real findings — see references/consistency.md for the measure-then-flag method. What stays unflagged is consistent style you merely dislike.
- TODOs that are tracked and ticketed in an obviously maintained area.
- "Could be `constexpr`", "could use `std::span`" — modernization is only worth reporting if the project itself is mid-migration (check for `C++20` flags and existing span/constexpr usage).
- Any finding whose evidence is a grep hit you never opened in context.
- Defensive redundancy like extra `if (!ptr) return;` before unconditionally-safe code — harmless, not worth a line.

## Severity rubric

The authoritative rubric lives in SKILL.md §4 (Blocker / Major / Minor / Nit) — don't duplicate it here. One addition worth keeping in mind: **scale matters** — in a 1M-LOC codebase a missing `explicit` is a nit, while a systemic raw-`new` pattern is a Major even if no single site has crashed yet.
