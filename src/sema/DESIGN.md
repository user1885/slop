# Semantic analysis for slop v0 — working spec

Scope of this document: everything between a parsed AST and a tree the
backend can lower without asking a single question. It is written for
whoever implements `src/sema/`, which for now is me.

`GRAMMAR.md` is authoritative. Where this document goes beyond it, section 12
lists exactly what was decided here and why — those are the parts that need a
second pair of eyes, and three of them want a `GRAMMAR.md` amendment.

---

## 1. What sema is responsible for

`GRAMMAR.md` section 7 splits the front end into passes. Sema owns passes
2–4:

| Pass | Name | Input | Output |
|------|------|-------|--------|
| 2 | global name collection | every `Program` | one global symbol table, duplicates diagnosed |
| 3 | type resolution | that table | every `Ty` resolved, sized, laid out |
| 4 | body checking | one function at a time | every `Expr` typed, every rule below enforced |

The split is not cosmetic. It is what makes declaration order irrelevant and
multi-file work: a body may call a function declared 500 lines below it, and
a struct field may name a struct from another file. A single pass genuinely
cannot do this.

**Non-goals** (`GRAMMAR.md` section 8, plus v0 notes): exhaustiveness
checking for `match`, constant expressions beyond literals, `sizeof` of an
expression, function pointers, generics, struct initializers, type inference
beyond literals, unused-variable or dead-code warnings, any optimisation.
Duplicate enum values stay legal and unchecked.

---

## 2. Shape of the thing

```
src/sema/
    types.h  types.c     the semantic type universe: Ty, interning, layout
    scope.h  scope.c     symbol tables, global and lexical
    sema.h   sema.c      the three passes
```

Entry point, and the only thing the driver sees:

```c
/* Checks a whole program: every file that was parsed, together. Returns the
 * number of errors; 0 means the trees are safe to lower. Everything sema
 * allocates comes from `arena`, which must be the one the trees are in. */
int sema_check(Arena *arena, Program **programs, size_t nprograms);
```

The driver currently parses one file per invocation. Sema takes an array
because pass 2 has to see every file before pass 4 checks any body; wiring
argv into one array is a `driver` change and part of this branch.

Dependencies, all of them already in `src/support/`: `arena` for storage,
`diag` for errors, `srcpos`/`strview` for positions and names. Sema adds one
support need — a string-keyed hash map (section 4). Nothing in `src/sema/`
may include anything from `src/driver/`.

---

## 3. The semantic type universe (`types.h`)

The AST's `Type` is syntax: `TYPE_NAME` holds an unresolved `StrView`, and
nothing knows a size. Sema builds a parallel, resolved universe. Different
name, deliberately, so a mix-up cannot compile:

```c
typedef enum {
    TY_VOID, TY_I8, TY_U8, TY_I32, TY_U32, TY_I64, TY_U64,
    TY_F32, TY_F64, TY_B8, TY_B32, TY_B64,      /* core, in this order */
    TY_PTR, TY_ARRAY, TY_STRUCT, TY_ENUM,
    TY_UNTYPED_INT,     /* an integer or char literal that has no context yet */
    TY_UNTYPED_BOOL,    /* true/false, and the result of a comparison */
    TY_NULL             /* the type of `null` */
} TyKind;

typedef struct Ty Ty;
typedef struct { StrView name; Ty *type; uint32_t offset; SrcPos pos; } FieldSym;
typedef struct { StrView name; uint64_t value; SrcPos pos; } EnumSym;

struct Ty {
    TyKind kind;
    uint32_t size;      /* bytes; 0 for TY_VOID and while a struct is opening */
    uint32_t align;     /* bytes; 1 minimum */
    Ty *elem;           /* TY_PTR, TY_ARRAY */
    uint64_t length;    /* TY_ARRAY */
    StrView name;       /* TY_STRUCT, TY_ENUM */
    FieldSym *fields;   uint32_t nfields;   /* TY_STRUCT */
    EnumSym *members;   uint32_t nmembers;  /* TY_ENUM */
    Ty *base;           /* TY_ENUM: its underlying core type */
    SrcPos pos;         /* where it was declared */
    int state;          /* TY_STRUCT layout: unstarted / in progress / done */
};
```

