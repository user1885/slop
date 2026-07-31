#!/bin/sh
# Golden-file tests for the slop front end.
#
# Each tests/cases/<name>.slop is fed to the compiler and its combined
# stdout+stderr+exit status is compared against tests/cases/<name>.expected.
# A case named lex_* runs with --tokens and pins the token stream; anything
# else runs the parser and pins the shape of the AST.
#
# The point is to assert the front end is *correct*, not merely that it does
# not crash: the AST dump is the only externally visible statement of how a
# program was parsed, so freezing it freezes precedence, associativity and
# recovery behaviour.
#
#   ./tests/run.sh [path/to/slop]     run every case
#   ./tests/run.sh --update [path]    rewrite the .expected files
#
# --update is how you accept a deliberate change to the dump format. Read the
# resulting diff before committing it: a golden file nobody looked at proves
# only that the output did not change, never that it was right.
#
# Cases run with the case directory as cwd, so diagnostics contain a bare
# filename and the goldens stay independent of where the repo lives.

set -eu

update=0
if [ "${1:-}" = "--update" ]; then
    update=1
    shift
fi

here=$(cd "$(dirname "$0")" && pwd)
cases="$here/cases"

slop=${1:-$here/../build/slop}
case "$slop" in
/*) ;;
*) slop="$(pwd)/$slop" ;;
esac

if [ ! -x "$slop" ]; then
    echo "run.sh: no slop binary at $slop" >&2
    echo "run.sh: build it first, or pass the path as an argument" >&2
    exit 2
fi

passed=0
failed=0
updated=0

for path in "$cases"/*.slop; do
    name=$(basename "$path" .slop)

    args=""
    case "$name" in
    lex_*) args="--tokens" ;;
    esac

    # errexit is inherited by this subshell, and a case that exits nonzero is
    # the normal outcome for the error tests, so turn it off for the run
    # itself: the exit status is data here, not a failure.
    actual=$(
        set +e
        cd "$cases" || exit 1
        "$slop" $args "$name.slop" 2>&1
        printf 'exit: %d' "$?"
    )
    expected="$cases/$name.expected"

    if [ "$update" -eq 1 ]; then
        printf '%s\n' "$actual" > "$expected"
        updated=$((updated + 1))
        continue
    fi

    if [ ! -f "$expected" ]; then
        printf 'MISSING %s (no .expected; run --update to create it)\n' "$name"
        failed=$((failed + 1))
        continue
    fi

    if printf '%s\n' "$actual" | diff -u "$expected" - > /tmp/slop-test-diff.$$ 2>&1; then
        passed=$((passed + 1))
    else
        printf 'FAIL    %s\n' "$name"
        sed 's/^/        /' /tmp/slop-test-diff.$$
        failed=$((failed + 1))
    fi
    rm -f /tmp/slop-test-diff.$$
done

if [ "$update" -eq 1 ]; then
    printf 'updated %d golden file(s)\n' "$updated"
    exit 0
fi

printf '%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
