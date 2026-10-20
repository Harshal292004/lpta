# LPTA Change Log

This document records the implementation work performed in this workspace for
the LPTA (LLVM Pass Transformation Analyzer) PRD.

**Baseline:** git `HEAD` (`dc9f04a`) had no LPTA implementation. The working
tree now contains the complete plugin, reporter, offline viewer, wrapper,
build system, documentation, and tests.

**State:** all new files are uncommitted. The tracked deletions that were
already present in the working tree (`LEARN.md`, `PLAN.md`, `labs/*`,
`tests/c/*`, `tests/dot.ll`) were left untouched.

---

## 1. Summary of the implementation

LPTA was implemented from scratch as a loadable new-PM LLVM pass plugin. It:

- observes every non-infrastructure pass invocation through
  `PassInstrumentationCallbacks`;
- records before/after metrics and optional IR snapshots;
- computes structural deltas and a weighted significance score;
- emits terminal, JSON, and HTML reports;
- supports function/pass/min-score/top filters;
- captures unified IR diffs on request;
- supports cross-run trace comparison;
- includes an offline reader/viewer and a compiler wrapper;
- does not add, remove, or reorder optimization passes, and does not modify
  LLVM or any frontend.

---

## 2. Files added

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Builds the plugin, shared analysis library, viewer, tests, and install rules. |
| `src/Version.h` | LPTA version and trace-schema version macros. |
| `src/Plugin.cpp` | Plugin entry point, runtime LLVM major-version guard, callback registration. |
| `src/Collector.h` | Public collector lifecycle API. |
| `src/Collector.cpp` | Pass instrumentation, snapshots, event construction, JSON lifecycle, report emission. |
| `src/Options.h` | Options and process-introspection API. |
| `src/Options.cpp` | `-lpta-*` flags, `LPTA_OPTIONS`, weights, route/pipeline detection. |
| `src/Metrics.h` | Metrics, deltas, weights, and significance API. |
| `src/Metrics.cpp` | IR metrics, loop/SCC aggregation, weight parsing, score formula. |
| `src/Diff.h` | Unified-diff API. |
| `src/Diff.cpp` | Bounded Myers diff, fallback block diff, 4 MiB output guard. |
| `src/Trace.h` | JSON writer/reader and snapshot-file API. |
| `src/Trace.cpp` | Streaming JSON schema 1.0 writer/reader, atomic trace files, diff refs. |
| `src/Report.h` | Shared event/trace/statistics models, filters, report APIs. |
| `src/Report.cpp` | Aggregation, ranking, terminal/HTML rendering, comparison, atomic writes. |
| `tools/lpta-report.cpp` | Offline trace viewer and comparator. |
| `scripts/lpta` | Compiler wrapper that supplies the plugin and `LPTA_OPTIONS`. |
| `tests/unit/metrics_test.cpp` | Unit checks for metrics, loops, scoring, and diffs. |
| `tests/lpta/sample.c` | C integration input with inlining/loop/dead-branch opportunities. |
| `tests/lpta/handwritten.ll` | Hand-written IR input for the `opt` route. |
| `tests/lpta/run.sh` | End-to-end integration checks. |
| `tests/lpta/wrapper-test.sh` | Wrapper round-trip check. |
| `README.md` | User-facing usage, options, schema, architecture summary, limitations. |
| `CHANGES.md` | This change log. |
| `ARCHITECTURE.md` | Detailed internals and module-interaction guide. |

The second documentation pass also added links to both new documents from the
main README.

---

## 3. Behavioral changes by area

### 3.1 Build and plugin loading

- Added a CMake build for three main artifacts:
  - `build/LPTA.so` — the loadable pass plugin;
  - `build/lpta-report` — the offline trace viewer/comparator;
  - `build/lpta-metrics-test` — the unit-test executable.
- Added a shared static/pic analysis library, `lpta_analysis`, containing
  `Metrics`, `Diff`, `Report`, and `Trace` so the plugin and viewer share the
  same implementation.
- Enforced LLVM 22+ at CMake configure time.
- Added fallback linking to LLVM component libraries for installations that do
  not provide an imported `LLVM` target.
