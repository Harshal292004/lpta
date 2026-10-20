# LPTA Architecture

This document explains how LPTA works internally and how every source module
interacts. It describes the implementation as it exists in this workspace, not
an abstract design.

For the user-facing CLI, see [README.md](README.md). For the implementation
change log, see [CHANGES.md](CHANGES.md).

---

## 1. What LPTA is

LPTA is a loadable LLVM new-pass-manager plugin. It observes the optimization
pipeline through `PassInstrumentationCallbacks`, records a snapshot before and
after each pass invocation, computes structural metrics and deltas, and emits a
report at process exit.

It has two logical layers:

1. **Collector** — callback registration, pass frames, IR snapshots, metrics,
   event construction, JSON streaming, and the exit-time report trigger.
2. **Reporter** — aggregation, ranking, terminal/HTML rendering, diff display,
   comparison, and atomic file output.

The collector does not alter the pass pipeline. It does not add, remove,
reorder, or replace passes, and it does not patch LLVM or any frontend.

### Core invariants

- The plugin is loaded by the host compiler/tool; it is not a separate
  out-of-tree optimization driver.
- A pass callback never rewrites IR. It only reads IR and writes report data.
- A report is emitted once per process from `atexit`.
- Terminal output defaults to stderr; JSON and HTML are opt-in.
- Event indices are dense for the events written to the trace, starting at 0.
- Expensive work (IR printing, diffing, loop analysis) is only done when the
  selected output mode requires it.

---

## 2. Build targets and dependency graph

```text
                         CMakeLists.txt
                              |
          +-------------------+-------------------+
          |                   |                   |
          v                   v                   v
  lpta_analysis (STATIC)     LPTA (MODULE)      lpta-report (EXE)
  Metrics.cpp                Plugin.cpp         lpta-report.cpp
  Diff.cpp                   Collector.cpp             |
  Report.cpp                 Options.cpp               |
  Trace.cpp                       |                    |
          |                       |                    |
          +-----------+-----------+--------------------+
                      |
                      v
                 LLVM libraries
```

### Target responsibilities

| Target | Type | Contents | Links |
| --- | --- | --- | --- |
| `lpta_analysis` | static PIC library | `Metrics`, `Diff`, `Report`, `Trace` | LLVM Core/Support/Analysis/etc. |
| `LPTA` | loadable module | `Plugin`, `Collector`, `Options` | `lpta_analysis`, LLVM |
| `lpta-report` | executable | offline viewer/comparator | `lpta_analysis`, LLVM |
| `lpta-metrics-test` | executable | unit tests | `lpta_analysis`, LLVM |

The split is intentional:

- `lpta_analysis` contains no plugin command-line options. This lets
  `lpta-report` reuse the trace reader, aggregator, renderer, and diff code
  without registering `-lpta-*` options or colliding with LLVM's own
  `cl::opt` registry.
- `Options.cpp` is linked only into the plugin because it owns the
  plugin-specific command-line flags and process-introspection logic.
- `Collector.cpp` is the composition root for the plugin: it includes
  `Options`, `Metrics`, `Diff`, `Report`, `Trace`, and `Version`.

### Include/dependency direction

```text
Metrics.h                 standalone metric data and functions
Diff.h                    standalone unified-diff API
Report.h        --------> Metrics.h; defines Event/Trace/ReportOptions/Aggregator
Trace.h         --------> Report.h; defines JsonTraceWriter and readTrace
Options.h       --------> Report.h; defines option accessors
Collector.cpp   --------> Options, Diff, Report, Trace, Version
Plugin.cpp      --------> Collector, Version, LLVM plugin API
lpta-report.cpp --------> Report, Trace
```

`Trace.h` includes `Report.h` because a trace is a `TraceHeader` plus a vector
of `Event` objects. `Report.h` does not include `Trace.h`; the JSON writer is a
separate layer over the shared event model.

---

## 3. End-to-end runtime flow

```text
Host driver: clang / clang++ / opt / rustc / another LLVM tool
        |
        | loads LPTA.so
        v
llvmGetPassPluginInfo()  [Plugin.cpp]
        |
        | checks runtime LLVM major against compile-time LLVM major
        | returns PassBuilder callback
        v
PassBuilder constructed by the host
        |
        | calls LPTA callback
        v
registerCollectorCallbacks(PB)  [Collector.cpp]
        |
        | gets PassInstrumentationCallbacks
        | registers before/after/skipped/invalidated callbacks
        | registers atexit(emitReportAtExit)
        v
+-------------------------------------------------------------+
|                    optimization pipeline                     |
|                                                             |
|  beforePass -------> Frame (thread-local)                    |
|      |                  + before Snapshot                     |
|      |                  + parent/depth                        |
|      |                  + start timestamp                     |
|      v                                                       |
|  pass runs (LPTA does not modify it)                         |
|      |                                                       |
|  afterPass ---------> pop Frame                              |
|      |                  + self duration                      |
|      |                  + after Snapshot                     |
|      |                  + Event                               |
|      |                  + changed flag / score / diff         |
|      v                                                       |
|  finalizeEvent ----> dense index                             |
|      |                  + JsonTraceWriter (optional)         |
|      |                  + Aggregator (in-memory stats)       |
+-------------------------------------------------------------+
        |
        | process exits
        v
atexit -> Collector::emit()
        |
        +-- finish JSON writer
        +-- load baseline trace and compare (optional)
        +-- render HTML (optional)
        +-- render terminal report
        +-- write -lpta-out (optional) or stderr
        +-- print IO errors
```

For an `opt` replay, the same flow occurs, but the host is `opt` and the route
header is `opt-replay`. For `clang`, the route is `in-compiler`.