**Types are interned.** One `Ty` per distinct type, so `a == b` *is* type
equality and there is no `ty_equal()` to get wrong. The twelve core types are
singletons created at startup; pointers and arrays are looked up in a table
keyed by `(kind, elem, length)`; structs and enums are one per declaration.

**Layout** — `GRAMMAR.md` section 6 makes this a guarantee, not an
implementation detail, because the common-prefix trick for AST nodes depends
on it. Fields in declaration order, no reordering, C-compatible:

| type | size | align |
|------|------|-------|
| `i8` `u8` `b8` | 1 | 1 |
| `i32` `u32` `f32` `b32` | 4 | 4 |
| `i64` `u64` `f64` `b64` | 8 | 8 |
| `T*` | 8 | 8 |
| `T[N]` | `N * size(T)` | `align(T)` |
| `struct` | fields at aligned offsets in order, total rounded up to the struct's align | max of field aligns, 1 if empty |
| `enum` | size/align of its underlying core type | same |
| `void` | — | incomplete, may only appear as a return type or behind a pointer |

Pointers are 8 bytes because the only target in sight is x86-64. When that
stops being true it becomes a target property; nothing outside `types.c`
should assume it, so no other file writes the literal 8.

---

## 4. Symbol tables (`scope.h`)

Two namespaces, and they do not collide with each other:

- **types** — struct and enum names. Only ever consulted from type position.
- **values** — functions, extern functions, globals, enum members, locals and
  parameters. `GRAMMAR.md` section 6: enum members land in the global
  namespace as constants of their enum type, written unqualified.

So `struct Token` and a variable named `Token` coexist, exactly as in C, and
neither use site is ambiguous.

```c
typedef enum { SYM_FN, SYM_GLOBAL, SYM_ENUM_MEMBER, SYM_LOCAL, SYM_PARAM } SymKind;

typedef struct {
    SymKind kind;
    StrView name;
    Ty *type;           /* SYM_FN: the return type; params live in the FnSym */
    SrcPos pos;
    int is_mut;
    uint64_t value;     /* SYM_ENUM_MEMBER */
    FnDecl *decl;       /* SYM_FN, for the signature and the body */
} Sym;
```

The global table is a hash map; lexical scopes are a stack of small vectors,
searched innermost first and popped on block exit. Function bodies are small
and a linear scan through a handful of locals beats a hash lookup, but the
global table sees every name in the program and must not be linear.

**That map does not exist yet.** `support/map` — open addressing, FNV-1a over
a `StrView`, no deletion needed — is a prerequisite. It belongs to `support`
by the ownership table, so it is a separate claim and a separate branch, and
this branch codes against its header. Roughly 80 lines; do it first.

Shadowing: an inner scope may shadow an outer one, and a local may shadow a
global. Two declarations of the same name *in the same scope* are an error.
Parameters live in the function's outermost scope, so a local shadowing a
parameter is legal.

---

## 5. Pass 2 — global name collection

Walk **only** the top level of every program. Do not descend into a body.
For each item:

- `ITEM_STRUCT` — declare the name in the type namespace with an empty
  `TY_STRUCT`. Fields are *not* resolved yet; they may name a struct that
  appears later or in another file.
- `ITEM_ENUM` — declare the name in the type namespace, resolve the
  underlying core type (`TOK_KW_I32` when `: T` was omitted), then number the
  members: start at 0, an explicit `= int_lit` sets the counter, implicit
  members continue from the last one. Each value must fit the underlying
  type. Declare every member in the value namespace as a `SYM_ENUM_MEMBER` of
  that enum type.
- `ITEM_FN` — declare in the value namespace. Signature types are resolved in
  pass 3, not here.