- Added `-fno-rtti`, `-Wall`, and `-Wextra` for LLVM-compatible compilation.
- Added installation rules for `LPTA.so`, `lpta-report`, and `scripts/lpta`.
- Added a runtime LLVM major-version check in `Plugin.cpp` using
  `LLVMGetVersion`. A mismatched plugin prints one clear error and returns a
  null callback instead of entering an incompatible ABI.

### 3.2 Instrumentation and collection

- Registered all four relevant pass-instrumentation callbacks:
  - before non-skipped pass;
  - before skipped pass;
  - after pass;
  - after pass invalidated.
- Added thread-local pass frames so nested passes are tracked independently per
  compilation thread.
- Added real nesting context:
  - `parent_pass` is the enclosing pass frame;
  - `depth` is the number of enclosing frames;
  - pass-manager and adaptor frames are retained for context even though they
    are not emitted as report events.
- Added infrastructure filtering for pass managers, adaptors, analysis
  invalidation/requirement bookkeeping, repeated passes, and wrapper passes.
  This prevents wrappers and analysis bookkeeping from appearing as
  transformation passes or double-counting child changes.
- Added before/after snapshots for module, function, loop, SCC, and fallback
  "other" IR units.
- Added constant-time-ish function prefiltering (`unitMayMatch`) so a function
  regex does not force expensive snapshots for unrelated module/loop/SCC
  invocations.
- Added optional skipped/invalidated event recording via `-lpta-skipped`.
- Added before/after metric capture, function attribution, affected-function
  union, and per-function records.
- Added self-time accounting: a pass's duration excludes nested child time,
  while full elapsed child time is propagated to the parent frame.
- Added event finalization that assigns dense indices only to events that pass
  the report filters.
- Added report emission at process exit with `atexit`, making the plugin output
  one merged report for the process while keeping callback state consistent.
- Added thread safety around global state with a mutex, atomic next-event index,
  atomic overhead counter, and an idempotent emit flag.

### 3.3 Options and process introspection

- Added all `-lpta-*` options:
  `report`, `top`, `function`, `pass`, `min-score`, `diff`, `color`, `out`,
  `html`, `json`, `compare`, `weights`, `demangle`, `pipeline`, and `skipped`.
- Added `LPTA_OPTIONS` parsing with shell-like quoting and boolean forms.
- Established precedence: built-in defaults, then `LPTA_OPTIONS`, then explicit
  command-line flags.
- Added validation and warnings for unknown views, colors, demangle modes, and
  malformed boolean values.
- Added regex filters with substring fallback when a regex is invalid.
- Added a weights-file loader supporting every metric key, comments, blank
  lines, and finite non-negative values.
- Added route detection: `opt`/`opt-*` or `-load-pass-plugin` means
  `opt-replay`; everything else means `in-compiler`.
- Added pipeline detection from `-passes=`/`-passes` or the last `-O0` through
  `-Oz` flag, with an override option.
- Intentionally leaked the option and process-argument singletons because the
  report is emitted from `atexit`, after function-local static destruction would
  normally have run.

### 3.4 Metrics and scoring

- Added all requested counters:
  instructions, basic blocks, functions, loads, stores, calls, branches, phis,
  allocas, loops, maximum loop depth, and call-graph edges.
- Counted direct call edges as unique direct callees per function; indirect
  calls count as calls but do not create a call-graph edge.
- Added module aggregation over definitions only.
- Added loop metrics over the loop and all nested subloops.
- Added SCC metrics and per-function attribution.
- Added LLVM-version-guarded branch detection (`UncondBr`/`CondBr` on LLVM 23+,
  `BranchInst` otherwise).
- Added the weighted significance formula:
  `sum(weight * abs(delta)) / max(1, before.insts)`.
- Added default weights and custom weights.
- Added unit and integration coverage for metric values and the score formula.

### 3.5 Reporting

- Added terminal views:
  - `summary` (default);
  - `passes`;
  - `functions`;
  - `full`;
  - `off`.
