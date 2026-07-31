#!/bin/sh
# Tests the IR and every backend, using the hand-built module in ir_demo.c.
#
# Two kinds of check, and the second is the one that matters:
#
#   Golden text — each backend's output is compared against
#   tests/ir/demo<ext>.expected. This catches unintended changes to what is
#   emitted, but it only proves the text did not change.
#
#   Execution — the C backend's output is compiled with the host C compiler
#   and run. The module computes 45 - 3 - 5 + -1, so it must exit with 36.
#   That proves the emitted code *means* the right thing: control flow,
#   struct field offsets, array indexing, sign extension and truncation all
#   have to be right for the number to come out.
#
# The LLVM backend has no equivalent execution check here because no LLVM
# toolchain is installed. If you have one, this is the check to add:
#
#   llvm-as demo.ll -o /dev/null     # syntax and type checking
#   lli demo.ll; test $? -eq 36      # same execution check
#
#   ./tests/run_ir.sh [path/to/slop-ir-demo]
#   ./tests/run_ir.sh --update [path]

set -eu

update=0
if [ "${1:-}" = "--update" ]; then
    update=1
    shift
fi

here=$(cd "$(dirname "$0")" && pwd)
golden="$here/ir"

demo=${1:-$here/../build/slop-ir-demo}
case "$demo" in
/*) ;;
*) demo="$(pwd)/$demo" ;;
esac

if [ ! -x "$demo" ]; then
    echo "run_ir.sh: no ir_demo binary at $demo" >&2
    exit 2
fi

mkdir -p "$golden"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

passed=0
failed=0

check_golden() {
    backend=$1
    ext=$2
    expected="$golden/demo$ext.expected"

    "$demo" "$backend" > "$work/out$ext"

    if [ "$update" -eq 1 ]; then
        cp "$work/out$ext" "$expected"
        echo "updated $expected"
        return 0
    fi

    if [ ! -f "$expected" ]; then
        echo "MISSING $expected (run --update)"
        failed=$((failed + 1))
        return 0
    fi

    if diff -u "$expected" "$work/out$ext" > "$work/diff"; then
        echo "ok      $backend golden"
        passed=$((passed + 1))
    else
        echo "FAIL    $backend golden"
        sed 's/^/        /' "$work/diff"
        failed=$((failed + 1))
    fi
}

check_golden llvm .ll
check_golden c .c

if [ "$update" -eq 1 ]; then
    exit 0
fi

# The execution check. 36 is what the module computes; see ir_demo.c.
cc=${CC:-cc}
if ! command -v "$cc" > /dev/null 2>&1; then
    echo "skip    c execution ($cc not found)"
else
    # -fno-builtin: an extern's slop signature (u8* -> void *) does not match
    # the compiler's built-in idea of printf. See the comment in backend_c.c.
    # Warnings are failures here: generated code that warns is generated code
    # nobody will want to compile.
    if "$cc" -std=c99 -Wall -Wextra -fno-builtin -o "$work/demo" "$work/out.c" 2> "$work/cc.log"; then
        if [ -s "$work/cc.log" ]; then
            echo "FAIL    c execution (generated code produced warnings)"
            sed 's/^/        /' "$work/cc.log"
            failed=$((failed + 1))
        else
            set +e
            "$work/demo" > "$work/run.out" 2>&1
            status=$?
            set -e
            if [ "$status" -eq 36 ]; then
                echo "ok      c execution (exit 36)"
                passed=$((passed + 1))
            else
                echo "FAIL    c execution: expected exit 36, got $status"
                sed 's/^/        /' "$work/run.out"
                failed=$((failed + 1))
            fi
        fi
    else
        echo "FAIL    c execution (generated code did not compile)"
        sed 's/^/        /' "$work/cc.log"
        failed=$((failed + 1))
    fi
fi

# If an LLVM toolchain is present, hold the .ll to the same standard.
if command -v llvm-as > /dev/null 2>&1; then
    if llvm-as "$work/out.ll" -o /dev/null 2> "$work/as.log"; then
        echo "ok      llvm-as accepts the module"
        passed=$((passed + 1))
    else
        echo "FAIL    llvm-as rejected the module"
        sed 's/^/        /' "$work/as.log"
        failed=$((failed + 1))
    fi
else
    echo "skip    llvm-as (not installed)"
fi

if command -v lli > /dev/null 2>&1; then
    set +e
    lli "$work/out.ll" > /dev/null 2>&1
    status=$?
    set -e
    if [ "$status" -eq 36 ]; then
        echo "ok      llvm execution (exit 36)"
        passed=$((passed + 1))
    else
        echo "FAIL    llvm execution: expected exit 36, got $status"
        failed=$((failed + 1))
    fi
else
    echo "skip    lli (not installed)"
fi

printf '%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
