cmake_minimum_required(VERSION 3.16)
project(wrenjit C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(WREN_JIT "Enable tracing JIT compiler" ON)

# ---------- Wren VM sources ----------
set(WREN_DIR ${CMAKE_SOURCE_DIR}/vendor/wren)
set(WREN_VM_SOURCES
    ${WREN_DIR}/src/vm/wren_compiler.c
    ${WREN_DIR}/src/vm/wren_core.c
    ${WREN_DIR}/src/vm/wren_debug.c
    ${WREN_DIR}/src/vm/wren_primitive.c
    ${WREN_DIR}/src/vm/wren_utils.c
    ${WREN_DIR}/src/vm/wren_value.c
    ${WREN_DIR}/src/vm/wren_vm.c
    ${WREN_DIR}/src/optional/wren_opt_meta.c
    ${WREN_DIR}/src/optional/wren_opt_random.c
)

set(WREN_INCLUDE_DIRS
    ${WREN_DIR}/src/include
    ${WREN_DIR}/src/vm
    ${WREN_DIR}/src/optional
)

# ---------- JIT sources (conditional) ----------
set(JIT_SOURCES "")
set(JIT_INCLUDE_DIRS "")

if(WREN_JIT)
    set(SLJIT_DIR ${CMAKE_SOURCE_DIR}/vendor/sljit)
    set(JIT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_ir.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_snapshot.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_trace.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_opt.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_regalloc.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_codegen.c
        ${CMAKE_SOURCE_DIR}/src/jit/wren_jit_memory.c
        ${SLJIT_DIR}/sljit_src/sljitLir.c
    )
    set(JIT_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/src/jit
        ${SLJIT_DIR}/sljit_src
    )
endif()

# ---------- VM library ----------
add_library(wrenjit_vm STATIC
    ${WREN_VM_SOURCES}
    ${JIT_SOURCES}
)

target_include_directories(wrenjit_vm PUBLIC
    ${WREN_INCLUDE_DIRS}
    ${JIT_INCLUDE_DIRS}
)

if(WREN_JIT)
    target_compile_definitions(wrenjit_vm PUBLIC WREN_JIT=1)
    # SLJIT needs this to compile as a single translation unit
    target_compile_definitions(wrenjit_vm PRIVATE SLJIT_CONFIG_AUTO=1)
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(wrenjit_vm PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# Platform libs for JIT executable memory
if(WREN_JIT)
    if(UNIX AND NOT APPLE)
        target_link_libraries(wrenjit_vm PRIVATE pthread)
    endif()
    if(APPLE)
        target_link_libraries(wrenjit_vm PRIVATE pthread)
    endif()
endif()

# Math library on non-MSVC
if(NOT MSVC)
    target_link_libraries(wrenjit_vm PRIVATE m)
endif()

# ---------- CLI runner ----------
add_executable(wrenjit_cli bench/runner.c)
target_link_libraries(wrenjit_cli PRIVATE wrenjit_vm)

# ---------- C equivalent benchmarks ----------
add_executable(bench_sum_c bench/c_equivalents/sum.c)
if(NOT MSVC)
    target_compile_options(bench_sum_c PRIVATE -O3)
endif()

add_executable(bench_fib_c bench/c_equivalents/fib.c)
if(NOT MSVC)
    target_compile_options(bench_fib_c PRIVATE -O3)
endif()

# ---------- Tests ----------
enable_testing()

# Run upstream Wren test suite (if test runner exists)
if(EXISTS ${WREN_DIR}/test)
    add_test(NAME wren_upstream_tests
        COMMAND ${CMAKE_COMMAND} -E echo "TODO: integrate upstream Wren tests"
    )
endif()

# JIT test targets
if(WREN_JIT)
    add_executable(test_ir test/test_ir.c)
    target_link_libraries(test_ir PRIVATE wrenjit_vm)
    add_test(NAME test_ir COMMAND test_ir)

    add_executable(test_jit test/test_jit.c)
    target_link_libraries(test_jit PRIVATE wrenjit_vm)
    add_test(NAME test_jit COMMAND test_jit)
endif()