- Added ranked pass and function tables, function timelines, all-invocation
  output, inline IR diffs, and comparison output.
- Added 80-column-aware formatting for ranked terminal views. Long names are
  truncated with `...`; full values remain in JSON/HTML.
- Added ANSI color control with `auto`, `always`, `never`, and `NO_COLOR`.
- Added header reporting for source, producer, route, pipeline, LLVM version,
  passes run, changed passes, functions touched, and collection overhead.
- Added the heuristic disclaimer required by the PRD.
- Added a final "More:" hint for deeper views.
- Added self-contained HTML with inline CSS, top passes, top functions,
  expandable function timelines, and expandable IR diffs.
- Added `-lpta-out` file output and atomic writes for terminal reports, HTML,
  JSON, and diff snapshots.
- Removed the earlier terminal/HTML truncation caps so `full` really means all
  retained invocations and HTML timelines are not row-capped.

### 3.6 JSON trace and snapshots

- Added trace schema `1.0` with header fields and an events array.
- Added dense deterministic event indices for the events written to a trace.
- Added optional event fields instead of nulls:
  `unit_display_name`, `parent_pass`, `depth`, `affected_functions`,
  `skipped`, `invalidated`, `functions`, and `diff_ref`.
- Added per-event before/after/delta metric objects.
- Added per-function attribution arrays for non-function units.
- Added streaming JSON writing so only one event is held in memory at a time.
- Added atomic trace writing through a temporary file and rename.
- Added explicit write-error detection and `abort()` cleanup so a failed trace
  is not renamed into place.
- Added only report `Wrote: ...` status after a successful trace finish.
- Added diff snapshot files in `<trace-stem>.snapshots/<index>.diff` and
  relative `diff_ref` paths in the trace.
- Added a reader that validates schema majors and rejects unknown major
  versions, reconstructs metrics/deltas, resolves diff refs relative to the
  trace, and restores implicit function records for function events.
- Added schema-rejection integration coverage.

### 3.7 Diffs

- Added a bounded Myers line diff with a maximum edit distance of 1200.
- Added a prefix/suffix block-replacement fallback for very large changes.
- Added three lines of context and standard unified-diff headers.
- Added a 4 MiB output guard with an explicit truncation marker.
- Added empty output for identical snapshots.

### 3.8 Comparison, offline viewer, and wrapper

- Added live comparison through `-lpta-compare`.
- Added offline comparison through `lpta-report diff <base> <current>`.
- Alignment key: `(function-or-unit, pass, occurrence)`.
- First divergence checks before/after metrics, changed flag, unit kind, and
  missing/extra invocations.
- Added per-pass run/instruction/call differences, ignoring timing and score
  for divergence detection.
- Added an offline viewer with the same terminal views and filters as the live
  report.
- Added schema-aware trace loading in the viewer.
- Added a manually parsed viewer CLI to avoid colliding with LLVM's own
  `cl::opt` registrations.
- Added `scripts/lpta` with plugin discovery from `LPTA_PLUGIN`, `../build`,
  `../lib`, the script directory, and the current directory.
- Added wrapper option mapping from `--name=value` to `-lpta-name=value`.
- Added route-specific plugin loading:
  - `opt`/`opt-*`: `-load-pass-plugin`;
  - `rustc`: `-Z llvm-plugins`;
  - other compilers: `-fpass-plugin`.

### 3.9 Tests and documentation

- Added a unit test for instruction/block/load/store/call/branch/phi/alloca,
  loop, maximum-depth, call-edge, module, score, and diff behavior.
- Expanded the end-to-end suite to 53 checks covering:
  - clang in-compiler loading and report output;
  - stderr/stdout separation;
  - byte-identical code generation with and without the plugin;
  - JSON schema, dense indices, nesting, function attribution, loop/SCC units,
    infrastructure filtering, diff refs, and snapshots;
  - unknown schema rejection;
  - offline viewer and `--top`;
  - self-contained HTML and expandable diffs;
  - all four terminal views and 80-column checks;
  - function/pass/min-score filters;
  - `-lpta-out`;
  - `LPTA_OPTIONS`;
  - `opt`/hand-written IR;
  - inline diffs;
  - cross-run comparison;
  - C++ plugin loading and object identity;
  - Rust IR replay through `opt`;
  - wrapper operation.
