# slop v0 — grammar

Goal: the smallest language that can host a slop compiler written in slop.
Declaration order at top level does not matter (see "Passes").

Items marked `[+]` were not in the original sketch. Each one is removable with
a single edit if you want to cut v0 further.

---

## 1. Lexical

```ebnf
whitespace   = [ \t\r\n]+                (* skipped *)
line_comment = "#" [^\n]*                (* skipped *)

identifier = [A-Za-z_][A-Za-z0-9_]*      (* and not a keyword *)

int_lit    = dec_lit | hex_lit
dec_lit    = [0-9]+
hex_lit    = "0x" [0-9A-Fa-f]+           (* [+] *)
float_lit  = [0-9]+ "." [0-9]+
char_lit   = "'" ( escape | [^'\\\n] ) "'"
string_lit = '"' ( escape | [^"\\\n] )* '"'
escape     = "\\" ( "n" | "r" | "t" | "0" | "\\" | "'" | '"' )
           | "\\x" hex_digit hex_digit   (* [+] *)
bool_lit   = "true" | "false"
null_lit   = "null"

literal = int_lit | float_lit | char_lit | string_lit | bool_lit | null_lit

keyword   = "let" | "fn" | "if" | "else" | "while" | "break" | "continue"
          | "return" | "struct" | "enum" | "extern" | "mut"
          | "as" | "sizeof" | "match"
          | "true" | "false" | "null" | core_type

core_type = "i8" | "u8" | "i32" | "u32" | "i64" | "u64"
          | "f32" | "f64" | "b8" | "b32" | "b64" | "void"

operator  = "+" | "-" | "*" | "/" | "%" | "&&" | "||" | "!"
          | "&" | "|" | "^" | "~" | "<<" | ">>"
          | "<" | "<=" | ">" | ">=" | "==" | "!="
          | "=" | "+=" | "-=" | "*=" | "/=" | "%="        (* [+] *)
          | "&=" | "|=" | "^=" | "<<=" | ">>="            (* [+] *)

punct     = "(" | ")" | "{" | "}" | "[" | "]"
          | "." | "," | ";" | ":" | "->" | "=>" | "..."
```

The lexer uses maximal munch. Probe order matters:
`<<=` before `<<` before `<=` before `<`, `->` before `-`,
and both `==` and `=>` before `=`.

---

## 2. Types

```ebnf
Type        = PrimaryType TypeSuffix*
TypeSuffix  = "*" | "[" int_lit "]"
PrimaryType = core_type | identifier
```

Suffixes apply **left to right**:

| syntax     | meaning                       |
|------------|------    -------------------------|
| `i32*`     | pointer to i32                |
| `i32[10]`  | array of 10 i32               |
| `i32*[10]` | array of 10 pointers to i32   |
| `i32[10]*` | pointer to array of 10 i32    |

---

## 3. Top level

```ebnf
Program = Item*
Item    = FnDecl | StructDecl | EnumDecl | GlobalDecl | ExternBlock

FnDecl    = "fn" identifier "(" ParamList? ")" ( "->" Type )? Block
ParamList = Param ( "," Param )*
Param     = "mut"? Type identifier

StructDecl = "struct" identifier "{" Field* "}"
Field      = Type identifier ";"

EnumDecl = "enum" identifier ( ":" core_type )? "{" EnumBody? "}"
EnumBody = EnumItem ( "," EnumItem )* ","?
EnumItem = identifier "=" int_lit ?

GlobalDecl = "let" "mut"? Type identifier ( "=" literal )? ";"

ExternBlock  = "extern" string_lit "{" ExternFnDecl* "}"
ExternFnDecl = "fn" identifier "(" ExternParams? ")" ( "->" Type )? ";"
ExternParams = ParamList ( "," "..." )? | "..."
```

All five kinds of Item start with their own keyword, so the top level is
single-token dispatch.

Enum members are numbered from 0. An explicit `= int_lit` overrides a member's
value, and later implicit members continue from it:

```
enum NodeKind {        # N_INT = 0, N_VAR = 1, N_BIN = 2
    N_INT,
    N_VAR,
    N_BIN,
}

enum aboba : u8 {      # lol = 1, lol2 = 2
    lol = 1,
    lol2 = 2,
}

enum Mixed {           # A = 0, B = 5, C = 6
    A,
    B = 5,
    C,
}
```

