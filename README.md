# slop

A small systems language, and a compiler for it written in C99.

The goal in `GRAMMAR.md` is deliberately narrow: **the smallest language that
can host a slop compiler written in slop.** No generics, no closures, no
function pointers, no `for` loop. What is there is enough to write a compiler
with — structs with guaranteed layout, pointers that scale by their pointee,
tagged unions via a common struct prefix, and `match`.

```
slop source ──▶ lexer ──▶ parser ──▶ sema ──▶ lowering ──▶ IR ──▶ backend
                                                                  ├─ LLVM IR
                                                                  └─ C99
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build
```

C99, no dependencies. LLVM is optional: without it the LLVM half of the tests
skips cleanly, and the C backend still takes a program all the way to a binary.

## Use

```bash
slop --emit prog.slop            # LLVM IR on stdout (the default backend)
slop --backend=c prog.slop       # C99 on stdout
slop --list-backends             # what is available

slop prog.slop                   # type-check and dump the annotated AST
slop --parse-only prog.slop      # stop after the parser, no types
slop --tokens prog.slop          # stop after the lexer
```

Declaration order does not matter, and neither does file order — names are
collected across every file before any body is checked:

```bash
slop --emit main.slop lib.slop > out.ll
llc -relocation-model=pic -filetype=obj out.ll -o out.o && cc out.o -o prog
```

Or without LLVM at all:

```bash
slop --backend=c main.slop lib.slop > out.c
cc -std=c99 -fno-builtin out.c -o prog
```

`-fno-builtin` is needed because slop's `u8*` becomes `void *`, so the
prototype emitted for an extern like `printf` does not match the one the C
compiler has built in. It is ABI-compatible; the complaint is about C's type
rules.

## The language in one program

```slop
extern "c" { fn printf(u8* fmt, ...) -> i32; }

struct Point { i32 x; i32 y; }

enum Kind { LEAF, NODE }

fn distance_squared(Point* p) -> i64 {
    return p.x.as(i64) * p.x.as(i64) + p.y.as(i64) * p.y.as(i64);
}

fn main() -> i32 {
    let mut Point p;          # no initializer means zeroed
    p.x = 3;
    p.y = 4;

    let mut i32 i = 0;
    while i < 3 {
        i += 1;
        if i == 2 { continue; }
    }

    match i {
        3 => printf("%lld\n", distance_squared(&p));
        _ => printf("unexpected\n");
    }
    return 0;
}
```

Things that differ from C on purpose:

- **Bitwise binds tighter than comparison.** `a & b == c` is `(a & b) == c`.
- **Casts are postfix**: `x.as(i64)`, so `n.as(Node*).kind` chains without
  parentheses. Every narrowing, signedness change and pointer conversion needs
  one; widening within a signedness is implicit.
- **Conditions must be `bN`.** No "nonzero is true" — write `p != null`.
- **`.` auto-dereferences one level**, so there is no `->`.
- **Struct layout is a guarantee**, not an implementation detail: fields in
  declaration order with C alignment. The common-prefix trick depends on it.
- **Structs may not be passed or returned by value**, only through pointers.
  That removes SysV argument classification, which is the nastiest part of
  writing a backend.

## Layout

One directory per compiler pass, and includes are spelled from `src/` down
(`#include "lexer/lexer.h"`), so a header says which pass it belongs to at
every use site.

| Directory      | What it is                                            |
|----------------|-------------------------------------------------------|
| `src/lexer/`   | source to tokens, maximal munch                        |
| `src/parser/`  | tokens to AST, recursive descent, panic-mode recovery  |
| `src/ast/`     | the tree and its debug dumper                          |
| `src/sema/`    | name collection, type resolution, body checking        |
| `src/ir/`      | an in-memory model of LLVM IR, plus a verifier         |
| `src/backend/` | lowering, and one file per target                      |
| `src/support/` | arena, diagnostics, string views, vectors, hash map    |

## Adding a backend

A backend is a name and one function:

```c
typedef struct {
    const char *name, *description, *extension;
    int (*emit)(const IrModule *module, FILE *out);
} Backend;
```

Write `emit`, add a row to the table in `src/backend/backend.c`, add the file
to `CMakeLists.txt`. A backend cannot reach the AST, the tokens or sema's
tables — it only sees a verified `IrModule`, which is what keeps targets
independent of the front end.

The IR is modelled on LLVM's, with two simplifications a backend may rely on:
**no phi nodes** (every local is an `alloca`, exactly what clang `-O0` emits
and `opt -mem2reg` promotes away), and **every value has an explicit name**.

## Tests

```bash
ctest --test-dir build
```

- `tests/run.sh` — golden token and AST dumps. Pinning the AST pins
  precedence, associativity and error recovery.
- `tests/run_ir.sh` — a module built by hand through every backend, with the
  C output compiled and run.
- `tests/run_e2e.sh` — every program in `examples/` through the whole
  pipeline on *both* backends, to native binaries, run, and required to agree
  with each other and with the golden output.

`examples/conformance.slop` is the language's own test: 90 checks with known
answers that report themselves and set the exit status.

Run both backends rather than trusting either. They fail differently, and each
has caught bugs the other hid — the C backend rounds allocas up to a
`uint64_t` and so survived a slot being written wider than it was allocated,
while the LLVM printer happily emitted a whole-struct load that only the C
backend refused.

## stage1: slop in slop

`stage1/` is the compiler being rewritten in its own language. The lexer,
the parser and the AST dumper are done:

```bash
slop --backend=c stage1/lexer.slop stage1/main.slop > stage1.c
cc -std=c99 -fno-builtin stage1.c -o stage1-lex
stage1-lex examples/demo.slop        # same output as: slop --tokens ...
```

It is held to byte-identical agreement with the C lexer — stdout and stderr
both — on every `.slop` file in the repository plus `GRAMMAR.md`, which is not
slop at all and so drives the scanner through bytes no source file contains.
The list includes `stage1/lexer.slop`, so the lexer lexes its own source.
`tests/run_stage1.sh` is that check.

The parser is held to the same standard as the lexer: the dumper is ported
too, so the two trees are diffed directly. Nineteen files produce
byte-identical dumps, `stage1/` included, which means the slop parser parses
and prints its own source exactly as the C one does. Item counts would not
notice a mis-parsed expression inside a function body; a tree diff does.

Error *recovery* is not ported. The C parser resynchronises at statement,
member and item boundaries to report many errors per run; stage1 reports the
first and stops. Both reject the same files, but they do not print the same
list of diagnostics.

What the port had to give up, and what it says about v0: the C lexer leans on
X-macros, unions, static tables and a preprocessor, and the C AST is a tagged
union. slop has none of those. The token kinds became an enum and two
functions, `Token` carries its payload fields side by side, and the AST uses
the common-prefix trick — every node starts with the same header, and
`.as(T*)` reinterprets it once the kind has been read. That trick is the
reason `GRAMMAR.md` makes struct layout a guarantee rather than an
implementation detail, so the tree is the part of the port the language was
most deliberately designed for. Nothing needed a language change.

## Status

The front end, sema, lowering and both backends work: `examples/demo.slop` is
563 lines of slop that compiles to a native binary through either path and
produces identical output. The lexer is ported to slop and bit-exact with the
original. Not done: parser, sema and backends in slop.
