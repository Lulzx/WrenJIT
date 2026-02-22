#include "wren_jit_codegen.h"

// SLJIT header (the .c file is compiled separately via CMakeLists.txt).
#include "sljitLir.h"
#include "wren_jit_memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// NaN-boxing constants (must match Wren's value representation)
// ---------------------------------------------------------------------------
#define WREN_SIGN_BIT  0x8000000000000000ULL
#define WREN_QNAN      0x7ffc000000000000ULL

// Tags for non-number values:
// FALSE_VAL = QNAN | 0x01
// TRUE_VAL  = QNAN | 0x02
// NULL_VAL  = QNAN | 0x03
// Obj*      = SIGN_BIT | QNAN | pointer
#define WREN_FALSE_VAL (WREN_QNAN | 0x01)
#define WREN_TRUE_VAL  (WREN_QNAN | 0x02)
#define WREN_NULL_VAL  (WREN_QNAN | 0x03)

// Offset of classObj pointer inside Obj struct.
// Obj layout: ObjType type (enum=int, 4 bytes), bool isDark (1 byte),
// padding to pointer alignment, then ObjClass* classObj, then Obj* next.
// On 64-bit: offset is 8.
#define OBJ_CLASS_OFFSET 8

// ---------------------------------------------------------------------------
// Register mapping: convert RegAlloc pool indices to SLJIT registers.
// ---------------------------------------------------------------------------

// GP scratch pool index 0-5 -> SLJIT_R0..SLJIT_R5.
// Pool indices 0 and 1 (R0, R1) are reserved in the regalloc as scratch;
// SSA values are allocated starting from index 2 (R2).
// FP scratch pool index 100-105 -> SLJIT_FR0..FR5 => SLJIT_FR(i - 100)
// FP saved pool index 200-203 -> SLJIT_FS0..FS3 => SLJIT_FS(i - 200)

#define FP_SCRATCH_BASE_CODE 100
#define FP_SAVED_BASE_CODE   200

static int mapGPReg(int poolIdx)
{
    // Pool indices 0-5 map to SLJIT_R0..R5.
    return SLJIT_R(poolIdx);
}

static int mapFPReg(int poolIdx)
{
    if (poolIdx >= FP_SAVED_BASE_CODE)
        return SLJIT_FS(poolIdx - FP_SAVED_BASE_CODE);
    if (poolIdx >= FP_SCRATCH_BASE_CODE)
        return SLJIT_FR(poolIdx - FP_SCRATCH_BASE_CODE);
    // Fallback (shouldn't happen for FP).
    return SLJIT_FR0;
}

// Get the SLJIT register for an SSA value. If spilled, returns -1.
// Also sets *is_fp to true if it's a floating point register.
static int ssaToSljitReg(const RegAllocState* ra, uint16_t ssaId, int* is_fp,
                          int* spillOff)
{
    RegAlloc alloc = regAllocGet(ra, ssaId);
    *is_fp = (alloc.reg_class == REG_CLASS_FP) ? 1 : 0;

    if (alloc.is_spill) {
        // Spill slot offset in the local frame.
        *spillOff = alloc.loc.spill_slot * 8;
        return -1;
    }

    *spillOff = 0;
    if (alloc.reg_class == REG_CLASS_FP) {
        return mapFPReg(alloc.loc.reg);
    } else {
        return mapGPReg(alloc.loc.reg);
    }
}

// Convenience: get GP register or spill offset for an SSA value.
// Asserts the value is GP class.
static void getGP(const RegAllocState* ra, uint16_t ssaId,
                   int* reg, int* memBase, sljit_sw* memOff)
{
    int is_fp, spillOff;
    int r = ssaToSljitReg(ra, ssaId, &is_fp, &spillOff);
    if (r >= 0) {
        *reg = r;
        *memBase = 0;
        *memOff = 0;
    } else {
        *reg = SLJIT_MEM1(SLJIT_SP);
        *memBase = 1;
        *memOff = (sljit_sw)spillOff;
    }
}