A value must fit the underlying type (`i32` when no `: core_type` is given).
Values cannot be negative — `int_lit` carries no sign. Duplicates are legal and
unchecked in v0; that only becomes a problem once `match` gains exhaustiveness
checking.

Global initializers are restricted to `literal`, and `EnumItem` values to
`int_lit`, so v0 needs no constant-expression evaluator.

---

## 4. Statements

```ebnf
Block = "{" Statement* "}"

Statement = Block
          | VarDecl
          | IfStmt
          | WhileStmt
          | MatchStmt                    (* [+] *)
          | ReturnStmt
          | "break" ";"
          | "continue" ";"
          | ExprStmt
          | ";"

VarDecl    = "let" "mut"? Type identifier ( "=" Expr )? ";"

IfStmt     = "if" Expr Block ( "else" ( IfStmt | Block ) )?
WhileStmt  = "while" Expr Block
ReturnStmt = "return" Expr? ";"

MatchStmt  = "match" Expr "{" MatchArm* "}"                    (* [+] *)
MatchArm   = MatchLabel ( "," MatchLabel )* "=>" Statement
MatchLabel = int_lit | char_lit | identifier | "_"

ExprStmt   = Expr ( AssignOp Expr )? ";"
AssignOp   = "=" | "+=" | "-=" | "*=" | "/=" | "%="
           | "&=" | "|=" | "^=" | "<<=" | ">>="
```

`ExprStmt` covers both assignment and a bare expression: parse a full `Expr`,
then look at the next token. No backtracking. If it is an `AssignOp`, the left
side must be an lvalue — but that is checked in sema by inspecting the tree
shape (`identifier`, `*e`, `e[i]`, `e.f`), not by the grammar. That way
`a + b = 5` produces a real diagnostic instead of "unexpected token".

An arm body is a single `Statement`, so `TOK_PLUS => return lhs + rhs;` works
for one-liners and `TOK_INT => { ... }` for anything longer. Block scoping
therefore applies to arms with no extra rule. **There is no fallthrough**;
`break` inside a `match` belongs to the enclosing loop.

`_` is the wildcard arm. It is not a keyword — it already lexes as an ordinary
identifier, and the parser only treats it specially in label position, so `_`
stays usable as a variable name elsewhere.

v0 does **not** check exhaustiveness: `_` is optional, and a value matching no
arm does nothing. Real exhaustiveness needs the checker to know an enum's full
member set and that the scrutinee has that enum type — a v1 concern.

---

## 5. Expressions

Loosest to tightest:

```ebnf
Expr           = LogicalOr
LogicalOr      = LogicalAnd     ( "||" LogicalAnd )*
LogicalAnd     = Equality       ( "&&" Equality )*
Equality       = Relational     ( ( "==" | "!=" ) Relational )*
Relational     = BitOr          ( ( "<" | "<=" | ">" | ">=" ) BitOr )*
BitOr          = BitXor         ( "|"  BitXor )*
BitXor         = BitAnd         ( "^"  BitAnd )*
BitAnd         = Shift          ( "&"  Shift )*
Shift          = Additive       ( ( "<<" | ">>" ) Additive )*
Additive       = Multiplicative ( ( "+" | "-" ) Multiplicative )*
Multiplicative = Unary          ( ( "*" | "/" | "%" ) Unary )*

Unary          = ( "-" | "!" | "~" | "*" | "&" ) Unary
               | "sizeof" "(" Type ")"
               | Postfix

Postfix        = Primary PostfixOp*
PostfixOp      = "(" ArgList? ")"
               | "[" Expr "]"
               | "." identifier
               | "." "as" "(" Type ")"
ArgList        = Expr ( "," Expr )*

Primary        = literal | identifier | "(" Expr ")"
```

Bitwise operators bind **tighter** than comparisons — unlike C, where
`a & b == c` means `a & (b == c)`. Here it means `(a & b) == c`.

`&&` and `||` short-circuit.

**Casts are postfix**: `x.as(T)`, with the target type delimited by its own
parentheses. There is no infix cast operator and no precedence level for
casting — `.as(T)` is a postfix operator alongside `.field`, `[i]` and calls,
and binds as tightly as they do:

```
n.as(BinNode*).lhs           # chains, no wrapping parens
x.as(i64) * 2                # unambiguous
(lhs < rhs).as(i64)          # parens only around a binary operand
-x.as(i32)                   # means -(x as i32)
```

Bracketing the type is what keeps the grammar unambiguous. An infix
`Unary ( "as" Type )*` cannot work alongside postfix `TypeSuffix`, because
`x as i32 * * y` then has two valid parses — `(x as i32) * (*y)` and
`(x as i32*) * y` — and an ambiguous grammar is neither LL(k) nor LR(k) for
any k. With the type in parentheses, `TypeSuffix*` always terminates at `)`.

---

## 6. Semantics to pin down before codegen

**Struct layout.** Fields are laid out in declaration order with C-compatible
alignment and no reordering. This is a guarantee, not an implementation
detail: the common-prefix trick for AST nodes depends on it.

```
struct Expr    { u32 kind; }
struct BinExpr { u32 kind; Expr* lhs; Expr* rhs; u32 op; }

let BinExpr* b = e.as(BinExpr*);   # legal when b.kind == EXPR_BIN
```

**Dot auto-derefs.** `p.field` works for both `T*` and `T`; one level of
pointer is stripped automatically. There is no `->`.

**Pointer arithmetic** scales by `sizeof(T)`: `p + 1` advances one element,
not one byte. `p - q` yields `i64` in elements. Use `.as(u8*)` for byte
arithmetic.

**Conditions** in `if`/`while` must have type `bN`. No "nonzero is true":
write `p != null`, `n != 0`.

**Return reachability.** A `match` counts as diverging when it has a `_` arm
and every arm diverges. Without that rule, a function ending in a `match`
whose arms all `return` would still demand a trailing unreachable `return`.

**Integer literals** are untyped and adopt the context type when the value
fits; with no context they default to `i32`. Widening within the same
signedness is implicit; everything else — narrowing, signedness changes,
int↔float, any pointer conversion — needs an explicit `.as(T)`.

`null` is assignable to a pointer of any type.

**Initialization.** Anything declared without an initializer is zeroed —
globals and locals alike, including structs and arrays. A variable without
`mut` must have an initializer (a sema error, not a parse error).

**Structs by value** are allowed as locals, globals, fields, and in assignment
(that is a memcpy). They are **forbidden as parameter types and return
types** — pass a pointer instead. This removes SysV argument classification,
the nastiest part of writing your own backend. Lift the restriction in v1.

**Enum members** land in the global namespace as constants of their enum type
and are written unqualified: `TOK_PLUS`, not `TokenKind.TOK_PLUS`. The default
underlying type is `i32`.

**String literals** have type `u8*`, point into static data, and are
NUL-terminated.

---

## 7. Passes

Order independence at top level and multi-file support both fall out of one
thing: collecting names is separated from checking bodies.

**0. Lexer** — every file in argv to a token stream.

**1. Parser** — one AST per file. It knows nothing about types: `let` made
declarations unambiguous, so no pre-scan for type names is needed.

**2. Global name collection** — walk *only* the top level of every file and
fill one shared table: struct names (fields not yet resolved), enums and their
members, function names and signatures, globals, extern functions. Duplicates
are caught here. After this pass, everything declared anywhere in the program
is visible.

**3. Type resolution** — struct fields (which may reference structs declared
later or in another file), sizes, alignments and field offsets, and the types
in function signatures. Recursion by value (`struct A { B b; } struct B { A a; }`)
is an error; recursion through a pointer is fine. Mark a struct "in progress"
while resolving it to tell the two apart.

**4. Body checking** — one function at a time, local scopes only, against the
now-complete global table. Function order is irrelevant by this point. Also
here: lvalue checks, inserting explicit conversion nodes, and `return`
reachability.

**5. Lowering to IR**, then backends.

Passes 2–4 are three straightforward walks of a couple hundred lines each.
A single pass genuinely cannot work: a function body may call a function
declared 500 lines below it, and a struct field may reference a struct from
another file.

---

## 8. Not in v0

Generics, closures, interfaces/traits, function pointers, namespaced modules,
`for`, the ternary operator, `union`, `defer`, overloading, type inference
beyond literals, struct initializers `{...}`, `sizeof(expression)`, constant
expressions outside literals.

None of it is needed for stage1 to compile itself.