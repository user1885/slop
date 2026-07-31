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
        src/sema/types.c src/sema/types.h)

target_include_directories(slop PRIVATE src)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slop PRIVATE -Wall -Wextra)
endif ()
