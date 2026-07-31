cmake_minimum_required(VERSION 4.4)
project(slop C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# One directory per compiler pass, mirroring the areas in COLLABORATION.md.
# Includes are spelled from src/ down ("lexer/lexer.h"), so a header says
# which area it belongs to at every use site.
add_executable(slop
        src/driver/main.c
        src/driver/source_file.c src/driver/source_file.h
        src/driver/token_dump.c src/driver/token_dump.h
        src/support/srcpos.h
        src/support/strview.c src/support/strview.h
        src/support/arena.c src/support/arena.h
        src/support/vec.c src/support/vec.h
        src/support/map.c src/support/map.h
        src/support/diag.c src/support/diag.h
        src/lexer/lexer.c src/lexer/lexer.h
        src/ast/ast.c src/ast/ast.h
        src/ast/ast_dump.c src/ast/ast_dump.h
        src/parser/parser.c src/parser/parser.h
        src/ir/ir.c src/ir/ir.h
        src/backend/backend.c src/backend/backend.h
        src/backend/backend_llvm.c
        src/backend/backend_c.c)

target_include_directories(slop PRIVATE src)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slop PRIVATE -Wall -Wextra)
endif ()

# Builds an IR module by hand and emits it through a chosen backend. Until
# lowering exists this is the only exercise the IR and the backends get, so
# it is a test binary, not a sample.
add_executable(slop-ir-demo
        tests/ir_demo.c
        src/ir/ir.c
        src/backend/backend.c
        src/backend/backend_llvm.c
        src/backend/backend_c.c
        src/support/arena.c)

target_include_directories(slop-ir-demo PRIVATE src)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slop-ir-demo PRIVATE -Wall -Wextra)
endif ()

enable_testing()

# Golden-file tests over the built binary: `ctest --test-dir build`, or
# `./tests/run.sh` directly. See tests/run.sh for what they pin and how to
# accept a deliberate change to the output.
add_test(NAME front-end
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:slop>)

# Golden output for each backend, plus the check that matters most: the C
# backend's output is compiled and run, and must exit with the value the
# module computes.
add_test(NAME ir-backends
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ir.sh $<TARGET_FILE:slop-ir-demo>)
