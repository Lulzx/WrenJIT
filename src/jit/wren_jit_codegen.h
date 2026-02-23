#ifndef WREN_JIT_CODEGEN_H
#define WREN_JIT_CODEGEN_H

#include "wren_jit_ir.h"
#include "wren_jit_snapshot.h"
#include "wren_jit_regalloc.h"
#include "wren_jit.h"

// Compile IR + register allocation to native code using SLJIT.
// modVarsBase: base pointer of the module variables array (modVarsData), used
//              to compute REG_MOD_VARS-relative offsets for LOAD/STORE_MODULE_VAR.
//              Pass NULL to fall back to absolute-pointer mode.
// Returns a JitTrace with compiled code, or NULL on error.
JitTrace* wrenJitCodegen(void* vm, IRBuffer* ir, RegAllocState* ra,
                         uint8_t* anchorPC, void* modVarsBase);

#endif