---

## 4. Plugin entry (`src/Plugin.cpp`)

`llvmGetPassPluginInfo()` is the exported entry point expected by LLVM's pass
plugin loader.

It performs three jobs:

1. Calls `LLVMGetVersion()` from the LLVM C API.
2. Compares the runtime major version with the compile-time
   `LLVM_VERSION_MAJOR`.
3. Returns a `PassPluginLibraryInfo` whose callback calls
   `lpta::registerCollectorCallbacks(PB)`.

On a major-version mismatch it prints:

```text
LPTA error: plugin built for LLVM X but loaded into LLVM Y.Z.W;
rebuild LPTA against the driver's LLVM.
```

and returns a null callback, so no LPTA instrumentation is installed instead of
entering an incompatible ABI.

The plugin header location is guarded:

```cpp
#if __has_include(<llvm/Plugins/PassPlugin.h>)
#include <llvm/Plugins/PassPlugin.h>
#else
#include <llvm/Passes/PassPlugin.h>
#endif
```

This supports the LLVM 23 header layout while retaining the LLVM 22 layout.

---

## 5. Options subsystem (`src/Options.cpp`)

Options are collected lazily on first use. This matters because the host driver
parses its own command line before optimization begins; by the time a pass
callback runs, `-lpta-*` options and `-mllvm` forwarding are available.

### 5.1 Sources and precedence

```text
built-in defaults
      |
      v
LPTA_OPTIONS environment variable
      |
      v
explicit command-line options
      |
      v
validation + weights loading + filter compilation + pipeline detection
```

- The `-lpta-*` flags are native LLVM `cl::opt` objects in `Options.cpp`.
- `LPTA_OPTIONS` is split with a small shell-like parser supporting single
  quotes, double quotes, backslash escapes, spaces, tabs, and newlines.
- Environment entries may be written as `-lpta-report=passes`,
  `--report=passes`, or `report passes`-style separated values where the parser
  recognizes the option as taking a value.
- Command-line occurrences override environment values via
  `getNumOccurrences()`.
- Unknown environment entries and invalid boolean values produce warnings.
- Invalid views/colors/demangle modes fall back to defaults with warnings.

### 5.2 Options API

```cpp
const ReportOptions &getOptions();
const std::vector<std::string> &getProcessArgs();
std::string getProcessName();
std::string detectRoute();
std::string detectPipeline(llvm::StringRef Explicit = {});
```

`getOptions()` and `getProcessArgs()` intentionally allocate their singleton
objects with `new` and never free them. The report is emitted from `atexit`;
normal function-local statics constructed during pass execution would be
destroyed before the exit handler runs, leaving dangling path/argument strings.

### 5.3 Process inspection

- **Linux:** reads `/proc/self/cmdline` and splits on NUL bytes.
- **macOS:** uses `_NSGetArgc()` and `_NSGetArgv()`.
- Other platforms return an empty argument vector; the plugin still works, but
  source fallback/route/pipeline detection may be less precise.

`detectRoute()`:

- returns `opt-replay` if the process basename is `opt` or starts with `opt-`;
- also returns `opt-replay` if any argument is `-load-pass-plugin` or
  `-load-pass-plugin=...`;
- otherwise returns `in-compiler`.

`detectPipeline()`:

1. uses an explicit `-lpta-pipeline` value if provided;
2. scans for `-passes=...` or `-passes ...`;
3. otherwise scans arguments in reverse and maps the last `-O0`, `-O1`, `-O2`,
   `-O3`, `-Os`, or `-Oz` to `default<Ox>`;
4. returns `unknown` if nothing matches.

### 5.4 Filter compilation

`ReportOptions::compileFilters()` constructs `NameFilter` objects for the
function and pass patterns.

- Empty pattern = match everything.
- Valid LLVM regex = regex match.
- Invalid regex = warning and fallback to substring matching.
- Function matching checks both raw symbols and demangled display names where
  applicable.

### 5.5 Weights loading

`-lpta-weights=FILE` is read during option construction. The parser accepts
`name=value` lines, ignores blank lines and `#` comments, rejects non-finite or
negative values, and warns on unknown metric names. Missing keys keep their
defaults.

---

## 6. Collector (`src/Collector.cpp`)

The collector is a process-wide singleton inside an anonymous namespace.
`collector()` returns a static `Collector`, and the public functions
`registerCollectorCallbacks()` and `emitReportAtExit()` delegate to it.

### 6.1 State

```cpp
thread_local std::vector<Frame> Frames;

class Collector {
  std::mutex Mutex;
  std::set<PassInstrumentationCallbacks *> Registered;
  std::atomic<uint64_t> NextIndex;
  std::atomic<uint64_t> OverheadUs;
  std::atomic<bool> Emitted;

  TraceHeader Header;
  bool HeaderCaptured;

  std::unique_ptr<Aggregator> Agg;
  JsonTraceWriter Json;
  bool JsonOpenAttempted;
  bool JsonFailed;
};
```

Why the split:

- `Frames` is thread-local because LLVM can run function passes on multiple
  compiler threads. Each thread maintains its own nesting stack.
- `NextIndex`, `OverheadUs`, and `Emitted` are atomic because finalization and
  exit emission can be reached from multiple threads.
- Header, aggregator, and JSON writer are guarded by `Mutex`.
- `Agg` is created lazily when the first event is finalized.
- `Json` is inert unless `-lpta-json` is set.

### 6.2 Frame

A `Frame` represents one pass invocation:

```cpp
struct Frame {
  std::string pass;          // pass ID
  bool infra;                // pass manager / adaptor / bookkeeping
  bool recording;            // has a valid before snapshot
  Clock::time_point start;
  uint64_t childNs;          // nested child time
  Snapshot before;
  std::string parent;        // enclosing frame's pass ID
  unsigned depth = 0;        // number of enclosing frames
};
```

