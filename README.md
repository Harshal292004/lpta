# LPTA - LLVM Pass Transformation Analyzer

LPTA is a loadable LLVM pass plugin that observes an optimization pipeline and
reports **which pass changed what, for which IR unit, and how much**. It uses
the new pass manager's `PassInstrumentationCallbacks`; it does not add,
remove, or reorder optimization passes, and it does not patch LLVM.

The default terminal report goes to **stderr**, so compiler output such as
`clang -S -emit-llvm -o -` stays clean. JSON traces and self-contained HTML
reports are opt-in.

```
LPTA  tests/lpta/sample.c
producer=clang version 23.1.3  LLVM 23.1.3
route=in-compiler  pipeline=default<O2>
passes 262 | changed 48 | functions 3 | overhead 155us

TOP PASSES BY SIGNIFICANCE
 #  pass                      runs  changed  d_insts  d_calls      ms  score
  1  SROAPass                    11        3      -30       -4     0.1   4.53
  2  LoopVectorizePass            2        1      +25       +1     0.5   2.56

TOP FUNCTIONS BY TOTAL CHANGE
 #  function                      insts (start -> end)    changed by
  1  sum_squares                   34 -> 39 (+15%)         SROA, LoopVectoriz...
  2  dead_branch                   14 -> 4 (-71%)          SROA, SimplifyCFG

score is a heuristic for ranking change magnitude, not a predicted speedup.
More: -lpta-report=functions | -lpta-function=<name> | -lpta-html=report.html
```

## Version note

The project is designed for the LLVM 22.x pass-manager API. The checked-out
environment has **LLVM 23.1.3**, so this tree is built and tested against that
toolchain. The few API differences used here (the plugin header location and
the LLVM 23 branch opcodes) are guarded; no LLVM sources are modified. LLVM 22
compatibility is therefore source-level only and has not been runtime-tested
here. The plugin checks the runtime LLVM major version at load time and refuses
a mismatched driver instead of entering an incompatible ABI; it must still be
built against the exact LLVM installation used by `clang`/`opt`.

## Documentation

- [CHANGES.md](CHANGES.md) — exact file-by-file implementation change log,
  hardening fixes, validation performed, and intentionally unchanged scope.
- [ARCHITECTURE.md](ARCHITECTURE.md) — detailed runtime flow, callback
  lifecycle, data model, per-module responsibilities, and how the modules
  interact.

The rest of this README is the user-facing guide.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces:

- `build/LPTA.so` - the pass plugin
- `build/lpta-report` - the offline trace viewer and comparator
- `build/lpta-metrics-test` - unit tests, when `LPTA_BUILD_TESTS=ON`

`LPTA_BUILD_TESTS` defaults to `ON`. Run the test suite with:

```bash
ctest --test-dir build --output-on-failure
```

Install the plugin, viewer, and wrapper with:

```bash
cmake --install build --prefix "$HOME/.local"
```

## Quick start

### clang / clang++

LLVM options are passed through clang with `-mllvm`:

```bash
clang -O2 -fpass-plugin=./build/LPTA.so \
  -c tests/lpta/sample.c -o /tmp/sample.o
```

Write a trace and HTML report:

```bash
clang -O2 -fpass-plugin=./build/LPTA.so \
  -mllvm -lpta-report=off \
  -mllvm -lpta-json=/tmp/trace.json \
  -mllvm -lpta-html=/tmp/report.html \
  -mllvm -lpta-diff \
  -c tests/lpta/sample.c -o /tmp/sample.o
```

`-lpta-diff` also prints unified IR diffs in the terminal. When JSON is
enabled, unified diff snapshots are written beside the trace in
`<trace-stem>.snapshots/` and referenced by `diff_ref` in the trace. The
persisted files are diffs, not full before/after IR dumps; the IR text is kept
only transiently to compute them.

### opt

```bash
opt -load-pass-plugin=./build/LPTA.so \
  -passes='default<O2>' tests/lpta/handwritten.ll -disable-output
```

LPTA records `route=in-compiler` for clang and `route=opt-replay` for `opt`.

### Wrapper

`scripts/lpta` finds the plugin and sets `LPTA_OPTIONS` automatically:

