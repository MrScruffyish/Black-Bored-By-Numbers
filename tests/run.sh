#!/usr/bin/env bash
# Regression tests for board.
#
# Each test is tests/cases/NAME.txt with expected output in NAME.expected.
# Optional files:
#   NAME.args    extra flags, one per line
#   NAME.status  expected exit status (default 0)
#
# stdout and stderr are combined before comparing, so error tests work too.

set -u
cd "$(dirname "$0")/.."

board=./board
pass=0
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ ! -x "$board" ]; then
    echo "error: $board not built; run make first" >&2
    exit 1
fi

for input in tests/cases/*.txt; do
    name=$(basename "$input" .txt)
    expected="tests/cases/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no $expected)"
        continue
    fi

    args=()
    if [ -f "tests/cases/$name.args" ]; then
        mapfile -t args < "tests/cases/$name.args"
    fi

    want_status=0
    if [ -f "tests/cases/$name.status" ]; then
        want_status=$(cat "tests/cases/$name.status")
    fi

    "$board" "$input" "${args[@]+"${args[@]}"}" > "$tmp/out" 2>&1
    got_status=$?

    ok=1
    if [ "$got_status" -ne "$want_status" ]; then
        ok=0
    fi
    if ! diff -u "$expected" "$tmp/out" > "$tmp/diff"; then
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
        echo "PASS $name"
    else
        fail=$((fail + 1))
        echo "FAIL $name (exit $got_status, want $want_status)"
        cat "$tmp/diff"
    fi
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
