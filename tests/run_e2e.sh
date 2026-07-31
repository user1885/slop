#!/bin/sh
# End-to-end: compile real slop programs and run what comes out.
#
# Every other suite checks one stage in isolation. This one takes each program
# in examples/ through the whole pipeline twice — once to LLVM IR and a native
# binary, once to C and a native binary — runs both, and requires that they
# print the same thing and that it matches the golden output.
#
# Running *both* backends is the point, not thoroughness for its own sake.
# They fail differently, and each has caught bugs the other hid:
#
#   The C backend rounds every alloca up to a uint64_t of storage, so it
#   survived a slot being written wider than it was allocated. Only the
#   native build from the LLVM IR crashed.
#
#   The LLVM backend printed a whole-struct load into a memcpy argument
#   without complaint. Only the C backend refused to emit it.
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

cc=${CC:-cc}
passed=0
failed=0

note_fail() {
    echo "FAIL    $1"
    if [ -n "${2:-}" ] && [ -f "$2" ]; then
        sed 's/^/        /' "$2"
    fi
    failed=$((failed + 1))
}

note_ok() {
    echo "ok      $1"
    passed=$((passed + 1))
}

for src in "$root"/examples/*.slop; do
    name=$(basename "$src" .slop)
    expected="$golden/$name.out.expected"
    got_c=""
    got_llvm=""

    # ---- the C backend, all the way to a running program -----------------
    if "$slop" --backend=c "$src" > "$work/$name.c" 2> "$work/$name.emit.log" &&
        [ ! -s "$work/$name.emit.log" ]; then
        if "$cc" -std=c99 -Wall -Wextra -fno-builtin -o "$work/$name.c.bin" \
            "$work/$name.c" 2> "$work/$name.cc.log" && [ ! -s "$work/$name.cc.log" ]; then
            set +e
            "$work/$name.c.bin" > "$work/$name.c.out" 2>&1
            c_status=$?
            set -e
            got_c="$work/$name.c.out"
            note_ok "$name: c backend built and ran (exit $c_status)"
        else
            note_fail "$name: generated C did not compile cleanly" "$work/$name.cc.log"
        fi
    else
        note_fail "$name: c backend failed" "$work/$name.emit.log"
    fi

    # ---- the LLVM backend, through llvm-as and llc ------------------------
    if "$slop" --emit "$src" > "$work/$name.ll" 2> "$work/$name.ll.log"; then
        note_ok "$name: llvm ir emitted ($(wc -l < "$work/$name.ll") lines)"

        if command -v llvm-as > /dev/null 2>&1; then
            if llvm-as "$work/$name.ll" -o /dev/null 2> "$work/$name.as.log"; then
                note_ok "$name: llvm-as accepts the module"
            else
                note_fail "$name: llvm-as rejected the module" "$work/$name.as.log"
            fi
        fi

        if command -v llc > /dev/null 2>&1; then
            if llc -relocation-model=pic -filetype=obj "$work/$name.ll" -o "$work/$name.o" \
                2> "$work/$name.llc.log" &&
                "$cc" -o "$work/$name.ll.bin" "$work/$name.o" 2>> "$work/$name.llc.log"; then
                set +e
                "$work/$name.ll.bin" > "$work/$name.ll.out" 2>&1
                ll_status=$?
                set -e
                got_llvm="$work/$name.ll.out"
                note_ok "$name: llvm backend built and ran (exit $ll_status)"
            else
                note_fail "$name: llc or link failed" "$work/$name.llc.log"
            fi
        else
            echo "skip    $name: llvm toolchain not installed"
        fi
    else
        note_fail "$name: llvm backend failed" "$work/$name.ll.log"
    fi

    # ---- the two must agree, and match the golden -------------------------
    if [ -n "$got_c" ] && [ -n "$got_llvm" ]; then
        if diff -u "$got_llvm" "$got_c" > "$work/$name.cross"; then
            note_ok "$name: both backends printed the same thing"
        else
            note_fail "$name: backends disagree (llvm left, c right)" "$work/$name.cross"
        fi
    fi

    if [ -n "$got_c" ]; then
        if [ "$update" -eq 1 ]; then
            cp "$got_c" "$expected"
            echo "updated $expected"
        elif [ ! -f "$expected" ]; then
            note_fail "$name: no golden output (run --update)" ""
        elif diff -u "$expected" "$got_c" > "$work/$name.diff"; then
            note_ok "$name: output matches the golden"
        else
            note_fail "$name: output changed" "$work/$name.diff"
        fi
    fi
done

if [ "$update" -eq 1 ]; then
    exit 0
fi

printf '%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