Frames are pushed for every pass, including infrastructure passes. Infrastructure
frames do not create events, but they are needed for:

- correct parent/depth context;
- correct self-time subtraction;
- propagating child time to enclosing passes.

### 6.3 Callback registration

`registerCallbacks(PassBuilder &PB)`:

1. Gets `PB.getPassInstrumentationCallbacks()`.
2. If unavailable, prints an error and returns.
3. Inserts the callback set pointer into `Registered`, so the same callback set
   is instrumented only once.
4. Registers the `atexit` handler exactly once with `std::call_once`.
5. Registers:
   - `registerBeforeNonSkippedPassCallback` → `beforePass`;
   - `registerBeforeSkippedPassCallback` → `beforeSkippedPass`;
   - `registerAfterPassCallback` → `afterPass`;
   - `registerAfterPassInvalidatedCallback` → `afterPassInvalidated`.

### 6.4 Snapshot model

```cpp
struct Snapshot {
  bool valid;
  std::string kind;                     // module/function/loop/scc/other
  std::string name;
  std::string display;
  std::vector<std::string> affected;    // function names
  Metrics metrics;
  std::map<std::string, Metrics> functions;
  std::string ir;                       // optional printed IR
};
```

`makeSnapshot(llvm::Any &IR, bool CaptureIR)` uses `probeAny<T>()` to recover
pointers from the callback payload and normalizes the IR unit.

| Unit | Kind | Name | Metrics | Function attribution | IR text |
| --- | --- | --- | --- | --- | --- |
| `Module` | `module` | module source filename or `<module>` | `computeModuleMetrics` | all defined functions | `Module::print` |
| `Function` | `function` | raw function name | `computeFunctionMetrics` | itself | `Function::print` |
| `Loop` | `loop` | `function:loop@header` | `computeLoopMetrics` | whole containing function | `Loop::print` |
| `LazyCallGraph::SCC` | `scc` | comma-separated function names | sum of per-function metrics | every function in the SCC | concatenated function IR |
| anything else | `other` | `<?>` | empty | none | none |

Important details:

- Module snapshots skip declarations in the module total but include only
  definitions in the by-function map.
- Function snapshots set `functions = 1`; declarations return that count with
  no instructions.
- Loop snapshots use loop-region metrics for the event delta, but attribute the
  whole containing function so function timelines remain comparable with
  function and module events.
- SCC snapshots sum per-function metrics, take the maximum loop depth, and
  attribute each function separately.
- IR text is only captured when `ReportOptions::snapshotsEnabled()` is true:
  `-lpta-diff` or `-lpta-html`.

### 6.5 Before a pass

`beforePass(PassID, IR)` does the following under normal conditions:

1. Loads `getOptions()`.
2. Computes `NeedAnalysis`:
   `reportEnabled || json || html || compare`.
3. Creates a `Frame` with:
   - pass ID;
   - `infra = isInfrastructure(PassID)`;
   - start timestamp;
   - `depth = Frames.size()`;
   - `parent = Frames.back().pass` if a frame exists.
4. If analysis is needed, the pass is not infrastructure, the pass filter
   matches, and `unitMayMatch` accepts the IR:
   - creates a before snapshot, including IR if snapshots are enabled;
   - marks `recording` if the snapshot is valid;
   - adds snapshot time to `OverheadUs`;
   - if a function filter exists, verifies that at least one affected function
     matches, otherwise disables recording;
   - captures the trace header from the module if this is the first recorded
     event.
5. Pushes the frame.

`isInfrastructure()` filters these pass IDs:

```text
contains "PassManager<"
ends with "PassAdaptor"
starts with "InvalidateAnalysisPass<"
starts with "RequireAnalysisPass<"
ends with "RepeatedPass"
ends with "WrapperPass"
```

These passes do not directly transform IR (or are analysis bookkeeping/wrappers),
so their child invocations are recorded instead. They still remain as frames for
nesting and timing.

`unitMayMatch()` avoids expensive snapshots for a function filter:

- module: true if any defined function matches;
- function: match the function name/display name;
- loop: match the containing function;
- SCC: true if any function in the SCC matches;
- other: true.

### 6.6 After a pass

`afterPass(PassID, IR, PA)`:

1. Searches `Frames` backwards for the innermost frame with this pass ID.
2. Removes that frame and any stale frames above it.
3. Computes total elapsed time and self time:
   `SelfUs = max(0, elapsed_ns - child_ns) / 1000`.
4. If the frame is infrastructure:
   - propagates the full elapsed child time to its parent;
   - returns without creating an event.
5. If the frame was recording:
   - creates an after snapshot (with IR only if snapshots are enabled);
   - builds an `Event`;
   - uses the after unit identity when valid, otherwise falls back to the
     before identity;
   - unions before/after affected function names;
   - builds per-function records using the union of function names;
   - sets `changed = !PA.areAllPreserved()`;
   - sets `score = significance(...)` only when `changed`, otherwise zero;
   - if snapshots are enabled and the event changed, computes a unified diff
     from before/after IR;
   - applies `E.score >= minScore` and `eventMatches(...)`;
   - finalizes the event under the mutex.
6. Propagates the full elapsed child time to the parent frame.

The changed flag follows the PRD: it is derived from `PreservedAnalyses`.
Consequently, a pass that abandons an analysis without changing IR can appear as
changed. The known example in the current corpus is
`CorrelatedValuePropagationPass`, which discards `LazyValueAnalysis`.

### 6.7 Skipped passes

