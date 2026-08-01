#!/bin/sh
# stage1: the slop lexer, written in slop, checked against the C one.
#
# The C compiler builds stage1/*.slop into a binary, that binary lexes every
# .slop file in the repository, and its output must be byte-identical to
# `slop --tokens` on the same file — stdout and stderr both, so diagnostics
# are compared too.
#
# That is the whole measure. A self-hosting port is either bit-exact with the
# implementation it replaces or it is a different lexer, and "looks right"
# is not a check. The set of inputs includes stage1/lexer.slop itself, so the
# lexer lexes its own source, and a non-slop file, because a lexer has to
# survive arbitrary bytes rather than only the language.
#
#   ./tests/run_stage1.sh [path/to/slop]

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

slop=${1:-$root/build/slop}
case "$slop" in
/*) ;;
*) slop="$(pwd)/$slop" ;;
esac

if [ ! -x "$slop" ]; then
    echo "run_stage1.sh: no slop binary at $slop" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
passed=0
failed=0

# ---- build the slop lexer with the C compiler ----------------------------
# Every stage1 source, so adding one never needs this list edited again.
if ! "$slop" --backend=c "$root"/stage1/*.slop \
    > "$work/stage1.c" 2> "$work/emit.log" || [ -s "$work/emit.log" ]; then
    echo "FAIL    stage1 did not compile"
    sed 's/^/        /' "$work/emit.log"
    exit 1
fi

if ! "$cc" -std=c99 -fno-builtin -o "$work/stage1-lex" "$work/stage1.c" 2> "$work/cc.log"; then
    echo "FAIL    generated C for stage1 did not build"
    sed 's/^/        /' "$work/cc.log"
    exit 1
fi
echo "ok      stage1 lexer built from slop"
passed=$((passed + 1))

# ---- and it must agree with the C lexer, everywhere ----------------------
# GRAMMAR.md is in the list on purpose: it is not slop, so it drives the
# scanner through bytes no .slop file contains.
for src in "$root"/examples/*.slop "$root"/stage1/*.slop "$root"/tests/cases/*.slop \
    "$root"/tests/multi/*.slop "$root/GRAMMAR.md"; do
    [ -f "$src" ] || continue
    name=$(basename "$src")

    set +e
    "$slop" --tokens "$src" > "$work/c.out" 2> "$work/c.err"
    "$work/stage1-lex" "$src" > "$work/s.out" 2> "$work/s.err"
    set -e

    if diff -u "$work/c.out" "$work/s.out" > "$work/out.diff" &&
        diff -u "$work/c.err" "$work/s.err" > "$work/err.diff"; then
        passed=$((passed + 1))
    else
        echo "FAIL    $name: stage1 lexer disagrees with the C lexer"
        sed 's/^/        /' "$work/out.diff" | head -20
        sed 's/^/        /' "$work/err.diff" | head -20
        failed=$((failed + 1))
    fi
done

if [ "$failed" -eq 0 ]; then
    echo "ok      stage1 lexer matches the C lexer on $((passed - 1)) files"
fi

# ---- the parser and the dumper --------------------------------------------
# Now that the dumper is ported, the two trees can be diffed directly, which
# is what actually pins the shape of what the parser builds. Item counts
# would not notice a mis-parsed expression inside a function body; this does.
for src in "$root"/examples/*.slop "$root"/stage1/*.slop "$root"/tests/multi/*.slop \
    "$root"/tests/cases/parse_*.slop "$root"/tests/cases/sema_*.slop; do
    [ -f "$src" ] || continue
    name=$(basename "$src")

    set +e
    "$slop" --parse-only "$src" > "$work/c.tree" 2> /dev/null
    "$work/stage1-lex" --parse "$src" > "$work/s.tree" 2> /dev/null
    set -e

    if diff -u "$work/c.tree" "$work/s.tree" > "$work/tree.diff"; then
        passed=$((passed + 1))
    else
        echo "FAIL    $name: stage1 builds a different tree"
        sed 's/^/        /' "$work/tree.diff" | head -20
        failed=$((failed + 1))
    fi
done

if [ "$failed" -eq 0 ]; then
    echo "ok      stage1 parser builds trees identical to the C parser"
fi

# ---- sema -----------------------------------------------------------------
# One file at a time, because stage1's driver does not yet compile several
# together. Diagnostics are not compared: stage1 words them differently and
# does not recover, so what is checked is the verdict -- the same files are
# accepted and the same ones rejected.
for src in "$root"/examples/*.slop "$root"/tests/cases/sema_*.slop \
    "$root"/tests/cases/parse_*.slop; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    set +e
    "$slop" "$src" > /dev/null 2>&1
    c_status=$?
    "$work/stage1-lex" --check "$src" > /dev/null 2>&1
    s_status=$?
    set -e
    if [ "$c_status" -eq "$s_status" ]; then
        passed=$((passed + 1))
    else
        echo "FAIL    $name: sema verdicts differ (C $c_status, stage1 $s_status)"
        failed=$((failed + 1))
    fi
done

if [ "$failed" -eq 0 ]; then
    echo "ok      stage1 sema accepts and rejects the same files as the C one"
fi

# ---- the front end on itself ----------------------------------------------
# The whole point of stage1: its lexer, parser and sema, compiled from slop,
# accepting the slop they are written in. All seven files go through together,
# which also exercises cross-file name resolution.
set +e
"$slop" "$root"/stage1/*.slop > /dev/null 2>&1
c_status=$?
"$work/stage1-lex" --check "$root"/stage1/*.slop > /dev/null 2>&1
s_status=$?
set -e
if [ "$c_status" -eq 0 ] && [ "$s_status" -eq 0 ]; then
    echo "ok      stage1's front end type-checks its own source"
    passed=$((passed + 1))
else
    echo "FAIL    stage1 on itself (C $c_status, stage1 $s_status)"
    failed=$((failed + 1))
fi

# Two files compiled together, where main uses names declared only in lib.
set +e
"$work/stage1-lex" --check "$root/tests/multi/main.slop" "$root/tests/multi/lib.slop" \
    > /dev/null 2>&1
s_status=$?
set -e
if [ "$s_status" -eq 0 ]; then
    echo "ok      stage1 resolves names across files"
    passed=$((passed + 1))
else
    echo "FAIL    stage1 multi-file name resolution"
    failed=$((failed + 1))
fi

# ---- the type universe ----------------------------------------------------
# Layout is a guarantee in GRAMMAR.md section 6, not an implementation
# detail, so the ported type table has to compute exactly what the C
# compiler does. These are the numbers the C compiler's own sizeof gives for
# the same declarations.
cat > "$work/types.want" <<'WANT'
i32       size=4 align=4
i32*      size=8 align=8
i32[10]   size=40 align=4
u8[3]     size=3 align=1
interned  yes
Padded    size=24 align=8 a@0 b@8 c@16
WANT

if "$work/stage1-lex" --types > "$work/types.got" 2>&1 &&
    diff -u "$work/types.want" "$work/types.got" > "$work/types.diff"; then
    echo "ok      stage1 type layout matches the C compiler"
    passed=$((passed + 1))
else
    echo "FAIL    stage1 type layout differs"
    sed 's/^/        /' "$work/types.diff"
    failed=$((failed + 1))
fi

# ---- the IR and the C backend ---------------------------------------------
# stage1 builds a module by hand and emits C from it. That C is compiled and
# run: the module computes 45 - 9, so it must exit 36. Same module and same
# number as the C side's ir_demo, so the two backends can be compared.
if "$work/stage1-lex" --ir > "$work/gen.c" 2> "$work/ir.log" && [ ! -s "$work/ir.log" ]; then
    if "$cc" -std=c99 -Wall -Wextra -fno-builtin -o "$work/gen" "$work/gen.c" \
        2> "$work/gencc.log" && [ ! -s "$work/gencc.log" ]; then
        set +e
        "$work/gen"
        gen_status=$?
        set -e
        if [ "$gen_status" -eq 36 ]; then
            echo "ok      stage1's C backend emits code that runs (exit 36)"
            passed=$((passed + 1))
        else
            echo "FAIL    stage1 backend output exited $gen_status, expected 36"
            failed=$((failed + 1))
        fi
    else
        echo "FAIL    stage1 backend output did not compile cleanly"
        sed 's/^/        /' "$work/gencc.log"
        failed=$((failed + 1))
    fi
else
    echo "FAIL    stage1 --ir failed"
    sed 's/^/        /' "$work/ir.log"
    failed=$((failed + 1))
fi

printf '%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
