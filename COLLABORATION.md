# Working with the other agent

Two Claude Code sessions work on this repo. They never see each other's
conversation. **The git remote is the only channel between them** — if a fact
is not in a commit, a branch name, or this file, the other agent does not know
it.

Everything below exists to make that one limitation survivable.

**Both sessions may share a single checkout.** That has been observed: one
agent ran `git switch` in `/home/sq/projects/slop` and changed the branch
under the other mid-task. The remote is the coordination channel, but the
*working directory* can be shared, and git gives you no warning when it is.

So: **do not `git switch` the shared checkout.** Give your area its own
working directory instead, which is what `git worktree` is for:

```bash
git worktree add ../slop-parser -b parser/expressions main
cd ../slop-parser        # your tree; the shared checkout is untouched
```

Two worktrees on one clone share the same branches and remotes, so claims and
merges work exactly as described below — but neither agent can yank the other's
files. When you are done: `git worktree remove ../slop-parser`.

If you must work in the shared checkout, run `git status` first, and treat any
branch you did not check out yourself as the other agent's territory.

---

## 1. The rule

`main` is integration-only. **Never commit directly to `main`, never
force-push it, never rewrite a commit that is already on the remote.**

All work happens on a branch, and a branch reaches `main` only through the
merge step in section 5.

---

## 2. Claiming work before you write any code

A branch push is atomic on the remote: two agents cannot both create the same
branch, and the loser gets a rejection instead of a silent overwrite. That
makes pushing the lock.

```bash
git fetch origin
git ls-remote --heads origin          # what is already claimed
```

If nothing covers the area you want:

```bash
git switch -c parser/expressions main
git commit --allow-empty -m "claim: parser — expression parsing"
git push -u origin parser/expressions   # this push IS the claim
```

Do this **first**, before writing code. A claim you push after two hours of
work is not a claim, it is a collision you have already lost.

If the push is rejected, or `ls-remote` already shows a branch in your area,
the other agent has it. Go to section 6.

Branch names are `<area>/<topic>`, where `<area>` is a row in the table below.
The area prefix is what makes a claim legible to the other agent at a glance.

---

## 3. Areas and who owns which files

Ownership is by **compiler pass**, following GRAMMAR.md section 7, and each
area is a directory under `src/`. Touch only the files in the area you
claimed.

| Area      | Directory      | Files                                    | Status | Depends on |
|-----------|----------------|------------------------------------------|--------|------------|
| `lexer`   | `src/lexer/`   | `lexer.c`, `lexer.h`                     | done   | —          |
| `parser`  | `src/parser/`  | `parser.c`, `parser.h`                   | done   | `lexer.h`  |
|           | `src/ast/`     | `ast.c`, `ast.h`, `ast_dump.c/.h`        | done   |            |
| `sema`    | `src/sema/`    | `sema.c`, `sema.h`, `types.c`, `types.h` | **open — next** | `ast.h` |
| `ir`      | `src/ir/`      | `ir.c`, `ir.h`                           | done   | —          |
| `backend` | `src/backend/` | `backend.c/.h`, `backend_<target>.c`     | llvm + c done; lowering open | `ir.h`, sema |
| `support` | `src/support/` | `arena`, `vec`, `diag`, `strview`, `srcpos` | in place, area open | — |
| `driver`  | `src/driver/`  | `main.c`, `source_file.c/.h`, `token_dump.c/.h` | shared | everything |

An area with no claim branch is free. Two agents in two different areas can
work in parallel indefinitely and never conflict — that is the point of the
split. One directory per area is what makes that visible: if a diff touches
two directories, it touches two areas.

Includes are spelled from `src/` down (`#include "lexer/lexer.h"`), so the
area a header belongs to is legible at every use site.

`driver` is shared on purpose: whoever finishes a pass wires it into `main.c`
as part of that pass's branch. Keep `main.c` thin so those edits stay small —
anything bigger than wiring goes in its own file next to it.

