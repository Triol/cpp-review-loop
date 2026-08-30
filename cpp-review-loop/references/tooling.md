# Static Analysis Tooling for C++ Reviews

Rules first:

- **Never install anything into the user's project** and never modify project files to make a tool run. If a tool is missing, note it in the coverage statement and move on.
- If the project has its own `.clang-tidy`, use it (maybe with extra `bugprone-*`/`clang-analyzer-*` checks added on the command line). Do not impose a foreign check list on a codebase that has opinions.
- Static analysis output = candidates, same as grep. A clang-tidy warning on a line you haven't read is not a finding; read the code, confirm, then report.

## Availability checks (run before promising anything)

```bash
clang-tidy --version          # Windows: may be bundled with VS ("C:\Program Files\Microsoft Visual Studio\...\clang-tidy.exe") or LLVM ("C:\Program Files\LLVM\bin")
cppcheck --version
clang-format --version        # tells you whether the project's style config is even enforceable
```

## compile_commands.json

clang-tidy and clangd both need it. Generate without building if the project is CMake:

```bash
cmake -S . -B build-review -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

(On Windows/MSVC generators this doesn't work — use `-G Ninja` with `clang` or use the VS-bundled clang-tidy with its own detection. If it fights you, skip tooling; the review stands on its own.)

## clang-tidy — the default pass

Reasonable review profile on top of the project's own config:

```bash
clang-tidy -p build-review --checks='-*,bugprone-*,performance-*,modernize-*,clang-analyzer-*' \
  --warnings-as-errors='bugprone-*' <the hotspot files>
```

Run it on the hotspot files you already picked, **not** the whole repo — whole-repo runs take ages and bury you in nits. Highest-value clang-tidy checks for reviews: `bugprone-use-after-move`, `bugprone-dangling-handle`, `bugprone-sizeof-expression`, `clang-analyzer-core.*`, `performance-for-range-copy`, `performance-unnecessary-copy-initialization`, `modernize-use-override` (missing `override` is a real override-typo detector).

## cppcheck — zero-config fallback

Works without a build system, decent for C-style code:

```bash
cppcheck --enable=warning,portability,performance --inline-suppr --suppress=missingIncludeSystem <src-dir>
```

## include-what-you-use

For header-hygiene findings (§2 of the checklist) — needs IWYU installed and the compile db. If absent, fall back to manual inspection of public headers.

## Sanitizers / dynamic verification

ASan/TSan runs (data races, UAF) require configuring and building the project — out of scope for a plain review. Only offer it explicitly ("我可以配一个 TSan 构建来验证这几个可疑的 data race") and only if a working build already exists. Never build-modify unilaterally.

## When nothing is installed

Say so in the coverage statement ("本机未检测到 clang-tidy/cppcheck，以下结论均来自人工阅读") and lean on the bundled hotspots.py + manual reading. That is a perfectly respectable review.
