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
./build/slop examples/demo.slop        # dumps tokens; exercises all of v0
```

C99, no extensions — this compiler has to be written in slop eventually, so
keep the C boring. Warnings are on (`-Wall -Wextra`) and must stay silent.
Format with the project `.clang-format`.
