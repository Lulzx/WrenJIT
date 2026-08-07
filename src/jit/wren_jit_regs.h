#ifndef wren_jit_regs_h
#define wren_jit_regs_h

#include "sljitLir.h"

#include "wren_jit_regalloc.h"

// Physical register budget, shared by the register allocator and the code
// generator so the two can never disagree about how many registers exist.
//
// SLJIT splits registers into scratch (clobbered by a call) and saved
// (preserved across one), and how many of each a target has varies widely:
// arm64 has 8 saved float registers, x86-64 SysV has none at all. Asking
// sljit_emit_enter for more saved registers than the target provides trips an
// assert there and emits a broken prologue without one, so the split is
// computed per target instead of hardcoded.
//
// Traces never emit calls, so a saved float register is nothing but extra
// capacity here and a scratch register substitutes for it one for one. That
// keeps the FP pool the same size everywhere: targets with no saved float
// registers simply take all ten from the scratch bank.

#define WREN_JIT_MIN(a, b) ((a) < (b) ? (a) : (b))

#define GP_SCRATCH_COUNT WREN_JIT_MIN(GP_SCRATCH_POOL_MAX, \
                                      SLJIT_NUMBER_OF_SCRATCH_REGISTERS)
#define GP_SAVED_COUNT   4

#define FP_SAVED_COUNT   WREN_JIT_MIN(FP_SAVED_POOL_MAX, \
                                      SLJIT_NUMBER_OF_SAVED_FLOAT_REGISTERS)
#define FP_SCRATCH_COUNT WREN_JIT_MIN(FP_POOL_TOTAL - FP_SAVED_COUNT, \
                                      SLJIT_NUMBER_OF_SCRATCH_FLOAT_REGISTERS)

// Codegen pins S0..S3 to vm, fiber, stackStart and moduleVarsData, and uses
// R0/R1 and FR0/FR1 as fixed scratch, so these are hard floors rather than
// things to clamp.
#if GP_SAVED_COUNT > SLJIT_NUMBER_OF_SAVED_REGISTERS
#error "WrenJIT needs 4 saved GP registers (vm, fiber, stackStart, moduleVars)"
#endif
#if GP_SCRATCH_COUNT < 4
#error "WrenJIT needs at least 4 scratch GP registers (R0/R1 are reserved)"
#endif
#if FP_SCRATCH_COUNT < 4
#error "WrenJIT needs at least 4 scratch FP registers (FR0/FR1 are reserved)"
#endif

#endif // wren_jit_regs_h
