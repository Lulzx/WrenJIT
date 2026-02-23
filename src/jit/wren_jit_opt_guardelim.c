// ===========================================================================
// Pass 11: Guard Elimination (~300 LOC)
//
// Phase A — prove-and-delete loop-invariant guards:
//   After GVN + LICM + Guard Hoisting, some guards that were hoisted out of
//   the loop still have duplicates inside the loop body (for the same SSA
//   value). A second deduplication pass — this one NOT resetting knowledge at
//   LOOP_HEADER — catches these redundant inner-loop guards.
//
//   Additionally, GUARD_NUM on a LOAD_MODULE_VAR or LOAD_STACK can be
//   eliminated when the corresponding write (STORE_MODULE_VAR / STORE_STACK)
//   for the same address always stores a BOX_NUM result (i.e., always numeric).
//
// Phase B — snapshot-aware STORE_STACK liveness:
//   This pass marks STORE_STACK nodes that are NOT needed as live because no
//   CALL_WREN or CALL_C follows before the next SNAPSHOT or SIDE_EXIT. The
//   DCE pass's treatment of STORE_STACK as an unconditional root is overridden
//   by setting the IR_FLAG_DEAD flag on dispensable stores before DCE runs.
//   (The snapshots already capture the live SSA values; the exit stubs restore
//   the interpreter stack when needed.)
// ===========================================================================

#include "wren_jit_ir.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define BITSET_WORDS_GE ((IR_MAX_NODES + 63) / 64)

static inline void bsSet(uint64_t* bs, uint16_t id) {
    bs[id >> 6] |= (uint64_t)1 << (id & 63);
}
static inline bool bsTest(const uint64_t* bs, uint16_t id) {
    return (bs[id >> 6] & ((uint64_t)1 << (id & 63))) != 0;
}

// ---------------------------------------------------------------------------
// Phase A helpers
// ---------------------------------------------------------------------------

// Return true if the value written by STORE_MODULE_VAR / STORE_STACK is
// always a NaN-boxed number.  We consider it numeric if:
//   - its type is IR_TYPE_NUM  (already unboxed double)
//   - its op is IR_BOX_NUM     (result of boxing a double)
//   - its op is IR_CONST_NUM   (a constant double)
static bool writtenValueIsNumeric(const IRBuffer* buf, uint16_t val_id)
{
    if (val_id == IR_NONE || val_id >= buf->count) return false;
    const IRNode* v = &buf->nodes[val_id];
    if (v->type == IR_TYPE_NUM) return true;
    if (v->op == IR_BOX_NUM)    return true;
    if (v->op == IR_CONST_NUM)  return true;
    // BOX_BOOL / CONST_BOOL results are Wren booleans, not numbers.
    return false;
}

