#ifndef wren_jit_opt_h
#define wren_jit_opt_h

#include "wren_jit_ir.h"

// Pass 0: Promote loop-carried module variables to register PHI nodes.
// Must be called before all other passes. Fills pre-header NOP slots
// (allocated by wrenJitStartRecording) with LOAD + UNBOX_NUM + PHI tuples
// and replaces in-loop UNBOX_NUM(LOAD_MODULE_VAR) with the PHI so that
// IV type inference can fire and eliminate FP box/unbox overhead.
void irOptPromoteLoopVars(IRBuffer* buf);

// Run all optimization passes on the IR buffer.
// Passes run in order:
//   1. Box/unbox elimination
//   2. Redundant guard elimination
//   3. Constant folding & propagation
//   4. Global value numbering (CSE/GVN)
//   5. Loop-invariant code motion (LICM)
//   6. Guard hoisting
//   7. Strength reduction
//   8. Bounds check elimination
//   9. Escape analysis
//  10. Dead code elimination
//  11. Guard elimination (prove-and-delete loop-invariant guards)
//  12. IV type inference (integer induction variable promotion)
void irOptimize(IRBuffer* buf);

// Individual passes (exposed for testing / selective use).
void irOptBoxUnboxElim(IRBuffer* buf);
void irOptRedundantGuardElim(IRBuffer* buf);
void irOptConstPropFold(IRBuffer* buf);
void irOptGVN(IRBuffer* buf);
void irOptLICM(IRBuffer* buf);
void irOptGuardHoist(IRBuffer* buf);
void irOptStrengthReduce(IRBuffer* buf);
void irOptBoundsCheckElim(IRBuffer* buf);
void irOptEscapeAnalysis(IRBuffer* buf);
void irOptDCE(IRBuffer* buf);
void irOptGuardElim(IRBuffer* buf);
void irOptIVTypeInference(IRBuffer* buf);

#endif // wren_jit_opt_h
