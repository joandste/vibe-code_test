#!/bin/sh
# Run the reference-implementation test suite.
#
# tests/cases/*.lang   must exit 0; stdout must match *.expected
# tests/errors/*.lang  must exit non-zero; combined output must match *.expected
#
# Expected outputs use <file> in place of the test's absolute path.

set -u
root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/langrun"
fail=0
count=0

if [ ! -x "$bin" ]; then
    echo "error: $bin not built (run make)" >&2
    exit 1
fi

normalize() {
    # $1 = file with output, $2 = source path
    sed "s|$2|<file>|g" "$1"
}

for t in "$root"/tests/cases/*.lang; do
    name=$(basename "$t" .lang)
    expected="$root/tests/cases/$name.expected"
    actual="$root/tests/cases/$name.actual"
    count=$((count + 1))
    "$bin" -p "$t" >"$actual.raw" 2>&1
    status=$?
    normalize "$actual.raw" "$t" >"$actual"
    rm -f "$actual.raw"
    if [ $status -ne 0 ]; then
        echo "FAIL $name (exit status $status)"
        cat "$actual"
        fail=1
        continue
    fi
    if diff -u "$expected" "$actual"; then
        rm -f "$actual"
        echo "ok   $name"
    else
        echo "FAIL $name"
        fail=1
    fi
done

for t in "$root"/tests/errors/*.lang; do
    name=$(basename "$t" .lang)
    expected="$root/tests/errors/$name.expected"
    actual="$root/tests/errors/$name.actual"
    count=$((count + 1))
    "$bin" -p "$t" >"$actual.raw" 2>&1
    status=$?
    normalize "$actual.raw" "$t" >"$actual"
    rm -f "$actual.raw"
    if [ $status -eq 0 ]; then
        echo "FAIL $name (expected a non-zero exit status)"
        cat "$actual"
        fail=1
        continue
    fi
    if diff -u "$expected" "$actual"; then
        rm -f "$actual"
        echo "ok   $name"
    else
        echo "FAIL $name"
        fail=1
    fi
done

echo "---"
if [ $fail -ne 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "all $count tests passed"