// ---------------------------------------------------------------------------
// Phase A: main guard elimination
// ---------------------------------------------------------------------------
static void phaseA(IRBuffer* buf)
{
    // guardedNum[id] = true iff we have already seen and proven GUARD_NUM(id).
    // We scan the entire buffer WITHOUT resetting at LOOP_HEADER, so guards
    // proved before the loop remain valid inside it (same SSA value).
    static uint64_t guardedNum[BITSET_WORDS_GE];
    static uint64_t guardedTrue[BITSET_WORDS_GE];
    static uint64_t guardedFalse[BITSET_WORDS_GE];
    memset(guardedNum,   0, sizeof(guardedNum));
    memset(guardedTrue,  0, sizeof(guardedTrue));
    memset(guardedFalse, 0, sizeof(guardedFalse));

    // Pre-scan: for each LOAD_MODULE_VAR / LOAD_STACK, check whether every
    // STORE_MODULE_VAR / STORE_STACK for the same slot writes a numeric value.
    // If so, mark that load SSA id as "provably numeric at all times".
    static uint64_t provenNumericLoad[BITSET_WORDS_GE];
    memset(provenNumericLoad, 0, sizeof(provenNumericLoad));

    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;

        if (n->op == IR_LOAD_MODULE_VAR) {
            // Assume numeric until proven otherwise.
            bool allNumericWrites = true;
            bool found = false;
            for (uint16_t j = 0; j < buf->count; j++) {
                const IRNode* s = &buf->nodes[j];
                if (s->flags & IR_FLAG_DEAD) continue;
                if (s->op != IR_STORE_MODULE_VAR) continue;
                if (s->imm.ptr != n->imm.ptr) continue; // different var
                found = true;
                if (!writtenValueIsNumeric(buf, s->op1)) {
                    allNumericWrites = false;
                    break;
                }
            }
            // If there's at least one matching STORE that's always numeric,
            // (or no stores at all — value unchanged from initial load), mark
            // this load as provably numeric.
            (void)found;
            if (allNumericWrites) {
                bsSet(provenNumericLoad, i);
            }
        }

        if (n->op == IR_LOAD_STACK) {
            bool allNumericWrites = true;
            for (uint16_t j = 0; j < buf->count; j++) {
                const IRNode* s = &buf->nodes[j];
                if (s->flags & IR_FLAG_DEAD) continue;
                if (s->op != IR_STORE_STACK) continue;
                if (s->imm.mem.slot != n->imm.mem.slot) continue;
                if (!writtenValueIsNumeric(buf, s->op1)) {
                    allNumericWrites = false;
                    break;
                }
            }
            if (allNumericWrites) {
                bsSet(provenNumericLoad, i);
            }
        }
    }

    // Pre-mark arithmetic and constant nodes as guardedNum (they cannot
    // possibly be non-numbers).
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        switch (n->op) {
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
            case IR_NEG: case IR_CONST_NUM: case IR_UNBOX_NUM: case IR_UNBOX_INT:
                bsSet(guardedNum, i);
                break;
            default:
                if (n->type == IR_TYPE_NUM || n->type == IR_TYPE_INT)
                    bsSet(guardedNum, i);
                break;
        }
        if (bsTest(provenNumericLoad, i))
            bsSet(guardedNum, i);
    }

    // Now walk the IR, eliminating redundant guards (no reset at loop header).
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op1 == IR_NONE) continue;
        uint16_t val = n->op1;

        switch (n->op) {
            case IR_GUARD_NUM:
                if (bsTest(guardedNum, val)) {
                    // Guard is provably unnecessary — kill it.
                    n->op    = IR_NOP;
                    n->op1   = IR_NONE;
                    n->op2   = IR_NONE;
                    n->flags |= IR_FLAG_DEAD;
                } else {
                    bsSet(guardedNum, val);
                }
                break;

            case IR_GUARD_TRUE:
                if (bsTest(guardedTrue, val)) {
                    n->op    = IR_NOP;
                    n->op1   = IR_NONE;
                    n->op2   = IR_NONE;
                    n->flags |= IR_FLAG_DEAD;
                } else {
                    bsSet(guardedTrue, val);
                }
                break;

            case IR_GUARD_FALSE:
                if (bsTest(guardedFalse, val)) {
                    n->op    = IR_NOP;
                    n->op1   = IR_NONE;
                    n->op2   = IR_NONE;
                    n->flags |= IR_FLAG_DEAD;
                } else {
                    bsSet(guardedFalse, val);
                }
                break;

            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Phase B: mark dispensable STORE_STACK nodes as dead before DCE.
//
// A STORE_STACK is dispensable only when ALL of the following hold:
//   1. No CALL_WREN or CALL_C follows it before the next SNAPSHOT/SIDE_EXIT.
//   2. The stored slot is NOT loaded by any LOAD_STACK inside the loop body
//      (between LOOP_HEADER and LOOP_BACK inclusive), because LOOP_BACK jumps
//      back to LOOP_HEADER and re-executes those LOAD_STACK nodes from the
//      interpreter stack on the next iteration.
//
// Without condition 2, eliminating STORE_STACK for a loop variable would
// cause LOAD_STACK at the top of the next iteration to read a stale value,
// producing an infinite loop.
// ---------------------------------------------------------------------------
static void phaseB(IRBuffer* buf)
{
    // Find the loop header and loop back indices.
    uint16_t loopHeader = buf->count; // sentinel: none
    uint16_t loopBack   = buf->count;
    for (uint16_t i = 0; i < buf->count; i++) {
        IROp op = buf->nodes[i].op;
        if (op == IR_LOOP_HEADER && loopHeader == buf->count) loopHeader = i;
        if (op == IR_LOOP_BACK)   loopBack   = i;
    }

    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op != IR_STORE_STACK) continue;

        uint16_t slot = n->imm.mem.slot;

        // Condition 2: if the slot is loaded inside the loop body
        // (loopHeader..loopBack), the STORE_STACK must be kept.
        if (loopHeader < buf->count && loopBack < buf->count) {
            bool loadedInLoop = false;
            for (uint16_t k = loopHeader; k <= loopBack && k < buf->count; k++) {
                const IRNode* m = &buf->nodes[k];
                if (m->flags & IR_FLAG_DEAD) continue;
                if (m->op == IR_LOAD_STACK && m->imm.mem.slot == slot) {
                    loadedInLoop = true;
                    break;
                }
            }
            if (loadedInLoop) continue; // must keep this STORE_STACK
        }

        // Condition 1: no CALL between here and the next snapshot/exit.
        bool needs_live_stack = false;
        for (uint16_t k = (uint16_t)(i + 1); k < buf->count; k++) {
            IROp op = buf->nodes[k].op;
            if (buf->nodes[k].flags & IR_FLAG_DEAD) continue;
            if (op == IR_CALL_WREN || op == IR_CALL_C) {
                needs_live_stack = true;
                break;
            }
            if (op == IR_SNAPSHOT || op == IR_SIDE_EXIT || op == IR_LOOP_BACK) {
                break;
            }
        }

        if (!needs_live_stack) {
            n->flags |= IR_FLAG_DEAD;
        }
    }
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
void irOptGuardElim(IRBuffer* buf)
{
    if (!buf || buf->count == 0) return;
    phaseA(buf);
    phaseB(buf);
}