- `ITEM_GLOBAL` — declare in the value namespace.
- `ITEM_EXTERN` — declare each `FnDecl` in the value namespace, flagged
  variadic where the parser set `is_variadic`. The ABI string is recorded and
  otherwise ignored in v0; anything other than `"c"` is an error, because
  nothing else is implemented and silently accepting it would be a lie.

Diagnosed here, and only here: **duplicate declarations**. Both the collision
and the original position get printed. Enum member values that do not fit the
underlying type are diagnosed here too, since numbering happens here.

After this pass everything declared anywhere in the program is visible.

---

## 6. Pass 3 — type resolution and layout

Resolving an AST `Type` to a `Ty`:

| AST | resolves to |
|-----|-------------|
| `TYPE_CORE` | the interned core singleton |
| `TYPE_NAME` | the type-namespace entry, or `unknown type 'X'` |
| `TYPE_POINTER` | `ty_ptr(resolve(elem))` — the pointee may still be opening |
| `TYPE_ARRAY` | `ty_array(resolve(elem), length)`; the element must be sized |

Then, for every struct, lay it out. The recursion is where the interesting
error lives:

```
layout(S):
    if S.state == DONE:        return
    if S.state == IN_PROGRESS: error "struct 'S' contains itself"; S.state = DONE; return
    S.state = IN_PROGRESS
    offset = 0; align = 1
    for each field:
        t = resolve(field.type)
        if t is a struct: layout(t)          # by value -> must be complete
        offset = round_up(offset, align(t)); field.offset = offset; offset += size(t)
        align = max(align, align(t))
    S.size = round_up(offset, align); S.align = align; S.state = DONE
```

Recursion **by value** is the error (`struct A { B b; } struct B { A a; }`).
Recursion through a pointer is fine and must stay fine — `struct Node { Node*
next; }` is the whole point — which falls out of `ty_ptr()` never calling
`layout()` on its pointee.

Also resolved and checked in this pass:

- **Function signatures.** Parameter and return types.
- **Struct by value is forbidden as a parameter type and as a return type**
  (`GRAMMAR.md` section 6). This removes SysV argument classification, which
  is the nastiest part of writing a backend. Pass a pointer. Diagnose it with
  that suggestion in the message.
- `void` as a parameter type, field type, variable type or array element is
  an error; as a return type it is the normal case, and `void*` is fine.
- Array length 0 is an error.
- **Global initializers.** The parser already restricted them to a literal;
  check that the literal is assignable to the declared type. A global without
  an initializer is zeroed, and — like a local — must be declared `mut`: an
  immutable binding that is never initialised can never hold anything but
  zero. `GRAMMAR.md` says "a variable without `mut` must have an
  initializer"; this reads *variable* as covering globals (section 12).

---

## 7. Pass 4 — body checking

One function at a time, against the now-complete global table. Function
order is irrelevant by this point.

### 7.1 The typing core

Everything hangs off one function:

```c
static Ty *check_expr(Sema *s, Expr *e, Ty *expected);
```

`expected` is the **context type**, or NULL where there is none. It is what
makes untyped literals work, and it must be threaded, not bolted on:

- `let u64 x = ...` passes `u64` into the initializer.
- an argument passes the parameter's type.
- an assignment passes the target's type.
- a binary operator passes the *other* side's type once that side is typed,
  and NULL when neither is.
- `return` passes the function's return type.

`GRAMMAR.md` section 6: *integer literals are untyped and adopt the context
type when the value fits; with no context they default to `i32`.* That is
`TY_UNTYPED_INT`, resolved at the point of use, never stored in the tree as
"untyped" once a context is known.

Worked example, from `demo.slop`, which does not type-check without this:

```
let u64 aligned = (size + 7) & ~7;
```

`size` is `u64`, so `7` adopts `u64`; the sum is `u64`, which becomes the
context for the right operand of `&`; that context reaches *through* the `~`
to its operand, so `~7` is `~(u64)7`, not a sign-extended `i32`. Context
propagates into unary operands. Likewise `popcount(0xF0F0F0F0)` only works
because the parameter type is `u64` — the value does not fit `i32`, and with
no context it would be an error rather than a wrap.