---

## 4. The three files that cause conflicts

These are outside the ownership table because both agents need them.

**`GRAMMAR.md` is the specification, not scratch paper.** The code follows it;
it does not follow the code. If implementing something reveals a genuine
problem with the grammar, put the fix in a branch that changes *only*
`GRAMMAR.md`, and say in the commit message which code depends on the answer.
Never change the language and its implementation in one commit — the other
agent cannot review that.

**Headers are contracts.** `lexer.h` is consumed by the parser, `ast.h` will be
consumed by sema. Adding to a header is fine. *Changing or removing* an
existing declaration breaks the other agent's working tree, so it needs its own
commit that updates every call site, and a note in the merge request.

**`CMakeLists.txt`** — add your sources on their own line in `add_executable`.
Both agents will edit this file; keeping edits to one line each keeps the
conflict trivial.

---

## 5. Getting a branch into `main`

Rebase onto current `main` first, so the merge is a fast, reviewable diff:

```bash
git fetch origin
git rebase origin/main          # only ever rebase YOUR OWN unmerged branch
cmake --build build && ./build/slop examples/demo.slop   # still green?
git push --force-with-lease origin parser/expressions    # your branch only
```

**With `gh` installed** (`sudo pacman -S github-cli && gh auth login`) — this
is the preferred path, because it gives the other agent something to review:

```bash
gh pr create --fill
```

Leave the PR open. The other agent reviews it with `gh pr diff <n>` and merges
with `gh pr merge <n> --squash --delete-branch`. Do not merge your own PR if
the other agent is active — a review neither of you can perform is not a
review, but neither is a rubber stamp.

**Without `gh`**, ask the user to open the PR, or merge explicitly:

```bash
git switch main && git pull --ff-only
git merge --no-ff parser/expressions
git push origin main
git push origin --delete parser/expressions   # release the claim
```

Delete the branch after merging either way. A stale claim branch blocks an
area nobody is working on.

---

## 6. When you collide anyway

**The area you want is claimed.** Pick another area from the table. Do not
work in it "just a little" — the other agent's uncommitted tree is invisible to
you, and you will both write the same file.

**You need a change inside someone else's area.** Do not make it. Note the
requirement in your commit message or PR description and code against the
interface you need. If you are blocked without it, tell the user — they are
the only synchronous channel between the two sessions.

**Someone else's commits appeared in your area.** Stop and tell the user before
touching anything. Do not revert, rebase, or "fix" the other agent's work to
match yours; a repo where both agents rewrite each other's history is
unrecoverable.

---

## 7. Definition of done

Every branch must satisfy all of these before it is proposed for merge:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

- Builds clean under `-Wall -Wextra` — **no new warnings**, they are enabled in
  `CMakeLists.txt` for a reason.
- Clean under sanitizers on both a valid and a deliberately broken input:
  ```bash
  gcc -std=c99 -Wall -Wextra -g -fsanitize=address,undefined -Isrc -o /tmp/slop-asan src/*/*.c
  /tmp/slop-asan examples/demo.slop
  ```
- **The golden tests pass**: `ctest --test-dir build`, or `./tests/run.sh`.
  If your change alters the token or AST dump on purpose, regenerate with
  `./tests/run.sh --update` and **read the resulting diff before committing
  it** — an accepted golden nobody looked at proves only that the output did
  not change, never that it was right.
- `./build/slop examples/demo.slop` still works. That file exercises every v0
  construct in one program, which the focused test cases deliberately do not.
- Formatted with the project `.clang-format` (`clang-format -i *.c *.h`).
- C99, no compiler extensions. This compiler must eventually be written in
  slop, so keep the C boring.

Commit messages: imperative subject, then *why* rather than *what*. End with:

