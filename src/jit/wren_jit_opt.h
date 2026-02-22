#ifndef wren_jit_opt_h
#define wren_jit_opt_h

#include "wren_jit_ir.h"

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

#endif // wren_jit_opt_h
