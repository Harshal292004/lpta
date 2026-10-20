#!/usr/bin/env bash
set -euo pipefail

TESTS="${1:?}"
TMP="${2:?}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WRAPPER="$HERE/../../scripts/lpta"

"$WRAPPER" --json="$TMP/wrapper.json" --report=off -- \
  clang -O2 -c "$TESTS/lpta/sample.c" -o "$TMP/wrapper.o"
test -s "$TMP/wrapper.json"
grep -q '"schema_version": "1.0"' "$TMP/wrapper.json"