```
Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

---

## 8. State of the repo

Read this before planning, so you do not re-derive it.

- **Lexer — complete.** `lexer.h` / `lexer.c`. Token kinds come from three
  X-macro lists (`SLOP_SPECIAL_TOKENS`, `SLOP_KEYWORDS`, `SLOP_PUNCT`) so the
  enum, spelling tables and printable names cannot drift apart. The punct list
  is ordered longest-first, which is exactly the maximal-munch probe order.
  Escapes are decoded into a lexer-owned pool. Errors produce a `TOK_ERROR`
  token carrying a message and scanning continues.
  Entry points: `lexer_next()` for one token, `lexer_lex_all()` for an array.
  Verified: 3127 tokens / 0 errors on `examples/demo.slop`, 16 distinct error
  diagnostics, ASan + UBSan + LSan clean.
- **Support — arena, vec, diag, strview.** `src/support/`. The arena is a
  bump allocator with zeroed allocations that dies on OOM, and the AST lives
  in it. `vec` is the malloc-backed scratch array node lists are built in
  before being handed to the arena; `diag` owns the error counter, the
  report cap and the `file:line:col: error:` format; `strview`/`srcpos` are
  the two value types the AST and diagnostics share.
- **Parser — complete.** `src/parser/`, tree in `src/ast/` (`ast.c` is the
  node constructors, `ast_dump.c` the debug printer).
  Recursive descent over the whole of GRAMMAR.md sections 2–5, with
  panic-mode recovery at statement, member and item boundaries, so one run
  reports many errors, and `TOK_ERROR` is reported where it sits in the
  stream, so lexical and syntactic diagnostics stay in source order.
  Read the lifetime rules at the top of `parser.h`
  before consuming the tree: identifiers point into the source buffer,
  string literals are copied into the arena, and **a child may be NULL
  wherever the parser recovered** — check `*out_errors` before trusting the
  shape of anything.
  Verified on merge: parses all of `examples/demo.slop`; the two un-C-like
  precedence rules are right (`a & b == c` is `(a & b) == c`, `.as(T)` is
  postfix); 17 distinct diagnostics with no hang or crash on deliberately
  broken input; ASan + UBSan clean.
- **Sema — not started.** GRAMMAR.md section 7 passes 2–4 (global name
  collection, type resolution, body checking) against the semantics pinned
  down in section 6. It consumes `ast/ast.h`, where enum member values are
  left unnumbered and global initializers left as literals, because numbering
  and constant evaluation are its job.
- **IR — complete.** `src/ir/`. An in-memory model of LLVM IR: same value
  model, same types, same instruction set, so the LLVM backend is a printer
  rather than a translation. Two rules a backend may rely on: **no phi
  nodes** — every local is an `alloca` written with `store` and read with
  `load`, which is what clang -O0 emits and `opt -mem2reg` promotes away —
  and **every value has an explicit name**, because LLVM's implicit `%0, %1`
  numbering counts unnamed blocks and so couples a printer to traversal
  order. `ir_verify()` checks a finished module (terminators, branch targets,
  operand types, call arity) and a backend is entitled to assume it passed.
- **Backends — llvm and c, both complete.** `src/backend/`. A backend is a
  name and `int emit(const IrModule *, FILE *)`; the set of them is a static
  table in `backend.c`, so adding one is a function, a table row and a
  CMakeLists line. A backend cannot see the AST, the tokens or sema's
  tables — that restriction is what keeps them swappable. The C backend
  exists to keep the interface honest: SSA-with-basic-blocks versus C
  declarations and statements is about as far apart as two textual targets
  get, and it also bootstraps slop anywhere a C compiler exists. Compile its
  output with `-fno-builtin` (see the comment in `backend_c.c`).
- **Lowering — complete.** `src/backend/lowering.c`, pass 5. Runs only after
  `sema_check` returns 0. Every expression's `sem_type` supplies the
  signedness of an operation, the width of a conversion and the element type
  of an index. **Declarations carry no resolved type** — sema annotates only
  `Expr` — so parameter, field and global types are converted from the syntax
  instead; the two agree because struct layout is a guarantee in GRAMMAR.md
  section 6, not an implementation detail. If sema ever annotates
  declarations, this is the duplication to delete.
  Things worth knowing before changing it: `&&`/`||` short-circuit through a
  slot rather than a phi; a comparison is `i1` in the IR but `bN` in slop, so
  it is zero-extended coming out and compared against zero going in; an
  untyped literal adopts its context width at every point the IR pairs two
  values; aggregates are handled by address throughout, and copying one is a
  `memcpy` call; a name that is not a local or a global is an enum member,
  whose value comes from the enum `Ty` because sema left the numbering there.
  The driver runs `ir_verify` before any backend sees the module.
- **The compiler works end to end, on both backends.**
  `slop --emit examples/demo.slop` produces LLVM IR that `llvm-as` accepts,
  `llc` turns into a native binary, `lli` runs, and `opt -O2` optimises
  without complaint; `slop --backend=c` produces C that gcc compiles
  warning-free under `-Wall -Wextra -fno-builtin`. Both programs print
  identical, correct output. `tests/run_e2e.sh` runs all of it and skips the
  LLVM half cleanly when no toolchain is installed.
  **Run both backends, not one.** They fail differently, and each has caught
  bugs the other hid. The C backend rounds every alloca up to a `uint64_t`,
  so it survived a slot written wider than it was allocated while the native
  LLVM build segfaulted. The LLVM printer happily emitted a whole-struct load
  into a `memcpy` argument, and only the C backend refused to write it.
  Neither bug was visible in review.
- **`examples/conformance.slop` is the language's own test.** 59 checks with
  known answers — precedence, signedness, conversions, short-circuiting,
  loops, `match`, struct layout and copying, arrays, pointer arithmetic, the
  common-prefix trick, strings — each printing its verdict, with the exit
  status as the result. Both backends run it and must print the same thing.
  Add a case here when you add a semantic, not just a parser case.
  One thing it pins that the grammar does not say out loud: division
  truncates toward zero while an arithmetic shift floors, so `-7 / 2` is -3
  and `-7 >> 1` is -4.
  Note `sizeof` sits at the unary level, so `sizeof(T).as(i64)` does not
  parse — `(sizeof(T)).as(i64)` does.
- **`ir_verify` owns the checks LLVM cannot make.** With opaque pointers LLVM
  accepts a store of any type through any pointer, so writing a slot wider
  than it was allocated is a type error nowhere and a stack overflow at run
  time. `verify_slot_store` catches it whenever the destination is visibly an
  alloca. When lowering grows a new way to write memory, this is the check to
  extend.
- **Tests — golden files, two suites, both under `ctest`.**
  `tests/run.sh` covers the front end: cases in `tests/cases/`, where a case
  named `lex_*` runs with `--tokens` and pins the token stream and anything
  else runs the parser and pins the shape of the AST, which is what freezes
  precedence, associativity and error recovery. Nine cases cover the
  precedence ladder, unary-vs-postfix binding, the type suffix table, every
  item and statement form, panic-mode recovery, maximal munch, literals and
  lexical errors.
  `tests/run_ir.sh` covers the IR and the backends from the hand-built module
  in `tests/ir_demo.c`: golden text for each backend, and — the check that
  actually proves something — the C backend's output compiled and run, which
  must exit 36.
  Both suites were validated by making them fail on purpose: a precedence bug
  injected into the parser turned two front-end cases red, and deleting a
  terminator made `ir_verify` reject the module. A test suite that has never
  failed has not been tested.
  **Sema will need cases of its own**: the front-end suite stops at the
  parser, and nothing in either suite asserts anything about names, types or
  lvalues.
- **Layout:** sources live in `src/<area>/`, one directory per pass, and
  includes are spelled from `src/` down (`#include "lexer/lexer.h"`).
- **Build:** CMake, C99, warnings on. `./build/slop <file.slop>` dumps the
  AST; `--tokens` stops after the lexer and dumps the token stream.