### 7.2 Assignability

`assignable(from, to)` — the one relation the whole pass leans on:

| from | to | allowed |
|------|-----|---------|
| `T` | `T` | yes, always (structs included: it is a memcpy) |
| `TY_UNTYPED_INT` | any integer or float | if the value fits |
| `TY_UNTYPED_BOOL` | any `bN` | yes |
| `TY_NULL` | any pointer | yes |
| `iN` | `iM`, N < M | yes — widening within the same signedness |
| `uN` | `uM`, N < M | yes |
| `f32` | `f64` | yes |
| `bN` | `bM`, N < M | yes |
| enum `E` | integer `T` | if `assignable(underlying(E), T)` |
| everything else | | **no** — needs an explicit `.as(T)` |

"Everything else" is `GRAMMAR.md` section 6 verbatim: narrowing, signedness
changes, int↔float, and **any** pointer conversion, `void*` included. The
demo casts every one of those explicitly; the strictness is the design.

The one direction that is *not* in `GRAMMAR.md` but is required by
`demo.slop` is enum → its underlying integer (`n.kind = N_INT` where `kind`
is `i32`). See section 12.

### 7.3 Expressions

| node | rule | result |
|------|------|--------|
| `EXPR_INT`, `EXPR_CHAR` | — | `TY_UNTYPED_INT`, default `i32` / `u8` |
| `EXPR_FLOAT` | — | untyped float, default `f64` |
| `EXPR_STRING` | — | `u8*`, static, NUL-terminated |
| `EXPR_BOOL` | — | `TY_UNTYPED_BOOL`, default `b8` |
| `EXPR_NULL` | — | `TY_NULL` |
| `EXPR_NAME` | look up in the value namespace | the symbol's type; a function name outside a call is an error (no function pointers in v0) |
| `+` `-` | both numeric → common type; or `ptr ± integer` → the pointer, **scaled by `sizeof(T)`**; or `ptr - ptr` of the same type → `i64` in elements | as stated |
| `*` `/` | both numeric, same type | that type |
| `%` | both **integer**, same type | that type; on floats it is an error |
| `& \| ^` `<< >>` | integers only — not floats, not `bN`. For shifts the operands are typed independently and the result is the left operand's type | as stated |
| `~` | integer | same type |
| `< <= > >=` | both numeric, or two pointers of the same type | `TY_UNTYPED_BOOL` |
| `== !=` | as above, plus pointer vs `null`, plus two values of the same enum | `TY_UNTYPED_BOOL` |
| `&& \|\|` | both `bN` (short-circuit) | `TY_UNTYPED_BOOL` |
| `!` | `bN` | `TY_UNTYPED_BOOL` |
| unary `-` | signed integer or float; on an unsigned type it is an error | same type |
| unary `*` | pointer, not `void*` | the pointee |
| unary `&` | an lvalue (7.5) | pointer to its type |
| `EXPR_INDEX` | base is an array or a pointer, index is an integer | the element type |
| `EXPR_FIELD` | base is a struct or a pointer to one — **one level of pointer is stripped automatically**, there is no `->` | the field's type |
| `EXPR_CALL` | callee must be `EXPR_NAME` resolving to a function; arity exact, or `>= nparams` for a variadic extern; each argument checked against its parameter as context | the return type; `void` here is only usable as an `ExprStmt` |
| `EXPR_CAST` | see below | the target type |
| `EXPR_SIZEOF` | the type must be sized | `u64` |

**Variadic arguments** past the last declared parameter get C's default
promotions, which is what makes `printf` work: untyped literals take their
default type, `f32` becomes `f64`, and integers narrower than 4 bytes become
`i32`/`u32`. No other implicit conversion applies there.

**`.as(T)` permits** numeric ↔ numeric (any direction), pointer ↔ pointer
(any direction, `void*` included), `bN` → integer, and enum ↔ its underlying
integer. It does **not** permit integer ↔ pointer, anything ↔ struct, or
anything ↔ array. A cast that is already an identity is legal and free.