- Added `README.md` usage, options, views, schema, weights, comparison,
  architecture summary, and limitations.
- Added `ARCHITECTURE.md` and this `CHANGES.md`.
- Documented the remaining limitations, including the `PreservedAnalyses` /
  `CorrelatedValuePropagationPass` changed-flag caveat, transient-vs-persisted
  IR snapshots, LLVM 22 runtime status, multi-module headers, and P2/stretch
  scope.

---

## 4. Hardening fixes made during final audit

These were corrections made after the first working implementation:

1. **Atomic-file cleanup**
   - Failed temp writes and failed renames now remove their temporary files in
     `Report.cpp` and `Trace.cpp`.

2. **JSON write correctness**
   - `JsonTraceWriter::write()` checks the underlying stream error state.
   - A failed or incomplete trace is aborted instead of being renamed.
   - `Wrote: <trace>` is only printed after a successful finish.

3. **Runtime version safety**
   - `Plugin.cpp` compares the compile-time LLVM major with the runtime
     `LLVMGetVersion()` result and prints a clear mismatch message.

4. **Infrastructure filtering**
   - Broadened `isInfrastructure()` to exclude:
     - pass managers (`PassManager<...>`);
     - adaptors (`*PassAdaptor`);
     - analysis invalidation/requirement passes;
     - repeated passes (`*RepeatedPass`);
     - wrapper passes (`*WrapperPass`).
   - After this change the sample trace reports 262 events and 48 changed
     events; the filtered wrapper/bookkeeping invocations no longer appear.

5. **Real nesting context**
   - `parent_pass` and `depth` now include enclosing pass-manager/adaptor
     frames. Before this fix, nested events had no parent/depth because only
     non-infrastructure frames were counted.

6. **Truncation removal**
   - Removed the 300-row function-timeline cap, the 5000-row full-view cap, and
     the 500-row HTML timeline cap. The full view and HTML timelines now report
     all retained rows.

7. **Test expansion**
   - Added nesting, infrastructure-filtering, schema-rejection, `-lpta-out`,
     view-width, C++, Rust, and wrapper checks.
   - Final integration result: 53 passed, 0 failed.

8. **Documentation of caveats**
   - Added the changed-flag/CVP note and the P2/stretch list to the README.

---

## 5. Validation performed

Environment: `clang`, `opt`, and `llvm-config` version **23.1.3** (the code is
written for LLVM 22+, with LLVM 22 runtime testing unavailable here).

Commands run:

```bash
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Results:

- Clean build with `-Wall -Wextra`, no warnings.
- `lpta-metrics`: passed.
- `lpta-integration`: passed.
- Integration checks: **53 passed, 0 failed**.
- Terminal `summary`, `passes`, `functions`, and `full` views: **0 lines over
  80 columns** in the width check.
- C++ object output is byte-identical with and without the plugin.
- Rust IR generated by `rustc` replays through `opt` and is reported with
  `producer=rustc` and `route=opt-replay`.
- Hand-written `.ll` input works through the `opt` route.
- Measured overhead on a synthetic 200-function translation unit was about
  **1%** (base ~0.566 s vs plugin ~0.569 s), well under the PRD's 2x ceiling.
- The changed-pass set matches LLVM's `-print-changed` on the sample except for
  `CorrelatedValuePropagationPass`, which abandons `LazyValueAnalysis` without
  changing IR; this is documented.

---

## 6. What was intentionally not changed

- No LLVM source files were patched.
- No optimization passes were added, removed, reordered, or forced.
- No frontend source was patched.
- No pre-existing tracked files were restored or modified.
- No commit was created.
- P2/stretch items from the PRD remain out of scope for this v1:
  - standalone `.ll`/`.bc` driver;
  - source/debug-info mapping;
  - codegen correlation;
  - compact CI-oriented summary JSON;
  - optional Markdown export.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the detailed runtime design and
module interactions.
