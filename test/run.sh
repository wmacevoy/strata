#!/bin/sh
# run.sh -- the one command that says whether strata works.
#
# IT EXISTS BECAUSE ITS ABSENCE COST SIX MONTHS OF TRUST.
#
# The test binaries load fixtures by RELATIVE path -- dens/echo.c, examples/,
# and so on. Run them the obvious way, from the build directory like any other
# cmake project, and SEVEN OF SIXTEEN FAIL:
#
#     cd build_test && ./test_den
#     failed to load dens/echo.c
#     Assertion failed: (rc == 0), function main, test_den.c line 27
#
# Run them from the repo root and all sixteen pass. Nothing in the tree said
# so -- README.md was 8 bytes, and neither CLAUDE.md nor ARCHITECTURE.md
# mentioned the working directory. So anyone who checked, including the author,
# saw a project that was 44% broken and was looking at a `cd`.
#
# A CHECK THAT FAILS FOR THE WRONG REASON IS WORSE THAN NO CHECK, because it
# teaches you to stop reading the result -- and then a real failure hides in
# the noise you have learned to ignore.
#
# Usage:  sh test/run.sh [build-dir]     (default: build_test)

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${1:-build_test}"

# THE WHOLE POINT. Everything below runs from here, whatever directory the
# caller was standing in.
cd "$ROOT" || exit 2

if [ ! -d "$BUILD" ]; then
    echo "no build directory at $ROOT/$BUILD"
    echo "  configure one first, e.g.:  cmake -B $BUILD && cmake --build $BUILD"
    exit 2
fi

n=$(ls "$BUILD"/test_* 2>/dev/null | wc -l | tr -d ' ')
if [ "$n" = "0" ]; then
    echo "no test binaries in $BUILD -- build them first."
    echo "  This is NOT the same as 'the tests pass'."
    exit 2
fi

pass=0; fail=0; failed=''
for b in "$BUILD"/test_*; do
    [ -x "$b" ] || continue
    t=$(basename "$b")
    if out=$("$b" 2>&1); then
        pass=$((pass + 1))
    else
        rc=$?
        fail=$((fail + 1))
        failed="$failed $t"
        printf 'FAIL  %-26s exit=%s\n' "$t" "$rc"
        printf '%s\n' "$out" | tail -3 | sed 's/^/      /'
    fi
done

printf '\n%d passed, %d failed   (%s, from %s)\n' "$pass" "$fail" "$BUILD" "$ROOT"
[ -n "$failed" ] && printf 'failed:%s\n' "$failed"

# Exit status is the answer. Nothing else here is.
[ "$fail" -eq 0 ]