```bash
./scripts/lpta --json=/tmp/trace.json -- \
  clang -O2 -c tests/lpta/sample.c -o /tmp/sample.o

./scripts/lpta --report=functions --function=sum_squares -- \
  clang -O2 -c tests/lpta/sample.c -o /tmp/sample.o

LPTA_PLUGIN=./build/LPTA.so ./scripts/lpta --report=passes --top=5 -- \
  opt -passes='default<O2>' tests/lpta/handwritten.ll -disable-output
```

It also recognizes `opt` and `rustc` command names. Set `LPTA_PLUGIN` when the
plugin is not in `../build`, `../lib`, or the current directory.

## Options

All options are LLVM command-line options named `-lpta-*`. They can also be
supplied through the `LPTA_OPTIONS` environment variable, using the same syntax
as the command line; command-line flags win over the environment.

| Option | Default | Meaning |
| --- | --- | --- |
| `-lpta-report=VIEW` | `summary` | `summary`, `passes`, `functions`, `full`, or `off` |
| `-lpta-top=N` | `10` | Rows in ranked tables; `0` means all |
| `-lpta-function=REGEX` | empty | Limit views/events to matching function names |
| `-lpta-pass=REGEX` | empty | Limit views/events to matching pass names |
| `-lpta-min-score=X` | `0` | Hide invocations below this score |
| `-lpta-diff[=BOOL]` | `false` | Capture and print IR diffs for changed passes |
| `-lpta-color=MODE` | `auto` | `auto`, `always`, or `never`; `NO_COLOR` is honored |
| `-lpta-out=FILE` | empty | Write the terminal report to a file instead of stderr |
| `-lpta-html=FILE` | empty | Write a self-contained HTML report |
| `-lpta-json=FILE` | empty | Write a versioned JSON trace |
| `-lpta-compare=FILE` | empty | Compare this run with a saved JSON trace |
| `-lpta-weights=FILE` | built-in | Load score weights (`name=value` per line) |
| `-lpta-demangle=auto\|off` | `auto` | Demangle function names in views |
| `-lpta-pipeline=LABEL` | detected | Override the pipeline label in reports |
| `-lpta-skipped` | off | Record passes skipped by optional-pass callbacks |

Examples:

```bash
LPTA_OPTIONS="-lpta-report=passes -lpta-top=5" \
  clang -O2 -fpass-plugin=./build/LPTA.so -c foo.c -o foo.o
```

`-lpta-out` writes the report atomically and suppresses stderr output. JSON
and HTML files are also written through a temporary file and renamed into
place on completion.

### Weights

The score is:

```
sum(weight[metric] * abs(after[metric] - before[metric]))
---------------------------------------------------------
                max(1, before.insts)
```

It is a heuristic for ranking change magnitude, not a predicted speedup.
Weights can be overridden:

```text
# custom.weights
insts=1.0
bbs=0.5
calls=3.0
loops=5.0
max_loop_depth=5.0
```

Recognized keys are `insts`, `bbs`, `functions`, `loads`, `stores`, `calls`,
`branches`, `phis`, `allocas`, `loops`, `max_loop_depth`, and `call_edges`.
Unknown keys produce a warning and are ignored. The `LPTA_OPTIONS` value is
split with shell-like quoting, so paths containing spaces can be quoted.

## Views

- `summary` (default): top passes by significance and top functions by total
  change, with the pass names that contributed most.
- `passes`: ranked pass invocations, aggregated over unit kinds.
- `functions`: per-function timelines showing the pass, unit kind, instruction
  and call deltas, duration, and score.
- `full`: every retained invocation, including skipped/invalidated markers.
- `off`: no terminal report; useful with `-lpta-json` or `-lpta-html`.

Ranked terminal views are limited to 80 columns. Long names are truncated
with `...`; the full values remain available in JSON and HTML. Inline unified
IR diffs may exceed 80 columns because their lines mirror the IR text.

The standalone viewer accepts the same views and filters:

```bash
./build/lpta-report --view=summary --top=5 /tmp/trace.json
./build/lpta-report --view=functions --function=sum_squares --diff /tmp/trace.json
./build/lpta-report --output=report.txt /tmp/trace.json
```

## Cross-run comparison

```bash
# Baseline and later runs
clang -O2 -fpass-plugin=./build/LPTA.so -mllvm -lpta-report=off \
  -mllvm -lpta-json=/tmp/base.json -c foo.c -o /tmp/a.o
clang -O3 -fpass-plugin=./build/LPTA.so -mllvm -lpta-report=off \
  -mllvm -lpta-json=/tmp/later.json -c foo.c -o /tmp/b.o

./build/lpta-report diff /tmp/base.json /tmp/later.json
```

