#include "wren_jit_codegen.h"
#include "wren_vm.h"

// SLJIT header (the .c file is compiled separately via CMakeLists.txt).
#include "sljitLir.h"
#include "wren_jit_memory.h"
#include "wren_jit_regs.h"
#include "wren_value.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// NaN-boxing constants (must match Wren's value representation)
// ---------------------------------------------------------------------------
#define WREN_SIGN_BIT  0x8000000000000000ULL
#define WREN_QNAN      0x7ffc000000000000ULL

// Use the VM's singleton definitions directly.  Their tag ordering is an
// implementation detail and has changed between Wren revisions; copying the
// numeric tags here made otherwise-valid boolean guards always side-exit.
#define WREN_FALSE_VAL ((sljit_uw)FALSE_VAL)
#define WREN_TRUE_VAL  ((sljit_uw)TRUE_VAL)
#define WREN_NULL_VAL  ((sljit_uw)NULL_VAL)

// Derive VM object offsets from the actual Wren layout rather than assuming
// one compiler/ABI's padding.
#define OBJ_CLASS_OFFSET ((sljit_sw)offsetof(Obj, classObj))
#define OBJ_FIELDS_OFFSET ((sljit_sw)offsetof(ObjInstance, fields))
#define RANGE_FROM_OFFSET ((sljit_sw)offsetof(ObjRange, from))
#define RANGE_TO_OFFSET ((sljit_sw)offsetof(ObjRange, to))
#define RANGE_INCLUSIVE_OFFSET ((sljit_sw)offsetof(ObjRange, isInclusive))
#define LIST_DATA_OFFSET ((sljit_sw)(offsetof(ObjList, elements) + \
                                     offsetof(ValueBuffer, data)))
#define LIST_COUNT_OFFSET ((sljit_sw)(offsetof(ObjList, elements) + \
                                      offsetof(ValueBuffer, count)))
// ObjMap open-addressing layout: capacity (uint32), count (uint32), entries
// (MapEntry*, 16 bytes each: key at +0, value at +8).
#define MAP_CAPACITY_OFFSET ((sljit_sw)offsetof(ObjMap, capacity))
#define MAP_COUNT_OFFSET    ((sljit_sw)offsetof(ObjMap, count))
#define MAP_ENTRIES_OFFSET  ((sljit_sw)offsetof(ObjMap, entries))
#define MAP_ENTRY_KEY_OFFSET   0
#define MAP_ENTRY_VALUE_OFFSET 8
#define MAP_UNDEFINED_VAL ((sljit_uw)(WREN_QNAN | 4ULL))  // TAG_UNDEFINED
#define MAP_FALSE_VAL     ((sljit_uw)(WREN_QNAN | 2ULL))  // TAG_FALSE
// Sunk module stores write the loop-carried PHI back only after the loop body
// has executed at least once. The generated code tracks that with a flag in
// WrenJitState: reset by the trace prologue, set by LOOP_BACK, tested by the
// exit stubs. vm->jit is a POINTER to a separately-allocated WrenJitState, so
// the flag requires a two-level access: load the pointer at VM_JIT_OFFSET,
// then index JIT_FLAG_OFFSET within the pointed-to state. Treating vm+328+200
// as the flag would write 168 bytes past the WrenVM allocation.
#define VM_JIT_OFFSET ((sljit_sw)offsetof(WrenVM, jit))
#define JIT_FLAG_OFFSET ((sljit_sw)offsetof(WrenJitState, trace_store_flag))

// REG_TMP_LOAD_VM_JIT: R1 = vm->jit
#define EMIT_LOAD_VM_JIT()                                                     \
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,                                  \
                   SLJIT_MEM1(REG_VM), VM_JIT_OFFSET)
// Write R0 to vm->jit->trace_store_flag.
#define EMIT_SET_FLAG_FROM_R0()                                                \
    do {                                                                       \
        EMIT_LOAD_VM_JIT();                                                    \
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), JIT_FLAG_OFFSET,    \
                       SLJIT_R0, 0);                                           \
    } while (0)
// Read vm->jit->trace_store_flag into R0.
#define EMIT_READ_FLAG_TO_R0()                                                 \
    do {                                                                       \
        EMIT_LOAD_VM_JIT();                                                    \
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,                              \
                       SLJIT_MEM1(SLJIT_R1), JIT_FLAG_OFFSET);                 \
    } while (0)

// Maximum native jumps targeting one deoptimization snapshot. Code generation
// preflights this bound instead of silently dropping an additional guard.
#define MAX_EXITS_PER_SNAP 64

// ---------------------------------------------------------------------------
// Register mapping: convert RegAlloc pool indices to SLJIT registers.
// ---------------------------------------------------------------------------

// GP scratch pool index 0-5 -> SLJIT_R0..SLJIT_R5.
// Pool indices 0-3 (R0-R3) are reserved in the regalloc as scratch; R0/R1 for
// guards, box/unbox and loads/stores, R2/R3 for the map probe loop. SSA
// values are allocated starting from index 4 (R4).
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

// Number of saved GP registers we use (S0-S3 plus S4 for the QNAN constant).
#define NUM_SAVEDS     5
// Register holding the NaN-boxing QNAN tag for the whole trace, so guards can
// compare against a register instead of materializing the 64-bit constant.
#define CONST_QNAN_REG  SLJIT_S4
#if NUM_SAVEDS > SLJIT_NUMBER_OF_SAVED_REGISTERS
#error "WrenJIT needs 5 saved GP registers (S4 holds the QNAN constant)"
#endif
// Number of scratch GP registers available to the allocator.
#define NUM_SCRATCHES  GP_SCRATCH_COUNT
// Number of FP scratch registers.
#define NUM_FP_SCRATCH FP_SCRATCH_COUNT
// Number of FP saved registers.
#define NUM_FP_SAVED   FP_SAVED_COUNT

// Temporary spill area offset (past all regalloc spill slots).
// We reserve 16 bytes for box/unbox temporaries.
#define TMP_AREA_SIZE 96

static bool isAscendingIntegralPhi(const IRBuffer* ir, uint16_t id)
{
    if (id == IR_NONE || id >= ir->count) return false;
    const IRNode* phi = &ir->nodes[id];
    if (phi->op != IR_PHI || phi->op2 == IR_NONE || phi->op2 >= ir->count)
        return false;

    // The phi must belong to a loop: it must precede the LOOP_HEADER of the
    // loop it carries. Single-loop traces emit every promoted phi before the
    // one header; nested loops emit the inner phis before a second header, so
    // scan forward from the phi for the next header rather than comparing
    // against a single loop_header field.
    bool beforeHeader = false;
    for (uint16_t i = (uint16_t)(id + 1); i < ir->count; i++) {
        if (ir->nodes[i].op == IR_LOOP_HEADER) { beforeHeader = true; break; }
    }
    if (!beforeHeader) return false;

    const IRNode* back = &ir->nodes[phi->op2];
    if (back->op != IR_ADD) return false;
    uint16_t stepId = back->op1 == id ? back->op2 :
                      (back->op2 == id ? back->op1 : IR_NONE);
    if (stepId == IR_NONE || stepId >= ir->count) return false;
    const IRNode* step = &ir->nodes[stepId];
    if (phi->type == IR_TYPE_NUM) {
        return step->op == IR_CONST_NUM && step->imm.num > 0.0 &&
               step->imm.num == (double)(int64_t)step->imm.num;
    }
    if (phi->type == IR_TYPE_INT) {
        return step->op == IR_CONST_INT && step->imm.i64 > 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

// Materialize a forward-referenced constant at its first use. Strength-
// reduction rewrites `x / C` -> `x * (1/C)` (and MUL->LSHIFT, MOD->BAND) by
// appending a fresh CONST at the END of the IR buffer, so a constant's node
// index can exceed the index of the op that uses it. The linear sweep would
// otherwise emit the load after the loop back-edge (dead code) and the op
// would read an unwritten register. regAllocComputeRanges gives such
// constants a live range spanning their first use, so loading the register
// (or spill slot) here is safe.
static void materializeForwardConsts(struct sljit_compiler* C,
                                     const RegAllocState* ra,
                                     const IRBuffer* ir,
                                     sljit_sw tmpOff,
                                     uint16_t i)
{
    const IRNode* n = &ir->nodes[i];
    uint16_t refs[2];
    int nrefs = 0;
    if (n->op1 != IR_NONE && n->op1 < ir->count) refs[nrefs++] = n->op1;
    if (n->op2 != IR_NONE && n->op2 < ir->count) refs[nrefs++] = n->op2;

    for (int r = 0; r < nrefs; r++) {
        uint16_t c = refs[r];
        if (c <= i) continue; // not a forward reference (def after use)
        const IRNode* cn = &ir->nodes[c];
        if (cn->flags & IR_FLAG_DEAD) continue;

        switch (cn->op) {
        case IR_CONST_NUM: {
            int dreg, dmem; sljit_sw doff;
            getFP(ra, c, &dreg, &dmem, &doff);
            union { double d; sljit_sw w; } bits;
            bits.d = cn->imm.num;
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, bits.w);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                           (sljit_sw)tmpOff, SLJIT_R0, 0);
            if (dmem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
                sljit_emit_fop1(C, SLJIT_MOV_F64, dreg, doff, SLJIT_FR0, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dreg, 0,
                                SLJIT_MEM1(SLJIT_SP), (sljit_sw)tmpOff);
            }
            break;
        }
        case IR_CONST_INT:
        case IR_CONST_BOOL:
        case IR_CONST_NULL:
        case IR_CONST_OBJ: {
            int dreg, dmem; sljit_sw doff;
            getGP(ra, c, &dreg, &dmem, &doff);
            sljit_sw immVal = 0;
            if (cn->op == IR_CONST_BOOL) {
                immVal = cn->imm.intval ? (sljit_sw)WREN_TRUE_VAL
                                        : (sljit_sw)WREN_FALSE_VAL;
            } else if (cn->op == IR_CONST_NULL) {
                immVal = (sljit_sw)WREN_NULL_VAL;
            } else if (cn->op == IR_CONST_OBJ) {
                immVal = (sljit_sw)(uintptr_t)cn->imm.ptr;
            } else { // IR_CONST_INT
                immVal = (sljit_sw)cn->imm.i64;
            }
            if (dmem) {
                sljit_emit_op1(C, SLJIT_MOV, dreg, doff, SLJIT_IMM, immVal);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, dreg, 0, SLJIT_IMM, immVal);
            }
            break;
        }
        default:
            continue;
        }
    }
}

// Emit back-edge copies (phi = op2) for the loop-carried PHIs of a nested
// loop. Only PHIs whose back-edge value is computed inside the loop being
// closed ([hdr, back_pos)) are copied. Scan the whole prefix: a loop can be
// opened after another nested loop was recorded, leaving its entry-store PHI
// before that intervening header (list-for around a matcher is the canonical
// case). The op2 range is the authoritative ownership test.
static void emitNestedBackedgeCopies(struct sljit_compiler* C,
                                     const RegAllocState* ra,
                                     const IRBuffer* ir,
                                     const uint16_t* backedgeToPhi,
                                     uint16_t start, uint16_t end,
                                     uint16_t hdr, uint16_t back_pos)
{
    for (uint16_t p = start; p < end && p < ir->count; p++) {
        const IRNode* phi = &ir->nodes[p];
        if ((phi->flags & IR_FLAG_DEAD) || phi->op != IR_PHI) continue;
        if (phi->op2 == IR_NONE || phi->op2 >= ir->count) continue;
        if (phi->op2 < hdr || phi->op2 >= back_pos) continue;
        if (backedgeToPhi[phi->op2] == phi->id) continue;
        if (phi->type == IR_TYPE_NUM) {
            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getFP(ra, phi->op2, &srcReg, &srcMem, &srcOff);
            getFP(ra, phi->id,  &dstReg, &dstMem, &dstOff);
            int sr = srcReg; sljit_sw sw = srcOff;
            if (srcMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, srcReg, srcOff);
                sr = SLJIT_FR0; sw = 0;
            }
            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, sr, sw);
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff, SLJIT_FR1, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, 0, sr, sw);
            }
        } else {
            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, phi->op2, &srcReg, &srcMem, &srcOff);
            getGP(ra, phi->id,  &dstReg, &dstMem, &dstOff);
            int sr = srcReg; sljit_sw sw = srcOff;
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                sr = SLJIT_R0; sw = 0;
            }
            sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, sr, sw);
        }
    }
}

// Emit a single branch that is taken when the comparison node `cmp` is FALSE.
// Used by fused IR_LOOP_EXIT (IR_FLAG_FUSED_LOOP_EXIT): the exit test's falsy
// branch leaves the loop, so we branch directly on the comparison flags
// instead of materializing a bool. Operand loading mirrors the comparison
// codegen: INT operands compare as GP integers, NUM operands as doubles (an
// INT operand alongside a NUM operand is converted to double).
static struct sljit_jump* emitCmpFalsyBranch(struct sljit_compiler* C,
                                             const RegAllocState* ra,
                                             const IRBuffer* ir,
                                             const IRNode* cmp)
{
    if (cmp->op1 >= ir->count || cmp->op2 >= ir->count)
        return NULL;

    bool hasFP = (ir->nodes[cmp->op1].type == IR_TYPE_NUM) ||
                 (ir->nodes[cmp->op2].type == IR_TYPE_NUM);

    if (!hasFP) {
        // Integer comparison.
        int s1r, s1m; sljit_sw s1o;
        int s2r, s2m; sljit_sw s2o;
        getGP(ra, cmp->op1, &s1r, &s1m, &s1o);
        getGP(ra, cmp->op2, &s2r, &s2m, &s2o);
        int a = s1r, b = s2r;
        if (s1m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, s1r, s1o); a = SLJIT_R0; }
        if (s2m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, s2r, s2o); b = SLJIT_R1; }
        sljit_s32 inverse;
        switch (cmp->op) {
            case IR_LT:  inverse = SLJIT_SIG_GREATER_EQUAL; break;
            case IR_GT:  inverse = SLJIT_SIG_LESS_EQUAL; break;
            case IR_LTE: inverse = SLJIT_SIG_GREATER; break;
            case IR_GTE: inverse = SLJIT_SIG_LESS; break;
            case IR_EQ:  inverse = SLJIT_NOT_EQUAL; break;
            case IR_NEQ: inverse = SLJIT_EQUAL; break;
            default:     inverse = SLJIT_SIG_GREATER_EQUAL; break;
        }
        return sljit_emit_cmp(C, inverse, a, 0, b, 0);
    }

    // FP comparison.
    int src1Reg, src1Mem; sljit_sw src1Off;
    int src2Reg, src2Mem; sljit_sw src2Off;
    int s1r, s2r;
    if (ir->nodes[cmp->op1].type == IR_TYPE_INT) {
        getGP(ra, cmp->op1, &src1Reg, &src1Mem, &src1Off);
        int gp = src1Reg;
        if (src1Mem) {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, src1Reg, src1Off);
            gp = SLJIT_R0;
        }
        sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0, gp, 0);
        s1r = SLJIT_FR0;
    } else {
        getFP(ra, cmp->op1, &src1Reg, &src1Mem, &src1Off);
        s1r = src1Reg;
        if (src1Mem) {
            sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, src1Reg, src1Off);
            s1r = SLJIT_FR0;
        }
    }
    if (ir->nodes[cmp->op2].type == IR_TYPE_INT) {
        getGP(ra, cmp->op2, &src2Reg, &src2Mem, &src2Off);
        int gp = src2Reg;
        if (src2Mem) {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, src2Reg, src2Off);
            gp = SLJIT_R1;
        }
        sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, SLJIT_FR1, 0, gp, 0);
        s2r = SLJIT_FR1;
    } else {
        getFP(ra, cmp->op2, &src2Reg, &src2Mem, &src2Off);
        s2r = src2Reg;
        if (src2Mem) {
            sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, src2Reg, src2Off);
            s2r = SLJIT_FR1;
        }
    }
    sljit_s32 inverse;
    switch (cmp->op) {
        case IR_LT:  inverse = SLJIT_UNORDERED_OR_GREATER_EQUAL; break;
        case IR_GT:  inverse = SLJIT_UNORDERED_OR_LESS_EQUAL; break;
        case IR_LTE: inverse = SLJIT_UNORDERED_OR_GREATER; break;
        case IR_GTE: inverse = SLJIT_UNORDERED_OR_LESS; break;
        case IR_EQ:  inverse = SLJIT_UNORDERED_OR_NOT_EQUAL; break;
        case IR_NEQ: inverse = SLJIT_ORDERED_EQUAL; break;
        default:     inverse = SLJIT_UNORDERED_OR_GREATER_EQUAL; break;
    }
    return sljit_emit_fcmp(C, inverse, s1r, 0, s2r, 0);
}