### 7.4 Statements

- `STMT_VAR` — resolve the type, check the initializer against it, declare
  the name. **A variable without `mut` must have an initializer**
  (`GRAMMAR.md` section 6 — a sema error, not a parse error). Everything
  declared without one is zeroed, structs and arrays included.
- `STMT_IF`, `STMT_WHILE` — the condition must have type `bN`. There is no
  "nonzero is true": write `p != null`, `n != 0`. This is the single rule
  most likely to be hit by someone writing C out of habit, so the message
  says what to write instead.
- `STMT_MATCH` — the scrutinee must be an integer or an enum. Labels are
  integer literals, char literals, or names of enum members; each must be
  assignable to the scrutinee's type. `_` is the wildcard. **No exhaustiveness
  check in v0** and no fallthrough; a value matching no arm does nothing.
  `break` inside an arm belongs to the enclosing loop, not the match.
- `STMT_RETURN` — with a value, it must be assignable to the return type,
  with the return type as context; bare `return;` only in a `void` function.
- `STMT_BREAK`, `STMT_CONTINUE` — only inside a `while`. Sema carries a loop
  depth counter.
- `STMT_ASSIGN` — 7.5.
- `STMT_EXPR` — any expression; no "result unused" rule in v0.
- `STMT_BLOCK` — push a scope, check, pop.

### 7.5 Lvalues and mutability

The parser deliberately accepts `a + b = 5` so that sema can produce a real
diagnostic instead of "unexpected token" (`GRAMMAR.md` section 4). So the
check is on tree shape:

An **lvalue** is `EXPR_NAME` naming a variable, parameter or global (not a
function, not an enum member); `*e`; `e[i]`; or `e.f`. Anything else is
`cannot assign to this expression`.

**Mutability** follows the root of the lvalue path, and the path stops at the
first pointer dereference: `x`, `x.f`, `x[i]` where `x` is an array — all
require `x` to be `mut`. `*p`, `p[i]` and `p.f` where `p` is a *pointer* are
always assignable, because v0 has no `const` and a pointer says nothing about
what it points at. Assigning to a non-`mut` binding names it and points at
where it was declared.

Compound assignment `x op= y` is `x = x op y`: the same lvalue and mutability
rules, `x`'s type as the context for `y`, and the result must be assignable
back to `x` without narrowing. Array assignment as a whole is forbidden;
struct assignment is a memcpy and is allowed.

### 7.6 Return reachability

A non-`void` function must not be able to fall off its end.

```
diverges(stmt):
    return            -> true
    block             -> any statement in it diverges
    if                -> has an else, and both branches diverge
    match             -> has a `_` arm, and every arm diverges
    while             -> false
    break, continue   -> true (for the enclosing block's fallthrough only)
```

The `match` rule is from `GRAMMAR.md` section 6 and it earns its place:
without it, a function whose every arm returns would still demand a trailing
unreachable `return`. `while` is `false` even for `while true` — v0 does not
reason about conditions, and the cost is one unreachable `return` in a loop
that never exits.

---

## 8. What this needs from `ast.h`

`ast.h` is a contract (`COLLABORATION.md` section 4): adding is fine,
changing is not. Sema needs two additions, both purely additive, in their own
commit, before any sema code lands:

```c
struct Expr {
    ExprKind kind;
    SrcPos pos;
    Ty *sem_type;      /* filled by sema; NULL until then. Forward-declared. */
    union { ... } u;
};
```

and one new expression kind for the conversions pass 4 inserts
(`GRAMMAR.md` section 7: *inserting explicit conversion nodes*):

```c
EXPR_CONV      /* u.conv.operand, target type in sem_type. Never parsed. */
```

Every implicit conversion the rules above allow becomes an `EXPR_CONV` in the
tree, so the backend never has to re-derive one.