Comparison aligns events by `(function, pass, occurrence)`, reports the first
metric divergence, then lists per-pass differences. Timing and scores are
ignored for divergence detection. The same comparison can be emitted during a
live run with `-lpta-compare=/tmp/base.json`.

## JSON trace schema 1.0

The trace is streamed to disk. Top-level fields include:

```json
{
  "schema_version": "1.0",
  "llvm_version": "23.1.3",
  "producer": "clang version 23.1.3",
  "route": "in-compiler",
  "pipeline": "default<O2>",
  "source": "tests/lpta/sample.c",
  "target_triple": "x86_64-...",
  "lpta_version": "0.1.0",
  "events": [
    {
      "index": 0,
      "pass": "MemProfRemoveInfo",
      "unit_kind": "module",
      "unit_name": "tests/lpta/sample.c",
      "changed": false,
      "duration_us": 10,
      "before": { "insts": 34, "...": 0 },
      "after":  { "insts": 34, "...": 0 },
      "delta":  { "insts": 0, "...": 0 },
      "score": 0.0,
      "functions": [
        { "name": "sum_squares", "before_insts": 34,
          "after_insts": 34, "delta": { "insts": 0 } }
      ]
    }
  ]
}
```

Metrics are `insts`, `bbs`, `functions`, `loads`, `stores`, `calls`,
`branches`, `phis`, `allocas`, `loops`, `max_loop_depth`, and `call_edges`.
`index` is a dense, deterministic invocation number starting at zero. Optional
fields include `unit_display_name`, `parent_pass`, `depth`,
`affected_functions`, `skipped`, `invalidated`, `diff_ref`, and the per-function
attribution array. Unknown major schema versions are rejected by the reader;
minor versions are intended to remain readable.

## Architecture

```
src/Plugin.cpp      pass-plugin entry point
src/Collector.cpp   instrumentation, IR snapshots, unit normalization
src/Metrics.cpp     structural metrics, weights, significance
src/Diff.cpp        bounded Myers unified diff with fallback
src/Trace.cpp       streaming JSON writer/reader, snapshot files
src/Report.cpp      aggregation, terminal/HTML views, comparison
src/Options.cpp     -lpta-* flags, LPTA_OPTIONS, route/pipeline detection
tools/lpta-report.cpp  offline viewer and cross-run comparator
scripts/lpta        command wrapper
tests/               end-to-end and unit tests
```

IR units seen by the collector are normalized to `module`, `function`, `loop`,
`scc`, or `other`. Loop and SCC events carry per-function attribution so
function timelines remain comparable across unit kinds. The plugin reads
LLVM IR only; it is language-agnostic and works with any frontend that runs the
new pass manager.

## Limitations

- The score measures structural change, not runtime performance. It is not a
  speedup prediction and ignores instruction cost/TTI and data layout.
- Pass timing is wall-clock time around the callback, with nested child time
  subtracted. It is useful for spotting expensive passes, not for precise
  benchmarking.
- HTML reports are self-contained and can be large when full IR diffs are
  captured. Terminal diffs may exceed 80 columns by nature; ranked terminal
  views are width-limited.
- The changed flag follows `PreservedAnalyses` as specified. A pass that
  abandons an analysis without changing IR can therefore be reported as changed
  even though LLVM's `-print-changed` omits it. On the sample corpus the only
  such difference is `CorrelatedValuePropagationPass` (it abandons
  `LazyValueAnalysis`); pass-manager, adaptor, and analysis-bookkeeping
  invocations are filtered out.
- The plugin checks the runtime LLVM major version at load time, but it must
  still be built against the same LLVM installation/ABI as `clang`/`opt`.
- Multi-module and LTO-parallel runs attribute events globally; the header
  records the first module's source and producer. Per-module headers are not
  part of schema 1.0.
- `rustc` support is best-effort via `-Z llvm-plugins`; the exact plugin ABI is
  controlled by the rustc toolchain.
- This v1 does not implement the PRD's P2/stretch items: a standalone
  `.ll`/`.bc` driver, source/debug-info mapping, codegen correlation, a compact
  CI-oriented summary JSON, or optional Markdown report export. The offline
  viewer and cross-run comparator are included.
