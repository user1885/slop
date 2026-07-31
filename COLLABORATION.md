# Working with the other agent

Two Claude Code sessions work on this repo. They never see each other's
conversation. **The git remote is the only channel between them** — if a fact
is not in a commit, a branch name, or this file, the other agent does not know
it.

Everything below exists to make that one limitation survivable.

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

Ownership is by **compiler pass**, following GRAMMAR.md section 7. Touch only
the files in the area you claimed.

| Area      | Files                          | Status | Depends on |
|-----------|--------------------------------|--------|------------|
| `lexer`   | `lexer.c`, `lexer.h`           | done   | —          |
| `parser`  | `parser.c`, `parser.h`, `ast.h`| open   | `lexer.h`  |
| `sema`    | `sema.c`, `sema.h`, `types.c`, `types.h` | open | `ast.h` |
| `backend` | `ir.c`, `ir.h`, `codegen_*.c`  | open   | sema       |
| `support` | `arena.c`, `arena.h`, `diag.c`, `diag.h` | open | — |
| `driver`  | `main.c`                       | shared | everything |

An area with no claim branch is free. Two agents in two different areas can
work in parallel indefinitely and never conflict — that is the point of the
split.

`driver` is shared on purpose: whoever finishes a pass wires it into `main.c`
as part of that pass's branch. Keep `main.c` thin so those edits stay small.

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
  gcc -std=c99 -Wall -Wextra -g -fsanitize=address,undefined -o /tmp/slop-asan *.c
  /tmp/slop-asan examples/demo.slop
  ```
- `./build/slop examples/demo.slop` still works. That file exercises every v0
  construct and is the closest thing to a regression test until there is a
  test suite.
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
- **Parser, sema, backend — not started.** Next up is `parser` (GRAMMAR.md
  sections 2–5), which needs `ast.h` designed around the common-prefix trick
  described in section 6.
- **No test suite yet.** Whoever needs one first should claim `support` and
  build it; do not bolt tests onto a pass branch.
- **Build:** CMake, C99, warnings on. `./build/slop <file.slop>` currently
  dumps tokens.
