#!/usr/bin/env bash
# End-to-end checks for the LPTA pass plugin.  Invoked by CTest, but also
# runnable directly:
#   tests/lpta/run.sh build/LPTA.so build/lpta-report tests
set -euo pipefail

PLUGIN="${1:?usage: run.sh LPTA.so lpta-report tests-dir}"
VIEWER="${2:?usage: run.sh LPTA.so lpta-report tests-dir}"
TESTS="${3:?usage: run.sh LPTA.so lpta-report tests-dir}"

CLANG="${CLANG:-clang}"
OPT="${OPT:-opt}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lpta-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { echo "PASS: $1"; pass=$((pass + 1)); }
bad() { echo "FAIL: $1" >&2; fail=$((fail + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

SRC="$TESTS/lpta/sample.c"
IR="$TESTS/lpta/handwritten.ll"

# 1. In-compiler route through clang.  Report is on stderr, object is still
#    produced, and stdout remains clean when emitting IR.
"$CLANG" -O2 -fpass-plugin="$PLUGIN" -c "$SRC" -o "$TMP/with.so.o" \
  2>"$TMP/clang.err"
check "clang plugin loads and reports" "grep -q '^LPTA ' '$TMP/clang.err'"
check "summary has pass ranking" "grep -q 'TOP PASSES BY SIGNIFICANCE' '$TMP/clang.err'"
check "summary has function ranking" "grep -q 'TOP FUNCTIONS BY TOTAL CHANGE' '$TMP/clang.err'"
check "source name in header" "grep -q 'sample.c' '$TMP/clang.err'"
check "report says heuristic" "grep -q 'heuristic' '$TMP/clang.err'"
check "report suggests more detail" "grep -q 'More: -lpta-report=functions' '$TMP/clang.err'"
check "summary fits 80 columns" "awk 'length(\$0) > 80 { exit 1 }' '$TMP/clang.err'"

"$CLANG" -O2 -c "$SRC" -o "$TMP/without.o"
check "object is byte-identical with and without plugin" "cmp -s '$TMP/with.so.o' '$TMP/without.o'"

"$CLANG" -O2 -S -emit-llvm -fpass-plugin="$PLUGIN" "$SRC" -o - \
  >"$TMP/stdout.ll" 2>"$TMP/stdout.err"
check "IR on stdout is clean" "grep -q '^; ModuleID' '$TMP/stdout.ll'"
check "report still goes to stderr" "grep -q '^LPTA ' '$TMP/stdout.err'"

# 2. JSON trace is valid and the offline viewer replays it.  Put it in a
#    directory with a dot in its name: snapshot paths must not rely on the
#    parent directory ending in a separator.
TRACE_DIR="$TMP/trace.dir"
mkdir -p "$TRACE_DIR"
TRACE_JSON="$TRACE_DIR/trace.json"
"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=off \
  -mllvm -lpta-json="$TRACE_JSON" -mllvm -lpta-diff \
  -c "$SRC" -o "$TMP/trace.o" 2>"$TMP/json.err"
check "JSON trace exists" "test -s '$TRACE_JSON'"
check "JSON has schema 1.0" "grep -q '\"schema_version\": \"1.0\"' '$TRACE_JSON'"
check "JSON has events" "grep -q '\"events\"' '$TRACE_JSON'"
check "JSON starts indexing at zero" "grep -q '\"index\": 0' '$TRACE_JSON'"
check "JSON records nesting context" "grep -q '\"parent_pass\"' '$TRACE_JSON' && grep -q '\"depth\"' '$TRACE_JSON'"
check "JSON records function attribution" "grep -q '\"affected_functions\"' '$TRACE_JSON' && grep -q '\"functions\": \\[' '$TRACE_JSON'"
check "JSON includes loop and SCC units" "grep -q '\"unit_kind\": \"loop\"' '$TRACE_JSON' && grep -q '\"unit_kind\": \"scc\"' '$TRACE_JSON'"
check "JSON omits pass-manager bookkeeping" "! grep -Eq '\"pass\": \"(RequireAnalysisPass|InvalidateAnalysisPass|ModuleInlinerWrapperPass|DevirtSCCRepeatedPass)' '$TRACE_JSON'"
check "JSON references diff snapshots" "grep -q '\"diff_ref\": \"trace.snapshots/' '$TRACE_JSON'"
check "changes produce diff snapshots" "test -d '$TRACE_DIR/trace.snapshots' && ls '$TRACE_DIR/trace.snapshots'/*.diff >/dev/null 2>&1"
check "JSON-only run keeps stderr clean" "test ! -s '$TMP/json.err'"
check "reporting does not change codegen" "cmp -s '$TMP/trace.o' '$TMP/without.o'"

printf '{"schema_version":"2.0","events":[]}\n' >"$TMP/future.json"
if "$VIEWER" "$TMP/future.json" >"$TMP/future.out" 2>"$TMP/future.err"; then
  bad "offline viewer rejects unknown schema major"
else
  check "offline viewer rejects unknown schema major" \
    "grep -q 'unsupported trace schema major version' '$TMP/future.err'"
fi

"$VIEWER" --view=summary --top=5 "$TRACE_JSON" >"$TMP/viewer.txt"
check "offline viewer reads trace" "grep -q '^LPTA ' '$TMP/viewer.txt'"
check "offline viewer ranks passes" "grep -q 'TOP PASSES' '$TMP/viewer.txt'"
"$VIEWER" --view=passes --top=1 "$TRACE_JSON" >"$TMP/top1.txt"
check "offline viewer honors --top" \
  "test \"\$(grep -c '^ [ 0-9][0-9]  ' '$TMP/top1.txt')\" -eq 1"

"$VIEWER" --view=functions --function=sum_squares --top=1 "$TRACE_JSON" \
  >"$TMP/functions.txt"
check "function filter works" "grep -q 'sum_squares' '$TMP/functions.txt'"

# 3. HTML output is self-contained and includes timelines/diffs.
"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=off \
  -mllvm -lpta-html="$TMP/report.html" -c "$SRC" -o "$TMP/html.o"
check "HTML report exists" "test -s '$TMP/report.html'"
check "HTML is self-contained" "grep -q '<style>' '$TMP/report.html'"
check "HTML has timelines" "grep -q 'Function timelines' '$TMP/report.html'"
check "HTML has expandable diffs" "grep -q '<details>' '$TMP/report.html' && grep -q 'IR diffs' '$TMP/report.html'"

# 4. Terminal views and filters.
"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=passes \
  -mllvm -lpta-top=3 -c "$SRC" -o "$TMP/passes.o" 2>"$TMP/passes.txt"
check "passes view works" "grep -q 'PASSES BY SIGNIFICANCE' '$TMP/passes.txt'"
check "passes view fits 80 columns" "awk 'length(\$0) > 80 { exit 1 }' '$TMP/passes.txt'"

"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=functions \
  -mllvm -lpta-function=sum_squares -mllvm -lpta-top=1 \
  -c "$SRC" -o "$TMP/func.o" 2>"$TMP/func.txt"
check "live function filter works" "grep -q 'sum_squares' '$TMP/func.txt'"
check "functions view fits 80 columns" "awk 'length(\$0) > 80 { exit 1 }' '$TMP/func.txt'"

"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=full \
  -mllvm -lpta-top=0 -c "$SRC" -o "$TMP/full.o" 2>"$TMP/full.txt"
check "full view works" "grep -q 'ALL INVOCATIONS' '$TMP/full.txt'"
check "full view fits 80 columns" "awk 'length(\$0) > 80 { exit 1 }' '$TMP/full.txt'"

"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=passes \
  -mllvm -lpta-pass=SROAPass -mllvm -lpta-top=0 \
  -c "$SRC" -o "$TMP/passfilter.o" 2>"$TMP/passfilter.txt"
check "live pass filter works" "grep -q 'SROAPass' '$TMP/passfilter.txt'"
check "pass filter excludes others" "! grep -q 'InstCombinePass' '$TMP/passfilter.txt'"

"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-min-score=999 \
  -c "$SRC" -o "$TMP/minscore.o" 2>"$TMP/minscore.txt"
check "minimum score filter works" "grep -q 'no matching passes' '$TMP/minscore.txt'"

"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-out="$TMP/report.txt" \
  -c "$SRC" -o "$TMP/out.o" 2>"$TMP/out.err"
check "-lpta-out writes a report" "test -s '$TMP/report.txt'"
check "-lpta-out keeps stderr clean" "test ! -s '$TMP/out.err'"

LPTA_OPTIONS="-lpta-report=passes -lpta-top=1" \
  "$CLANG" -O2 -fpass-plugin="$PLUGIN" -c "$SRC" -o "$TMP/env.o" \
  2>"$TMP/env.txt"
check "LPTA_OPTIONS fallback works" "grep -q 'PASSES BY SIGNIFICANCE' '$TMP/env.txt'"

# 5. opt route and hand-written IR.
"$OPT" -load-pass-plugin="$PLUGIN" -passes='default<O2>' "$IR" -disable-output \
  2>"$TMP/opt.txt"
check "opt route is labelled" "grep -q 'route=opt-replay' '$TMP/opt.txt'"
check "opt reports handwritten IR" "grep -q 'dot' '$TMP/opt.txt'"

# 6. Diff output is inline on request.
"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-diff \
  -c "$SRC" -o "$TMP/diff.o" 2>"$TMP/diff.txt"
check "inline IR diffs appear" "grep -q 'IR DIFFS' '$TMP/diff.txt'"

# 7. Cross-run comparison.
"$CLANG" -O2 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=off \
  -mllvm -lpta-json="$TMP/base.json" -c "$SRC" -o "$TMP/base.o"
"$CLANG" -O3 -fpass-plugin="$PLUGIN" -mllvm -lpta-report=off \
  -mllvm -lpta-json="$TMP/later.json" -c "$SRC" -o "$TMP/later.o"
"$VIEWER" diff "$TMP/base.json" "$TMP/later.json" >"$TMP/compare.txt"
check "cross-run comparison runs" "grep -q 'CROSS-RUN COMPARISON' '$TMP/compare.txt'"
check "cross-run comparison names first divergence" "grep -q 'FIRST DIVERGENCE' '$TMP/compare.txt'"

# 8. C++ and Rust frontend routes validate language-agnostic collection.
CXX="${CXX:-clang++}"
if command -v "$CXX" >/dev/null 2>&1; then
  cat >"$TMP/sample.cpp" <<'EOF'
  #include <vector>
  static inline int square(int x) { return x * x; }
  int sum(const std::vector<int> &v) {
    int s = 0;
    for (int x : v) s += square(x);
    return s;
  }
  int main() { std::vector<int> v{1, 2, 3}; return sum(v) == 14 ? 0 : 1; }
EOF
  "$CXX" -O2 -fpass-plugin="$PLUGIN" -c "$TMP/sample.cpp" -o "$TMP/cpp.o" \
    2>"$TMP/cpp.err"
  check "C++ plugin loads and reports" "grep -q '^LPTA ' '$TMP/cpp.err'"
  "$CXX" -O2 -c "$TMP/sample.cpp" -o "$TMP/cpp-plain.o"
  check "C++ object is byte-identical" "cmp -s '$TMP/cpp.o' '$TMP/cpp-plain.o'"
fi

RUSTC="${RUSTC:-rustc}"
if command -v "$RUSTC" >/dev/null 2>&1; then
  cat >"$TMP/sample.rs" <<'EOF'
  fn square(x: i32) -> i32 { x * x }
  fn sum_squares(a: &[i32]) -> i32 {
      a.iter().map(|&x| square(x)).sum()
  }
  fn main() { println!("{}", sum_squares(&[1, 2, 3, 4])); }
EOF
  if "$RUSTC" --emit=llvm-ir -C opt-level=2 "$TMP/sample.rs" \
      -o "$TMP/rust.ll" >/dev/null 2>&1; then
    "$OPT" -load-pass-plugin="$PLUGIN" -passes='default<O2>' "$TMP/rust.ll" \
      -disable-output 2>"$TMP/rust.err"
    check "Rust IR replay reports rustc producer" \
      "grep -q 'producer=rustc' '$TMP/rust.err'"
    check "Rust IR replay uses opt route" \
      "grep -q 'route=opt-replay' '$TMP/rust.err'"
  fi
fi

# 9. Wrapper.
LPTA_PLUGIN="$PLUGIN" "$TESTS/lpta/wrapper-test.sh" "$TESTS" "$TMP" \
  && ok "lpta wrapper works" || bad "lpta wrapper works"

echo "----------------------------------------"
echo "LPTA integration: $pass passed, $fail failed"
test "$fail" -eq 0
