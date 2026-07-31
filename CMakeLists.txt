cmake_minimum_required(VERSION 4.4)
project(slop C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(slop
        main.c
        lexer.c lexer.h
        arena.c arena.h
        ast.c ast.h
        parser.c parser.h)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slop PRIVATE -Wall -Wextra)
endif ()