*Implemented differently:* an explicit `.as(T)` stays an `EXPR_CAST` instead
of being rewritten into an `EXPR_CONV`. Rewriting would change the shape of a
node the parser produced, which section 13 says sema does not do, and it
saves the backend one `case` at the price of a dump that no longer shows what
the source actually said.

Anything that walks the AST must tolerate `EXPR_CONV` after this lands —
which today means `ast_dump.c`, and it is in the same area.

---

## 9. Diagnostics

Reuse `support/diag`: same `file:line:col: error: ` format the lexer and
parser already print, the same cap on how many are printed, the same exact
count. One `Diag` for the whole run.

Sema must **not** stop at the first error. On a type error, give the
expression a poison type that is assignable to everything and produces no
further diagnostics, so one mistake yields one message instead of a cascade.

Wording rules: name the thing, quote it, and say what was expected rather
than what was found where that is shorter. Where a second position matters —
the original declaration of a duplicate, the declaration of a non-`mut`
binding — print it as a second `note:` line.

A first catalogue, to keep the wording consistent:

```
duplicate declaration of 'lex_next'          (note: first declared at ...)
unknown type 'Lexr'
struct 'A' contains itself; use a pointer
struct 'Token' cannot be passed by value; pass a pointer instead
'void' is not a value type
undeclared name 'buf'
'malloc' is a function; v0 has no function pointers
cannot assign to this expression
'i' is not mutable; declare it with 'let mut'    (note: declared at ...)
condition must have type b8, b32 or b64, not i32; write 'n != 0'
cannot convert u8 to i64 implicitly; use '.as(i64)'
integer literal 4294967295 does not fit in i32
'%' is not defined for f64
argument 2 of 'printf': cannot convert Arena* to u8*
too few arguments to 'arena_alloc': expected 2, got 1
'sizeof' needs a sized type
enum value 300 does not fit in u8
'break' outside a loop
missing return in a function returning i32
```

---

## 10. Order of work

Each milestone builds clean under `-Wall -Wextra`, is ASan+UBSan clean, and
leaves `demo.slop` working — `COLLABORATION.md` section 7 applies to every
one of them, not just the last.

- **M0 — `support/map`.** Separate area, separate claim, separate branch.
  Open-addressing `StrView` → `void*`. Done when it has its own smoke test.
- **M1 — `types.c`.** The universe, interning, core singletons, layout for
  pointers and arrays. No AST involvement yet. Done when a hand-built
  `struct` gets the same offsets as the equivalent C struct.
- **M2 — passes 2 and 3.** Global table, struct layout, cycle detection,
  signatures, globals. Done when `demo.slop` produces zero diagnostics and
  every duplicate/unknown-type/self-recursion case produces exactly one.
- **M3 — pass 4, expressions.** `check_expr` with context typing,
  assignability, the operator table. Done when every expression in
  `demo.slop` gets a type and the untyped-literal cases in 7.1 come out
  right.
- **M4 — pass 4, statements.** Scopes, lvalues, mutability, conditions,
  match, break/continue, return reachability. Done when `demo.slop` checks
  clean end to end.
- **M5 — conversion nodes.** `EXPR_CONV` insertion, `ast_dump` support. Done
  when a dump of `demo.slop` shows a conversion at every implicit widening.

M1–M2 are the ones with real design risk. M3 is the bulk of the code.

**Status: M0–M5 done.** M5 fell out of M3 rather than following it --
`coerce()` is the only place an implicit conversion can be introduced, so
inserting the node there covered every case at once. `demo.slop` checks clean
with six conversions in it, every one either an enum member reaching an `i32`
field or a vararg promotion, which is exactly what rules this strict predict.

---

## 11. How this gets tested

There is no test suite, and `examples/demo.slop` "only proves the front end
does not crash — nothing asserts the tree is *correct*"
(`COLLABORATION.md` section 8). For sema that gap is worse than it was for
the parser: a wrong type rule produces a program that compiles and then
misbehaves at runtime.