JitTrace* wrenJitCodegen(void* vm, IRBuffer* ir, RegAllocState* ra,
                         uint8_t* anchorPC, void* modVarsBase)
{
    if (!ir || ir->count == 0 || !ra) return NULL;

    uint16_t exitsPerSnapshot[IR_MAX_SNAPSHOTS];
    memset(exitsPerSnapshot, 0, sizeof(exitsPerSnapshot));
    for (uint16_t i = 0; i < ir->count; i++) {
        const IRNode* n = &ir->nodes[i];
        if ((n->flags & IR_FLAG_DEAD) || n->op == IR_NOP) continue;
        // Never produce a trace containing an operation whose switch case is
        // still a placeholder; silently omitting it is guaranteed wrong-code.
        if (n->op == IR_CALL_C || n->op == IR_CALL_WREN)
            return NULL;
        if (n->op == IR_MOD && n->type != IR_TYPE_INT) {
            uint16_t sid = n->imm.snapshot_id;
            if (sid >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[sid] + 3 > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[sid] += 3;
            continue;
        }
        if (n->op == IR_TOGGLE_COUNT_BULK) {
            uint16_t fallback = n->imm.bulk.fallback;
            uint16_t complete = n->imm.bulk.snapshot;
            if (fallback >= IR_MAX_SNAPSHOTS || complete >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[fallback] + 3 > MAX_EXITS_PER_SNAP ||
                exitsPerSnapshot[complete] + 1 > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[fallback] += 3;
            exitsPerSnapshot[complete] += 1;
            continue;
        }
        if (n->op == IR_RANGE_SUM_BULK) {
            uint16_t fallback = n->imm.arith.fallback;
            uint16_t complete = n->imm.arith.snapshot;
            if (fallback >= IR_MAX_SNAPSHOTS || complete >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[fallback] + 7 > MAX_EXITS_PER_SNAP ||
                exitsPerSnapshot[complete] + 1 > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[fallback] += 7;
            exitsPerSnapshot[complete] += 1;
            continue;
        }
        if (n->op == IR_LIST_ITERATE) {
            uint16_t sid = n->imm.list.snapshot;
            if (sid >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[sid] + 1 > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[sid] += 1;
            continue;
        }
        if (n->op == IR_LIST_LOAD || n->op == IR_LIST_STORE) {
            uint16_t sid = n->imm.list.snapshot;
            // A bounds-hoisted access (ascending integral PHI, hoisted
            // count guard) emits no per-access exits; an ascending integral
            // PHI alone hoists the exactness and negative-index checks.
            int exits = 3;
            if (n->flags & IR_FLAG_BOUNDS_HOISTED) exits -= 1;
            if (isAscendingIntegralPhi(ir, n->op2)) exits -= 2;
            if (exits < 0) exits = 0;
            if (sid >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[sid] + exits > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[sid] = (uint16_t)(exitsPerSnapshot[sid] + exits);
            continue;
        }
        if (n->op == IR_LIST_BOUNDS_GUARD) {
            uint16_t sid = n->imm.bounds.snapshot;
            if (sid >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[sid] + 1 > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[sid] += 1;
            continue;
        }
        if (n->op == IR_MAP_PUT) {
            // The resize guard side-exits to the interpreter, which performs
            // the real wrenMapSet (resize + insert) and re-records the next
            // iteration at the new capacity.
            uint16_t sid = n->imm.map.snapshot;
            uint16_t mapExits = (n->flags & IR_FLAG_MAP_REUSE_PUT) ? 2 : 1;
            if (sid >= IR_MAX_SNAPSHOTS ||
                exitsPerSnapshot[sid] + mapExits > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[sid] += mapExits;
            continue;
        }
        uint16_t sid = IR_NONE;
        uint16_t needed = 0;
        if (n->op == IR_GUARD_CLASS) {
            sid = n->op2;
            needed = 1;
        } else if (n->op == IR_GUARD_NUM ||
                   n->op == IR_GUARD_NUM_OR_NULL ||
                   n->op == IR_GUARD_BOOL ||
                   n->op == IR_GUARD_TRUE || n->op == IR_GUARD_FALSE ||
                   n->op == IR_GUARD_NOT_NULL || n->op == IR_SIDE_EXIT) {
            sid = n->imm.snapshot_id;
            needed = 1;
        } else if (n->op == IR_GUARD_RANGE) {
            // Exact-integer roundtrip (FP operands only) + two magnitude
            // compares.
            sid = n->imm.snapshot_id;
            needed = n->op1 < ir->count &&
                     ir->nodes[n->op1].type == IR_TYPE_INT ? 2 : 3;
        }
        if (n->flags & IR_FLAG_INT_GUARD) {
            sid = n->imm.snapshot_id;
            needed = n->op == IR_UNBOX_INT ? 3 : (n->op == IR_MOD ? 3 : 2);
        }
        if (n->flags & IR_FLAG_FUSED_TRUE_GUARD) {
            sid = n->imm.snapshot_id;
            needed = 1;
        }
        if (sid != IR_NONE && sid < IR_MAX_SNAPSHOTS) {
            if ((uint32_t)exitsPerSnapshot[sid] + needed > MAX_EXITS_PER_SNAP)
                return NULL;
            exitsPerSnapshot[sid] = (uint16_t)(exitsPerSnapshot[sid] + needed);
        }
    }

    struct sljit_compiler* C = sljit_create_compiler(NULL);
    if (!C) return NULL;
    if (getenv("WREN_JIT_SLJIT_VERBOSE")) sljit_compiler_verbose(C, stderr);

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

    // Materialize the QNAN tag once so every numeric guard can compare against
    // a saved register instead of rebuilding the 64-bit constant.
    sljit_emit_op1(C, SLJIT_MOV, CONST_QNAN_REG, 0,
                   SLJIT_IMM, (sljit_sw)WREN_QNAN);

    // Reset the sunk-store writeback flag: within this invocation no loop
    // back-edge has run yet, so exit stubs must not overwrite module
    // variables that still hold their pre-loop values. The flag lives in
    // vm->jit, so codegen tests that pass a NULL vm (compile-only) skip it.
    if (vm != NULL) {
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0);
        EMIT_SET_FLAG_FROM_R0();
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
    struct sljit_jump* exitJumpArr[IR_MAX_SNAPSHOTS][MAX_EXITS_PER_SNAP];
    int exitJumpCount[IR_MAX_SNAPSHOTS];
    memset(exitJumpCount, 0, sizeof(exitJumpCount));
    memset(exitJumpArr, 0, sizeof(exitJumpArr));

    // A floating-point induction variable that starts as an exact,
    // non-negative integer and advances by a positive integer remains exact
    // for every in-bounds list access. Guard that fact once at trace entry
    // instead of round-tripping FP->integer->FP on every access.
    bool hoistedListIndex[IR_MAX_NODES];
    memset(hoistedListIndex, 0, sizeof(hoistedListIndex));
    if (maxSnapshots > 0) {
        for (uint16_t i = ir->loop_header; i < ir->count; i++) {
            const IRNode* node = &ir->nodes[i];
            if (!(node->flags & IR_FLAG_DEAD) &&
                (node->op == IR_LIST_LOAD || node->op == IR_LIST_STORE) &&
                isAscendingIntegralPhi(ir, node->op2))
                hoistedListIndex[node->op2] = true;
        }
    }

    // Label for loop header (set when we encounter IR_LOOP_HEADER).
    struct sljit_label* loopHeaderLabel = NULL;

    // Labels for nested loop headers, keyed by the header node id, so a
    // nested IR_LOOP_BACK can jump back to its own header.
    #define MAX_NESTED_LABELS 8
    struct sljit_label* nestedLoopLabels[MAX_NESTED_LABELS];
    uint16_t nestedLoopHeaderId[MAX_NESTED_LABELS];
    int nestedLoopDepth = 0;

    // Forward branches emitted by IR_LOOP_EXIT, bound when codegen reaches
    // the target node (the code just past the nested loop's back edge).
    struct sljit_jump* fwdExitJumps[MAX_NESTED_LABELS * 4];
    uint16_t fwdExitTargets[MAX_NESTED_LABELS * 4];
    int fwdExitCount = 0;

    // Coalesce loop back-edge values into their PHI registers when the value
    // has no other observable use. This turns `phi = phi + step` into an
    // in-place add and removes a move from every iteration.
    uint16_t backedgeToPhi[IR_MAX_NODES];
    for (uint16_t i = 0; i < ir->count; i++) backedgeToPhi[i] = IR_NONE;
    for (uint16_t p = 0; p < ir->loop_header && p < ir->count; p++) {
        const IRNode* phi = &ir->nodes[p];
        uint16_t back = phi->op2;
        if ((phi->flags & IR_FLAG_DEAD) || phi->op != IR_PHI ||
            back == IR_NONE || back >= ir->count || back <= phi->id ||
            ir->nodes[back].type != phi->type) continue;
        bool phiUsedAfterBackedge = false;
        for (uint16_t i = (uint16_t)(back + 1); i < ir->count; i++) {
            const IRNode* n = &ir->nodes[i];
            if ((n->flags & IR_FLAG_DEAD) || n->op == IR_LOOP_BACK) continue;
            if (n->op1 == phi->id ||
                (n->op != IR_GUARD_CLASS && n->op2 == phi->id)) {
                phiUsedAfterBackedge = true;
                break;
            }
        }
        if (!phiUsedAfterBackedge) {
            backedgeToPhi[back] = phi->id;
            // All later users of the back-edge value must read the register
            // updated in place, including snapshots and bound checks.
            ra->ssa_to_reg[back] = ra->ssa_to_reg[phi->id];
        }
    }

    // ---------------------------------------------------------------------------
    // Main code generation loop.
    // ---------------------------------------------------------------------------
    for (uint16_t i = 0; i < ir->count; i++) {
        const IRNode* n = &ir->nodes[i];

        // Bind any IR_LOOP_EXIT forward branches whose target is this node.
        // The after-loop code they jump to starts here.
        struct sljit_label* fwdLbl = NULL;
        for (int f = 0; f < fwdExitCount; f++) {
            if (fwdExitTargets[f] != i || fwdExitJumps[f] == NULL)
                continue;
            if (fwdLbl == NULL) fwdLbl = sljit_emit_label(C);
            sljit_set_label(fwdExitJumps[f], fwdLbl);
            fwdExitJumps[f] = NULL;
        }

        // Skip dead/nop nodes.
        if ((n->flags & IR_FLAG_DEAD) || n->op == IR_NOP)
            continue;

        // If this op uses a CONST whose node index is greater than its own
        // (strength-reduction appends reciprocals after the loop back-edge),
        // the constant won't be materialized during the linear sweep. Emit
        // its load now so the register holds the value before first use.
        materializeForwardConsts(C, ra, ir, (sljit_sw)tmpOff, i);

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

            // A raw numeric operand is stored directly: a Wren number Value
            // IS the IEEE-754 bit pattern, so the FP store performs the box.
            // This is how promoted NUM loop-carried PHIs re-sync the
            // interpreter stack without a GPR round-trip (see
            // irOptPromoteNestedBoxedPhis).
            if (valId < ir->count &&
                ir->nodes[valId].type == IR_TYPE_NUM) {
                int srcReg, srcMem;
                sljit_sw srcOff;
                getFP(ra, valId, &srcReg, &srcMem, &srcOff);
                if (srcMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                    srcReg, srcOff);
                    srcReg = SLJIT_FR0;
                }
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(REG_STACK_BASE),
                                (sljit_sw)(slot * 8), srcReg, 0);
                break;
            }

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

            // Integer source: the value is a raw int64 in a GP register
            // (CONST_INT, IV inference, int arithmetic — e.g. a loop-carried
            // iteration PHI promoted by irOptPromoteNestedBoxedPhis whose
            // post-loop load was forwarded to the PHI). UNBOX_NUM means
            // "produce the double", so SCVTF the int to a double. FMOV would
            // reinterpret the raw int bits as a denormal double (50 -> 2.3e-322),
            // breaking every numeric comparison that follows.
            if (valId < ir->count && ir->nodes[valId].type == IR_TYPE_INT) {
                int gpSrc = srcReg;
                if (srcMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                    gpSrc = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, gpSrc, 0);
                if (dstMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOFP,
                                    SLJIT_FR0, 0);
                } else {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, 0, SLJIT_FR0, 0);
                }
                break;
            }

            // Fast path: both operands in registers — use sljit_emit_fcopy
            // (maps to FMOV on ARM64, no memory round-trip).
            if (!srcMem && !dstMem) {
                sljit_emit_fcopy(C, SLJIT_COPY_TO_F64, dstReg, srcReg);
            } else if (!srcMem && dstMem) {
                sljit_emit_fcopy(C, SLJIT_COPY_TO_F64, SLJIT_FR0, srcReg);
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOFP, SLJIT_FR0, 0);
            } else {
                // Slow path: source is spilled — use temp area.
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
            }
            break;
        }

        case IR_BOX_NUM: {
            // double (FP) -> Value (GP, uint64).
            // Store FP to temp area, then load as GP.
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;
            if (n->flags & IR_FLAG_SNAPSHOT_ONLY_BOX) break;

            // If the source is an integer (CONST_INT, or an int produced by
            // IV inference / integer arithmetic), the value lives in a GP
            // register, not an FP one. Box it through the integer path:
            // SCVTF converts the int64 to a double, and FMOV reinterprets the
            // double bits as the NaN-tagged Value (a Wren number IS its own
            // boxed form). Reading getFP here would box whatever happens to be
            // in the FP register (often a stale hoisted load), silently
            // corrupting the value.
            if (valId < ir->count && ir->nodes[valId].type == IR_TYPE_INT) {
                int srcReg, srcMem; sljit_sw srcOff;
                getGP(ra, valId, &srcReg, &srcMem, &srcOff);
                int dstReg, dstMem; sljit_sw dstOff;
                getGP(ra, n->id,  &dstReg, &dstMem, &dstOff);
                int gpSrc = srcReg;
                if (srcMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                    gpSrc = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, gpSrc, 0);
                if (dstMem) {
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64,
                                     SLJIT_FR0, SLJIT_R0);
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
                } else {
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, dstReg);
                }
                break;
            }

            int srcReg, srcMem;
            sljit_sw srcOff;
            getFP(ra, valId, &srcReg, &srcMem, &srcOff);

            int dstReg, dstMem;
            sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            // Fast path: both operands in registers — use sljit_emit_fcopy.
            if (!srcMem && !dstMem) {
                sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, srcReg, dstReg);
            } else if (!srcMem && dstMem) {
                sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, srcReg, SLJIT_R0);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                // Slow path: source is spilled — use temp area.
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
            }
            break;
        }

        case IR_UNBOX_INT: {
            // raw int64 (GP) <- NaN-tagged Value (GP) or raw double (FP).
            // A bitwise operand like the literal `1` is recorded as CONST_NUM,
            // which the register allocator keeps in an FP register. A raw
            // double IS its own boxed form, so the two source kinds differ
            // only in how the value reaches FR0:
            //   boxed Value: FMOV freg, gpreg  (reinterpret bits)
            //   raw double:  MOV  freg <- fpreg (already a double)
            // then FCVTZS dstreg, freg (truncate-toward-zero).
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id,  &dstReg, &dstMem, &dstOff);

            if (valId < ir->count &&
                ir->nodes[valId].type == IR_TYPE_INT) {
                // Source is already a raw int64 in a GP register (CONST_INT,
                // IV inference, or int arithmetic): unboxing is the identity.
                // Do NOT reinterpret through an FP register (FMOV of the raw
                // int bits is a denormal double, so an integrality guard would
                // spuriously fire) and emit no guard: the value is integral by
                // construction.
                int srcReg, srcMem; sljit_sw srcOff;
                getGP(ra, valId, &srcReg, &srcMem, &srcOff);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstMem ? dstOff : 0,
                               srcReg, srcMem ? srcOff : 0);
                break;
            }

            if (valId < ir->count &&
                ir->nodes[valId].type == IR_TYPE_NUM) {
                // Source is a raw double in an FP register: it is already the
                // boxed bit pattern, so copy it straight to FR0.
                int srcReg, srcMem; sljit_sw srcOff;
                getFP(ra, valId, &srcReg, &srcMem, &srcOff);
                sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                srcReg, srcMem ? srcOff : 0);
            } else {
                // Source is a NaN-tagged Value in a GP register.
                int srcReg, srcMem; sljit_sw srcOff;
                getGP(ra, valId, &srcReg, &srcMem, &srcOff);
                int gpSrc = srcReg;
                if (srcMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                    gpSrc = SLJIT_R0;
                }
                sljit_emit_fcopy(C, SLJIT_COPY_TO_F64, SLJIT_FR0, gpSrc);
            }

            // FCVTZS → truncate double to signed integer.
            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                                SLJIT_R0, 0, SLJIT_FR0, 0);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                                dstReg, 0, SLJIT_FR0, 0);
            }

            // A loop-body conversion (e.g. dna[i] feeding integer arithmetic)
            // is not covered by the LOOP_HEADER pre-header guard scan, so emit
            // the integrality check inline: re-convert the truncated int and
            // compare against the source double, plus the |value| < 2^53 range
            // test that keeps the FP representation exact. FR0 still holds the
            // source double (FCVTZS leaves its source untouched).
            if (n->flags & IR_FLAG_INT_GUARD) {
                uint16_t snapId = n->imm.snapshot_id;
                int resultReg = dstReg;
                if (dstMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, dstReg, dstOff);
                    resultReg = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR1, 0, resultReg, 0);
                struct sljit_jump* checks[3];
                checks[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                            SLJIT_FR0, 0, SLJIT_FR1, 0);
                checks[1] = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
                    resultReg, 0, SLJIT_IMM, (sljit_sw)(1LL << 53));
                checks[2] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                    resultReg, 0, SLJIT_IMM, (sljit_sw)-(1LL << 53));
                for (int ck = 0; ck < 3; ck++) {
                    if (snapId < (uint16_t)maxSnapshots &&
                        exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = checks[ck];
                }
            }

            break;
        }

        case IR_BOX_INT: {
            // raw int64 (GP) -> NaN-tagged Value (GP).
            // Convert integer to double, then bit-reinterpret as GP:
            //   SCVTF freg, gpreg
            //   FMOV  dstreg, freg
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;
            if (n->flags & IR_FLAG_SNAPSHOT_ONLY_BOX) break;

            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);
            getGP(ra, n->id,  &dstReg, &dstMem, &dstOff);

            // Load source into a GP scratch if spilled.
            int gpSrc = srcReg;
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                gpSrc = SLJIT_R0;
            }

            // SCVTF freg ← gpreg (integer → double).
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, gpSrc, 0);

            // FMOV gpreg ← freg (bit-reinterpret FP as GP).
            if (dstMem) {
                sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, SLJIT_R0);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, dstReg);
            }
            break;
        }

        case IR_BOOL_NOT: {
            // Wren's boolean tags differ only in bit zero. The recorder emits
            // GUARD_BOOL before this operation, so XOR is a complete and safe
            // implementation of logical negation for the admitted pattern.
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;
            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                srcReg = SLJIT_R0;
            }
            if (dstMem) {
                sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0, srcReg, 0,
                               SLJIT_IMM, 1);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
            } else {
                sljit_emit_op2(C, SLJIT_XOR, dstReg, 0, srcReg, 0,
                               SLJIT_IMM, 1);
            }
            break;
        }

        case IR_BOOL_TO_NUM: {
            // FALSE_VAL and TRUE_VAL differ in their low bit in Wren's
            // NaN-boxed representation. GUARD_BOOL proves no other tag can
            // reach this conversion.
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;
            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, 0);
            }
            if (dstMem) {
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 1);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff,
                               SLJIT_R0, 0);
            } else {
                sljit_emit_op2(C, SLJIT_AND, dstReg, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 1);
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

        // ----- Arithmetic (FP or integer) -----
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV: {
            // Integer path: when the node type is IR_TYPE_INT use GP integer ops.
            if (n->type == IR_TYPE_INT &&
                (n->op == IR_ADD || n->op == IR_SUB || n->op == IR_MUL)) {
                sljit_s32 iop;
                switch (n->op) {
                    case IR_ADD: iop = SLJIT_ADD; break;
                    case IR_SUB: iop = SLJIT_SUB; break;
                    case IR_MUL: iop = SLJIT_MUL; break;
                    default:     iop = SLJIT_ADD; break;
                }

                int s1r, s1m; sljit_sw s1o;
                int s2r, s2m; sljit_sw s2o;
                int dr, dm; sljit_sw dof;
                getGP(ra, n->op1, &s1r, &s1m, &s1o);
                getGP(ra, n->op2, &s2r, &s2m, &s2o);
                getGP(ra, n->id,  &dr,  &dm,  &dof);
                if (n->id < ir->count && backedgeToPhi[n->id] != IR_NONE)
                    getGP(ra, backedgeToPhi[n->id], &dr, &dm, &dof);

                // Load spilled operands into scratch GP regs.
                int a = s1r, b = s2r;
                if (s1m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, s1r, s1o); a = SLJIT_R0; }
                if (s2m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, s2r, s2o); b = SLJIT_R1; }

                if (dm) {
                    sljit_emit_op2(C, iop, SLJIT_R0, 0, a, 0, b, 0);
                    sljit_emit_op1(C, SLJIT_MOV, dr, dof, SLJIT_R0, 0);
                } else {
                    sljit_emit_op2(C, iop, dr, 0, a, 0, b, 0);
                }

                if (n->flags & IR_FLAG_INT_GUARD) {
                    uint16_t snapId = n->imm.snapshot_id;
                    int resultReg = dr;
                    if (dm) {
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, dr, dof);
                        resultReg = SLJIT_R0;
                    }
                    struct sljit_jump* checks[2];
                    checks[0] = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
                        resultReg, 0, SLJIT_IMM, (sljit_sw)(1LL << 53));
                    checks[1] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                        resultReg, 0, SLJIT_IMM, (sljit_sw)-(1LL << 53));
                    for (int ck = 0; ck < 2; ck++) {
                        if (snapId < (uint16_t)maxSnapshots &&
                            exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                            exitJumpArr[snapId][exitJumpCount[snapId]++] = checks[ck];
                    }
                }
                break;
            }

            // FP path (default).
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
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);
            if (n->id < ir->count && backedgeToPhi[n->id] != IR_NONE)
                getFP(ra, backedgeToPhi[n->id], &dstReg, &dstMem, &dstOff);

            // SLJIT fop2 can handle memory operands directly in some cases,
            // but for safety, load spilled operands into scratch FP regs.
            int s1r = 0, s2r = 0, dr = dstReg;
            sljit_sw s1w = 0, s2w = 0, dw = 0;

            if (ir->nodes[n->op1].type == IR_TYPE_INT) {
                getGP(ra, n->op1, &src1Reg, &src1Mem, &src1Off);
                int gp = src1Reg;
                if (src1Mem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   src1Reg, src1Off);
                    gp = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, gp, 0);
                s1r = SLJIT_FR0;
            } else {
                getFP(ra, n->op1, &src1Reg, &src1Mem, &src1Off);
                s1r = src1Reg;
                if (src1Mem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                    src1Reg, src1Off);
                    s1r = SLJIT_FR0;
                }
            }

            if (ir->nodes[n->op2].type == IR_TYPE_INT) {
                getGP(ra, n->op2, &src2Reg, &src2Mem, &src2Off);
                int gp = src2Reg;
                if (src2Mem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   src2Reg, src2Off);
                    gp = SLJIT_R1;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR1, 0, gp, 0);
                s2r = SLJIT_FR1;
            } else {
                getFP(ra, n->op2, &src2Reg, &src2Mem, &src2Off);
                s2r = src2Reg;
                if (src2Mem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0,
                                    src2Reg, src2Off);
                    s2r = SLJIT_FR1;
                }
            }

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

        case IR_SQRT:
        case IR_FLOOR: {
            int srcReg, srcMem; sljit_sw srcOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);

            int sr;
            if (n->op1 < ir->count &&
                ir->nodes[n->op1].type == IR_TYPE_INT) {
                // The IV pass can promote the operand to a raw int64 (e.g.
                // i*i feeding sqrt). Convert it to a double before the FP
                // operation; getFP on the INT-typed value would read a GP
                // register as float bits.
                int gr, gm; sljit_sw go;
                getGP(ra, n->op1, &gr, &gm, &go);
                if (gm) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, gr, go);
                    gr = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, gr, 0);
                sr = SLJIT_FR0;
            } else {
                getFP(ra, n->op1, &srcReg, &srcMem, &srcOff);
                sr = srcReg;
                if (srcMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                    srcReg, srcOff);
                    sr = SLJIT_FR0;
                }
            }

            int dr = dstMem ? SLJIT_FR1 : dstReg;

