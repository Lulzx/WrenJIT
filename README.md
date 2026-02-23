# WrenJIT

Tracing JIT compiler for the [Wren](https://wren.io) scripting language. Uses
[SLJIT](https://github.com/zherczeg/sljit) as the code generation backend.

## How it works

A hot loop counter is incremented on each `LOOP` bytecode. Once a threshold is
reached, the interpreter begins recording a trace. The trace is a linear
sequence of typed IR nodes representing one iteration of the loop. After
`LOOP_BACK` is encountered the trace is compiled to native code and installed
in a PC-keyed hash table. Subsequent executions of the same loop invoke the
compiled trace directly.

Speculative guards are emitted for type checks (`GUARD_NUM`, `GUARD_CLASS`).
When a guard fails the trace returns an exit index; the interpreter resumes at
the corresponding snapshot PC. Traces run until the loop condition guard fails,
at which point the interpreter takes over.

## IR

SSA-form IR with the following node types:

- Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `NEG`
- Comparison: `LT`, `GT`, `LTE`, `GTE`, `EQ`, `NEQ`
- Bitwise: `BAND`, `BOR`, `BXOR`, `BNOT`, `LSHIFT`, `RSHIFT`
- Memory: `LOAD_STACK`, `STORE_STACK`, `LOAD_FIELD`, `STORE_FIELD`, `LOAD_MODULE_VAR`, `STORE_MODULE_VAR`
- NaN-boxing: `BOX_NUM`, `UNBOX_NUM`, `BOX_OBJ`, `UNBOX_OBJ`, `BOX_BOOL`, `BOX_INT`, `UNBOX_INT`
- Guards: `GUARD_NUM`, `GUARD_CLASS`, `GUARD_TRUE`, `GUARD_FALSE`
- Control: `LOOP_HEADER`, `LOOP_BACK`, `SNAPSHOT`, `SIDE_EXIT`, `PHI`

## Optimizer

Thirteen passes run in sequence:

1. Loop variable promotion — replaces `LOAD_MODULE_VAR/STORE_MODULE_VAR` pairs for loop-carried variables with `PHI` nodes, keeping values in registers across iterations
2. Box/unbox elimination — cancels adjacent `BOX(UNBOX(x))` pairs; removes `BOX_NUM` nodes whose only consumers are `UNBOX_NUM`
3. Redundant guard elimination — bitset tracking per guard kind, reset at loop header
4. Constant propagation and folding — algebraic identities, comparison folding
5. GVN — hash-based CSE
6. LICM — hoists loop-invariant computations to pre-header NOP slots (alias-safe: skips `LOAD_STACK` nodes whose slot is written in the loop body)
7. Guard hoisting — moves type guards on pre-loop values before the loop
8. Strength reduction — `x*2 → x+x`, `x/c → x*(1/c)`
9. Bounds check elimination — removes redundant `GUARD_NUM` after arithmetic
10. Escape analysis — scalar replacement and store-load forwarding for fields
11. DCE — mark-sweep from side-effecting roots
12. Guard elimination — proves and deletes loop-invariant guards; eliminates dispensable `STORE_STACK` nodes (Phase B)
13. Integer IV type inference — detects integer induction variables (PHIs with integer constant steps), promotes arithmetic to integer GP operations, eliminates NaN-boxing overhead in tight loops
14. DCE — re-sweep after passes 12–13

## Register allocator

Linear scan over computed live ranges. Separate pools for GP scratch (R0–R5),
FP scratch (FR0–FR5), and FP saved (FS0–FS3). R0 and R1 are reserved as
scratch temporaries. Saved GP registers hold: vm pointer (S0), fiber pointer
(S1), stack base (S2), module variable base (S3). Spills to a fixed-size local
frame when all registers in a class are live.

## Performance

Measured on Apple M-series (ARM64). Times are the hot-loop body only (Wren
`System.clock`); process startup and JIT compilation are excluded.

`bench_sum.wren` — sum integers 0..999999 in a `while` loop:

| mode        | time    |
|-------------|---------|
| interpreter | ~24 ms  |
| JIT         | ~2.0 ms |
| C (-O3)     | ~0.4 ms |

~12× speedup over the interpreter. Integer IV inference keeps both loop
variables (`sum`, `i`) in GP registers with no NaN-boxing overhead.

`bench_for.wren` — sum 1..1000000 via `for i in range`:

| mode        | time    |
|-------------|---------|
| interpreter | ~28 ms  |
| JIT         | ~3.9 ms |

~7× speedup over the interpreter. Range iteration is inlined via monomorphic
`CALL_1` widening (`jitTryWidenCall1`); no aborts.

`bench_fib.wren` — recursive Fibonacci(35):

| mode        | time    | notes         |
|-------------|---------|---------------|
| interpreter | 645 ms  |               |
| JIT         | 649 ms  | 0 traces compiled |
| C (-O3)     | 29.6 ms |               |

Recursive calls do not form a traceable hot loop; JIT has no effect.

## Build

Requires CMake 3.16+ and a C99 compiler.

```sh
cmake -B build
cmake --build build
./build/test_jit
```

To disable the JIT:

```sh
cmake -B build -DWREN_JIT=OFF
```

## Usage

```sh
./build/wrenjit_cli script.wren --jit
./build/wrenjit_cli script.wren --no-jit
```

## Limitations

- Range-based `for` loops compile via monomorphic inlining; other object method
  calls on non-`Num` receivers abort recording.
- No OSR (on-stack replacement). The trace must be entered from the top of the
  loop.
- No trace chaining. Each compiled trace covers exactly one loop.
- x86-64 and ARM64 only (SLJIT constraint).

## Files

```
src/jit/
  wren_jit.c          trace cache, lifecycle, hot counting
  wren_jit_ir.c       IR construction and debug printing
  wren_jit_opt.c           optimizer pipeline (14 passes)
  wren_jit_opt_guardelim.c guard elimination + STORE_STACK liveness (pass 12)
  wren_jit_opt_iv.c        integer IV type inference (pass 13)
  wren_jit_trace_widen.c   monomorphic inlining for Range iteration
  wren_jit_regalloc.c linear scan register allocator
  wren_jit_codegen.c  SLJIT code generator
  wren_jit_trace.c    bytecode-to-IR recorder
  wren_jit_snapshot.c snapshot construction and writeback
  wren_jit_memory.c   executable memory allocation (mmap/VirtualAlloc)
vendor/
  wren/               upstream Wren VM (instrumented behind #ifdef WREN_JIT)
  sljit/              SLJIT portable JIT backend
```

## License

MIT