Two things are needed, and neither belongs on this branch
(`COLLABORATION.md`: *do not bolt tests onto a pass branch*):

1. **A conformance corpus.** `demo.slop` checking clean is necessary and not
   sufficient. It exercises every construct but asserts nothing about the
   types sema assigns. A dump of `expr -> type` compared against a checked-in
   expectation would.
2. **A rejection corpus.** One file per diagnostic in section 9, each
   expected to produce exactly that message at exactly that position. This is
   the part that catches a rule that is accidentally too permissive, which is
   the failure mode that matters.

Until a harness exists, every milestone is verified by hand against
`demo.slop` plus a scratch corpus, and that limitation gets recorded in the
merge request rather than quietly skipped.

---

## 12. Decisions this document makes that `GRAMMAR.md` does not

Ordered by how much it would hurt to get them wrong. The first three want a
`GRAMMAR.md` amendment — a branch that touches only that file, per
`COLLABORATION.md` section 4.

1. **Enum → its underlying integer is implicit.** Not stated anywhere, but
   `demo.slop` requires it: `n.kind = N_INT` assigns a `NodeKind` to an `i32`
   field, and the whole common-prefix trick is built on that. The reverse
   (integer → enum) needs `.as(T)`. **Wants an amendment.**
2. **Comparisons and `true`/`false` are untyped bools that adopt context and
   default to `b8`.** `GRAMMAR.md` says conditions must have type `bN` but
   never says which one a comparison produces. Making it untyped means
   `let b32 r = a < b;` works with no widening rule, using machinery that
   already exists for integers. **Wants an amendment.**
3. **Char literals are untyped integers, defaulting to `u8`.** `GRAMMAR.md`
   makes integer literals untyped and is silent about `char_lit`, but
   `c >= '0'` against a `u8` only works if they behave the same. `u8` rather
   than `i32` as the default because they are written as bytes.
   **Wants an amendment.**
4. **The "no `mut` needs an initializer" rule covers globals**, not just
   locals. `GRAMMAR.md` says "variable" and lists globals and locals together
   one sentence earlier, so this is the consistent reading — but it is a
   reading, and it rejects `let i32 x;` at file scope.
5. **Two namespaces**, types and values, as in C.
6. **Shadowing** is allowed inward, forbidden within one scope.
7. **Mutability stops at a pointer dereference** — `*p = x` is always legal,
   because v0 has no `const`.
8. **Unary `-` on an unsigned type is an error.** It is nearly always a bug,
   and `.as(i64)` says what was meant.
9. **`extern` ABIs other than `"c"` are rejected**, rather than accepted and
   ignored.
10. **Zero-length arrays are an error**; array assignment as a whole is
   forbidden; `%` on floats is an error.
11. **Duplicate `match` labels are not checked** in v0, consistent with
    duplicate enum values being explicitly legal and unchecked.
12. **Array parameters and array return types are rejected**, alongside the
    structs `GRAMMAR.md` already rejects. C decays an array parameter to a
    pointer; v0 has no decay rule, and inventing one inside sema would be
    inventing language.
13. **A local is declared after its own initializer is checked**, so
    `let i32 x = x;` names the outer `x` rather than reading itself. C does
    the opposite, and it is a known trap.
14. **`&x` is allowed on an immutable binding.** Combined with decision 7 --
    mutability stops at a dereference -- that is a hole: the pointer can be
    written through. Closing it needs `const`, which v0 does not have, so it
    is recorded rather than patched over.

---

## 13. Invariants to hold on to

- Sema never rewrites the shape of the tree. It annotates (`sem_type`) and
  inserts (`EXPR_CONV`). Nothing else.
- A tree that produced errors is never handed to the backend. `sema_check`
  returning non-zero means stop.
- The parser leaves NULL children behind wherever it recovered, so **every**
  sema walk tolerates a NULL child — but only ever after `parse_program`
  reported errors, in which case sema is not running at all. Assert rather
  than branch, once, at the entry point.
- Nothing in sema knows the target's word size except `types.c`.