`beforeSkippedPass` delegates to `recordSkipped`.

If `-lpta-skipped` is enabled, the pass is not infrastructure, and the pass
filter matches:

- a metrics-only snapshot is made (no IR);
- an event with `skipped = true`, `changed = false`, duration 0, and score 0 is
  built;
- the header is captured if needed;
- the event is finalized (and may be written to JSON).

Skipped events are excluded from aggregate pass/function statistics by the
aggregator. They can still appear in the `full` terminal view when retained.

### 6.8 Invalidated passes

`afterPassInvalidated` handles the callback used when a pass is invalidated and
no after-pass callback will run.

It:

1. Pops the matching frame.
2. Computes self time.
3. For infrastructure frames, propagates child time and returns.
4. For recording frames, builds an event with:
   - `before` metrics copied to `after`;
   - zero delta;
   - `invalidated = true`;
   - `changed` from `PreservedAnalyses`;
   - no after IR snapshot.
5. Finalizes the event and propagates child time.

### 6.9 Event finalization

`finalizeEvent(Event E)` is the boundary between collection and reporting:

1. Assigns `E.index = NextIndex.fetch_add(1)`. Indices are dense for events
   that reach finalization.
2. If JSON output is enabled and the writer has not failed:
   - opens the trace lazily on the first event (writes the header);
   - writes any diff file first and stores its relative `diff_ref`;
   - writes the event object;
   - marks the writer failed on error.
3. Creates the `Aggregator` lazily and calls `Agg->add(E)`.

JSON writing happens before aggregation so a JSON-only run with
`-lpta-report=off` never retains a full event vector in memory.

### 6.10 Header capture

The header is captured once, under the mutex, from the first recording event's
module:

- schema version;
- LLVM version;
- LPTA version;
- route;
- pipeline;
- source filename;
- target triple;
- producer from `llvm.ident` (first operand's `MDString`).

If no event is ever recorded, `captureDefaultHeader()` fills the version/route/
pipeline fields at exit. Source fallback scans process arguments for known
source/IR extensions.

For multi-module/LTO runs, the first captured module supplies the header; events
from all modules still share the single process-wide trace and report. Per-module
headers are not part of schema 1.0.

### 6.11 Exit-time emission

`emit()` is idempotent via `Emitted.exchange(true)`. Under the mutex it:

1. Ensures a default header exists.
2. Finishes the JSON writer:
   - opens the trace if no event was ever written;
   - aborts a failed trace;
   - otherwise finishes and records whether the trace was written.
3. Creates an empty aggregator if needed.
4. Sets the measured collection overhead.
5. If `-lpta-compare` is set:
   - reads the baseline trace;
   - builds a current `Trace` from `Agg->retainedEvents()`;
   - runs `compareTraces`.
6. Renders and writes HTML if requested.
7. Renders the terminal report if the view is not `off`:
   - writes to `-lpta-out` atomically, or
   - writes the complete string to stderr and flushes.
8. If the view is `off` but comparison text exists, prints the comparison.
9. Prints accumulated IO errors.

The report is built as one string before writing, so a normal terminal report is
not assembled from many unrelated `errs()` calls.

---

## 7. Metrics and scoring (`src/Metrics.cpp`)

### 7.1 Counters

For a function, LPTA iterates every basic block and instruction and counts:

| Metric | Rule |
| --- | --- |
| `insts` | every instruction, including terminators |
| `bbs` | every basic block |
| `functions` | 1 for a function snapshot; definitions only in module totals |
| `loads` | `LoadInst` |
| `stores` | `StoreInst` |
| `calls` | every `CallBase` |
| `branches` | conditional/unconditional branches, switch, indirectbr, callbr |
| `phis` | `PHINode` |
| `allocas` | `AllocaInst` |
| `loops` | `LoopInfo::getLoopsInPreorder().size()` |
| `max_loop_depth` | maximum `Loop::getLoopDepth()` |
| `call_edges` | unique direct callees per function |

Branch detection is version-guarded:

- LLVM 23+: `Instruction::UncondBr` and `Instruction::CondBr`;
- older LLVM: `BranchInst`, plus switch/indirectbr/callbr.

Indirect calls increment `calls` but do not add a direct callee edge.
`call_edges` for a module is the sum of each function's unique direct-callee
set, i.e. one edge per caller/callee pair.

### 7.2 Unit aggregation

- **Function:** `computeFunctionMetrics(F)` counts one function.
- **Module:** iterates definitions, sums counters, takes the maximum loop depth,
  and optionally fills a `std::map<std::string, Metrics>` by function.
- **Loop:** collects the loop's basic blocks plus all nested subloop blocks into
  a set, counts them once, then recursively counts loops and maximum depth.
- **SCC:** the collector sums the per-function metrics and records each
  function's metrics separately.

Loop analysis constructs a `DominatorTree` and `LoopInfo` per function snapshot.
This is the main reason LPTA's collection cost is higher than a pure instruction
count.

### 7.3 Delta and score

`delta(before, after)` subtracts every 64-bit metric.

`significance(before, delta, weights)` computes:

```text
sum(weight[metric] * abs(delta[metric]))
----------------------------------------
        max(1, before.insts)
```

Only changed events receive a score. The score ranks structural change
magnitude; it is not a runtime-speedup prediction.

Default weights are:

| Weight | Value |
| --- | ---: |
| `insts` | 1.0 |
| `bbs` | 0.5 |
| `functions` | 0.25 |
| `loads` | 1.0 |
| `stores` | 1.0 |
| `calls` | 3.0 |
| `branches` | 0.5 |
| `phis` | 0.5 |
| `allocas` | 2.0 |
| `loops` | 5.0 |
| `max_loop_depth` | 5.0 |
| `call_edges` | 2.0 |

### 7.4 Weight files

`loadWeightsFromFile`:

- reads the whole file through `MemoryBuffer`;
- splits lines and removes `#` comments;
- parses `key=value` with `strtod`;
- rejects empty/non-finite/negative values;
- ignores unknown keys with a warning;
- preserves defaults for missing keys.

---

## 8. Diff engine (`src/Diff.cpp`)

`unifiedDiff(before, after, label)`:

1. Returns empty immediately if the strings are identical.
2. Splits both inputs into lines. A trailing newline does not create an extra
   empty line.
3. Runs a bounded Myers diff with maximum edit distance 1200.
4. If the bound is exceeded, uses a prefix/suffix fallback:
   - keep the common prefix;
   - delete the changed middle of A;
   - insert the changed middle of B;
   - keep the common suffix.
5. Emits standard headers:
   `--- before/<label>` and `+++ after/<label>`.
6. Emits hunks with three lines of context.
7. Stops at 4 MiB of output and appends an explicit truncation marker.

The diff is computed only when:
- snapshots are enabled (`-lpta-diff` or `-lpta-html`);
- the event is marked changed;
- both before and after IR strings are non-empty.

---

## 9. JSON trace (`src/Trace.cpp`)

### 9.1 Writer lifecycle

`JsonTraceWriter` streams one event at a time:

```text
open(header, path)
  -> ensure parent directory exists
  -> temp path = path + ".tmp." + pid
  -> write top-level object/header
  -> begin events array
write(event)
  -> write one event object
finish()
  -> end array/object
  -> close stream
  -> check stream error
  -> rename temp to final path
abort()
  -> close stream
  -> remove temp
```

This design keeps memory bounded and prevents a partially written trace from
being mistaken for a complete one. The destructor closes an unclosed stream but
does not rename it.

### 9.2 Schema 1.0

Top-level fields:

```json
{
  "schema_version": "1.0",
  "llvm_version": "...",
  "producer": "...",
  "route": "in-compiler",
  "pipeline": "default<O2>",
  "source": "...",
  "target_triple": "...",
  "lpta_version": "0.1.0",
  "events": [ ... ]
}
```

Optional header fields are omitted when empty rather than emitted as null.

Each event contains:

- required: `index`, `pass`, `unit_kind`, `unit_name`, `changed`,
  `duration_us`, `before`, `after`, `delta`, `score`;
- optional: `unit_display_name`, `parent_pass`, `depth`, `affected_functions`,
  `skipped`, `invalidated`, `functions`, `diff_ref`.

`index` is dense for finalized events. The writer does not reorder events; they
appear in finalization/completion order. Nested leaf passes finalize as they
complete; infrastructure wrapper events are not written.

For non-function units, `functions` is an array of:

```json
{
  "name": "foo",
  "before_insts": 20,
  "after_insts": 18,
  "delta": { "...": 0 }
}
```

Function events omit the redundant array because the reader reconstructs an
implicit function record from the event's before/after metrics.

### 9.3 Diff snapshots

When an event has a diff and JSON is enabled:

- `writeSnapshotDiff` creates `<trace-stem>.snapshots/`;
- writes `<index>.diff` atomically;
- stores a relative path such as `trace.snapshots/42.diff` in `diff_ref`.

Only unified diffs are persisted, not full before/after IR text.

### 9.4 Reader

`readTrace(path, trace, err)`:

1. Reads the file and parses JSON.
2. Requires an object root and `schema_version`.
3. Parses the major version; rejects unknown majors with an explicit error.
4. Reads optional header fields.
5. Requires an `events` array.
6. Reconstructs each event, including metrics/deltas, optional fields, function
   records, and implicit function records.
7. Resolves each `diff_ref` relative to the trace file and loads its diff text.
8. Does not require event indices to be contiguous; it trusts the stored
   values.

The offline viewer uses this reader; the live plugin only writes traces.

---

## 10. Reporting model (`src/Report.h`, `src/Report.cpp`)

### 10.1 Shared event and header model

`Event` is the central handoff from collection to reporting:

```cpp
struct Event {
  uint64_t index;
  std::string pass;
  std::string unitKind;
  std::string unitName;
  std::string unitDisplayName;
  std::vector<std::string> affectedFunctions;
  std::map<std::string, FunctionRecord> functions;
  std::string parentPass;
  unsigned depth;
  bool changed;
  bool skipped;
  bool invalidated;
  uint64_t durationUs;
  Metrics before;
  Metrics after;
  MetricsDelta delta;
  double score;
  std::string diff;
  std::string diffRef;
};
```

`TraceHeader` carries source/producer/route/pipeline/version metadata.
`Trace` is a header plus a vector of events.

`FunctionRecord` stores before/after metrics and an optional stored delta. The
stored delta is used for trace-loaded module/SCC/loop functions; live data
usually computes it from before/after.

### 10.2 Filters

`NameFilter` wraps an LLVM regex; if compilation fails, it falls back to a
substring match and warns.

`eventMatches(E, opts)` checks:

1. pass filter;
2. minimum score;
3. function filter against affected functions or unit name/display name.

The collector applies an early version of the same checks to avoid snapshots and
JSON writes for excluded events; the aggregator re-applies them before adding
statistics or retaining events.

### 10.3 Aggregator

`Aggregator::add(E)` is the reporting core.

If the event does not match the filters, it is dropped.

Skipped events:

- are retained if the current view needs event history;
- are not added to pass/function statistics;
- return immediately otherwise.

For normal events:

1. Build `MatchedFunctions`:
   - no function filter: all per-event function records;
   - filter: only matching function names (raw or demangled);
   - fallback to `affectedFunctions` if there is no function map.
2. Compute `Effective` delta:
   - normally the event delta;
   - for a function-filtered non-function unit, sum the per-function deltas of
     matched functions.
3. Update `PassStats`:
   - `runs`;
   - `changed`;
   - summed delta;
   - summed duration;
   - summed score.
4. Update global stats:
   - `passesRun`;
   - `changedPasses`;
   - `totalTimeUs`;
   - function-name set.
5. Update per-function stats for each matched function:
   - first/last instruction counts;
   - absolute instruction movement;
   - weighted total change;
   - total score;
   - per-pass contribution map;
   - changed-pass count.
6. Retain the full event only if `ReportOptions::retainEvents()` is true.

`ReportOptions::retainEvents()` is true when:

- view is `functions` or `full`;
- HTML output is requested;
- comparison is requested;
- `-lpta-diff` is requested and the terminal report is enabled.

A default summary therefore keeps only aggregates, not every event. A JSON-only
run can stream events without retaining them.

`filteredScore()` recomputes a module/loop/SCC event's contribution when a
function filter selects a subset of functions. It uses the selected functions'
before instruction count and effective delta. Function events keep their stored
score.

### 10.4 Ranking

`rankedPasses` sorts by:

1. score descending;
2. absolute instruction delta descending;
3. pass name ascending.

`rankedFunctions` sorts by:

1. total weighted change descending;
2. total absolute instruction movement descending;
3. function name ascending.

`changedBy()` sorts a function's per-pass contributions and renders up to three
pretty pass names (`FooPass` becomes `Foo`).

### 10.5 Terminal report

`renderTerminal(header, aggregator, opts, compareText)` builds one string.

Header:

```text
LPTA  <source>
producer=<producer>  LLVM <version>
route=<route>  pipeline=<pipeline>
passes N | changed N | functions N | overhead N
```

Views:

- `summary`: top passes by significance, top functions by total change;
- `passes`: aggregated pass table;
- `functions`: per-function timelines;
- `full`: every retained invocation, including skipped/invalidated markers;
- `off`: no terminal view (the caller decides whether to print comparison).

Ranked table columns:

```text
pass, runs, changed, d_insts, d_calls, ms, score
function, insts (start -> end), changed by
```

Function timeline rows show only events that are relevant to that function:
changed function events, module events with a nonzero per-function delta, and
loop/SCC events with a delta or a changed flag touching the function.

The `functions` view limits the number of functions via `-lpta-top`; it does not
row-cap each timeline. The `full` view ignores `-lpta-top` and prints every
retained invocation.

`-lpta-diff` appends an `IR DIFFS` section after the selected view, header, or
comparison. Each diff begins with a truncated 80-column header and then the
unified diff body.

The footer always states that the score is a heuristic and points to deeper
views.

Formatting uses `truncate`, `padLeft`, and `padRight`; ranked terminal views are
designed to stay within 80 columns. Inline diff bodies may exceed 80 columns
because they mirror IR text.

Color:

- `always` = color;
- `never` = no color;
- `auto` = color only when stderr is a terminal, `-lpta-out` is not used, and
  `NO_COLOR` is unset.

### 10.6 HTML report

`renderHtml` produces a single self-contained document with inline CSS:

- header and stats;
- top passes table;
- top functions table;
- expandable function timelines;
- expandable IR diffs;
- schema/version/heuristic footer.

The same aggregator data is used. `-lpta-top` limits top passes/functions and
the number of function timelines. Timeline rows are not capped.

HTML is written through `writeFileAtomic`.

### 10.7 Cross-run comparison

`compareTraces(base, current, top)`:

1. Builds a map from comparison key to baseline events.
2. The key is `functionKey(event) + "\\x1f" + pass` (the ASCII unit separator).
   - One affected function: use it.
   - Multiple affected functions: use the event's unit name.
   - No affected functions: use the unit name.
3. Walks current events in order and tracks occurrence index per key.
4. Reports the first divergence when:
   - the key/occurrence is missing from the baseline;
   - before/after metrics differ;
   - the changed flag differs;
   - the unit kind differs.
5. If no current event diverges, checks for baseline events not present in the
   current trace.
6. Aggregates both traces with a filter-free pass view and reports per-pass
   differences in runs, instruction delta, and call delta.
7. Ignores timing and score for divergence detection.
8. Clips every output line to 80 columns.

The same function powers `-lpta-compare` and
`lpta-report diff <base> <current>`.

### 10.8 Atomic file output

`writeFileAtomic(path, contents, err)`:

1. Creates parent directories if needed.
2. Opens `path.tmp.<pid>` with `CD_CreateAlways`.
3. Writes the complete string.
4. Closes and checks the stream error state.
5. Renames the temp file onto the target.
6. Removes the temp file on any write/rename failure.

`-lpta-out`, HTML, JSON, and per-event diff files all use this pattern (JSON has
its own equivalent streaming writer lifecycle).

---

## 11. Offline viewer (`tools/lpta-report.cpp`)

`lpta-report` is deliberately independent of the live plugin's option parser.

It:

1. Parses `--view`, `--top`, `--function`, `--pass`, `--min-score`, `--diff`,
   `--color`, `--demangle`, `--output`/`-o`, and help.
2. Rejects unsupported views/colors/demangle modes.
3. Resolves `auto` color based on stdout, `NO_COLOR`, and whether output is a
   file.
4. Compiles filters.
5. Supports two modes:
   - **single trace:** load with `readTrace`, build an `Aggregator`, call
     `renderTerminal`, write stdout or the requested file;
   - **compare:** require `diff <base> <current>`, load both traces, call
     `compareTraces`, write stdout or the requested file.

The viewer links only `lpta_analysis`; it does not register LLVM `cl::opt`
objects. This avoids duplicate-option collisions such as a viewer-owned
`color` option conflicting with an LLVM option of the same name.

The viewer uses event scores stored in the trace. If a function filter causes a
module/loop/SCC pass score to be recomputed, it uses the viewer's default
`ScoreWeights`, because weights are not serialized in schema 1.0.

---

## 12. Wrapper (`scripts/lpta`)

The wrapper is a Bash script that makes the plugin easier to use without
manually adding `-fpass-plugin`/`-load-pass-plugin` and `LPTA_OPTIONS`.

Flow:

1. Parse wrapper options beginning with `--`.
2. Map `--report=...` to `-lpta-report=...`, `--diff` to `-lpta-diff`, etc.
3. Stop at `--` or the first non-option command.
4. Find the plugin from:
   - `LPTA_PLUGIN`;
   - `../build/LPTA.so`;
   - `../lib/LPTA.so`;
   - the script directory;
   - the current directory.
5. Quote the collected options into `LPTA_OPTIONS` using `printf '%q'`.
6. Select the loading mechanism from the command basename:
   - `opt`/`opt-*` → `-load-pass-plugin=<plugin>`;
   - `rustc` → `-Z llvm-plugins=<plugin>`;
   - anything else → `-fpass-plugin=<plugin>`.
7. `exec` the compiler so the wrapper does not add an extra process layer.

The wrapper passes the original compiler arguments unchanged after adding the
plugin flag.

---

## 13. Tests and build integration

### 13.1 CMake

- Requires LLVM 22+.
- Uses the imported `LLVM` target when available; otherwise maps components
  (`Core`, `Support`, `IRReader`, `Analysis`, `Passes`, `TransformUtils`,
  `Demangle`, `AsmParser`).
- Builds `lpta_analysis` with PIC so it can be linked into the loadable module.
- Builds `LPTA` with no prefix (`PREFIX ""`) and output name `LPTA`.
- Builds `lpta-report` and the unit test.
- Installs `LPTA.so`, `lpta-report`, and the wrapper.
- Registers two CTest tests:
  - `lpta-metrics` → `build/lpta-metrics-test`;
  - `lpta-integration` → `tests/lpta/run.sh <plugin> <viewer> <tests-dir>`.

### 13.2 Unit test

`tests/unit/metrics_test.cpp` parses a small IR module and checks:

- instruction/block/load/store/call/branch/phi/alloca counts;
- loop count and maximum nesting depth;
- call-edge count;
- module aggregate counts;
- significance formula;
- unified-diff header, deletion, insertion, and empty result for identical
  input.

### 13.3 Integration suite

`tests/lpta/run.sh` creates a temporary workspace and checks:

- clang plugin loading and report placement on stderr;
- source/header/heuristic/footer text;
- 80-column summary;
- byte-identical object output with and without the plugin;
- clean IR on stdout with reports on stderr;
- JSON schema, index start, nesting, function attribution, loop/SCC units,
  infrastructure filtering, diff refs, snapshots, and clean JSON-only stderr;
- rejection of an unknown schema major;
- offline viewer and `--top`;
- function filters;
- self-contained HTML and expandable diffs;
- `passes`, `functions`, and `full` views plus their width checks;
- pass filter, minimum-score filter, `-lpta-out`, and `LPTA_OPTIONS`;
- `opt` route and hand-written IR;
- inline diffs;
- cross-run comparison;
- C++ loading and object identity;
- Rust IR replay through `opt` when `rustc` is available;
- the wrapper.

`tests/lpta/wrapper-test.sh` separately verifies that the wrapper creates a
valid JSON trace with schema 1.0.

---

## 14. Interaction walkthroughs

### 14.1 Normal clang run

```text
clang -O2 -fpass-plugin=./build/LPTA.so -c foo.c
```

1. Clang loads `LPTA.so`; `Plugin.cpp` checks the LLVM major and returns the
   registration callback.
2. `Collector` registers instrumentation and `atexit`.
3. For each module/function/loop/SCC pass:
   - `beforePass` pushes a frame and optionally snapshots metrics;
   - the optimizer runs the pass;
   - `afterPass` pops the frame, snapshots after metrics, builds an event,
     scores it, and adds it to the aggregator.
4. At process exit, `emit()` renders the summary to stderr.
5. No JSON/HTML is written unless requested.

### 14.2 JSON + diffs only

```text
clang -O2 -fpass-plugin=./build/LPTA.so \
  -mllvm -lpta-report=off \
  -mllvm -lpta-json=/tmp/trace.json \
  -mllvm -lpta-diff \
  -c foo.c
```

- `NeedAnalysis` is true because JSON is requested.
- `snapshotsEnabled()` is true because `-lpta-diff` is set.
- `beforePass`/`afterPass` capture IR and compute diffs.
- Each finalized event is streamed to `JsonTraceWriter`.
- Diff files are written under `/tmp/trace.snapshots/`.
- `eventMatches`/aggregation still run, but no terminal report is rendered.
- JSON is finalized atomically at exit; no event vector is retained just for
  reporting.

### 14.3 HTML report

```text
clang -O2 -fpass-plugin=./build/LPTA.so \
  -mllvm -lpta-report=off \
  -mllvm -lpta-html=/tmp/report.html \
  -c foo.c
```

- HTML implies snapshots, so IR is captured and diffs are built.
- `retainEvents()` is true because HTML needs timelines/diffs.
- At exit, `renderHtml()` uses the aggregator's retained events.
- The file is written atomically; no terminal output is produced when the view
  is `off`.

### 14.4 Offline viewing

```text
./build/lpta-report --view=functions --function=sum_squares /tmp/trace.json
```

- The viewer reads the trace with `readTrace`.
- It rebuilds an aggregator from stored events.
- It renders the same `functions` view as the live reporter.
- Diff refs are resolved relative to the trace file.
- No plugin/LLVM pass manager is involved.

### 14.5 Cross-run comparison

```text
lpta-report diff /tmp/base.json /tmp/later.json
```

- Both traces are read and validated.
- Events are aligned by function/unit, pass, and occurrence.
- The first metric divergence is printed.
- Per-pass run/instruction/call differences are listed.
- Timing and score are ignored.

---

## 15. Extension points

### Add a metric

1. Add the field to `Metrics` and `MetricsDelta` in `Metrics.h`.
2. Compute it in `Metrics.cpp` for the relevant unit(s).
3. Include it in `delta()`, weight defaults, and `significance()`.
4. Add weight parsing support.
5. Add JSON read/write fields in `Trace.cpp`.
6. Add terminal/HTML columns or aggregation logic in `Report.cpp` if needed.
7. Update unit/integration tests and the weights documentation.

### Add a report view

1. Add the name to option validation in `Options.cpp` and `lpta-report.cpp`.
2. Decide whether it needs retained events in
   `ReportOptions::retainEvents()`.
3. Add the renderer branch in `renderTerminal`.
4. Add an HTML section if appropriate.
5. Add viewer/CLI tests.

### Add an IR unit

1. Extend `makeSnapshot()` and `probeAny` handling in `Collector.cpp`.
2. Define unit naming and function attribution.
3. Extend JSON unit-kind handling if the viewer needs special treatment.
4. Extend timeline rules in `timelineRelevant`.
5. Add tests and document the new unit.

### Change the trace schema

1. Update `Version.h`.
2. Update `Trace.cpp` writer/reader and validation.
3. Update README schema examples.
4. Add a trace round-trip/rejection test.

---

## 16. Invariants, concurrency, and lifetime

- **No IR mutation:** the collector only reads LLVM IR.
- **Thread-local nesting:** each compiler thread has independent pass frames.
- **Global finalization:** event numbering, JSON writing, header capture, and
  aggregation are mutex-protected.
- **Atomic counters:** event index, overhead, and emitted state are atomic.
- **Idempotent exit:** `emit()` can be called more than once safely.
- **Leaked option/arg singletons:** intentional, to survive until `atexit`.
- **Trace atomicity:** temp file + rename; failed traces are removed.
- **Report atomicity:** full string + temp file + rename for file output.
- **Filtering at two levels:** collector avoids unnecessary work; aggregator
  enforces report-level filters.
- **Single header for multi-module runs:** the first captured module wins;
  events are still merged globally.

---

## 17. Known boundaries

- The plugin is built for LLVM 22+ source APIs, but this environment only has
  LLVM 23.1.3; LLVM 22 has not been runtime-tested here.
- `changed` follows `PreservedAnalyses`, so an analysis-abandoning pass such as
  `CorrelatedValuePropagationPass` can be reported as changed even when
  `-print-changed` sees no IR change.
- Only unified diffs are persisted; full before/after IR text is transient.
- Multi-module/LTO runs use the first module's header; per-module headers are
  not in schema 1.0.
- `rustc` support is best-effort via `-Z llvm-plugins` and depends on the
  toolchain's plugin ABI.
- P2/stretch items (standalone `.ll`/`.bc` driver, debug-info mapping, codegen
  correlation, compact CI summary JSON, and Markdown export) are not
  implemented.

---

## 18. File map

```text
CMakeLists.txt              build targets, install, CTest registration
README.md                   user-facing usage and options
CHANGES.md                  exact implementation change log
ARCHITECTURE.md             this document
src/Version.h               version/schema macros
src/Plugin.cpp              plugin entry and LLVM version guard
src/Collector.h             collector public API
src/Collector.cpp           callbacks, frames, snapshots, events, emit
src/Options.h               options/process API
src/Options.cpp             flags, environment, route/pipeline, weights
src/Metrics.h               metrics/score data model
src/Metrics.cpp             metric counting, aggregation, score
src/Diff.h                  diff API
src/Diff.cpp                bounded Myers diff and fallback
src/Trace.h                 JSON writer/reader API
src/Trace.cpp               schema 1.0, atomic trace, diff refs
src/Report.h                event/trace/stat/report model
src/Report.cpp              aggregation, terminal/HTML, compare, atomic writes
tools/lpta-report.cpp       offline viewer/comparator
scripts/lpta                wrapper
tests/lpta/sample.c         C integration input
tests/lpta/handwritten.ll   hand-written opt input
tests/lpta/run.sh           integration suite
tests/lpta/wrapper-test.sh  wrapper check
tests/unit/metrics_test.cpp unit checks
```

---

## 19. Quick mental model

If you only remember one thing:

```text
PassInstrumentationCallbacks
    -> before snapshot
    -> after snapshot
    -> Event
    -> [JSON stream] + [Aggregator]
    -> atexit
    -> terminal / HTML / comparison
```

- `Collector.cpp` decides **what happened**.
- `Metrics.cpp` decides **how much structurally changed**.
- `Diff.cpp` decides **what text changed** when requested.
- `Trace.cpp` decides **how it is persisted and read back**.
- `Report.cpp` decides **how it is aggregated and presented**.
- `Options.cpp` decides **which mode and filters are active**.
- `Plugin.cpp` is the **load-time safety gate**.
- `lpta-report` is the **same reporting stack without the live plugin**.
- `scripts/lpta` is the **convenience path into all of it**.
