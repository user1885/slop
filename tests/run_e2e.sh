#!/bin/sh
# End-to-end: compile a real slop program and run the result.
#
# Everything else in the test suite checks a stage in isolation. This runs the
# whole pipeline — lexer, parser, sema, lowering, backend — over
# examples/demo.slop, which touches every construct in v0, and then *runs the
# program* and compares what it printed. Nothing else proves the compiler
# emits code that means what the source said.
#
#   ./tests/run_e2e.sh [path/to/slop]
#   ./tests/run_e2e.sh --update [path]

set -eu

update=0
if [ "${1:-}" = "--update" ]; then
    update=1
    shift
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
golden="$here/e2e"

slop=${1:-$root/build/slop}
case "$slop" in
/*) ;;
*) slop="$(pwd)/$slop" ;;
esac

if [ ! -x "$slop" ]; then
    echo "run_e2e.sh: no slop binary at $slop" >&2
    exit 2
fi

mkdir -p "$golden"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

src="$root/examples/demo.slop"
passed=0
failed=0

# The LLVM backend has to at least produce a module that passes ir_verify,
# which the driver runs before handing anything to a backend.
if "$slop" --backend=llvm "$src" > "$work/demo.ll" 2> "$work/ll.log"; then
    echo "ok      llvm emit ($(wc -l < "$work/demo.ll") lines)"
    passed=$((passed + 1))
else
    echo "FAIL    llvm emit"
    sed 's/^/        /' "$work/ll.log"
    failed=$((failed + 1))
fi

if command -v llvm-as > /dev/null 2>&1; then
    if llvm-as "$work/demo.ll" -o /dev/null 2> "$work/as.log"; then
        echo "ok      llvm-as accepts demo.ll"
        passed=$((passed + 1))
    else
        echo "FAIL    llvm-as rejected demo.ll"
        sed 's/^/        /' "$work/as.log"
        failed=$((failed + 1))
    fi
else
    echo "skip    llvm-as (not installed)"
fi

# The LLVM path all the way to a native binary. This is the check that found
# the short-circuit slot being written wider than it was allocated: the C
# backend rounds every alloca up to a uint64_t, so it survived the overflow
# and only the native build crashed. Two backends only cross-check each other
# if both are actually run.
if command -v llc > /dev/null 2>&1 && command -v cc > /dev/null 2>&1; then
    if llc -relocation-model=pic -filetype=obj "$work/demo.ll" -o "$work/demo.o" \
        2> "$work/llc.log" && cc -o "$work/demo_native" "$work/demo.o" 2>> "$work/llc.log"; then
        set +e
        "$work/demo_native" > "$work/native.txt" 2>&1
        status=$?
        set -e
        if [ "$status" -ne 0 ]; then
            echo "FAIL    native binary from LLVM IR exited $status"
            sed 's/^/        /' "$work/native.txt"
            failed=$((failed + 1))
        elif diff -u "$golden/demo.out.expected" "$work/native.txt" > "$work/ndiff"; then
            echo "ok      native binary from LLVM IR printed the expected output"
            passed=$((passed + 1))
        else
            echo "FAIL    native binary from LLVM IR printed something else"
            sed 's/^/        /' "$work/ndiff"
            failed=$((failed + 1))
        fi
    else
        echo "FAIL    llc/link of demo.ll"
        sed 's/^/        /' "$work/llc.log"
        failed=$((failed + 1))
    fi
else
    echo "skip    llc native build (llc or cc not found)"
fi

# The C backend is the one that can be taken all the way to a running program
# without an LLVM install.
"$slop" --backend=c "$src" > "$work/demo.c"

cc=${CC:-cc}
if ! command -v "$cc" > /dev/null 2>&1; then
    echo "skip    c execution ($cc not found)"
else
    # -fno-builtin: slop's u8* is void * here, which does not match the
    # compiler's built-in printf. See the comment in backend_c.c.
    if "$cc" -std=c99 -Wall -Wextra -fno-builtin -o "$work/demo" "$work/demo.c" \
        2> "$work/cc.log"; then
        if [ -s "$work/cc.log" ]; then
            echo "FAIL    generated C compiled with warnings"
            sed 's/^/        /' "$work/cc.log"
            failed=$((failed + 1))
        else
            set +e
            "$work/demo" > "$work/out.txt" 2>&1
            status=$?
            set -e

            if [ "$update" -eq 1 ]; then
                cp "$work/out.txt" "$golden/demo.out.expected"
                echo "updated $golden/demo.out.expected (exit $status)"
            elif [ "$status" -ne 0 ]; then
                echo "FAIL    compiled demo exited $status"
                sed 's/^/        /' "$work/out.txt"
                failed=$((failed + 1))
            elif diff -u "$golden/demo.out.expected" "$work/out.txt" > "$work/diff"; then
                echo "ok      compiled demo ran and printed the expected output"
                passed=$((passed + 1))
            else
                echo "FAIL    compiled demo printed something else"
                sed 's/^/        /' "$work/diff"
                failed=$((failed + 1))
            fi
        fi
    else
        echo "FAIL    generated C did not compile"
        sed 's/^/        /' "$work/cc.log"
        failed=$((failed + 1))
    fi
fi

if [ "$update" -eq 1 ]; then
    exit 0
fi

printf '%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
