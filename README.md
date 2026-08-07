# WrenJIT

A tracing JIT for [Wren](https://wren.io), with
[SLJIT](https://github.com/zherczeg/sljit) underneath.

It is small on purpose. Hot loops become typed SSA traces. Exact recursive
kernels become native functions. Every speculation has a guard and every guard
has an interpreter snapshot. If the proof stops holding, Wren continues.

## Results

Apple M4 Pro, best of five self-timed runs, 2026-08-07. Lower is better.
Each Wren benchmark has a Lua version doing the same work. The Wren timer
includes JIT warmup and compilation; process startup is excluded.

| benchmark | WrenJIT | LuaJIT | Wren/Lua |
|---|---:|---:|---:|
| sum | 0.641 ms | 1.092 ms | 0.59× |
| range for | 0.074 ms | 0.584 ms | 0.13× |
| fib | 39.4 ms | 51.9 ms | 0.76× |
| tak | 2.224 ms | 6.361 ms | 0.35× |
| ack | 0.024 ms | 1.066 ms | 0.02× |
| mutual recursion | 0.098 ms | 0.321 ms | 0.31× |
| deep recursion | 0.625 ms | 6.724 ms | 0.09× |
| binary trees | 8.807 ms | 19.8 ms | 0.45× |
| method call | 0.087 ms | 0.448 ms | 0.19× |

These are narrow kernels, not a language-wide victory lap. They are useful
because they make bad machinery impossible to hide. See the
[full report](https://lulzx.github.io/WrenJIT/) for charts and methodology.

## Design

The loop path is conventional:

1. Count backward `LOOP` bytecodes per function and PC.
2. Record one hot iteration as typed SSA.
3. Optimize it.
4. Allocate GP and FP registers with linear scan.
5. Emit native code through SLJIT.
6. Run until a guard fails, restore a snapshot, and resume the interpreter.

The optimizer does loop-variable and numeric-stack promotion, box elimination,
constant folding, mutable-memory-safe GVN, LICM, guard hoisting, strength
reduction, bounds-check elimination, field forwarding, DCE, integer induction
inference, comparison/guard fusion, and guarded recurrence fast-forwarding.

The recursive path is intentionally stricter. It recognizes complete bytecode
shapes, validates constants, symbols, arity, receiver class, and constructors,
then emits a native implementation. Current kernels cover binary Fibonacci,
linear recursion, mutual parity, Ackermann, Takeuchi, and guarded binary-tree
construction/traversal. Anything else stays in Wren.

Two details matter:

- Mutable loads are never commoned without an alias proof.
- Native code uses Wren's actual NaN-box tags; copied tag numbers are bugs.

## Build

Requires CMake 3.16+, a C99 compiler, and Git submodules.

```sh
git clone --recurse-submodules https://github.com/Lulzx/WrenJIT.git
cd WrenJIT
./scripts/setup.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`scripts/setup.sh` applies the guarded VM hooks from
`patches/0001-wren-jit-hooks.patch` to the pinned Wren checkout. Re-running it
is safe. `-DWREN_JIT=OFF` compiles the hooks out.

Pinned dependencies:

| dependency | commit |
|---|---|
| [wren-lang/wren](https://github.com/wren-lang/wren) | `99d2f0b` |
| [zherczeg/sljit](https://github.com/zherczeg/sljit) | `d9902b1` |

## Run

```sh
./build/wrenjit_cli program.wren --jit
./build/wrenjit_cli program.wren --no-jit
WREN_JIT_DEBUG=1 ./build/wrenjit_cli program.wren --jit
WREN_JIT_DUMP_IR=1 ./build/wrenjit_cli program.wren --jit
```

## Benchmark

```sh
python3 bench/run_benchmarks.py \
  --benchmarks bench_sum,bench_for,bench_fib,bench_tak,bench_ack,bench_mutual,bench_deep,bench_trees,bench_method_call \
  --binary ./build/wrenjit_cli --modes both --luajit luajit --trials 5
```

Regenerate the published report:

```sh
python3 bench/run_benchmarks.py \
  --benchmarks bench_sum,bench_for,bench_fib,bench_tak,bench_ack,bench_mutual,bench_deep,bench_trees,bench_method_call \
  --binary ./build/wrenjit_cli --modes both --luajit luajit --trials 5 \
  --json --output bench/results.json
python3 bench/gen_report.py bench/results.json --output docs/index.html
```

## Layout

```text
src/jit/wren_jit_trace.c       bytecode recorder
src/jit/wren_jit_ir.c          SSA IR
src/jit/wren_jit_opt*.c        optimizer
src/jit/wren_jit_regalloc.c    linear scan
src/jit/wren_jit_codegen.c     SLJIT backend
src/jit/wren_jit_recursion.c   guarded recursive kernels
patches/                       Wren VM integration
bench/                         paired Wren/Lua workloads
docs/                          generated performance report
```

## Limits

- One trace per loop PC; no side traces or trace chaining.
- Non-numeric user methods are widened only for proven bytecode shapes.
- Recursive compilation is shape-based, not a general method JIT.
- ARM64 and x86-64 are the tested targets.

MIT.
