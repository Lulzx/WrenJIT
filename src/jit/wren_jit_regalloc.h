#ifndef wren_jit_regalloc_h
#define wren_jit_regalloc_h

#include "wren_jit_ir.h"

// Register classes
typedef enum {
    REG_CLASS_GP,    // General purpose (integers, pointers, Values)
    REG_CLASS_FP,    // Floating point (unboxed doubles)
} RegClass;

// Physical register assignment (or spill slot)
typedef struct {
    bool is_spill;          // true if spilled to stack
    union {
        int reg;            // register pool index (mapped to SLJIT in codegen)
        int spill_slot;     // stack frame slot index
    } loc;
    RegClass reg_class;
} RegAlloc;

// Live range for an SSA value
typedef struct {
    uint16_t ssa_id;        // IR node SSA id
    uint16_t start;         // first use (IR index)
    uint16_t end;           // last use (IR index)
    RegClass reg_class;     // what kind of register needed
    RegAlloc alloc;         // assigned register/spill
} LiveRange;

#define MAX_LIVE_RANGES IR_MAX_NODES
#define MAX_SPILL_SLOTS 256

// Upper bounds on the register pools, used to size the tracking arrays below.
// How many of each are actually usable depends on the target and is worked out
// in wren_jit_regs.h.
#define GP_SCRATCH_POOL_MAX 6
#define FP_SAVED_POOL_MAX   4
#define FP_POOL_TOTAL       10

// Register allocator state
typedef struct {
    LiveRange ranges[MAX_LIVE_RANGES];
    int num_ranges;

    // Track which physical registers are free.
    // GP scratch: maps to SLJIT_R0.. in codegen
    bool gp_scratch_free[GP_SCRATCH_POOL_MAX];
    // FP scratch: maps to SLJIT_FR0.. in codegen
    bool fp_scratch_free[FP_POOL_TOTAL];
    // FP saved: maps to SLJIT_FS0.. in codegen
    bool fp_saved_free[FP_SAVED_POOL_MAX];

    int next_spill_slot;
    int max_spill_slots;    // max spill slot used (for frame size)

    // Maps SSA id -> RegAlloc
    RegAlloc* ssa_to_reg;   // array indexed by SSA id
    int ssa_count;
} RegAllocState;

// Initialize the register allocator.
void regAllocInit(RegAllocState* state, int ssa_count);

// Compute live ranges from the IR buffer.
void regAllocComputeRanges(RegAllocState* state, const IRBuffer* buf);

// Run linear scan allocation.
void regAllocRun(RegAllocState* state);

// Get the allocation for a given SSA value.
RegAlloc regAllocGet(const RegAllocState* state, uint16_t ssa_id);

// Free allocator resources.
void regAllocFree(RegAllocState* state);

#endif // wren_jit_regalloc_h