// Convenience: get FP register or spill for an SSA value.
static void getFP(const RegAllocState* ra, uint16_t ssaId,
                   int* reg, int* memBase, sljit_sw* memOff)
{
    int is_fp, spillOff;
    int r = ssaToSljitReg(ra, ssaId, &is_fp, &spillOff);
    if (r >= 0) {
        *reg = r;
        *memBase = 0;
        *memOff = 0;
    } else {
        *reg = SLJIT_MEM1(SLJIT_SP);
        *memBase = 1;
        *memOff = (sljit_sw)spillOff;
    }
}

// ---------------------------------------------------------------------------
// Saved register assignments for function arguments:
//   S0 = vm, S1 = fiber, S2 = stackStart, S3 = stackTop
// ---------------------------------------------------------------------------
#define REG_VM         SLJIT_S0
#define REG_FIBER      SLJIT_S1
#define REG_STACK_BASE SLJIT_S2
#define REG_MOD_VARS   SLJIT_S3

// Number of saved GP registers we use (S0-S3).
#define NUM_SAVEDS     4
// Number of scratch GP registers available to the allocator.
#define NUM_SCRATCHES  6
// Number of FP scratch registers.
#define NUM_FP_SCRATCH 6
// Number of FP saved registers.
#define NUM_FP_SAVED   4

// Temporary spill area offset (past all regalloc spill slots).
// We reserve 16 bytes for box/unbox temporaries.
#define TMP_AREA_SIZE 16

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