#if defined(SLJIT_CONFIG_ARM_64) && SLJIT_CONFIG_ARM_64
            sljit_s32 rn = sljit_get_register_index(SLJIT_FLOAT_REGISTER, sr);
            sljit_s32 rd = sljit_get_register_index(SLJIT_FLOAT_REGISTER, dr);
            sljit_u32 instruction =
                (n->op == IR_SQRT ? 0x1e61c000u : 0x1e654000u) |
                ((sljit_u32)rn << 5) | (sljit_u32)rd;
            if (rn < 0 || rd < 0 ||
                sljit_emit_op_custom(C, &instruction, sizeof(instruction)) !=
                    SLJIT_SUCCESS) {
                sljit_free_compiler(C);
                free(exitJumps);
                return NULL;
            }
#else
            // These instructions are currently emitted directly on AArch64.
            // Refuse the trace elsewhere until an equally safe backend exists.
            sljit_free_compiler(C);
            free(exitJumps);
            return NULL;
#endif
            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff,
                                SLJIT_FR1, 0);
            }
            break;
        }

        // ----- Comparison (FP or integer -> bool in GP) -----
        case IR_VAL_EQ:
        case IR_VAL_NEQ: {
            // 64-bit compare of two boxed Wren Values -> raw bool. Used for
            // ==/!= against null/bool, where identity is the whole meaning.
            int s1r, s1m; sljit_sw s1o;
            int s2r, s2m; sljit_sw s2o;
            int dr, dm;   sljit_sw dof;
            getGP(ra, n->op1, &s1r, &s1m, &s1o);
            getGP(ra, n->op2, &s2r, &s2m, &s2o);
            getGP(ra, n->id,  &dr,  &dm,  &dof);

            int a = s1r, b = s2r;
            if (s1m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, s1r, s1o); a = SLJIT_R0; }
            if (s2m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, s2r, s2o); b = SLJIT_R1; }

            sljit_s32 condition = (n->op == IR_VAL_EQ) ? SLJIT_EQUAL
                                                       : SLJIT_NOT_EQUAL;
            struct sljit_jump* isTrue =
                sljit_emit_cmp(C, condition, a, 0, b, 0);
            int out = dm ? SLJIT_R0 : dr;
            sljit_emit_op1(C, SLJIT_MOV, out, 0, SLJIT_IMM, 0);
            struct sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);
            struct sljit_label* trueLabel = sljit_emit_label(C);
            sljit_set_label(isTrue, trueLabel);
            sljit_emit_op1(C, SLJIT_MOV, out, 0, SLJIT_IMM, 1);
            struct sljit_label* doneLabel = sljit_emit_label(C);
            sljit_set_label(done, doneLabel);
            if (dm) sljit_emit_op1(C, SLJIT_MOV, dr, dof, out, 0);
            break;
        }

        case IR_SELECT_NULL: {
            int tr, tm; sljit_sw to;
            int nr, nm; sljit_sw no;
            int vr, vm; sljit_sw vo;
            int dr, dm; sljit_sw dof;
            getGP(ra, n->op1, &tr, &tm, &to);
            getGP(ra, n->op2, &nr, &nm, &no);
            getGP(ra, n->imm.select.value, &vr, &vm, &vo);
            getGP(ra, n->id, &dr, &dm, &dof);

            int out = dm ? SLJIT_R0 : dr;
            sljit_emit_op1(C, SLJIT_MOV, out, 0, vr, vm ? vo : 0);
            sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
                            tr, tm ? to : 0,
                            SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);
            sljit_emit_select(C, SLJIT_EQUAL, out, nr, nm ? no : 0, out);
            if (dm)
                sljit_emit_op1(C, SLJIT_MOV, dr, dof, out, 0);
            break;
        }

        case IR_LT:
        case IR_GT:
        case IR_LTE:
        case IR_GTE:
        case IR_EQ:
        case IR_NEQ: {
            // Fused into an IR_LOOP_EXIT (IR_FLAG_FUSED_LOOP_EXIT): the bool
            // result is dead; LOOP_EXIT codegen branches on the operands
            // directly, so the comparison itself emits nothing.
            if (n->flags & IR_FLAG_FUSED_LOOP_EXIT) break;

            // Integer path: when type is IR_TYPE_INT, emit integer compare.
            if (n->type == IR_TYPE_INT) {
                int s1r, s1m; sljit_sw s1o;
                int s2r, s2m; sljit_sw s2o;
                int dr, dm;   sljit_sw dof;
                getGP(ra, n->op1, &s1r, &s1m, &s1o);
                getGP(ra, n->op2, &s2r, &s2m, &s2o);
                getGP(ra, n->id,  &dr,  &dm,  &dof);

                int a = s1r, b = s2r;
                if (s1m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, s1r, s1o); a = SLJIT_R0; }
                if (s2m) { sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, s2r, s2o); b = SLJIT_R1; }

                // Materialize the signed comparison with explicit control
                // flow. SLJIT's SET flag encoding only accepts the underlying
                // even condition for complementary predicates, which made the
                // previous SET(SIG_LESS_EQUAL/EQUAL) construction assert in
                // argument-checking builds.
                sljit_s32 condition;
                switch (n->op) {
                    case IR_LT:  condition = SLJIT_SIG_LESS; break;
                    case IR_GT:  condition = SLJIT_SIG_GREATER; break;
                    case IR_LTE: condition = SLJIT_SIG_LESS_EQUAL; break;
                    case IR_GTE: condition = SLJIT_SIG_GREATER_EQUAL; break;
                    case IR_EQ:  condition = SLJIT_EQUAL; break;
                    case IR_NEQ: condition = SLJIT_NOT_EQUAL; break;
                    default:     condition = SLJIT_SIG_LESS; break;
                }

                if (n->flags & IR_FLAG_FUSED_TRUE_GUARD) {
                    sljit_s32 inverse;
                    switch (n->op) {
                        case IR_LT:  inverse = SLJIT_SIG_GREATER_EQUAL; break;
                        case IR_GT:  inverse = SLJIT_SIG_LESS_EQUAL; break;
                        case IR_LTE: inverse = SLJIT_SIG_GREATER; break;
                        case IR_GTE: inverse = SLJIT_SIG_LESS; break;
                        case IR_EQ:  inverse = SLJIT_NOT_EQUAL; break;
                        case IR_NEQ: inverse = SLJIT_EQUAL; break;
                        default:     inverse = SLJIT_SIG_GREATER_EQUAL; break;
                    }
                    struct sljit_jump* exit =
                        sljit_emit_cmp(C, inverse, a, 0, b, 0);
                    uint16_t snapId = n->imm.snapshot_id;
                    if (snapId < (uint16_t)maxSnapshots &&
                        exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = exit;
                    break;
                }

                struct sljit_jump* isTrue =
                    sljit_emit_cmp(C, condition, a, 0, b, 0);
                int out = dm ? SLJIT_R0 : dr;
                sljit_emit_op1(C, SLJIT_MOV, out, 0, SLJIT_IMM, 0);
                struct sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);
                struct sljit_label* trueLabel = sljit_emit_label(C);
                sljit_set_label(isTrue, trueLabel);
                sljit_emit_op1(C, SLJIT_MOV, out, 0, SLJIT_IMM, 1);
                struct sljit_label* doneLabel = sljit_emit_label(C);
                sljit_set_label(done, doneLabel);
                if (dm) sljit_emit_op1(C, SLJIT_MOV, dr, dof, out, 0);
                break;
            }

            int src1Reg, src1Mem; sljit_sw src1Off;
            int src2Reg, src2Mem; sljit_sw src2Off;

            int s1r, s2r;
            if (ir->nodes[n->op1].type == IR_TYPE_INT) {
                getGP(ra, n->op1, &src1Reg, &src1Mem, &src1Off);
                int gp = src1Reg;
                if (src1Mem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   src1Reg, src1Off);
                    gp = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, gp, 0);
                s1r = SLJIT_FR0;
            } else {
                getFP(ra, n->op1, &src1Reg, &src1Mem, &src1Off);
                s1r = src1Reg;
                if (src1Mem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                    src1Reg, src1Off);
                    s1r = SLJIT_FR0;
                }
            }
            if (ir->nodes[n->op2].type == IR_TYPE_INT) {
                getGP(ra, n->op2, &src2Reg, &src2Mem, &src2Off);
                int gp = src2Reg;
                if (src2Mem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   src2Reg, src2Off);
                    gp = SLJIT_R1;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR1, 0, gp, 0);
                s2r = SLJIT_FR1;
            } else {
                getFP(ra, n->op2, &src2Reg, &src2Mem, &src2Off);
                s2r = src2Reg;
                if (src2Mem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0,
                                    src2Reg, src2Off);
                    s2r = SLJIT_FR1;
                }
            }

            if (n->flags & IR_FLAG_FUSED_TRUE_GUARD) {
                sljit_s32 inverse;
                switch (n->op) {
                    case IR_LT:  inverse = SLJIT_UNORDERED_OR_GREATER_EQUAL; break;
                    case IR_GT:  inverse = SLJIT_UNORDERED_OR_LESS_EQUAL; break;
                    case IR_LTE: inverse = SLJIT_UNORDERED_OR_GREATER; break;
                    case IR_GTE: inverse = SLJIT_UNORDERED_OR_LESS; break;
                    case IR_EQ:  inverse = SLJIT_UNORDERED_OR_NOT_EQUAL; break;
                    case IR_NEQ: inverse = SLJIT_ORDERED_EQUAL; break;
                    default:     inverse = SLJIT_UNORDERED_OR_GREATER_EQUAL; break;
                }
                struct sljit_jump* exit =
                    sljit_emit_fcmp(C, inverse, s1r, 0, s2r, 0);
                uint16_t snapId = n->imm.snapshot_id;
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = exit;
                break;
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
                // Wren Num != follows C/IEEE semantics: unordered (NaN)
                // comparisons are also not equal.
                case IR_NEQ: cmpFlag = SLJIT_SET_UNORDERED_OR_NOT_EQUAL; resultFlag = SLJIT_UNORDERED_OR_NOT_EQUAL; break;
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
                SLJIT_R0, 0, CONST_QNAN_REG, 0);

            if (snapId < (uint16_t)maxSnapshots &&
                exitJumpCount[snapId] < MAX_EXITS_PER_SNAP) {
                exitJumpArr[snapId][exitJumpCount[snapId]++] = jmp;
            }
            break;
        }

        case IR_GUARD_NUM_OR_NULL: {
            // Materialize `tagged && value != null` without branching on the
            // common number/null distinction. The sole branch is the cold
            // side exit for every other Wren type.
            uint16_t valId = n->op1;
            uint16_t snapId = n->imm.snapshot_id;
            if (valId == IR_NONE) break;
            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);
            int src = srcMem ? SLJIT_R0 : srcReg;
            if (srcMem)
                sljit_emit_op1(C, SLJIT_MOV, src, 0, srcReg, srcOff);

            sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z, src, 0,
                            SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);
            sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R1, 0,
                                SLJIT_NOT_EQUAL);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, src, 0,
                           SLJIT_IMM, (sljit_sw)WREN_QNAN);
            sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0,
                            CONST_QNAN_REG, 0);
            sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_EQUAL);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_R1, 0);
            struct sljit_jump* jmp = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                SLJIT_R0, 0, SLJIT_IMM, 0);
            if (snapId < (uint16_t)maxSnapshots &&
                exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                exitJumpArr[snapId][exitJumpCount[snapId]++] = jmp;
            break;
        }

        case IR_GUARD_RANGE: {
            // Assert op1 is an exact integer with |op1| <= imm.arith.limit
            // (the limit is a constant node; imm.arith.snapshot is the deopt
            // point). Emitted pre-header by the denom-recurrence pass: proving
            // the loop inputs stay small lets the pass drop per-iteration
            // IR_FLAG_INT_GUARD checks and the floor-chain rewrite stays exact.
            uint16_t valId = n->op1;
            uint16_t limitId = n->imm.arith.limit;
            uint16_t snapId = n->imm.arith.snapshot;
            if (valId == IR_NONE || valId >= ir->count ||
                limitId >= ir->count) break;
            if (ir->nodes[limitId].op != IR_CONST_NUM &&
                ir->nodes[limitId].op != IR_CONST_INT) break;

            sljit_sw lim = (ir->nodes[limitId].op == IR_CONST_INT)
                ? (sljit_sw)ir->nodes[limitId].imm.i64
                : (sljit_sw)ir->nodes[limitId].imm.num;

            struct sljit_jump* js[3];
            int numJumps = 0;

            if (ir->nodes[valId].type == IR_TYPE_INT) {
                // Integer operand: exact by construction, range check only.
                int vr, vm; sljit_sw vo;
                getGP(ra, valId, &vr, &vm, &vo);
                int vg = vr;
                if (vm) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, vr, vo);
                    vg = SLJIT_R0;
                }
                js[numJumps++] = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
                    vg, 0, SLJIT_IMM, lim);
                js[numJumps++] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                    vg, 0, SLJIT_IMM, -lim);
            } else {
                // FP operand: round-trip through integer to prove the value is
                // an exact integer, then check the magnitude.
                int vr, vm; sljit_sw vo;
                getFP(ra, valId, &vr, &vm, &vo);
                int vf = vr; sljit_sw vw = vo;
                if (vm) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, vr, vo);
                    vf = SLJIT_FR0; vw = 0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64, SLJIT_R0, 0,
                                vf, vw);
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0,
                                SLJIT_R0, 0);
                // Not an exact integer (or NaN/inf): side exit.
                js[numJumps++] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                    vf, vw, SLJIT_FR0, 0);
                js[numJumps++] = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
                    SLJIT_R0, 0, SLJIT_IMM, lim);
                js[numJumps++] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                    SLJIT_R0, 0, SLJIT_IMM, -lim);
            }

            for (int k = 0; k < numJumps; k++) {
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = js[k];
            }
            break;
        }

        case IR_GUARD_BOOL: {
            // FALSE_VAL and TRUE_VAL differ only in bit zero. Mask that bit
            // and reject every other Wren value before BOOL_NOT can use XOR.
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
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)~(sljit_uw)1);
            struct sljit_jump* jmp = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);
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

            // R1 = raw NaN-tagged Value (SIGN_BIT | QNAN | obj_ptr).
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, 0);
            }

            // Unmask to get obj pointer: ptr = val & ~(SIGN_BIT | QNAN)
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));

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

        case IR_TOGGLE_COUNT_BULK: {
            uint16_t iterId = n->op1;
            uint16_t countId = n->op2;
            uint16_t limitId = n->imm.bulk.limit;
            uint16_t stateId = n->imm.bulk.state;
            uint16_t objectId = n->imm.bulk.object;
            uint16_t fallback = n->imm.bulk.fallback;
            uint16_t complete = n->imm.bulk.snapshot;

            int irg, im; sljit_sw io;
            int lrg, lm; sljit_sw lo;
            getFP(ra, iterId, &irg, &im, &io);
            getFP(ra, limitId, &lrg, &lm, &lo);

            // Exact integer conversion of iterator and inclusive limit.
            sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                            SLJIT_R0, 0, irg, io);
            sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                            SLJIT_R1, 0, lrg, lo);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, SLJIT_R0, 0);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR1, 0, SLJIT_R1, 0);
            struct sljit_jump* bad[3];
            bad[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                     irg, io, SLJIT_FR0, 0);
            bad[1] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                     lrg, lo, SLJIT_FR1, 0);

            // R0 = number of valid iterations still remaining.
            sljit_emit_op2(C, SLJIT_SUB, SLJIT_R0, 0,
                           SLJIT_R1, 0, SLJIT_R0, 0);
            bad[2] = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
                                    SLJIT_R0, 0, SLJIT_IMM, 0);
            for (int ck = 0; ck < 3; ck++) {
                if (fallback < (uint16_t)maxSnapshots &&
                    exitJumpCount[fallback] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[fallback][exitJumpCount[fallback]++] = bad[ck];
            }

            // An odd number of toggles flips the field; an even number leaves
            // it untouched. Toggle in memory without consuming R0 (remaining).
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
                           SLJIT_R0, 0, SLJIT_IMM, 1);
            struct sljit_jump* even = sljit_emit_cmp(
                C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
            int org, om; sljit_sw oo;
            getGP(ra, objectId, &org, &om, &oo);
            if (om) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, org, oo);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, org, 0);
            }
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
                           SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_sw fieldOff = OBJ_FIELDS_OFFSET +
                                (sljit_sw)n->imm.bulk.field * 8;
            sljit_emit_op2(C, SLJIT_XOR,
                           SLJIT_MEM1(SLJIT_R1), fieldOff,
                           SLJIT_MEM1(SLJIT_R1), fieldOff,
                           SLJIT_IMM, 1);
            sljit_set_label(even, sljit_emit_label(C));

            // true results among N toggles are
            // floor((N + 1 - initialState) / 2).
            int srg, sm; sljit_sw so;
            getGP(ra, stateId, &srg, &sm, &so);
            if (sm) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srg, so);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srg, 0);
            }
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
                           SLJIT_R1, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_SUB, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_op2(C, SLJIT_ASHR, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_IMM, 1);

            int crg, cm; sljit_sw co;
            getGP(ra, countId, &crg, &cm, &co);
            if (cm) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, crg, co);
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
                               SLJIT_R1, 0, SLJIT_R0, 0);
                sljit_emit_op1(C, SLJIT_MOV, crg, co, SLJIT_R1, 0);
            } else {
                sljit_emit_op2(C, SLJIT_ADD, crg, 0,
                               crg, 0, SLJIT_R0, 0);
            }

            struct sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);
            if (complete < (uint16_t)maxSnapshots &&
                exitJumpCount[complete] < MAX_EXITS_PER_SNAP)
                exitJumpArr[complete][exitJumpCount[complete]++] = done;
            break;
        }

        case IR_RANGE_SUM_BULK: {
            uint16_t iterId = n->op1;
            uint16_t sumId = n->op2;
            uint16_t limitId = n->imm.arith.limit;
            uint16_t fallback = n->imm.arith.fallback;
            uint16_t complete = n->imm.arith.snapshot;
            int irg, im; sljit_sw io;
            int lrg, lm; sljit_sw lo;
            int srg, sm; sljit_sw so;
            getFP(ra, iterId, &irg, &im, &io);
            getFP(ra, limitId, &lrg, &lm, &lo);
            getFP(ra, sumId, &srg, &sm, &so);

            struct sljit_jump* bad[7];
            sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                            SLJIT_R0, 0, irg, io);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, SLJIT_R0, 0);
            bad[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                     irg, io, SLJIT_FR0, 0);
            sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                            SLJIT_R0, 0, lrg, lo);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, SLJIT_R0, 0);
            bad[1] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                     lrg, lo, SLJIT_FR0, 0);
            sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                            SLJIT_R0, 0, srg, so);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, SLJIT_R0, 0);
            bad[2] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                     srg, so, SLJIT_FR0, 0);

            union { double d; sljit_sw w; } bits;
            bits.d = 0.0;
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, bits.w);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff,
                           SLJIT_R0, 0);
            bad[3] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_LESS,
                                     irg, io, SLJIT_MEM1(SLJIT_SP), tmpOff);
            bad[4] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_LESS,
                                     lrg, lo, irg, io);
            bad[5] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_LESS,
                                     srg, so, SLJIT_MEM1(SLJIT_SP), tmpOff);

            // remaining sum = (limit - iter) * (iter + limit + 1) / 2
            sljit_emit_fop2(C, SLJIT_SUB_F64, SLJIT_FR0, 0,
                            lrg, lo, irg, io);
            sljit_emit_fop2(C, SLJIT_ADD_F64, SLJIT_FR1, 0,
                            irg, io, lrg, lo);
            bits.d = 1.0;
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, bits.w);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff,
                           SLJIT_R0, 0);
            sljit_emit_fop2(C, SLJIT_ADD_F64, SLJIT_FR1, 0,
                            SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), tmpOff);
            sljit_emit_fop2(C, SLJIT_MUL_F64, SLJIT_FR0, 0,
                            SLJIT_FR0, 0, SLJIT_FR1, 0);
            bits.d = 0.5;
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, bits.w);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff,
                           SLJIT_R0, 0);
            sljit_emit_fop2(C, SLJIT_MUL_F64, SLJIT_FR0, 0,
                            SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), tmpOff);
            sljit_emit_fop2(C, SLJIT_ADD_F64, SLJIT_FR0, 0,
                            SLJIT_FR0, 0, srg, so);

            bits.d = (double)(1LL << 53);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, bits.w);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff,
                           SLJIT_R0, 0);
            bad[6] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_GREATER,
                                     SLJIT_FR0, 0,
                                     SLJIT_MEM1(SLJIT_SP), tmpOff);
            for (int ck = 0; ck < 7; ck++) {
                if (fallback < (uint16_t)maxSnapshots &&
                    exitJumpCount[fallback] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[fallback][exitJumpCount[fallback]++] = bad[ck];
            }

            if (sm) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, srg, so, SLJIT_FR0, 0);
            } else {
                sljit_emit_fop1(C, SLJIT_MOV_F64, srg, 0, SLJIT_FR0, 0);
            }
            struct sljit_jump* done = sljit_emit_jump(C, SLJIT_JUMP);
            if (complete < (uint16_t)maxSnapshots &&
                exitJumpCount[complete] < MAX_EXITS_PER_SNAP)
                exitJumpArr[complete][exitJumpCount[complete]++] = done;
            break;
        }

        // ----- Hoisted list bounds guard -----
        case IR_LIST_BOUNDS_GUARD: {
            // Guard list.count >= limit (direction 0, index < limit) or
            // list.count > limit (direction 1, index <= limit) once at loop
            // entry. The list's hoisted GUARD_CLASS has already run, so its
            // count field is safe to read. The limit is a raw double.
            int objReg, objMem; sljit_sw objOff;
            getGP(ra, n->op1, &objReg, &objMem, &objOff);
            if (objMem)
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
            else
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R1), LIST_COUNT_OFFSET);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, SLJIT_R0, 0);

            int limReg, limMem; sljit_sw limOff;
            int fpLim; sljit_sw fpLimOff = 0;
            if (ir->nodes[n->op2].type == IR_TYPE_INT) {
                getGP(ra, n->op2, &limReg, &limMem, &limOff);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                               limReg, limMem ? limOff : 0);
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR1, 0, SLJIT_R1, 0);
                fpLim = SLJIT_FR1;
            } else {
                getFP(ra, n->op2, &limReg, &limMem, &limOff);
                fpLim = limReg;
                if (limMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0,
                                    limReg, limOff);
                    fpLim = SLJIT_FR1;
                }
            }

            uint16_t snapId = n->imm.bounds.snapshot;
            struct sljit_jump* bad;
            if (n->imm.bounds.direction == 0) {
                // index < limit: exit when count < limit.
                bad = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_LESS,
                                      SLJIT_FR0, 0, fpLim, fpLimOff);
            } else {
                // index <= limit: exit when count <= limit.
                bad = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_LESS_EQUAL,
                                      SLJIT_FR0, 0, fpLim, fpLimOff);
            }
            if (snapId < (uint16_t)maxSnapshots &&
                exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                exitJumpArr[snapId][exitJumpCount[snapId]++] = bad;
            break;
        }

        // ----- Cached list elements.data pointer -----
        case IR_LIST_DATA: {
            // data = ((ObjList*)obj)->elements.data. The list's hoisted
            // GUARD_CLASS ran before this pre-header node, so the object is a
            // List and reading the elements pointer is safe.
            int objReg, objMem; sljit_sw objOff;
            getGP(ra, n->op1, &objReg, &objMem, &objOff);
            if (objMem)
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
            else
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_R1), LIST_DATA_OFFSET);
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);
            sljit_emit_op1(C, SLJIT_MOV, dstReg, dstMem ? dstOff : 0,
                           SLJIT_R1, 0);
            break;
        }

        // ----- Control flow -----
        case IR_LOOP_HEADER: {
            // The pre-header UNBOX_INT guard scan and hoisted-bounds validation
            // apply only to the anchor loop header. Nested loop headers (whose
            // pre-header region is the anchor body) re-materialize stack values
            // each iteration; their guards are emitted inline at the access.
            if (i == ir->loop_header) {
            // Validate pre-header integer conversions only after every
            // pre-header value has been materialized. Side-exit snapshots may
            // reference constants defined after the UNBOX_INT node.
            for (uint16_t p = 0; p < i; p++) {
                const IRNode* conv = &ir->nodes[p];
                if (conv->op != IR_UNBOX_INT ||
                    !(conv->flags & IR_FLAG_INT_GUARD)) continue;

                // An IR_TYPE_INT source (CONST_INT, IV-inferred int, or int
                // arithmetic) is already a raw integer: its value is integral
                // by construction, so no guard is needed and none can be
                // emitted (reinterpreting the raw int bits through an FP
                // register would fabricate a denormal that the integrality
                // check would spuriously reject).
                if (conv->op1 < ir->count &&
                    ir->nodes[conv->op1].type == IR_TYPE_INT) continue;

                // FR0 = the boxed source re-read as a double. A boxed Value
                // (GP) is bit-reinterpreted; a raw double (FP) — e.g. the
                // integer literal `1` recorded as CONST_NUM — is already the
                // boxed form and needs no reinterpret.
                if (conv->op1 < ir->count &&
                    ir->nodes[conv->op1].type == IR_TYPE_NUM) {
                    int vr, vm; sljit_sw vo;
                    getFP(ra, conv->op1, &vr, &vm, &vo);
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                    vr, vm ? vo : 0);
                } else {
                    int vr, vm; sljit_sw vo;
                    getGP(ra, conv->op1, &vr, &vm, &vo);
                    if (vm) {
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, vr, vo);
                        vr = SLJIT_R0;
                    }
                    sljit_emit_fcopy(C, SLJIT_COPY_TO_F64, SLJIT_FR0, vr);
                }

                int irg, im; sljit_sw io;
                getGP(ra, conv->id, &irg, &im, &io);
                if (im) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, irg, io);
                    irg = SLJIT_R1;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR1, 0, irg, 0);

                uint16_t snapId = conv->imm.snapshot_id;
                struct sljit_jump* checks[3];
                checks[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                            SLJIT_FR0, 0, SLJIT_FR1, 0);
                checks[1] = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
                    irg, 0, SLJIT_IMM, (sljit_sw)(1LL << 53));
                checks[2] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                    irg, 0, SLJIT_IMM, (sljit_sw)-(1LL << 53));
                for (int ck = 0; ck < 3; ck++) {
                    if (snapId < (uint16_t)maxSnapshots &&
                        exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = checks[ck];
                }
            }
            for (uint16_t p = 0; p < i; p++) {
                if (!hoistedListIndex[p]) continue;
                const IRNode* hn = &ir->nodes[p];
                if (hn->type == IR_TYPE_INT) {
                    // Raw integer induction variable: already integral by
                    // construction, so only the non-negative half of the shape
                    // guard applies. Reading it through an FP register (as the
                    // FP path does) would pass the GP register index to a float
                    // op, which SLJIT maps to the same-index dN register — never
                    // written here — and the spurious exit would fire on every
                    // entry, turning the loop back into the interpreter.
                    int gr, gm; sljit_sw go;
                    getGP(ra, p, &gr, &gm, &go);
                    int gp = gr; sljit_sw gpo = go;
                    if (gm) {
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, gr, go);
                        gp = SLJIT_R1; gpo = 0;
                    }
                    struct sljit_jump* check =
                        sljit_emit_cmp(C, SLJIT_SIG_LESS, gp, gpo,
                                       SLJIT_IMM, 0);
                    if (exitJumpCount[0] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[0][exitJumpCount[0]++] = check;
                    continue;
                }
                int pr, pm; sljit_sw po;
                getFP(ra, p, &pr, &pm, &po);
                int fp = pr; sljit_sw fpo = po;
                if (pm) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, pr, po);
                    fp = SLJIT_FR1; fpo = 0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                                SLJIT_R0, 0, fp, fpo);
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, SLJIT_R0, 0);
                struct sljit_jump* checks[2];
                checks[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                            fp, fpo, SLJIT_FR0, 0);
                checks[1] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                                           SLJIT_R0, 0, SLJIT_IMM, 0);
                for (int ck = 0; ck < 2; ck++) {
                    if (exitJumpCount[0] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[0][exitJumpCount[0]++] = checks[ck];
                }
            }
            } // if (i == ir->loop_header)

            struct sljit_label* hdrLbl = sljit_emit_label(C);
            if (i == ir->loop_header) {
                loopHeaderLabel = hdrLbl;
            } else if (nestedLoopDepth < MAX_NESTED_LABELS) {
                nestedLoopLabels[nestedLoopDepth] = hdrLbl;
                nestedLoopHeaderId[nestedLoopDepth] = i;
                nestedLoopDepth++;
            }
            break;
        }

        case IR_LOOP_BACK: {
            // Any completed iteration implies the sunk module stores ran, so
            // exit stubs may write their loop-carried PHIs back. Guards that
            // fire before the first back-edge must leave the module variables
            // untouched.
            if (vm != NULL) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 1);
                EMIT_SET_FLAG_FROM_R0();
            }
            // A nested back edge (targeting a nested loop header) copies that
            // loop's PHIs — the loop-carried locals pass 0 promoted into the
            // anchor body — before jumping back. The anchor back edge copies
            // pre-header PHIs below.
            if (n->op1 != ir->loop_header) {
                struct sljit_label* nlbl = NULL;
                for (int nl = 0; nl < nestedLoopDepth; nl++) {
                    if (nestedLoopHeaderId[nl] == n->op1) {
                        nlbl = nestedLoopLabels[nl];
                        break;
                    }
                }
                if (nlbl) {
                    uint16_t hdr = n->op1;
                    emitNestedBackedgeCopies(C, ra, ir, backedgeToPhi,
                                             0, hdr, hdr, i);
                    struct sljit_jump* backJump = sljit_emit_jump(C, SLJIT_JUMP);
                    sljit_set_label(backJump, nlbl);
                }
                break;
            }

            // Emit back-edge copies: phi_reg = op2_reg for all pre-header PHIs.
            uint16_t hdr = ir->loop_header;
            for (uint16_t p = 0; p < hdr && p < ir->count; p++) {
                const IRNode* phi = &ir->nodes[p];
                if ((phi->flags & IR_FLAG_DEAD) || phi->op != IR_PHI) continue;
                if (phi->op2 == IR_NONE || phi->op2 >= ir->count) continue;
                if (backedgeToPhi[phi->op2] == phi->id) continue;
                if (phi->type == IR_TYPE_NUM) {
                    int srcReg, srcMem; sljit_sw srcOff;
                    int dstReg, dstMem; sljit_sw dstOff;
                    getFP(ra, phi->op2, &srcReg, &srcMem, &srcOff);
                    getFP(ra, phi->id,  &dstReg, &dstMem, &dstOff);
                    int sr = srcReg; sljit_sw sw = srcOff;
                    if (srcMem) {
                        sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, srcReg, srcOff);
                        sr = SLJIT_FR0; sw = 0;
                    }
                    if (dstMem) {
                        sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, sr, sw);
                        sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff, SLJIT_FR1, 0);
                    } else {
                        sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, 0, sr, sw);
                    }
                } else {
                    int srcReg, srcMem; sljit_sw srcOff;
                    int dstReg, dstMem; sljit_sw dstOff;
                    getGP(ra, phi->op2, &srcReg, &srcMem, &srcOff);
                    getGP(ra, phi->id,  &dstReg, &dstMem, &dstOff);
                    int sr = srcReg; sljit_sw sw = srcOff;
                    if (srcMem) {
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                        sr = SLJIT_R0; sw = 0;
                    }
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, sr, sw);
                }
            }
            if (loopHeaderLabel) {
                struct sljit_jump* backJump = sljit_emit_jump(C, SLJIT_JUMP);
                sljit_set_label(backJump, loopHeaderLabel);
            }
            break;
        }

        case IR_LOOP_EXIT: {
            // Nested-loop exit test.
            //
            // target != IR_NONE: cond falsy jumps forward past the loop to
            //   imm.jump.target (the after-loop code, recorded in-trace). A
            //   truthy cond falls through to the body, unless
            //   imm.jump.snapshot != IR_NONE (loop recorded on its final
            //   iteration), in which case truthy deopts to that snapshot.
            //
            // target == IR_NONE: the trace ended at the nested back-edge, so
            //   there is no after-loop code. A falsy cond deopts to the
            //   interpreter at imm.jump.snapshot (empty snapshot: the machine
            //   code's STORE_STACK writes already left the final values on the
            //   interpreter stack); a truthy cond falls through to the body.
            uint16_t condId = n->op1;
            uint16_t target = n->imm.jump.target;
            uint16_t snapId = n->imm.jump.snapshot;
            if (condId == IR_NONE || condId >= ir->count) break;

            // Fused exit test (IR_FLAG_FUSED_LOOP_EXIT): cond is a comparison
            // whose bool result is dead (marked by irOptFuseComparisonGuards).
            // Branch directly on the comparison operands instead of loading a
            // materialized bool, removing the fcmp/cset spill/reload round-trip.
            if (ir->nodes[condId].flags & IR_FLAG_FUSED_LOOP_EXIT) {
                struct sljit_jump* falsyJump =
                    emitCmpFalsyBranch(C, ra, ir, &ir->nodes[condId]);
                if (falsyJump == NULL) break;
                if (target == IR_NONE) {
                    // Trace ended at the nested back-edge: falsy deopts (the
                    // interpreter stack is already final); truthy falls through.
                    if (snapId < (uint16_t)maxSnapshots &&
                        exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = falsyJump;
                } else {
                    // Truthy path: deopt if the loop would have continued,
                    // otherwise fall through into the loop body.
                    if (snapId != IR_NONE && snapId < (uint16_t)maxSnapshots &&
                        exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] =
                            sljit_emit_jump(C, SLJIT_JUMP);
                    if (fwdExitCount < MAX_NESTED_LABELS * 4) {
                        fwdExitJumps[fwdExitCount] = falsyJump;
                        fwdExitTargets[fwdExitCount] = target;
                        fwdExitCount++;
                    }
                }
                break;
            }

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, condId, &srcReg, &srcMem, &srcOff);
            if (srcMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, 0);
            }

            IRType condType = ir->nodes[condId].type;

            // Falsy branches.
            struct sljit_jump* falsy[2];
            int falsyCount = 0;
            if (condType == IR_TYPE_BOOL) {
                // Raw boolean: falsy == 0.
                falsy[falsyCount++] = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, 0);
            } else if (condType == IR_TYPE_INT) {
                // Integer comparison results are materialized as 0/1 by the
                // comparison codegen, so branch on 0 directly. This is only
                // valid for comparison nodes: a genuine int loop counter must
                // NOT take this path (Wren treats 0 as truthy). An IR_TYPE_INT
                // can only reach LOOP_EXIT via the comparison-rewire in
                // irOptFuseComparisonGuards, but stay defensive.
                IRNode* cond = &ir->nodes[condId];
                bool isCmp = false;
                switch (cond->op) {
                    case IR_LT: case IR_GT: case IR_LTE: case IR_GTE:
                    case IR_EQ: case IR_NEQ: isCmp = true; break;
                    default: break;
                }
                if (isCmp) {
                    falsy[falsyCount++] = sljit_emit_cmp(C, SLJIT_EQUAL,
                        SLJIT_R0, 0, SLJIT_IMM, 0);
                } else {
                    falsy[falsyCount++] = sljit_emit_cmp(C, SLJIT_EQUAL,
                        SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);
                    falsy[falsyCount++] = sljit_emit_cmp(C, SLJIT_EQUAL,
                        SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);
                }
            } else {
                // Boxed Wren Value: falsy == false or null.
                falsy[falsyCount++] = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);
                falsy[falsyCount++] = sljit_emit_cmp(C, SLJIT_EQUAL,
                    SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);
            }

            if (target == IR_NONE) {
                // Trace ended at the nested back-edge: falsy deopts (the
                // interpreter stack is already final); truthy falls through
                // into the loop body.
                for (int f = 0; f < falsyCount; f++) {
                    if (snapId < (uint16_t)maxSnapshots &&
                        exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[snapId][exitJumpCount[snapId]++] = falsy[f];
                }
            } else {
                // Truthy path: deopt if the loop would have continued,
                // otherwise fall through into the loop body.
                if (snapId != IR_NONE && snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP) {
                    exitJumpArr[snapId][exitJumpCount[snapId]++] =
                        sljit_emit_jump(C, SLJIT_JUMP);
                }

                // Record the falsy forward branches to be bound at the target.
                for (int f = 0; f < falsyCount; f++) {
                    if (fwdExitCount < MAX_NESTED_LABELS * 4) {
                        fwdExitJumps[fwdExitCount] = falsy[f];
                        fwdExitTargets[fwdExitCount] = target;
                        fwdExitCount++;
                    }
                }
            }
            break;
        }

        // ----- PHI, SNAPSHOT, SIDE_EXIT -----
        case IR_PHI: {
            // Emit initial assignment: phi_reg = op1_reg (pre-loop value).
            // At LOOP_BACK we emit phi_reg = op2_reg (back-edge value).
            if (n->op1 == IR_NONE || n->op1 >= ir->count) break;
            if (n->type == IR_TYPE_NUM) {
                int srcReg, srcMem; sljit_sw srcOff;
                int dstReg, dstMem; sljit_sw dstOff;
                getFP(ra, n->op1, &srcReg, &srcMem, &srcOff);
                getFP(ra, n->id,  &dstReg, &dstMem, &dstOff);
                int sr = srcReg; sljit_sw sw = srcOff;
                if (srcMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, srcReg, srcOff);
                    sr = SLJIT_FR0; sw = 0;
                }
                if (dstMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, sr, sw);
                    sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff, SLJIT_FR1, 0);
                } else {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, 0, sr, sw);
                }
            } else {
                int srcReg, srcMem; sljit_sw srcOff;
                int dstReg, dstMem; sljit_sw dstOff;
                getGP(ra, n->op1, &srcReg, &srcMem, &srcOff);
                getGP(ra, n->id,  &dstReg, &dstMem, &dstOff);
                int sr = srcReg; sljit_sw sw = srcOff;
                if (srcMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                    sr = SLJIT_R0; sw = 0;
                }
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, sr, sw);
            }
            break;
        }

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
        case IR_RSHIFT:
        case IR_ASHR: {
            sljit_s32 op2code;
            switch (n->op) {
                case IR_BAND:   op2code = SLJIT_AND | SLJIT_32; break;
                case IR_BOR:    op2code = SLJIT_OR | SLJIT_32; break;
                case IR_BXOR:   op2code = SLJIT_XOR | SLJIT_32; break;
                case IR_LSHIFT: op2code = SLJIT_SHL | SLJIT_32; break;
                case IR_RSHIFT: op2code = SLJIT_LSHR | SLJIT_32; break;
                case IR_ASHR:   op2code = SLJIT_ASHR | SLJIT_32; break;
                default: op2code = SLJIT_AND; break;
            }

            int s1r, s1m; sljit_sw s1o;
            int dr, dm; sljit_sw dof;
            getGP(ra, n->op1, &s1r, &s1m, &s1o);
            getGP(ra, n->id, &dr, &dm, &dof);

            // Materialize a CONST_INT second operand directly as an immediate.
            // This is required for FLOOR-pattern ASHRs: the shift constant is
            // appended at the end of the buffer (after its use), so the linear
            // scan assigns it a stale register that already holds another live
            // value mid-loop. An immediate also saves a load instruction.
            bool s2Imm = (n->op2 < ir->count &&
                          ir->nodes[n->op2].op == IR_CONST_INT &&
                          !(ir->nodes[n->op2].flags & IR_FLAG_DEAD));
            sljit_sw s2imm = s2Imm ? ir->nodes[n->op2].imm.i64 : 0;

            int a = SLJIT_R0;
            if (s1m) {
                sljit_emit_op1(C, SLJIT_MOV, a, 0, s1r, s1o);
            } else { a = s1r; }

            if (dm) {
                if (s2Imm) {
                    sljit_emit_op2(C, op2code, SLJIT_R0, 0, a, 0,
                                   SLJIT_IMM, s2imm);
                } else {
                    int s2r, s2m; sljit_sw s2o;
                    int b = SLJIT_R1;
                    getGP(ra, n->op2, &s2r, &s2m, &s2o);
                    if (s2m) {
                        sljit_emit_op1(C, SLJIT_MOV, b, 0, s2r, s2o);
                    } else { b = s2r; }
                    sljit_emit_op2(C, op2code, SLJIT_R0, 0, a, 0, b, 0);
                }
                sljit_emit_op1(C, SLJIT_MOV, dr, dof, SLJIT_R0, 0);
            } else {
                if (s2Imm) {
                    sljit_emit_op2(C, op2code, dr, 0, a, 0,
                                   SLJIT_IMM, s2imm);
                } else {
                    int s2r, s2m; sljit_sw s2o;
                    int b = SLJIT_R1;
                    getGP(ra, n->op2, &s2r, &s2m, &s2o);
                    if (s2m) {
                        sljit_emit_op1(C, SLJIT_MOV, b, 0, s2r, s2o);
                    } else { b = s2r; }
                    sljit_emit_op2(C, op2code, dr, 0, a, 0, b, 0);
                }
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
            if (n->type != IR_TYPE_INT) {
                // Specialize numeric modulo for exact integer-valued doubles.
                // Fractional operands side-exit to Wren's general primitive.
                int lr, lm; sljit_sw lo;
                int rr, rm; sljit_sw ro;
                int dr, dm; sljit_sw dso;
                getFP(ra, n->op1, &lr, &lm, &lo);
                getFP(ra, n->op2, &rr, &rm, &ro);
                getFP(ra, n->id, &dr, &dm, &dso);
                if (lm) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, lr, lo);
                    lr = SLJIT_FR0; lo = 0;
                }
                if (rm) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, rr, ro);
                    rr = SLJIT_FR1; ro = 0;
                }
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(SLJIT_SP), tmpOff, lr, lo);
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(SLJIT_SP), tmpOff + 8, rr, ro);
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64, SLJIT_R0, 0,
                                SLJIT_MEM1(SLJIT_SP), tmpOff);
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64, SLJIT_R1, 0,
                                SLJIT_MEM1(SLJIT_SP), tmpOff + 8);
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, SLJIT_R0, 0);
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR1, 0, SLJIT_R1, 0);
                struct sljit_jump* bad[3];
                bad[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                    SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), tmpOff);
                bad[1] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                    SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), tmpOff + 8);
                bad[2] = sljit_emit_cmp(C, SLJIT_EQUAL,
                                       SLJIT_R1, 0, SLJIT_IMM, 0);
                uint16_t sid = n->imm.snapshot_id;
                for (int ck = 0; ck < 3; ck++) {
                    if (sid < (uint16_t)maxSnapshots &&
                        exitJumpCount[sid] < MAX_EXITS_PER_SNAP)
                        exitJumpArr[sid][exitJumpCount[sid]++] = bad[ck];
                }
                sljit_emit_op0(C, SLJIT_DIVMOD_SW);
                int fpDst = dm ? SLJIT_FR0 : dr;
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                fpDst, 0, SLJIT_R1, 0);
                if (dm)
                    sljit_emit_fop1(C, SLJIT_MOV_F64, dr, dso,
                                    SLJIT_FR0, 0);
                break;
            }

            // Integer-specialized Wren modulo. Integer inference proves both
            // inputs exact and attaches the deoptimization snapshot.
            int leftReg, leftMem; sljit_sw leftOff;
            int rightReg, rightMem; sljit_sw rightOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->op1, &leftReg, &leftMem, &leftOff);
            getGP(ra, n->op2, &rightReg, &rightMem, &rightOff);
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           leftReg, leftMem ? leftOff : 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           rightReg, rightMem ? rightOff : 0);
            uint16_t sid = n->imm.snapshot_id;
            struct sljit_jump* zero = sljit_emit_cmp(C, SLJIT_EQUAL,
                                                     SLJIT_R1, 0,
                                                     SLJIT_IMM, 0);
            if (sid < (uint16_t)maxSnapshots &&
                exitJumpCount[sid] < MAX_EXITS_PER_SNAP)
                exitJumpArr[sid][exitJumpCount[sid]++] = zero;
            sljit_emit_op0(C, SLJIT_DIVMOD_SW);
            sljit_emit_op1(C, SLJIT_MOV, dstReg, dstMem ? dstOff : 0,
                           SLJIT_R1, 0);
            break;
        }

        case IR_LOAD_RANGE: {
            // Read one shape field out of an ObjRange as a raw double. The
            // receiver has already been proven to be a Range by a preceding
            // IR_GUARD_CLASS, so the unmasked pointer is safe to dereference.
            uint16_t objId = n->op1;
            if (objId == IR_NONE) break;

            int objReg, objMem; sljit_sw objOff;
            getGP(ra, objId, &objReg, &objMem, &objOff);

            // R1 = boxed range Value.
            if (objMem) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
            }
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));

            int dstReg, dstMem; sljit_sw dstOff;
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);
            int fpDst = dstMem ? SLJIT_FR0 : dstReg;

            if (n->imm.mem.field == (uint16_t)IR_RANGE_INCLUSIVE) {
                // isInclusive is a C bool; widen the byte and convert so the
                // result is a plain 0.0/1.0 double like the other two fields.
                sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R1),
                               (sljit_sw)RANGE_INCLUSIVE_OFFSET);
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, fpDst, 0,
                                SLJIT_R0, 0);
            } else {
                sljit_sw off = (n->imm.mem.field == (uint16_t)IR_RANGE_FROM)
                                   ? RANGE_FROM_OFFSET
                                   : RANGE_TO_OFFSET;
                sljit_emit_fop1(C, SLJIT_MOV_F64, fpDst, 0,
                                SLJIT_MEM1(SLJIT_R1), off);
            }

            if (dstMem) {
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff,
                                SLJIT_FR0, 0);
            }
            break;
        }

        case IR_LIST_COUNT: {
            int objReg, objMem; sljit_sw objOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->op1, &objReg, &objMem, &objOff);
            getFP(ra, n->id, &dstReg, &dstMem, &dstOff);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           objReg, objMem ? objOff : 0);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R1), LIST_COUNT_OFFSET);
            int fpDst = dstMem ? SLJIT_FR0 : dstReg;
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, fpDst, 0,
                            SLJIT_R0, 0);
            if (dstMem)
                sljit_emit_fop1(C, SLJIT_MOV_F64, dstReg, dstOff,
                                SLJIT_FR0, 0);
            break;
        }

        case IR_LIST_ITERATE: {
            // Exact List.iterate(_) protocol for its normal null/number
            // states. Returning false in-trace lets the enclosing for-loop
            // terminate as a native nested branch instead of side-exiting.
            int listReg, listMem; sljit_sw listOff;
            int iterReg, iterMem; sljit_sw iterOff;
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->op1, &listReg, &listMem, &listOff);
            getGP(ra, n->op2, &iterReg, &iterMem, &iterOff);
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           iterReg, iterMem ? iterOff : 0);
            struct sljit_jump* start = sljit_emit_cmp(
                C, SLJIT_EQUAL, SLJIT_R0, 0,
                SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);

            // Numeric continuation: validateInt(), then advance. Integral
            // negative/out-of-range values mean completion in Wren.
            sljit_emit_fcopy(C, SLJIT_COPY_TO_F64, SLJIT_FR0, SLJIT_R0);
            sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                            SLJIT_R1, 0, SLJIT_FR0, 0);
            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR1, 0, SLJIT_R1, 0);
            struct sljit_jump* nonIntegral = sljit_emit_fcmp(
                C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                SLJIT_FR0, 0, SLJIT_FR1, 0);
            uint16_t sid = n->imm.list.snapshot;
            if (sid < (uint16_t)maxSnapshots &&
                exitJumpCount[sid] < MAX_EXITS_PER_SNAP)
                exitJumpArr[sid][exitJumpCount[sid]++] = nonIntegral;
            struct sljit_jump* negative = sljit_emit_cmp(
                C, SLJIT_SIG_LESS, SLJIT_R1, 0, SLJIT_IMM, 0);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
                           SLJIT_R1, 0, SLJIT_IMM, 1);

            // R2 = untagged ObjList*, R3 = element count.
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           listReg, listMem ? listOff : 0);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0,
                           SLJIT_IMM,
                           (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_R2), LIST_COUNT_OFFSET);
            struct sljit_jump* complete = sljit_emit_cmp(
                C, SLJIT_SIG_GREATER_EQUAL,
                SLJIT_R1, 0, SLJIT_R3, 0);

            sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                            SLJIT_FR0, 0, SLJIT_R1, 0);
            sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, SLJIT_R0);
            struct sljit_jump* doneNumeric = sljit_emit_jump(C, SLJIT_JUMP);

            // Null start: numeric zero for a non-empty list.
            sljit_set_label(start, sljit_emit_label(C));
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           listReg, listMem ? listOff : 0);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0, SLJIT_R2, 0,
                           SLJIT_IMM,
                           (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_R2), LIST_COUNT_OFFSET);
            struct sljit_jump* empty = sljit_emit_cmp(
                C, SLJIT_EQUAL, SLJIT_R3, 0, SLJIT_IMM, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0);
            struct sljit_jump* doneStart = sljit_emit_jump(C, SLJIT_JUMP);

            struct sljit_label* falseLabel = sljit_emit_label(C);
            sljit_set_label(negative, falseLabel);
            sljit_set_label(complete, falseLabel);
            sljit_set_label(empty, falseLabel);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)WREN_FALSE_VAL);
            struct sljit_label* done = sljit_emit_label(C);
            sljit_set_label(doneNumeric, done);
            sljit_set_label(doneStart, done);
            sljit_emit_op1(C, SLJIT_MOV, dstReg, dstMem ? dstOff : 0,
                           SLJIT_R0, 0);
            break;
        }

        case IR_LIST_LOAD:
        case IR_LIST_STORE: {
            // The class and numeric guards precede this node. Validate that
            // the numeric index is an exact non-negative integer in bounds,
            // then access ObjList.elements.data[index] directly.
            int objReg, objMem; sljit_sw objOff;
            getGP(ra, n->op1, &objReg, &objMem, &objOff);

            // When the optimizer cached the list's elements.data pointer in a
            // pre-header IR_LIST_DATA node, op1 is that raw pointer; skip the
            // tag-strip and the data load below.
            bool dataCached = (n->op1 < ir->count) &&
                              (ir->nodes[n->op1].op == IR_LIST_DATA);
            bool boundsHoisted = (n->flags & IR_FLAG_BOUNDS_HOISTED) != 0;
            if (dataCached) boundsHoisted = true;

            int dataBase;
            if (dataCached && !objMem) {
                dataBase = objReg;
            } else {
                if (objMem)
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
                else
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
                if (!dataCached)
                    sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                                   SLJIT_IMM,
                                   (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
                dataBase = SLJIT_R1;
            }

            uint16_t sid = n->imm.list.snapshot;
            struct sljit_jump* bad[3] = { NULL, NULL, NULL };
            bool indexFactsHoisted = hoistedListIndex[n->op2];
            if (ir->nodes[n->op2].type == IR_TYPE_INT) {
                int idxReg, idxMem; sljit_sw idxOff;
                getGP(ra, n->op2, &idxReg, &idxMem, &idxOff);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               idxReg, idxMem ? idxOff : 0);
            } else {
                int idxReg, idxMem; sljit_sw idxOff;
                getFP(ra, n->op2, &idxReg, &idxMem, &idxOff);
                int fpIndex = idxReg;
                sljit_sw fpIndexOff = 0;
                if (idxMem) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                    idxReg, idxOff);
                    fpIndex = SLJIT_FR0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_SW_FROM_F64,
                                SLJIT_R0, 0, fpIndex, fpIndexOff);
                if (!indexFactsHoisted) {
                    sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                    SLJIT_FR1, 0, SLJIT_R0, 0);
                    bad[0] = sljit_emit_fcmp(C, SLJIT_UNORDERED_OR_NOT_EQUAL,
                                             fpIndex, fpIndexOff, SLJIT_FR1, 0);
                }
            }
            if (!indexFactsHoisted)
                bad[1] = sljit_emit_cmp(C, SLJIT_SIG_LESS,
                                        SLJIT_R0, 0, SLJIT_IMM, 0);
            if (!boundsHoisted) {
                // ValueBuffer.count is a 32-bit int. Loading it as a machine
                // word would accidentally include capacity in the high half
                // and let out-of-bounds indices pass on 64-bit targets.
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R1, 0,
                               SLJIT_MEM1(dataBase), LIST_COUNT_OFFSET);
                bad[2] = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
                                        SLJIT_R0, 0, SLJIT_R1, 0);
            }
            for (int ck = 0; ck < 3; ck++) {
                if (bad[ck] != NULL && sid < (uint16_t)maxSnapshots &&
                    exitJumpCount[sid] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[sid][exitJumpCount[sid]++] = bad[ck];
            }

            if (!boundsHoisted) {
                // Reload the object pointer after using R1 for the count.
                if (objMem)
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, objOff);
                else
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, objReg, 0);
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                               SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            }

            if (n->op == IR_LIST_STORE) {
                uint16_t valueId = n->imm.list.value;
                int valueReg, valueMem; sljit_sw valueOff;
                getGP(ra, valueId, &valueReg, &valueMem, &valueOff);
                if (valueMem)
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0,
                                    valueReg, valueOff);
                else
                    sljit_emit_fcopy(C, SLJIT_COPY_TO_F64,
                                     SLJIT_FR1, valueReg);
                if (!dataCached)
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   SLJIT_MEM1(dataBase), LIST_DATA_OFFSET);
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM2(dataBase, SLJIT_R0),
                                SLJIT_WORD_SHIFT, SLJIT_FR1, 0);
            } else {
                int dstReg, dstMem; sljit_sw dstOff;
                getGP(ra, n->id, &dstReg, &dstMem, &dstOff);
                if (!dataCached)
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   SLJIT_MEM1(dataBase), LIST_DATA_OFFSET);
                if (dstMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   SLJIT_MEM2(dataBase, SLJIT_R0),
                                   SLJIT_WORD_SHIFT);
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R0, 0);
                } else {
                    // Load straight into the destination register; avoids the
                    // extra scratch move that shows up as `ldr x0; mov xN,x0`.
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                                   SLJIT_MEM2(dataBase, SLJIT_R0),
                                   SLJIT_WORD_SHIFT);
                }
            }
            break;
        }

        // ----- Map hash-table access (inline linear probe) -----
        case IR_MAP_GET:
        case IR_MAP_PUT: {
            // op1 = boxed map Value, op2 = boxed key Value. Wren's ObjMap is
            // open addressing with linear probing: index = hashValue(key) %
            // capacity (capacity is a power of two, so & (cap-1)), entries are
            // 16-byte MapEntry{key, value}. A slot whose key is UNDEFINED_VAL
            // is free (value FALSE_VAL = empty, TRUE_VAL = tombstone; the
            // probe continues past tombstones). The getter returns null on a
            // miss; the setter grows the table when count+1 > capacity*75/100
            // and side-exits so the real wrenMapSet runs the resize.
            //
            // Register-resident probe: R0 = idx (byte offset, loop-carried),
            // R1 = entries base, R2 = entry addr / scratch, R3 = key. Only
            // register operands are used so the backend's well-tested paths
            // apply. The wrap mask lives in the temp area.
            bool isPut = (n->op == IR_MAP_PUT);
            uint16_t snapId = isPut ? n->imm.map.snapshot : IR_NONE;

            // The paired getter already hashed/probed and cached the resolved
            // entry address at tmp+40. Reuse it for overwrite/insert.
            if (isPut && (n->flags & IR_FLAG_MAP_REUSE_PUT)) {
                uint16_t valId = n->imm.map.value;
                if (valId < ir->count && ir->nodes[valId].type == IR_TYPE_VALUE) {
                    int vr, vm; sljit_sw vo;
                    getGP(ra, valId, &vr, &vm, &vo);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, vr, vm ? vo : 0);
                } else if (valId < ir->count &&
                           ir->nodes[valId].type == IR_TYPE_NUM) {
                    int vr, vm; sljit_sw vo;
                    getFP(ra, valId, &vr, &vm, &vo);
                    int vf = vr;
                    if (vm) {
                        sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, vr, vo);
                        vf = SLJIT_FR0;
                    }
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, vf, SLJIT_R1);
                } else {
                    int vr, vm; sljit_sw vo;
                    getGP(ra, valId, &vr, &vm, &vo);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, vr, vm ? vo : 0);
                    sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0,
                                    SLJIT_R1, 0);
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, SLJIT_R1);
                }
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 8,
                               SLJIT_R1, 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 40);
                struct sljit_jump* noEntry = sljit_emit_cmp(
                    C, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0);
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = noEntry;

                // Match wrenMapSet exactly: its load-factor check runs before
                // it knows whether this is an overwrite, so it may resize even
                // for an existing key (which can affect iteration order).
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 32);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R3), MAP_COUNT_OFFSET);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R1, 0,
                               SLJIT_MEM1(SLJIT_R3), MAP_CAPACITY_OFFSET);
                // Map capacities are powers of two, hence cap * 75 / 100 is
                // exactly cap * 3 / 4.  `count + 1 > threshold` is therefore
                // `count >= threshold`; this avoids two hot multiplications.
                sljit_emit_op2(C, SLJIT_MUL, SLJIT_R1, 0, SLJIT_R1, 0,
                               SLJIT_IMM, 3);
                sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R1, 0,
                               SLJIT_IMM, 2);
                struct sljit_jump* growReuse = sljit_emit_cmp(
                    C, SLJIT_SIG_GREATER_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = growReuse;

                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R2), MAP_ENTRY_KEY_OFFSET);
                struct sljit_jump* missing = sljit_emit_cmp(
                    C, SLJIT_EQUAL, SLJIT_R0, 0,
                    SLJIT_IMM, (sljit_sw)MAP_UNDEFINED_VAL);

                // Existing key: overwrite without a resize check.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 8);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
                               MAP_ENTRY_VALUE_OFFSET, SLJIT_R0, 0);
                struct sljit_jump* reuseDone = sljit_emit_jump(C, SLJIT_JUMP);

                // Missing key: insert into the cached empty/tombstone entry
                // and increment count (capacity was guarded above).
                sljit_set_label(missing, sljit_emit_label(C));
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
                               MAP_ENTRY_KEY_OFFSET, SLJIT_R0, 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 8);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
                               MAP_ENTRY_VALUE_OFFSET, SLJIT_R0, 0);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R3), MAP_COUNT_OFFSET);
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 1);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_MEM1(SLJIT_R3),
                               MAP_COUNT_OFFSET, SLJIT_R0, 0);
                sljit_set_label(reuseDone, sljit_emit_label(C));
                break;
            }

            // Load the boxed map Value into R1, strip the NaN tag into R2.
            int mapReg, mapMem; sljit_sw mapOff;
            getGP(ra, n->op1, &mapReg, &mapMem, &mapOff);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           mapMem ? SLJIT_MEM1(mapReg) : mapReg,
                           mapMem ? mapOff : 0);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 32,
                           SLJIT_R2, 0); // map ptr backup

            // Load the boxed key into R3; back it up for the hash step.
            int keyReg, keyMem; sljit_sw keyOff;
            getGP(ra, n->op2, &keyReg, &keyMem, &keyOff);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                           keyMem ? SLJIT_MEM1(keyReg) : keyReg,
                           keyMem ? keyOff : 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff,
                           SLJIT_R3, 0);

            struct sljit_jump* emptyMap = NULL;
            struct sljit_jump* grow = NULL;
            if (isPut || (n->flags & IR_FLAG_MAP_PROBE_GET)) {
                // Fused getters also track tombstones for the later insert.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 24,
                               SLJIT_IMM, 0);
            }
            if (isPut) {
                // Spill the stored value for the insert site. A Wren Num's
                // NaN-boxed Value bits ARE the double bits, so an unboxed NUM
                // operand (the optimizer may fold a boxed constant) is boxed
                // by copying the FP register to a GP register.
                uint16_t valId = n->imm.map.value;
                if (valId < ir->count &&
                    ir->nodes[valId].type == IR_TYPE_VALUE) {
                    int valReg, valMem; sljit_sw valOff;
                    getGP(ra, valId, &valReg, &valMem, &valOff);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   valMem ? SLJIT_MEM1(valReg) : valReg,
                                   valMem ? valOff : 0);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 8,
                                   SLJIT_R1, 0);
                } else if (valId < ir->count &&
                           ir->nodes[valId].type == IR_TYPE_NUM) {
                    int valReg, valMem; sljit_sw valOff;
                    getFP(ra, valId, &valReg, &valMem, &valOff);
                    int fr = valReg;
                    if (valMem) {
                        sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                                        valReg, valOff);
                        fr = SLJIT_FR0;
                    }
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, fr, SLJIT_R1);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 8,
                                   SLJIT_R1, 0);
                } else {
                    // Raw integer: box by converting to a double.
                    int valReg, valMem; sljit_sw valOff;
                    getGP(ra, valId, &valReg, &valMem, &valOff);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   valMem ? SLJIT_MEM1(valReg) : valReg,
                                   valMem ? valOff : 0);
                    sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0,
                                    SLJIT_R1, 0);
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, SLJIT_R1);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 8,
                                   SLJIT_R1, 0);
                }
                // Resize guard: (count + 1) * 100 > capacity * 75 -> exit.
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R2), MAP_COUNT_OFFSET);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R1, 0,
                               SLJIT_MEM1(SLJIT_R2), MAP_CAPACITY_OFFSET);
                sljit_emit_op2(C, SLJIT_MUL, SLJIT_R1, 0, SLJIT_R1, 0,
                               SLJIT_IMM, 3);
                sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R1, 0,
                               SLJIT_IMM, 2);
                grow = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
                                      SLJIT_R0, 0, SLJIT_R1, 0);
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] = grow;
            }

            // ---- Hash the boxed key bits (Thomas Wang, matches hashBits) ----
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_SP), tmpOff);
            // hash = ~hash + (hash << 18)
            sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 18);
            sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, -1);   // ~hash
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            // hash ^= hash >> 31
            sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 31);
            sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            // hash *= 21  ==  hash + (hash << 2) + (hash << 4)
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_R0, 0);
            sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 2);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, 4);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            // hash ^= hash >> 11
            sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 11);
            sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            // hash += hash << 6
            sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 6);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            // hash ^= hash >> 22
            sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R1, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 22);
            sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            // hash &= 0x3fffffff (hashValue returns 30 bits)
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, (sljit_sw)0x3fffffff);

            // ---- index = hash & (capacity - 1), byte offset * 16 ----
            sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_R2), MAP_CAPACITY_OFFSET);
            emptyMap = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0,
                                      SLJIT_IMM, 0);
            sljit_emit_op2(C, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R1, 0);
            sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 4);
            sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, 4);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 16,
                           SLJIT_R1, 0); // wrap mask (bytes)
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_R2), MAP_ENTRIES_OFFSET);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_SP), tmpOff); // key

            // ---- probe loop ----
            struct sljit_label* probeTop = sljit_emit_label(C);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R1, 0,
                           SLJIT_R0, 0);                 // R2 = entry addr
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R2), MAP_ENTRY_KEY_OFFSET);
            struct sljit_jump* keyUndef = sljit_emit_cmp(C, SLJIT_EQUAL,
                SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)MAP_UNDEFINED_VAL);
            struct sljit_jump* keyMatch = sljit_emit_cmp(C, SLJIT_EQUAL,
                SLJIT_R2, 0, SLJIT_R3, 0);
            // Neither undefined nor equal: advance.
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 16);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_SP), tmpOff + 16);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R2, 0);
            struct sljit_jump* probeAgain = sljit_emit_jump(C, SLJIT_JUMP);
            sljit_set_label(probeAgain, probeTop);

            // keyMatch: found — load the stored value (getter) or overwrite it
            // (setter, which never resizes on an existing key).
            sljit_set_label(keyMatch, sljit_emit_label(C));
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R1, 0,
                           SLJIT_R0, 0);                 // R2 = entry addr
            if (n->flags & IR_FLAG_MAP_PROBE_GET)
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 40,
                               SLJIT_R2, 0);
            if (isPut) {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 8);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
                               MAP_ENTRY_VALUE_OFFSET, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R2), MAP_ENTRY_VALUE_OFFSET);
            }
            struct sljit_jump* doneFound = sljit_emit_jump(C, SLJIT_JUMP);

            sljit_set_label(keyUndef, sljit_emit_label(C));
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R1, 0,
                           SLJIT_R0, 0);                 // R2 = entry addr
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R2), MAP_ENTRY_VALUE_OFFSET);
            struct sljit_jump* valueFalse = sljit_emit_cmp(C, SLJIT_EQUAL,
                SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)MAP_FALSE_VAL);
            // Tombstone: remember it (only the first) and keep probing.
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_SP), tmpOff + 24);
            struct sljit_jump* tsSeen = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                SLJIT_R2, 0, SLJIT_IMM, 0);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R1, 0,
                           SLJIT_R0, 0);                 // R2 = entry addr
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 24,
                           SLJIT_R2, 0);
            sljit_set_label(tsSeen, sljit_emit_label(C));
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 16);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_SP), tmpOff + 16);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0,
                           SLJIT_R2, 0);
            struct sljit_jump* probeTomb = sljit_emit_jump(C, SLJIT_JUMP);
            sljit_set_label(probeTomb, probeTop);

            // valueFalse: empty slot — getter misses, setter inserts.
            sljit_set_label(valueFalse, sljit_emit_label(C));
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R1, 0,
                           SLJIT_R0, 0);                 // R2 = entry addr
            if (!isPut && (n->flags & IR_FLAG_MAP_PROBE_GET)) {
                // Cache the first tombstone when present, otherwise the empty
                // entry just found.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 24);
                struct sljit_jump* cachedTomb = sljit_emit_cmp(
                    C, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R2, 0);
                sljit_set_label(cachedTomb, sljit_emit_label(C));
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), tmpOff + 40,
                               SLJIT_R0, 0);
            }
            if (isPut) {
                // Insert at the remembered tombstone or this empty slot. The
                // tombstone check must leave R0 holding the empty entry addr
                // on the no-tombstone path: a branch to a label placed after
                // the mov would skip it and write through a NULL pointer.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 24);
                struct sljit_jump* useTombstone = sljit_emit_cmp(
                    C, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R2, 0);
                sljit_set_label(useTombstone, sljit_emit_label(C));
                // R0 = insertion entry addr.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
                               MAP_ENTRY_KEY_OFFSET, SLJIT_R3, 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 8);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
                               MAP_ENTRY_VALUE_OFFSET, SLJIT_R2, 0);
                // map->count++
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                               SLJIT_MEM1(SLJIT_SP), tmpOff + 32);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_R2), MAP_COUNT_OFFSET);
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 1);
                sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_MEM1(SLJIT_R2),
                               MAP_COUNT_OFFSET, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);
            }
            struct sljit_jump* doneMap = sljit_emit_jump(C, SLJIT_JUMP);
            if (!isPut) {
                // Getter on an empty map also misses.
                sljit_set_label(emptyMap, sljit_emit_label(C));
                if (n->flags & IR_FLAG_MAP_PROBE_GET)
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                                   tmpOff + 40, SLJIT_IMM, 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)WREN_NULL_VAL);
                struct sljit_jump* doneEmpty = sljit_emit_jump(C, SLJIT_JUMP);
                sljit_set_label(doneEmpty, sljit_emit_label(C));
            } else {
                // Put on a zero-capacity map: the resize guard should have
                // fired already; route here to the interpreter's insert for
                // safety (capacity can never be 0 past the guard).
                sljit_set_label(emptyMap, sljit_emit_label(C));
                if (snapId < (uint16_t)maxSnapshots &&
                    exitJumpCount[snapId] < MAX_EXITS_PER_SNAP)
                    exitJumpArr[snapId][exitJumpCount[snapId]++] =
                        sljit_emit_jump(C, SLJIT_JUMP);
                struct sljit_jump* putEmptyDone = sljit_emit_jump(C, SLJIT_JUMP);
                sljit_set_label(putEmptyDone, sljit_emit_label(C));
            }

            // done: store result R0 into n->id (getter only).
            sljit_set_label(doneMap, sljit_emit_label(C));
            sljit_set_label(doneFound, sljit_emit_label(C));
            if (!isPut) {
                int dstReg, dstMem; sljit_sw dstOff;
                getGP(ra, n->id, &dstReg, &dstMem, &dstOff);
                sljit_emit_op1(C, SLJIT_MOV, dstReg, dstMem ? dstOff : 0,
                               SLJIT_R0, 0);
            }
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

            // IR field operands are boxed Wren object Values. Unmask before
            // dereferencing (the interpreter recorder uses the same contract).
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_sw fieldOff = OBJ_FIELDS_OFFSET + (sljit_sw)(fieldIdx * 8);

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

            sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)~(WREN_SIGN_BIT | WREN_QNAN));
            sljit_sw fieldOff = OBJ_FIELDS_OFFSET + (sljit_sw)(fieldIdx * 8);

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
            // Load a Value from the module variables array.
            // Fast path (when modVarsBase is known): use REG_MOD_VARS + offset
            // (single instruction on ARM64).  Fallback: absolute pointer.
            int dstReg, dstMem; sljit_sw dstOff;
            getGP(ra, n->id, &dstReg, &dstMem, &dstOff);

            if (modVarsBase != NULL) {
                // Compute byte offset from module vars base.
                sljit_sw mvOff = (sljit_sw)(
                    ((char*)n->imm.ptr - (char*)modVarsBase));
                if (dstMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   SLJIT_MEM1(REG_MOD_VARS), mvOff);
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff,
                                   SLJIT_R0, 0);
                } else {
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                                   SLJIT_MEM1(REG_MOD_VARS), mvOff);
                }
            } else {
                // Fallback: load absolute address into R0, then dereference.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)(uintptr_t)n->imm.ptr);
                if (dstMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                                   SLJIT_MEM1(SLJIT_R0), 0);
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, dstOff, SLJIT_R1, 0);
                } else {
                    sljit_emit_op1(C, SLJIT_MOV, dstReg, 0,
                                   SLJIT_MEM1(SLJIT_R0), 0);
                }
            }
            break;
        }

        case IR_STORE_MODULE_VAR: {
            // Store a Value to the module variables array.
            // op1 = SSA value to store.
            uint16_t valId = n->op1;
            if (valId == IR_NONE) break;

            int srcReg, srcMem; sljit_sw srcOff;
            getGP(ra, valId, &srcReg, &srcMem, &srcOff);

            if (modVarsBase != NULL) {
                sljit_sw mvOff = (sljit_sw)(
                    ((char*)n->imm.ptr - (char*)modVarsBase));
                if (srcMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, srcReg, srcOff);
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(REG_MOD_VARS), mvOff,
                                   SLJIT_R0, 0);
                } else {
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(REG_MOD_VARS), mvOff,
                                   srcReg, 0);
                }
            } else {
                // Fallback: absolute pointer.
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)(uintptr_t)n->imm.ptr);
                if (srcMem) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, srcReg, srcOff);
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R1, 0);
                } else {
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(SLJIT_R0), 0, srcReg, 0);
                }
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
    //   1. Writes all snapshot-captured SSA values back to the interpreter stack
    //      so the interpreter can resume at resume_pc with a valid stack.
    //   2. Returns exitIdx + 1 so the caller can call wrenJitRestoreExit.
    // ---------------------------------------------------------------------------
    struct sljit_label* exitLabels[IR_MAX_SNAPSHOTS];

    for (int si = 0; si < maxSnapshots; si++) {
        exitLabels[si] = sljit_emit_label(C);

        // Materialize module stores that were sunk out of the hot loop. The
        // runtime flag distinguishes an entry-time exit (the loop body never
        // ran, module variables still hold their pre-loop values and must be
        // left alone) from a post-iteration exit (the PHIs hold the current
        // values and must be written back). A static resume-pc test cannot
        // tell them apart: both the fractional-counter INT_GUARD and the
        // range-iterate overflow guard resume inside the loop bytecode.
        struct sljit_jump* skipWritebacks = NULL;
        if (vm != NULL) {
            EMIT_READ_FLAG_TO_R0();
            skipWritebacks = sljit_emit_cmp(
                C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
        }
        for (uint16_t me = 0; me < ir->exit_module_entry_count; me++) {
            const IRExitModuleEntry* entry = &ir->exit_module_entries[me];
            if (entry->snapshot_id != (uint16_t)si ||
                entry->ssa_ref >= ir->count) continue;

            uint16_t ref = entry->ssa_ref;
            if (backedgeToPhi[ref] != IR_NONE) ref = backedgeToPhi[ref];
            IRType type = ir->nodes[ref].type;
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_IMM, (sljit_sw)(uintptr_t)entry->address);

            if (type == IR_TYPE_NUM) {
                int sr, sm; sljit_sw so;
                getFP(ra, ref, &sr, &sm, &so);
                if (sm) {
                    sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, sr, so);
                    sr = SLJIT_FR0; so = 0;
                }
                // A Wren numeric Value is exactly the IEEE-754 bit pattern,
                // so an FP store performs boxing and the module write at once.
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(SLJIT_R1), 0, sr, so);
            } else if (type == IR_TYPE_INT) {
                int sr, sm; sljit_sw so;
                getGP(ra, ref, &sr, &sm, &so);
                if (sm) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, sr, so);
                    sr = SLJIT_R0;
                }
                sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                SLJIT_FR0, 0, sr, 0);
                sljit_emit_fop1(C, SLJIT_MOV_F64,
                                SLJIT_MEM1(SLJIT_R1), 0, SLJIT_FR0, 0);
            } else {
                int sr, sm; sljit_sw so;
                getGP(ra, ref, &sr, &sm, &so);
                if (sm) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, sr, so);
                    sr = SLJIT_R0; so = 0;
                }
                sljit_emit_op1(C, SLJIT_MOV,
                               SLJIT_MEM1(SLJIT_R1), 0, sr, so);
            }
        }
        // When no loop back-edge ran within this invocation, leave the module
        // variables at their pre-loop values and skip the writebacks above.
        sljit_set_label(skipWritebacks, sljit_emit_label(C));

        // Write back all snapshot-captured SSA values to the interpreter stack
        // so the interpreter can resume at resume_pc with a valid stack.
        if (si < (int)ir->snapshot_count) {
            const IRSnapshot* snap = &ir->snapshots[si];
            for (int e = 0; e < (int)snap->num_entries; e++) {
                int entry_idx = (int)snap->entry_start + e;
                if (entry_idx >= (int)ir->snapshot_entry_count) break;
                uint16_t slot = ir->snapshot_entries[entry_idx].slot;
                uint16_t ref  = ir->snapshot_entries[entry_idx].ssa_ref;
                if (ref < ir->count &&
                    (ir->nodes[ref].flags & IR_FLAG_SNAPSHOT_ONLY_BOX))
                    ref = ir->nodes[ref].op1;
                if (ref >= (uint16_t)ra->ssa_count) continue;

                // Constants: the exit stub runs after the whole trace, so the
                // register a constant was allocated to may hold an unrelated
                // value by then. Materialize the constant directly instead of
                // reading a possibly-clobbered register.
                sljit_sw dstOff = (sljit_sw)(slot) * 8;
                if (ref < ir->count) {
                    const IRNode* cn = &ir->nodes[ref];
                    if (cn->op == IR_CONST_NUM) {
                        // A Wren numeric Value is the raw IEEE-754 bit pattern.
                        union { double d; sljit_sw w; } bits;
                        bits.d = cn->imm.num;
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                       SLJIT_IMM, bits.w);
                        sljit_emit_op1(C, SLJIT_MOV,
                                       SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                       SLJIT_R0, 0);
                        continue;
                    }
                    if (cn->op == IR_CONST_INT) {
                        // Box the raw int as a double Value.
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                       SLJIT_IMM, cn->imm.i64);
                        sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                        SLJIT_FR0, 0, SLJIT_R0, 0);
                        sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64,
                                         SLJIT_FR0, SLJIT_R0);
                        sljit_emit_op1(C, SLJIT_MOV,
                                       SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                       SLJIT_R0, 0);
                        continue;
                    }
                    if (cn->op == IR_CONST_BOOL || cn->op == IR_CONST_NULL ||
                        cn->op == IR_CONST_OBJ) {
                        sljit_sw immVal = 0;
                        if (cn->op == IR_CONST_BOOL)
                            immVal = cn->imm.intval ? (sljit_sw)WREN_TRUE_VAL
                                                    : (sljit_sw)WREN_FALSE_VAL;
                        else if (cn->op == IR_CONST_NULL)
                            immVal = (sljit_sw)WREN_NULL_VAL;
                        else immVal = (sljit_sw)(uintptr_t)cn->imm.ptr;
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                       SLJIT_IMM, immVal);
                        sljit_emit_op1(C, SLJIT_MOV,
                                       SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                       SLJIT_R0, 0);
                        continue;
                    }
                }

                // A LOAD_STACK entry mirrors the interpreter's own slot, and
                // STORE_STACK keeps that slot current whenever the trace
                // changes it. The register is only materialized when the loop
                // actually ran, so an entry-time exit (a loop-header guard
                // firing before the body) must not read it -- copy the value
                // straight from the interpreter slot instead, which is exact
                // at every point. The source and destination coincide, so this
                // is a no-op for the slot's own entry.
                if (ref < ir->count && ir->nodes[ref].op == IR_LOAD_STACK) {
                    uint16_t srcSlot = ir->nodes[ref].imm.mem.slot;
                    if (srcSlot == slot) {
                        continue;
                    }
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   SLJIT_MEM1(REG_STACK_BASE),
                                   (sljit_sw)srcSlot * 8);
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                   SLJIT_R0, 0);
                    continue;
                }

                // A LOAD_MODULE_VAR entry is the value the interpreter pushed
                // from the module variable. Re-read it from memory rather than
                // the (possibly stale or reused) register: the deferred-module
                // writeback above has already stored the loop-carried PHI, and
                // for an entry-time exit the variable still holds its pre-loop
                // value -- both exact for the resumed interpreter.
                if (ref < ir->count && ir->nodes[ref].op == IR_LOAD_MODULE_VAR) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   SLJIT_IMM,
                                   (sljit_sw)(uintptr_t)ir->nodes[ref].imm.ptr);
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                   SLJIT_MEM1(SLJIT_R0), 0);
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                   SLJIT_R0, 0);
                    continue;
                }

                // A snapshot-box that boxes an INT value holds a raw int64,
                // not a NaN-tagged Value. Convert it to a double (which IS a
                // valid boxed Value) before writing to the interpreter stack.
                bool intToBox = (ref < ir->count &&
                                 ir->nodes[ref].type == IR_TYPE_INT);

                int is_fp, spillOff;
                int r = ssaToSljitReg(ra, ref, &is_fp, &spillOff);

                if (is_fp) {
                    // FP value (raw double) - shouldn't be in snapshots
                    // (slot_map always holds boxed Values), but handle defensively.
                    if (r >= 0) {
                        sljit_emit_fop1(C, SLJIT_MOV_F64,
                                        SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                        r, 0);
                    } else {
                        sljit_emit_fop1(C, SLJIT_MOV_F64,
                                        SLJIT_FR0, 0,
                                        SLJIT_MEM1(SLJIT_SP), (sljit_sw)spillOff);
                        sljit_emit_fop1(C, SLJIT_MOV_F64,
                                        SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                        SLJIT_FR0, 0);
                    }
                } else if (intToBox) {
                    // Raw int64 in a GP register: SCVTF to double, then
                    // bit-reinterpret as a NaN-tagged Value.
                    if (r >= 0) {
                        sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                        SLJIT_FR0, 0, r, 0);
                    } else {
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                       SLJIT_MEM1(SLJIT_SP), (sljit_sw)spillOff);
                        sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_SW,
                                        SLJIT_FR0, 0, SLJIT_R0, 0);
                    }
                    sljit_emit_fcopy(C, SLJIT_COPY_FROM_F64, SLJIT_FR0, SLJIT_R0);
                    sljit_emit_op1(C, SLJIT_MOV,
                                   SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                   SLJIT_R0, 0);
                } else {
                    // GP register (NaN-tagged Value) - write to interpreter stack.
                    if (r >= 0) {
                        sljit_emit_op1(C, SLJIT_MOV,
                                       SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                       r, 0);
                    } else {
                        // Spill slot: load via R0 (reserved scratch, safe to clobber).
                        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                                       SLJIT_MEM1(SLJIT_SP), (sljit_sw)spillOff);
                        sljit_emit_op1(C, SLJIT_MOV,
                                       SLJIT_MEM1(REG_STACK_BASE), dstOff,
                                       SLJIT_R0, 0);
                    }
                }
            }
        }

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

    if (getenv("WREN_JIT_DUMP_STUBS")) {
        for (int si = 0; si < maxSnapshots; si++) {
            sljit_uw addr = (sljit_uw)sljit_get_label_addr(exitLabels[si]);
            fprintf(stderr, "[JIT] stub si=%d addr=0x%llx jumps=%d resume=0x%llx\n",
                    si, (unsigned long long)addr, exitJumpCount[si],
                    (unsigned long long)(uintptr_t)ir->snapshots[si].resume_pc);
        }
    }

    if (getenv("WREN_JIT_DUMP_ASM")) {
        static int dumpCount = 0;
        char path[128];
        snprintf(path, sizeof(path), "/tmp/jit_trace_%d.bin", dumpCount++);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(generatedCode, 1, codeSize, f);
            fclose(f);
            fprintf(stderr, "[JIT] dumped %u bytes to %s\n",
                    (unsigned)codeSize, path);
        }
    }

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
    trace->loop_exit_snapshot = IR_NONE;
    trace->loop_exit_snapshot2 = IR_NONE;
    trace->bounds_hoisted_guards = 0;
    for (uint16_t i = 0; i < ir->count; i++) {
        if (ir->nodes[i].op == IR_LIST_BOUNDS_GUARD)
            trace->bounds_hoisted_guards++;
    }
    for (uint16_t i = ir->loop_header; i < ir->count; i++) {
        const IRNode* node = &ir->nodes[i];
        if (node->flags & IR_FLAG_DEAD) continue;
        uint16_t sid = IR_NONE;
        if (node->flags & IR_FLAG_FUSED_TRUE_GUARD) {
            sid = node->imm.snapshot_id;
        } else if (node->op == IR_GUARD_TRUE || node->op == IR_GUARD_FALSE) {
            sid = node->imm.snapshot_id;
        }
        if (sid != IR_NONE) {
            if (trace->loop_exit_snapshot == IR_NONE) {
                trace->loop_exit_snapshot = sid;
            } else if (sid != trace->loop_exit_snapshot) {
                trace->loop_exit_snapshot2 = sid;
                break;
            }
        }
    }

    // Copy snapshot data.
    trace->num_snapshots = (uint16_t)maxSnapshots;
    if (maxSnapshots > 0) {
        trace->snapshots = (JitSnapshot*)calloc((size_t)maxSnapshots,
                                                sizeof(JitSnapshot));
        trace->nested_exit_cache = (uint8_t*)calloc((size_t)maxSnapshots, 1);
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

    // Collect every GC object pointer embedded in native code. Class guards
    // contain ObjClass pointers just as constants contain Obj pointers.
    int numRoots = 0;
    for (uint16_t i = 0; i < ir->count; i++) {
        IROp op = ir->nodes[i].op;
        if ((op == IR_CONST_OBJ || op == IR_GUARD_CLASS) &&
            ir->nodes[i].imm.ptr != NULL) numRoots++;
    }
    if (numRoots > 0) {
        trace->gc_roots = (void**)calloc((size_t)numRoots, sizeof(void*));
        if (trace->gc_roots) {
            int idx = 0;
            for (uint16_t i = 0; i < ir->count; i++) {
                IROp op = ir->nodes[i].op;
                if ((op == IR_CONST_OBJ || op == IR_GUARD_CLASS) &&
                    ir->nodes[i].imm.ptr != NULL)
                    trace->gc_roots[idx++] = ir->nodes[i].imm.ptr;
            }
            trace->num_gc_roots = (uint16_t)idx;
        }
    }

    trace->exec_count = 0;
    trace->exit_count = 0;

    return trace;
}
