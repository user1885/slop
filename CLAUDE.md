# slop

A small self-hosting language. `GRAMMAR.md` is the specification — read it
before writing anything, and treat it as authoritative over the code.

## Another agent is working in this repo

A second Claude Code session works on this project in parallel and cannot see
your conversation. **Read `COLLABORATION.md` before you touch a file.** It is
short and it is not optional:

- `main` is integration-only — never commit to it directly, never force-push it.
- Claim an area by pushing a branch **before** writing code
  (`git ls-remote --heads origin` shows what is taken).
- Stay inside the area you claimed. Ownership is per compiler pass.

## Build and check

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build                 # golden tests; also ./tests/run.sh
./build/slop examples/demo.slop        # dumps the AST; exercises all of v0
./build/slop --tokens examples/demo.slop
```

Sources live in `src/<area>/`, one directory per compiler pass, and includes
are spelled from `src/` down (`#include "lexer/lexer.h"`).

The golden tests pin the token and AST dumps, so they fail on any change to
how a program parses. That is the point. If the change was intended, run
`./tests/run.sh --update` and read the diff before committing it.

C99, no extensions — this compiler has to be written in slop eventually, so
keep the C boring. Warnings are on (`-Wall -Wextra`) and must stay silent.
Format with the project `.clang-format`.