JitTrace* wrenJitCodegen(void* vm, IRBuffer* ir, RegAllocState* ra,
                         uint8_t* anchorPC)
{
    if (!ir || ir->count == 0) return NULL;

    struct sljit_compiler* C = sljit_create_compiler(NULL);
    if (!C) return NULL;

    // Compute local frame size: regalloc spill area + temporary area.
    int spillBytes = ra->max_spill_slots * 8;
    int localSize = spillBytes + TMP_AREA_SIZE;
    // Offset for the temporary area (used for box/unbox).
    int tmpOff = spillBytes;

    // Prologue: 4 pointer args -> S0..S3.
    // SLJIT_ARGS4(W, P, P, P, P): return machine word, 4 pointer args.
    sljit_s32 fpScratchBits = SLJIT_ENTER_FLOAT(NUM_FP_SCRATCH);
    sljit_s32 fpSavedBits = SLJIT_ENTER_FLOAT(NUM_FP_SAVED);

    if (sljit_emit_enter(C, 0, SLJIT_ARGS4(W, P, P, P, P),
                         NUM_SCRATCHES | fpScratchBits,
                         NUM_SAVEDS | fpSavedBits,
                         localSize) != SLJIT_SUCCESS) {
        sljit_free_compiler(C);
        return NULL;
    }

    // ---------------------------------------------------------------------------
    // Pre-scan: count side exits for jump target allocation.
    // ---------------------------------------------------------------------------
    int maxSnapshots = (int)ir->snapshot_count;

    // Allocate arrays for side-exit jumps and labels.
    // exitJumps[snapIdx] = linked list of jumps to that exit stub.
    struct sljit_jump** exitJumps = (struct sljit_jump**)calloc(
        (size_t)(maxSnapshots + 1), sizeof(struct sljit_jump*));
    // We'll chain multiple jumps per snapshot via a parallel array.
    // Simple approach: store up to 16 jumps per snapshot.
    #define MAX_EXITS_PER_SNAP 16
    struct sljit_jump* exitJumpArr[IR_MAX_SNAPSHOTS][MAX_EXITS_PER_SNAP];
    int exitJumpCount[IR_MAX_SNAPSHOTS];
    memset(exitJumpCount, 0, sizeof(exitJumpCount));
    memset(exitJumpArr, 0, sizeof(exitJumpArr));

    // Label for loop header (set when we encounter IR_LOOP_HEADER).
    struct sljit_label* loopHeaderLabel = NULL;

    // ---------------------------------------------------------------------------
    // Main code generation loop.
    // ---------------------------------------------------------------------------
    for (uint16_t i = 0; i < ir->count; i++) {
        const IRNode* n = &ir->nodes[i];

        // Skip dead/nop nodes.
        if ((n->flags & IR_FLAG_DEAD) || n->op == IR_NOP)
            continue;

        switch (n->op) {

        // ----- Constants -----
        case IR_CONST_NUM: {
            // Load a double constant into the allocated FP register.
            // Strategy: store the 64-bit raw bits to the temp area via GP,
            // then load as f64.
            int dstReg, dstMem;
            sljit_sw dstOff;
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);

            union { double d; sljit_sw w; } bits;
            bits.d = n->imm.num;

            // Store full 64-bit value via GP register to temp area.
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, bits.w);
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff,
                           SLJIT_R0, 0);

            // Load as f64.
            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff,
                                SLJIT_FR0, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, 0,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
            }
            break;
        }

        case IR_CONST_BOOL:
        case IR_CONST_NULL:
        case IR_CONST_OBJ:
        case IR_CONST_INT: {
            // These produce GP values (Value/ptr/int).
            int dstReg, dstMem;
            sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            sljit_sw immVal = 0;
            if (n->op == IR_CONST_BOOL) {
                immVal = n->imm.intval ? (sljit_sw)WREN_TRUE_VAL
                                       : (sljit_sw)WREN_FALSE_VAL;
            } else if (n->op == IR_CONST_NULL) {
                immVal = (sljit_sw)WREN_NULL_VAL;
            } else if (n->op == IR_CONST_OBJ) {
                immVal = (sljit_sw)(uintptr_t)n->imm.ptr;
            } else { // IR_CONST_INT
                immVal = (sljit_sw)n->imm.i64;
            }

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff,
                               SLJIT_IMM, immVal);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                               SLJIT_IMM, immVal);
            }
            break;
        }

        // ----- Stack access -----
        case IR_LOAD_STACK: {
            // Load a NaN-tagged Value from interpreter stack slot.
            // Value = stackStart[slot], 64-bit.
            uint16_t slot = n->imm.mem.slot;
            int dstReg, dstMem;
            sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(REG_STACK_BASE), (sljit_sw)(slot * 8));
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                               SLJIT_MEM1(REG_STACK_BASE), (sljit_sw)(slot * 8));
            }
            break;
        }

        case IR_STORE_STACK: {
            // Store a Value to interpreter stack slot.
            uint16_t slot = n->imm.mem.slot;
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem;
            sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(REG_STACK_BASE), (sljit_sw)(slot * 8),
                               SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(REG_STACK_BASE), (sljit_sw)(slot * 8),
                               srcReg, 0);
            }
            break;
        }

        // ----- NaN-boxing -----
        case IR_UNBOX_NUM: {
            // Value (GP, uint64) -> double (FP).
            // In Wren's NaN-boxing, a number is stored as raw IEEE754 bits
            // when (val & QNAN) != QNAN. Just reinterpret bits.
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem;
            sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            int dstReg, dstMem;
            sljit_sw dstOFP;
            getFP(ra, n->id, &dstReg, &dstMem, &dstOFP);

            // Store GP value to temp area, then load as f64.
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff,
                               SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff,
                               srcReg, 0);
            }

            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOFP,
                                SLJIT_FR0, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, 0,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
            }
            break;
        }

        case IR_BOX_NUM: {
            // double (FP) -> Value (GP, uint64).
            // Store FP to temp area, then load as GP.
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem;
            sljit_sw srcOff;
            getFP(ra, valId, &srcReg, &srcMem, &srcOff);

            int dstReg, dstMem;
            sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (srcMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                srcReg, srcOff);
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff,
                                SLJIT_FR0, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff,
                                srcReg, 0);
            }

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                               SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
            }
            break;
        }

        case IR_BOX_BOOL: {
            // Raw boolean (0/1) -> Wren Value (FALSE_VAL/TRUE_VAL).
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, 0);
            }

            // if R0 == 0: result = FALSE_VAL, else result = TRUE_VAL
            struct sljit_jump* isFalse = sljit_emit_cmp(C, SLJIT_EQUAL,
                SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)WREN_TRUE_VAL);
            struct sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);

            struct sljit_label* falseLabel = sljit_emit_label(C);
            sljit_set_label(isFalse, falseLabel);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);

            struct sljit_label* doneLabel = sljit_emit_label(C);
            sljit_set_label(done, doneLabel);

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0, SLJIT_R0, 0);
            }
            break;
        }

        case IR_BOX_OBJ: {
            // Obj* -> Value: val = SIGN_BIT | QNAN | ptr
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem;
            sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            int dstReg, dstMem;
            sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            // R1 = src
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, 0);
            }
            // R1 = R1 | (SIGN_BIT | QNAN)
            sljit_emit_op2(C, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)(WREN_SIGN_BIT | WREN_QNAN));

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R1, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0, SLJIT_R1, 0);
            }
            break;
        }

        case IR_UNBOX_OBJ: {
            // Value -> Obj*: ptr = val & ~(SIGN_BIT | QNAN)
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem;
            sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            int dstReg, dstMem;
            sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, 0);
            }
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R1, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0, SLJIT_R1, 0);
            }
            break;
        }

        // ----- Arithmetic (FP) -----
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV: {
            sljit_s32 fop;
            switch (n->op) {
                case IR_ADD: fop = SLJIT_ADD_F64; break;
                case IR_SUB: fop = SLJIT_SUB_F64; break;
                case IR_MUL: fop = SLJIT_MUL_F64; break;
                case IR_DIV: fop = SLJIT_DIV_F64; break;
                default: fop = SLJIT_ADD_F64; break;
            }

            int src1Reg, src1Mem; sljit_sw src1Off;
            int src2Reg, src2Mem; sljit_sw src2Off;
            int dstReg, dstMem; sljit_sw dstOff;

            getFP(ra, n->op1, &src1Reg, &src1Mem, &src1Off);
            getFP(ra, n->op2, &src2Reg, &src2Mem, &src2Off);
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);

            // SLJIT fop2 can handle memory operands directly in some cases,
            // but for safety, load spilled operands into scratch FP regs.
            int s1r = src1Reg, s2r = src2Reg, dr = dstReg;
            sljit_sw s1w = 0, s2w = 0, dw = 0;

            if (src1Mem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, src1Reg, src1Off);
                s1r = SLJIT_FR0; s1w = 0;
            } else { s1w = 0; }

            if (src2Mem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, src2Reg, src2Off);
                s2r = SLJIT_FR1; s2w = 0;
            } else { s2w = 0; }

            if (dstMem) {
                dr = SLJIT_FR0; dw = 0;
            } else { dw = 0; }

            sljit_emit_fop2(C, fop, dr, dw, s1r, s1w, s2r, s2w);

            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff, SLJIT_FR0, 0);
            }
            break;
        }

        case IR_NEG: {
            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getFP(ra, n->op1, &srcReg, &srcMem, &srcOff);
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);

            int sr = srcReg; sljit_sw sw2 = 0;
            if (srcMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, srcReg, srcOff);
                sr = SLJIT_FR0;
            }
            int dr = dstReg; sljit_sw dw2 = 0;
            if (dstMem) dr = SLJIT_FR0;

            sljit_emit_fop1(C, SLJIT_NEG_F64, dr, dw2, sr, sw2);

            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff, SLJIT_FR0, 0);
            }
            break;
        }

        // ----- Comparison (FP -> bool in GP) -----
        case IR_LT:
        case IR_GT:
        case IR_LTE:
        case IR_GTE:
        case IR_EQ:
        case IR_NEQ: {
            int src1Reg, src1Mem; sljit_sw src1Off;
            int src2Reg, src2Mem; sljit_sw src2Off;
            getFP(ra, n->op1, &src1Reg, &src1Mem, &src1Off);
            getFP(ra, n->op2, &src2Reg, &src2Mem, &src2Off);

            int s1r = src1Reg;
            int s2r = src2Reg;
            if (src1Mem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, src1Reg, src1Off);
                s1r = SLJIT_FR0;
            }
            if (src2Mem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, src2Reg, src2Off);
                s2r = SLJIT_FR1;
            }

            // Determine the SLJIT float comparison flag.
            sljit_s32 cmpFlag;
            sljit_s32 resultFlag;
            switch (n->op) {
                case IR_LT:  cmpFlag = SLJIT_SET_F_LESS;         resultFlag = SLJIT_F_LESS; break;
                case IR_GT:  cmpFlag = SLJIT_SET_F_LESS;         resultFlag = SLJIT_F_LESS; break;
                case IR_LTE: cmpFlag = SLJIT_SET_F_LESS_EQUAL;   resultFlag = SLJIT_F_LESS_EQUAL; break;
                case IR_GTE: cmpFlag = SLJIT_SET_F_LESS_EQUAL;   resultFlag = SLJIT_F_LESS_EQUAL; break;
                case IR_EQ:  cmpFlag = SLJIT_SET_ORDERED_EQUAL;  resultFlag = SLJIT_ORDERED_EQUAL; break;
                case IR_NEQ: cmpFlag = SLJIT_SET_ORDERED_NOT_EQUAL; resultFlag = SLJIT_ORDERED_NOT_EQUAL; break;
                default:     cmpFlag = SLJIT_SET_F_LESS;         resultFlag = SLJIT_F_LESS; break;
            }

            // For GT and GTE, swap operands to turn into LT/LTE.
            if (n->op == IR_GT || n->op == IR_GTE) {
                sljit_emit_fop1(C, SLJIT_CMP_F64 | cmpFlag, s2r, 0, s1r, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_CMP_F64 | cmpFlag, s1r, 0, s2r, 0);
            }

            // Materialize the boolean result into a GP register.
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (dstMem) {
                sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, resultFlag);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op_flags(C, SLJIT_MOV, dstReg, 0, resultFlag);
            }
            break;
        }

        // ----- Guards -----
        case IR_GUARD_NUM: {
            // Check if val is a number: (val & QNAN) != QNAN.
            // If it IS QNAN-tagged (not a number), jump to side exit.
            uint16_t valId = n->op1;
            uint16_t snapId = n->imm.snapshot_id;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            // tmp = val & QNAN
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)WREN_QNAN);
            } else {
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, srcReg, 0,
                               SLJIT_IMM, (sljit_sw)WREN_QNAN);
            }

            // CMP tmp, QNAN; if equal => not a number => side exit.
            struct sljit_jump* jmp = sljit_emit_cmp(C, SLJIT_EQUAL,
                SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_QNAN);

            if (snapId < (uint16_t)maxSnapshots &&
                exitJumpCount[snapId] < MAX_EXITS_PER_SNAP) {
                exitJumpArr[snapId][exitJumpCount[snapId]++] = jmp;
            }
            break;
        }

        case IR_GUARD_CLASS: {
            // Check obj->classObj == expected class pointer.
            uint16_t valId = n->op1;
            void* expectedClass = n->imm.ptr;
            uint16_t snapId = n->op2; // snapshot id stored in op2
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            // R1 = obj pointer (already unboxed, or unbox here).
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, 0);
            }

            // R0 = obj->classObj
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R1), (sljit_sw)OBJ_CLASS_OFFSET);

            struct sljit_jump* jmp = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)(uintptr_t)expectedClass);

            if (snapId < (uint16_t)maxSnapshots &&
                exitJumpCount[snapId] < MAX_EXITS_PER_SNAP) {
                exitJumpArr[snapId][exitJumpCount[snapId]++] = jmp;
            }
            break;
        }

        case IR_GUARD_TRUE: {
            // Guard that value is truthy.
            uint16_t valId = n->op1;
            uint16_t snapId = n->imm.snapshot_id;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, 0);
            }

            // Check the type of the input value. If it's a raw boolean
            // (from IR_LT etc.), check for 0. Otherwise check Wren Values.
            IRType inputType = (valId < ir->count) ? ir->nodes[valId].type
                                                    : IR_TYPE_VALUE;

            if (inputType == IR_TYPE_BOOL) {
                // Raw boolean: 0 = false, nonzero = true.
                // Side-exit if value == 0.
                struct sljit_jump* jmpFalse = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, 0);
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = jmpFalse;
            } else {
                // Wren Value: false and null are falsy.
                struct sljit_jump* jmpFalse = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);
                struct sljit_jump* jmpNull = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);

                if (snapId < (uint16_t)maxSnapshots) {
                    if (exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = jmpFalse;
                    if (exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = jmpNull;
                }
            }
            break;
        }

        case IR_GUARD_FALSE: {
            // Guard that value is falsy.
            uint16_t valId = n->op1;
            uint16_t snapId = n->imm.snapshot_id;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, 0);
            }

            IRType inputType = (valId < ir->count) ? ir->nodes[valId].type
                                                    : IR_TYPE_VALUE;

            if (inputType == IR_TYPE_BOOL) {
                // Raw boolean: side-exit if nonzero (truthy).
                struct sljit_jump* jmpExit = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, 0);
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = jmpExit;
            } else {
                // Wren Value: side-exit if not false and not null.
                struct sljit_jump* isFalse = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);
                struct sljit_jump* isNull = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);

                struct sljit_jump* jmpExit = sljit_emit_jump(C, SLJIT_JUMP);
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = jmpExit;

                struct sljit_label* okLabel = sljit_emit_label(C);
                sljit_set_label(isFalse, okLabel);
                sljit_set_label(isNull, okLabel);
            }
            break;
        }

        case IR_GUARD_NOT_NULL: {
            uint16_t valId = n->op1;
            uint16_t snapId = n->imm.snapshot_id;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, 0);
            }

            struct sljit_jump* jmp = sljit_emit_cmp(C, SLJIT_EQUAL,
                SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);

            if (snapId < (uint16_t)maxSnapshots &&
                exitJumpCount[snapId] < MAX_EXITS_PER_SNAP) {
                exitJumpArr[snapId][exitJumpCount[snapId]++] = jmp;
            }
            break;
        }

        // ----- Control flow -----
        case IR_LOOP_HEADER: {
            loopHeaderLabel = sljit_emit_label(C);
            break;
        }

        case IR_LOOP_BACK: {
            if (loopHeaderLabel) {
                struct sljit_jump* backJump = sljit_emit_jump(C, SLJIT_JUMP);
                sljit_set_label(backJump, loopHeaderLabel);
            }
            break;
        }

        // ----- PHI, SNAPSHOT, SIDE_EXIT -----
        case IR_PHI:
            // PHI nodes produce no code. The register allocator ensures
            // both inputs and the output share the same register.
            break;

        case IR_SNAPSHOT:
            // Snapshot nodes produce no code themselves.
            break;

        case IR_SIDE_EXIT:
            // Side exits are targets for guard failure jumps.
            // The actual exit stubs are generated after the main loop below.
            break;

        // ----- Bitwise ops (operate on integers) -----
        case IR_BAND:
        case IR_BOR:
        case IR_BXOR:
        case IR_LSHIFT:
        case IR_RSHIFT: {
            sljit_s32 op2code;
            switch (n->op) {
                case IR_BAND:   op2code = SLJIT_AND; break;
                case IR_BOR:    op2code = SLJIT_OR; break;
                case IR_BXOR:   op2code = SLJIT_XOR; break;
                case IR_LSHIFT: op2code = SLJIT_SHL; break;
                case IR_RSHIFT: op2code = SLJIT_ASHR; break;
                default: op2code = SLJIT_AND; break;
            }

            int s1r, s1m; sljit_sw s1o;
            int s2r, s2m; sljit_sw s2o;
            int dr, dm; sljit_sw dof;
            getGP(ra, n->op1, &s1r, &s1m, &s1o);
            getGP(ra, n->op2, &s2r, &s2m, &s2o);
            getGP(ra, n->id, &dr, &dm, &dof);

            int a = SLJIT_R0, b = SLJIT_R1;
            if (s1m) {
                sljit_emit_op1(C, SLJIT_MOV, a, 0, s1r, s1o);
            } else { a = s1r; }
            if (s2m) {
                sljit_emit_op1(C, SLJIT_MOV, b, 0, s2r, s2o);
            } else { b = s2r; }

            if (dm) {
                sljit_emit_op2(C, op2code, SLJIT_R0, 0, a, 0, b, 0);
                sljit_emit_op1(C, SLJIT_MOV, dr, dof, SLJIT_R0, 0);
            } else {
                sljit_emit_op2(C, op2code, dr, 0, a, 0, b, 0);
            }
            break;
        }

        case IR_BNOT: {
            int sr, sm; sljit_sw so2;
            int dr, dm; sljit_sw dof;
            getGP(ra, n->op1, &sr, &sm, &so2);
            getGP(ra, n->id, &dr, &dm, &dof);

            int a = sr;
            if (sm) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, sr, so2);
                a = SLJIT_R0;
            }
            // NOT x = XOR x, -1
            if (dm) {
                sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0, a, 0,
                               SLJIT_IMM, (sljit_sw)-1);
                sljit_emit_op1(C, SLJIT_MOV, dr, dof, SLJIT_R0, 0);
            } else {
                sljit_emit_op2(C, SLJIT_XOR, dr, 0, a, 0,
                               SLJIT_IMM, (sljit_sw)-1);
            }
            break;
        }

        case IR_MOD: {
            // Modulo: not directly supported by SLJIT. We'd need to call
            // fmod or implement integer mod. For now, use a C call.
            // Simplified: emit as remainder of integer division.
            // TODO: implement properly via C callback or SLJIT div+mul+sub.
            break;
        }

        // ----- Field access -----
        case IR_LOAD_FIELD: {
            // Load a field from an object. op1 = object pointer (GP),
            // imm.mem.field = field index.
            // ObjInstance layout: header (24 bytes on 64-bit) + Value fields[].
            uint16_t objId = n->op1;
            uint16_t fieldIdx = n->imm.mem.field;
            if (objId == IR_NONE) break;

            int objReg, objMem; sljit_sw objOff;
            getGP(ra, objId, &objReg, &objMem, &objOff);

            // R1 = object pointer
            if (objMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
            }

            // Fields start at offset 24 (after Obj header).
            sljit_sw fieldOff = 24 + (sljit_sw)(fieldIdx * 8);

            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R1), fieldOff);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                               SLJIT_MEM1(SLJIT_R1), fieldOff);
            }
            break;
        }

        case IR_STORE_FIELD: {
            // Store a value to an object field. op1 = obj ptr, op2 = value.
            uint16_t objId = n->op1;
            uint16_t valId = n->op2;
            uint16_t fieldIdx = n->imm.mem.field;
            if (objId == IR_NONE || valId == IR_NONE) break;

            int objReg, objMem; sljit_sw objOff;
            getGP(ra, objId, &objReg, &objMem, &objOff);

            if (objMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
            }

            sljit_sw fieldOff = 24 + (sljit_sw)(fieldIdx * 8);

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_R1), fieldOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_R1), fieldOff, srcReg, 0);
            }
            break;
        }

        case IR_LOAD_MODULE_VAR: {
            // Load a Value from an absolute address (imm.ptr).
            // The recorder stores the address of the module variable directly.
            void* varAddr = n->imm.ptr;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            // Load absolute address into R0, then load value from it.
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)(uintptr_t)varAddr);
            if (dstMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                               SLJIT_MEM1(SLJIT_R0), 0);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R1, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                               SLJIT_MEM1(SLJIT_R0), 0);
            }
            break;
        }

        case IR_STORE_MODULE_VAR: {
            // Store a Value to an absolute address (imm.ptr).
            // op1 = SSA value to store.
            void* varAddr = n->imm.ptr;
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            // Load absolute address into R0, then store value to it.
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)(uintptr_t)varAddr);
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, srcOff);
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R1, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_R0), 0, srcReg, 0);
            }
            break;
        }

        case IR_CALL_C:
        case IR_CALL_WREN:
            // Not yet implemented. These will require C function calls
            // via SLJIT's call infrastructure in a future iteration.
            break;

        default:
            break;
        }
    }

    // Success epilogue: return 0 (no side exit).
    sljit_emit_return(C, SLJIT_MOV, SLJIT_IMM, 0);

    // ---------------------------------------------------------------------------
    // Side-exit stubs.
    // For each snapshot, emit a stub that:
    //   1. Loads the exit index into SLJIT_R0
    //   2. Returns (the caller uses the return value to find the snapshot)
    // The actual snapshot writeback is done by the interpreter after the trace
    // returns, using the snapshot data stored in the JitTrace struct.
    // ---------------------------------------------------------------------------
    struct sljit_label* exitLabels[IR_MAX_SNAPSHOTS];

    for (int si = 0; si < maxSnapshots; si++) {
        exitLabels[si] = sljit_emit_label(C);

        // Return exitIdx + 1 (0 means success/no exit).
        sljit_emit_return(C, SLJIT_MOV, SLJIT_IMM, (sljit_sw)(si + 1));
    }

    // Patch all guard jumps to their respective exit stubs.
    for (int si = 0; si < maxSnapshots; si++) {
        for (int j = 0; j < exitJumpCount[si]; j++) {
            sljit_set_label(exitJumpArr[si][j], exitLabels[si]);
        }
    }

    // ---------------------------------------------------------------------------
    // Generate native code.
    // ---------------------------------------------------------------------------
    void* generatedCode = sljit_generate_code(C, 0, NULL);
    if (!generatedCode) {
        free(exitJumps);
        sljit_free_compiler(C);
        return NULL;
    }

    sljit_uw codeSize = sljit_get_generated_code_size(C);

    // Use SLJIT's generated code directly. We must NOT copy it to a new
    // buffer because that would invalidate all relative jumps and resolved
    // label addresses within the code.
    void* codeBuf = generatedCode;

    sljit_free_compiler(C);
    free(exitJumps);

    // ---------------------------------------------------------------------------
    // Build JitTrace structure.
    // ---------------------------------------------------------------------------
    JitTrace* trace = (JitTrace*)calloc(1, sizeof(JitTrace));
    if (!trace) {
        sljit_free_code(codeBuf, NULL);
        return NULL;
    }

    trace->anchor_pc = anchorPC;
    trace->code = codeBuf;
    trace->code_size = (uint32_t)codeSize;

    // Copy snapshot data.
    trace->num_snapshots = (uint16_t)maxSnapshots;
    if (maxSnapshots > 0) {
        trace->snapshots = (JitSnapshot*)calloc((size_t)maxSnapshots,
                                                sizeof(JitSnapshot));
        if (trace->snapshots) {
            for (int si = 0; si < maxSnapshots; si++) {
                const IRSnapshot* irSnap = &ir->snapshots[si];
                JitSnapshot* js = &trace->snapshots[si];
                jitSnapshotInit(js, irSnap->resume_pc, irSnap->stack_depth);

                // Copy entries from IR shared pool.
                for (uint16_t e = 0; e < irSnap->num_entries; e++) {
                    uint16_t entryIdx = irSnap->entry_start + e;
                    if (entryIdx >= ir->snapshot_entry_count) break;
                    jitSnapshotAddEntry(js,
                        ir->snapshot_entries[entryIdx].slot,
                        ir->snapshot_entries[entryIdx].ssa_ref);
                }
            }
        }
    }

    // Collect GC roots: object pointers embedded in the trace (from IR_CONST_OBJ).
    int numRoots = 0;
    for (uint16_t i = 0; i < ir->count; i++) {
        if (ir->nodes[i].op == IR_CONST_OBJ && ir->nodes[i].imm.ptr != NULL)
            numRoots++;
    }
    if (numRoots > 0) {
        trace->gc_roots = (void**)calloc((size_t)numRoots, sizeof(void*));
        if (trace->gc_roots) {
            int idx = 0;
            for (uint16_t i = 0; i < ir->count; i++) {
                if (ir->nodes[i].op == IR_CONST_OBJ && ir->nodes[i].imm.ptr != NULL) {
                    trace->gc_roots[idx++] = ir->nodes[i].imm.ptr;
                }
            }
        }
        trace->num_gc_roots = (uint16_t)numRoots;
    }

    trace->exec_count = 0;
    trace->exit_count = 0;

    return trace;
}
