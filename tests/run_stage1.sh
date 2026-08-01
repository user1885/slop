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
if ! "$slop" --backend=c "$root/stage1/lexer.slop" "$root/stage1/ast.slop" \
    "$root/stage1/parser.slop" "$root/stage1/main.slop" \
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

# ---- the parser -----------------------------------------------------------
# The AST dumper is not ported yet, so the two trees cannot be diffed. What
# can be compared is the parsers' agreement: the same item count on every
# well-formed file, and the same accept-or-reject verdict on the deliberately
# broken ones. That is weaker than the lexer's bit-exactness and is meant to
# be replaced by a dump comparison once the dumper lands.
for src in "$root"/examples/*.slop "$root"/stage1/*.slop "$root"/tests/multi/*.slop; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    c_items=$("$slop" --parse-only "$src" 2>/dev/null | head -1 |
        sed 's/.*(\([0-9]*\) items).*/\1/')
    s_items=$("$work/stage1-lex" --parse "$src" 2>/dev/null | sed 's/.*: \([0-9]*\) items.*/\1/')
    if [ -n "$c_items" ] && [ "$c_items" = "$s_items" ]; then
        passed=$((passed + 1))
    else
        echo "FAIL    $name: item count differs (C $c_items, stage1 $s_items)"
        failed=$((failed + 1))
    fi
done

for src in "$root"/tests/cases/*.slop; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    set +e
    "$slop" --parse-only "$src" > /dev/null 2>&1
    c_status=$?
    "$work/stage1-lex" --parse "$src" > /dev/null 2>&1
    s_status=$?
    set -e
    if [ "$c_status" -eq "$s_status" ]; then
        passed=$((passed + 1))
    else
        echo "FAIL    $name: parsers disagree on whether this is valid"
        failed=$((failed + 1))
    fi
done

if [ "$failed" -eq 0 ]; then
    echo "ok      stage1 parser agrees with the C parser on every file"
fi

printf '%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
