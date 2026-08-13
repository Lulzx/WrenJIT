#include "wren_jit_opt.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===========================================================================
// Bitset helpers (one bit per IR node)
// ===========================================================================
#define BITSET_WORDS ((IR_MAX_NODES + 63) / 64)

static inline void bitSet(uint64_t* bs, uint16_t id)
{
    bs[id >> 6] |= (uint64_t)1 << (id & 63);
}

static inline bool bitTest(const uint64_t* bs, uint16_t id)
{
    return (bs[id >> 6] & ((uint64_t)1 << (id & 63))) != 0;
}

// ===========================================================================
// Predicate helpers
// ===========================================================================

static inline bool isArith(IROp op)
{
    return op == IR_ADD || op == IR_SUB ||
           op == IR_MUL || op == IR_DIV || op == IR_MOD;
}

static inline bool isCmp(IROp op)
{
    return op == IR_LT  || op == IR_LTE ||
           op == IR_GT  || op == IR_GTE ||
           op == IR_EQ  || op == IR_NEQ;
}

static inline bool isGuard(IROp op)
{
    return op == IR_GUARD_NUM    || op == IR_GUARD_BOOL ||
           op == IR_GUARD_CLASS ||
           op == IR_GUARD_TRUE   || op == IR_GUARD_FALSE ||
           op == IR_GUARD_NOT_NULL || op == IR_GUARD_RANGE;
}

// Return the deoptimization snapshot used by a guard/exit node.
static uint16_t exitSnapshotId(const IRNode* n)
{
    if (n->op == IR_GUARD_CLASS) return n->op2;
    if (n->op == IR_SIDE_EXIT || n->op == IR_GUARD_NUM ||
        n->op == IR_GUARD_BOOL ||
        n->op == IR_GUARD_TRUE || n->op == IR_GUARD_FALSE ||
        n->op == IR_GUARD_NOT_NULL || n->op == IR_GUARD_RANGE)
        return n->imm.snapshot_id;
    if (n->op == IR_LOOP_EXIT)
        return n->imm.jump.snapshot;
    return IR_NONE;
}

static inline bool isConst(IROp op)
{
    return op == IR_CONST_NUM  || op == IR_CONST_BOOL ||
           op == IR_CONST_NULL || op == IR_CONST_OBJ  ||
           op == IR_CONST_INT;
}

static bool hasSideEffect(const IRNode* n)
{
    switch (n->op) {
        case IR_STORE_STACK:
        case IR_STORE_FIELD:
        case IR_LIST_STORE:
        case IR_STORE_MODULE_VAR:
        case IR_GUARD_NUM:
        case IR_GUARD_BOOL:
        case IR_GUARD_CLASS:
        case IR_GUARD_TRUE:
        case IR_GUARD_FALSE:
        case IR_GUARD_NOT_NULL:
        case IR_SIDE_EXIT:
        case IR_SNAPSHOT:
        case IR_CALL_C:
        case IR_CALL_WREN:
        case IR_LOOP_HEADER:
        case IR_LOOP_BACK:
        case IR_LOOP_EXIT:
            return true;
        default:
            return false;
    }
}

// Kill a node (mark dead, turn to NOP).
static void killNode(IRNode* n)
{
    n->op    = IR_NOP;
    n->op1   = IR_NONE;
    n->op2   = IR_NONE;
    memset(&n->imm, 0, sizeof(n->imm));
    n->flags |= IR_FLAG_DEAD;
}

// Rewrite a NOP slot in place as a fresh node.
static void initSlotNode(IRBuffer* buf, uint16_t slot, IROp op,
                         uint16_t op1, uint16_t op2, IRType type)
{
    IRNode* n = &buf->nodes[slot];
    memset(n, 0, sizeof(IRNode));
    n->op   = op;
    n->id   = slot;
    n->op1  = op1;
    n->op2  = op2;
    n->type = type;
}

// Replace every use of SSA id |old| with |rep| in the buffer.
static void replaceUses(IRBuffer* buf, uint16_t old, uint16_t rep)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op == IR_NOP) continue;
        // LOAD_MODULE_VAR's op1 is the module index (an immediate), not an
        // SSA reference.  Rewriting it when `old` happens to equal the index
        // corrupts the variable identity: LICM hoists a node whose *node id*
        // equals the module index (e.g. mass=33), and replaceUses then turns
        // every other mass load's op1 into the hoist slot.  imm.ptr stays
        // intact so codegen is unaffected, but regalloc iterates op1/op2 as
        // live-range dependencies and sees a bogus edge to the hoist slot.
        if (n->op != IR_LOAD_MODULE_VAR) {
            if (n->op1 == old) n->op1 = rep;
        }
        // GUARD_CLASS stores its snapshot id in op2 (not imm.snapshot_id),
        // so op2 is an immediate there too.  regalloc already special-cases
        // this; keep it out of use-rewrites the same way.
        if (n->op != IR_GUARD_CLASS) {
            if (n->op2 == old) n->op2 = rep;
        }
    }
    // Also update snapshot entries.
    for (uint16_t i = 0; i < buf->snapshot_entry_count; i++) {
        if (buf->snapshot_entries[i].ssa_ref == old)
            buf->snapshot_entries[i].ssa_ref = rep;
    }
    for (uint16_t i = 0; i < buf->exit_module_entry_count; i++) {
        if (buf->exit_module_entries[i].ssa_ref == old)
            buf->exit_module_entries[i].ssa_ref = rep;
    }
}

// Find the index of IR_LOOP_HEADER, or IR_NONE if absent.
static uint16_t findLoopHeader(const IRBuffer* buf)
{
    if (buf->loop_header < buf->count &&
        buf->nodes[buf->loop_header].op == IR_LOOP_HEADER)
        return buf->loop_header;
    for (uint16_t i = 0; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_HEADER) return i;
    }
    return IR_NONE;
}

// Find the index of IR_LOOP_BACK, or IR_NONE if absent.
static uint16_t findLoopBack(const IRBuffer* buf)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_BACK) return i;
    }
    return IR_NONE;
}

// ===========================================================================
// Pass 1: Box/Unbox Elimination (~200 LOC)
//
// Phase 1: Adjacent-pair cancellation.
//   BOX_NUM(UNBOX_NUM(x))  => x
//   UNBOX_NUM(BOX_NUM(x))  => x
//   BOX_OBJ(UNBOX_OBJ(x))  => x
//   UNBOX_OBJ(BOX_OBJ(x))  => x
//
// Phase 2: Use-count based elimination.
//   If a BOX_NUM(x) is only consumed by UNBOX_NUM nodes, replace every
//   UNBOX_NUM user with x directly, then mark the BOX_NUM dead.
// ===========================================================================
void irOptBoxUnboxElim(IRBuffer* buf)
{
    // --- Phase 1: adjacent-pair cancellation ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];

        if (n->op == IR_BOX_NUM && n->op1 != IR_NONE) {
            IRNode* src = &buf->nodes[n->op1];
            if (src->op == IR_UNBOX_NUM) {
                replaceUses(buf, i, src->op1);
                killNode(n);
                continue;
            }
        }

        if (n->op == IR_UNBOX_NUM && n->op1 != IR_NONE) {
            IRNode* src = &buf->nodes[n->op1];
            if (src->op == IR_BOX_NUM) {
                replaceUses(buf, i, src->op1);
                killNode(n);
                continue;
            }
            if (src->op == IR_CONST_NUM) {
                replaceUses(buf, i, n->op1);
                killNode(n);
                continue;
            }
        }

        if (n->op == IR_UNBOX_INT && n->op1 != IR_NONE) {
            IRNode* src = &buf->nodes[n->op1];
            // An IR_TYPE_INT source is already a raw integer (CONST_INT, IV
            // inference, int arithmetic, or a prior UNBOX_INT). Unboxing is
            // the identity, and its INT_GUARD is vacuously true.
            if (src->type == IR_TYPE_INT) {
                replaceUses(buf, i, n->op1);
                killNode(n);
                continue;
            }
        }

        if (n->op == IR_BOX_OBJ && n->op1 != IR_NONE) {
            IRNode* src = &buf->nodes[n->op1];
            if (src->op == IR_UNBOX_OBJ) {
                replaceUses(buf, i, src->op1);
                killNode(n);
                continue;
            }
        }

        if (n->op == IR_UNBOX_OBJ && n->op1 != IR_NONE) {
            IRNode* src = &buf->nodes[n->op1];
            if (src->op == IR_BOX_OBJ) {
                replaceUses(buf, i, src->op1);
                killNode(n);
                continue;
            }
        }
    }

    // --- Phase 2: use-count based elimination for BOX_NUM ---
    // If a BOX_NUM's *only* consumers are UNBOX_NUM, bypass the box entirely.
    //
    // Build two parallel use-count arrays in one O(n) forward pass:
    //   useCounts[i]      = total number of uses of node i
    //   unboxUseCounts[i] = how many of those uses are IR_UNBOX_NUM
    // A BOX_NUM i is eliminable iff useCounts[i] == unboxUseCounts[i] > 0
    // and it does not appear in any snapshot entry.
    static uint16_t useCounts[IR_MAX_NODES];
    static uint16_t unboxUseCounts[IR_MAX_NODES];

    memset(useCounts,      0, sizeof(uint16_t) * buf->count);
    memset(unboxUseCounts, 0, sizeof(uint16_t) * buf->count);

    for (uint16_t j = 0; j < buf->count; j++) {
        const IRNode* u = &buf->nodes[j];
        if (u->op == IR_NOP) continue;
        if (u->op1 != IR_NONE && u->op1 < buf->count) {
            useCounts[u->op1]++;
            if (u->op == IR_UNBOX_NUM) unboxUseCounts[u->op1]++;
        }
        if (u->op2 != IR_NONE && u->op2 < buf->count) {
            useCounts[u->op2]++;
            if (u->op == IR_UNBOX_NUM) unboxUseCounts[u->op2]++;
        }
    }

    // Build snapshot escape bitset in one pass.
    static uint64_t inSnapshot[BITSET_WORDS];
    memset(inSnapshot, 0, sizeof(inSnapshot));
    for (uint16_t s = 0; s < buf->snapshot_entry_count; s++) {
        uint16_t ref = buf->snapshot_entries[s].ssa_ref;
        if (ref != IR_NONE && ref < buf->count)
            bitSet(inSnapshot, ref);
    }

    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op != IR_BOX_NUM || n->op1 == IR_NONE) continue;
        if (useCounts[i] == 0) continue;
        if (bitTest(inSnapshot, i)) continue;        // escapes via snapshot
        if (useCounts[i] != unboxUseCounts[i]) continue; // non-UNBOX_NUM user

        uint16_t rawInput = n->op1;

        // Redirect each UNBOX_NUM consumer to use rawInput directly.
        for (uint16_t j = 0; j < buf->count; j++) {
            IRNode* u = &buf->nodes[j];
            if (u->op != IR_UNBOX_NUM || u->op1 != i) continue;
            replaceUses(buf, j, rawInput);
            killNode(u);
        }

        // Now the BOX_NUM itself has no users.
        killNode(n);
    }
}

// ===========================================================================
// Pass 2: Redundant Guard Elimination (~150 LOC)
//
// Track which SSA values have been guarded using bitsets. If a guard for the
// same value and same kind appears again, kill the duplicate. Reset knowledge
// at the loop header (guards inside the loop may see different dynamic values
// from those in the prologue).
// ===========================================================================
void irOptRedundantGuardElim(IRBuffer* buf)
{
    static uint64_t guardedNum[BITSET_WORDS];
    static uint64_t guardedTrue[BITSET_WORDS];
    static uint64_t guardedFalse[BITSET_WORDS];
    static uint64_t guardedNotNull[BITSET_WORDS];

    memset(guardedNum,     0, sizeof(guardedNum));
    memset(guardedTrue,    0, sizeof(guardedTrue));
    memset(guardedFalse,   0, sizeof(guardedFalse));
    memset(guardedNotNull, 0, sizeof(guardedNotNull));

    // For GUARD_CLASS we track the class pointer per SSA id.
    static void* guardedClassPtr[IR_MAX_NODES];
    memset(guardedClassPtr, 0, sizeof(guardedClassPtr));

    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];

        // Reset at loop header.
        if (n->op == IR_LOOP_HEADER) {
            memset(guardedNum,     0, sizeof(guardedNum));
            memset(guardedTrue,    0, sizeof(guardedTrue));
            memset(guardedFalse,   0, sizeof(guardedFalse));
            memset(guardedNotNull, 0, sizeof(guardedNotNull));
            memset(guardedClassPtr, 0, sizeof(guardedClassPtr));
            continue;
        }

        if (n->op == IR_NOP || n->op1 == IR_NONE) continue;
        uint16_t val = n->op1;

        switch (n->op) {
            case IR_GUARD_NUM:
                if (buf->nodes[val].op == IR_BOX_NUM ||
                    buf->nodes[val].op == IR_BOX_INT) {
                    killNode(n); // producer proves Wren Num representation
                } else if (bitTest(guardedNum, val)) {
                    killNode(n);
                } else {
                    bitSet(guardedNum, val);
                }
                break;

            case IR_GUARD_TRUE:
                // Every Wren number and object is truthy, including zero and
                // NaN. A boxing producer therefore proves this guard.
                if (buf->nodes[val].op == IR_BOX_NUM ||
                    buf->nodes[val].op == IR_BOX_INT ||
                    buf->nodes[val].op == IR_BOX_OBJ) {
                    killNode(n);
                } else if (bitTest(guardedTrue, val)) {
                    killNode(n);
                } else {
                    bitSet(guardedTrue, val);
                }
                break;

            case IR_GUARD_FALSE:
                if (bitTest(guardedFalse, val)) {
                    killNode(n);
                } else {
                    bitSet(guardedFalse, val);
                }
                break;

            case IR_GUARD_NOT_NULL:
                if (bitTest(guardedNotNull, val)) {
                    killNode(n);
                } else {
                    bitSet(guardedNotNull, val);
                }
                break;

            case IR_GUARD_CLASS:
                if (guardedClassPtr[val] != NULL &&
                    guardedClassPtr[val] == n->imm.ptr) {
                    killNode(n);
                } else {
                    guardedClassPtr[val] = n->imm.ptr;
                }
                break;

            default:
                break;
        }
    }
}

// ===========================================================================
// Pass 3: Constant Propagation & Folding (~250 LOC)
//
// - PHI propagation: collapse PHI nodes with identical inputs.
// - Constant folding for arithmetic, comparisons, bitwise, unary NEG/BNOT.
// - Algebraic identities (x+0, x*1, x*0, x/1, etc.).
// - Guard elimination when argument is a known constant.
// ===========================================================================
void irOptConstPropFold(IRBuffer* buf)
{
    // --- PHI propagation ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op != IR_PHI) continue;
        if (n->op1 == IR_NONE || n->op2 == IR_NONE) continue;

        // Same SSA id on both inputs.
        if (n->op1 == n->op2) {
            replaceUses(buf, i, n->op1);
            killNode(n);
            continue;
        }

        // Both inputs are the same constant number.
        IRNode* a = &buf->nodes[n->op1];
        IRNode* b = &buf->nodes[n->op2];
        if (a->op == IR_CONST_NUM && b->op == IR_CONST_NUM &&
            a->imm.num == b->imm.num) {
            replaceUses(buf, i, n->op1);
            killNode(n);
            continue;
        }
    }

    // --- Constant folding and algebraic identities ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];

        // Fold unary NEG of constant.
        if (n->op == IR_NEG && n->op1 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            if (a->op == IR_CONST_NUM) {
                n->op       = IR_CONST_NUM;
                n->type     = IR_TYPE_NUM;
                n->imm.num  = -a->imm.num;
                n->op1      = IR_NONE;
                continue;
            }
        }

        // Fold unary BNOT of const int.
        if (n->op == IR_BNOT && n->op1 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            if (a->op == IR_CONST_INT) {
                n->op       = IR_CONST_INT;
                n->type     = IR_TYPE_INT;
                n->imm.i64  = ~a->imm.i64;
                n->op1      = IR_NONE;
                continue;
            }
        }

        // Fold binary arithmetic (doubles).
        if (isArith(n->op) && n->op1 != IR_NONE && n->op2 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            IRNode* b = &buf->nodes[n->op2];

            if (a->op == IR_CONST_NUM && b->op == IR_CONST_NUM) {
                double result = 0;
                switch (n->op) {
                    case IR_ADD: result = a->imm.num + b->imm.num; break;
                    case IR_SUB: result = a->imm.num - b->imm.num; break;
                    case IR_MUL: result = a->imm.num * b->imm.num; break;
                    case IR_DIV: result = a->imm.num / b->imm.num; break;
                    case IR_MOD: result = fmod(a->imm.num, b->imm.num); break;
                    default: break;
                }
                n->op       = IR_CONST_NUM;
                n->type     = IR_TYPE_NUM;
                n->imm.num  = result;
                n->op1      = IR_NONE;
                n->op2      = IR_NONE;
                continue;
            }

            // --- Algebraic identities ---

            // x + 0 => x, x - 0 => x, 0 + x => x
            if (n->op == IR_ADD || n->op == IR_SUB) {
                if (b->op == IR_CONST_NUM && b->imm.num == 0.0) {
                    replaceUses(buf, i, n->op1);
                    killNode(n);
                    continue;
                }
                if (n->op == IR_ADD && a->op == IR_CONST_NUM &&
                    a->imm.num == 0.0) {
                    replaceUses(buf, i, n->op2);
                    killNode(n);
                    continue;
                }
            }

            // x * 1 => x, 1 * x => x, x * 0 => 0
            if (n->op == IR_MUL) {
                if (b->op == IR_CONST_NUM && b->imm.num == 1.0) {
                    replaceUses(buf, i, n->op1);
                    killNode(n);
                    continue;
                }
                if (a->op == IR_CONST_NUM && a->imm.num == 1.0) {
                    replaceUses(buf, i, n->op2);
                    killNode(n);
                    continue;
                }
                if ((b->op == IR_CONST_NUM && b->imm.num == 0.0) ||
                    (a->op == IR_CONST_NUM && a->imm.num == 0.0)) {
                    n->op       = IR_CONST_NUM;
                    n->type     = IR_TYPE_NUM;
                    n->imm.num  = 0.0;
                    n->op1      = IR_NONE;
                    n->op2      = IR_NONE;
                    continue;
                }
            }

            // x / 1 => x
            if (n->op == IR_DIV) {
                if (b->op == IR_CONST_NUM && b->imm.num == 1.0) {
                    replaceUses(buf, i, n->op1);
                    killNode(n);
                    continue;
                }
            }
        }

        // Fold comparisons of constant doubles.
        if (isCmp(n->op) && n->op1 != IR_NONE && n->op2 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            IRNode* b = &buf->nodes[n->op2];

            if (a->op == IR_CONST_NUM && b->op == IR_CONST_NUM) {
                int result = 0;
                switch (n->op) {
                    case IR_LT:  result = a->imm.num <  b->imm.num; break;
                    case IR_LTE: result = a->imm.num <= b->imm.num; break;
                    case IR_GT:  result = a->imm.num >  b->imm.num; break;
                    case IR_GTE: result = a->imm.num >= b->imm.num; break;
                    case IR_EQ:  result = a->imm.num == b->imm.num; break;
                    case IR_NEQ: result = a->imm.num != b->imm.num; break;
                    default: break;
                }
                n->op         = IR_CONST_BOOL;
                n->type       = IR_TYPE_BOOL;
                n->imm.intval = result;
                n->op1        = IR_NONE;
                n->op2        = IR_NONE;
                continue;
            }
        }

        // Fold bitwise ops on const-int operands.
        if ((n->op == IR_BAND || n->op == IR_BOR  || n->op == IR_BXOR ||
             n->op == IR_LSHIFT || n->op == IR_RSHIFT || n->op == IR_ASHR) &&
            n->op1 != IR_NONE && n->op2 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            IRNode* b = &buf->nodes[n->op2];
            if (a->op == IR_CONST_INT && b->op == IR_CONST_INT) {
                int64_t result = 0;
                switch (n->op) {
                    case IR_BAND:   result = a->imm.i64 & b->imm.i64; break;
                    case IR_BOR:    result = a->imm.i64 | b->imm.i64; break;
                    case IR_BXOR:   result = a->imm.i64 ^ b->imm.i64; break;
                    case IR_LSHIFT: result = a->imm.i64 << b->imm.i64; break;
                    case IR_RSHIFT: result = a->imm.i64 >> b->imm.i64; break;
                    case IR_ASHR:   result = a->imm.i64 >> b->imm.i64; break;
                    default: break;
                }
                n->op      = IR_CONST_INT;
                n->type    = IR_TYPE_INT;
                n->imm.i64 = result;
                n->op1     = IR_NONE;
                n->op2     = IR_NONE;
                continue;
            }
        }

        // GUARD_TRUE(CONST_BOOL(1)) => dead (always passes).
        if (n->op == IR_GUARD_TRUE && n->op1 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            if (a->op == IR_CONST_BOOL && a->imm.intval != 0) {
                killNode(n);
                continue;
            }
        }

        // GUARD_FALSE(CONST_BOOL(0)) => dead (always passes).
        if (n->op == IR_GUARD_FALSE && n->op1 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            if (a->op == IR_CONST_BOOL && a->imm.intval == 0) {
                killNode(n);
                continue;
            }
        }

        // GUARD_NUM on output of arithmetic/UNBOX_NUM/CONST_NUM => dead.
        if (n->op == IR_GUARD_NUM && n->op1 != IR_NONE) {
            IRNode* a = &buf->nodes[n->op1];
            if (isArith(a->op) || a->op == IR_NEG || a->op == IR_SQRT ||
                a->op == IR_FLOOR || a->op == IR_CONST_NUM ||
                a->op == IR_CONST_INT || a->op == IR_UNBOX_NUM) {
                killNode(n);
                continue;
            }
        }
    }
}

// ===========================================================================
// Pass 4: Global Value Numbering (hash-based dedup, ~250 LOC)
// ===========================================================================

#define GVN_TABLE_SIZE 2048
#define GVN_TABLE_MASK (GVN_TABLE_SIZE - 1)

static uint32_t gvnHash(const IRNode* n)
{
    uint32_t h = (uint32_t)n->op * 2654435761u;
    h ^= (uint32_t)n->type  * 2246822519u;
    h ^= (uint32_t)n->op1   * 3266489917u;
    h ^= (uint32_t)n->op2   * 668265263u;

    uint64_t raw = 0;
    memcpy(&raw, &n->imm,
           sizeof(raw) < sizeof(n->imm) ? sizeof(raw) : sizeof(n->imm));
    h ^= (uint32_t)(raw & 0xFFFFFFFF)   * 374761393u;
    h ^= (uint32_t)(raw >> 32)          * 2246822519u;
    return h;
}

static bool gvnEqual(const IRNode* a, const IRNode* b)
{
    return a->op   == b->op   &&
           a->type == b->type &&
           a->op1  == b->op1  &&
           a->op2  == b->op2  &&
           memcmp(&a->imm, &b->imm, sizeof(a->imm)) == 0;
}

void irOptGVN(IRBuffer* buf)
{
    static uint16_t table[GVN_TABLE_SIZE];
    memset(table, 0xFF, sizeof(table)); // fill with IR_NONE

    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op == IR_NOP || hasSideEffect(n)) continue;
        // Mutable memory reads are not ordinary pure expressions.  A store
        // between two syntactically identical loads can change the result,
        // and this table does not carry a memory version.  Keep them out of
        // GVN; the later alias-aware forwarding pass handles LOAD_FIELD when
        // it can prove which store supplies the value.
        if (n->op == IR_LOAD_STACK || n->op == IR_LOAD_FIELD ||
            n->op == IR_LOAD_MODULE_VAR)
            continue;
        // Do not deduplicate PHI or loop-control nodes.
        if (n->op == IR_PHI || n->op == IR_LOOP_HEADER ||
            n->op == IR_LOOP_BACK)
            continue;

        uint32_t h = gvnHash(n) & GVN_TABLE_MASK;

        for (uint32_t probe = 0; probe < GVN_TABLE_SIZE; probe++) {
            uint32_t idx = (h + probe) & GVN_TABLE_MASK;
            if (table[idx] == IR_NONE) {
                table[idx] = i;
                break;
            }

            IRNode* existing = &buf->nodes[table[idx]];
            if (existing->op == IR_NOP) {
                // Slot is stale; reuse.
                table[idx] = i;
                break;
            }

            if (gvnEqual(existing, n)) {
                replaceUses(buf, i, table[idx]);
                killNode(n);
                break;
            }
        }
    }
}

// ===========================================================================
// Pass 5: Loop-Invariant Code Motion (LICM, ~200 LOC)
//
// Walk nodes between LOOP_HEADER and LOOP_BACK. If a node's operands are
// all defined before LOOP_HEADER (or are constants or already marked
// invariant), the node is invariant. Move invariant nodes to an empty NOP
// slot before the loop header.
// ===========================================================================
void irOptLICM(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE) return;

    // The LAST back edge is the anchor loop's; nested loops close before it.
    // Alias scans must cover the whole loop body (including the anchor
    // back-edge stores that follow a nested loop), or a LOAD_STACK inside a
    // nested loop would be wrongly marked invariant and hoisted with a stale
    // read.
    uint16_t back = IR_NONE;
    for (uint16_t i = 0; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_BACK) back = i;
    }
    if (back == IR_NONE) return;

    // First pass: mark nodes that are loop-invariant.
    // We iterate until no more changes (fixed-point), because an invariant
    // node's result makes downstream nodes potentially invariant too.
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint16_t i = header + 1; i < back; i++) {
            IRNode* n = &buf->nodes[i];
            if (n->op == IR_NOP || hasSideEffect(n)) continue;
            if (n->op == IR_PHI) continue;
            if (n->flags & IR_FLAG_INVARIANT) continue;

            // Memory reads whose slot/var is written inside the loop body are
            // NOT invariant — hoisting them would cause stale reads on the
            // next iteration (classic LICM alias check).
            if (n->op == IR_LOAD_STACK) {
                uint16_t slot = n->imm.mem.slot;
                bool written = false;
                for (uint16_t k = header + 1; k < back && !written; k++) {
                    const IRNode* s = &buf->nodes[k];
                    if (s->flags & IR_FLAG_DEAD) continue;
                    if (s->op == IR_STORE_STACK && s->imm.mem.slot == slot)
                        written = true;
                }
                if (written) continue; // leave not-invariant
            }
            if (n->op == IR_LOAD_MODULE_VAR) {
                void* var_ptr = n->imm.ptr;
                bool written = false;
                for (uint16_t k = header + 1; k < back && !written; k++) {
                    const IRNode* s = &buf->nodes[k];
                    if (s->flags & IR_FLAG_DEAD) continue;
                    if (s->op == IR_STORE_MODULE_VAR && s->imm.ptr == var_ptr)
                        written = true;
                }
                if (written) continue; // leave not-invariant
            }
            // Keep field reads in the loop. Besides stores/calls, moving a
            // field load changes its SSA id and can invalidate the register
            // lifetime expected by field consumers after LICM compaction.
            if (n->op == IR_LOAD_FIELD) continue;
            if (n->op == IR_LIST_LOAD) {
                // The list object is identified by how its SSA value is
                // produced: a module variable, a stack slot, or a field.  Two
                // LIST_LOAD/LIST_STORE nodes for the same underlying list get
                // distinct SSA ids (separate LOAD_MODULE_VAR emissions), so
                // compare the storage key, not the SSA id.  If any LIST_STORE
                // in the loop writes the same list, hoisting the load would
                // read a stale element on the next iteration.
                void* listKey = NULL;
                int   slotKey = -1;
                uint16_t listSsa = n->op1;
                if (listSsa < buf->count) {
                    const IRNode* ln = &buf->nodes[listSsa];
                    if (ln->op == IR_LOAD_MODULE_VAR) {
                        listKey = ln->imm.ptr;
                    } else if (ln->op == IR_LOAD_STACK) {
                        slotKey = ln->imm.mem.slot;
                    }
                }
                bool listAliased = false;
                for (uint16_t k = header + 1; k < back && !listAliased; k++) {
                    const IRNode* s = &buf->nodes[k];
                    if (s->flags & IR_FLAG_DEAD) continue;
                    if (s->op != IR_LIST_STORE) continue;
                    uint16_t sList = s->op1;
                    if (sList >= buf->count) { listAliased = true; break; }
                    const IRNode* sn = &buf->nodes[sList];
                    if (listKey != NULL && sn->op == IR_LOAD_MODULE_VAR &&
                        sn->imm.ptr == listKey) {
                        listAliased = true;
                    } else if (slotKey >= 0 && sn->op == IR_LOAD_STACK &&
                               sn->imm.mem.slot == slotKey) {
                        listAliased = true;
                    } else if (listKey == NULL && slotKey < 0) {
                        // Unknown list source: be conservative.
                        listAliased = true;
                    }
                }
                if (listAliased) continue; // leave not-invariant
                // Fall through to the generic operand-invariance check.
            }
            // IR_LOAD_RANGE dereferences its operand, which is only safe after
            // the GUARD_CLASS proving it is a Range. Guards carry side effects
            // and so never hoist, and hoisting the load past one would
            // dereference whatever the slot happens to hold.
            if (n->op == IR_LOAD_RANGE || n->op == IR_LIST_COUNT) continue;

            bool invariant = true;

            if (n->op1 != IR_NONE && n->op1 < buf->count) {
                if (n->op1 >= header) {
                    IRNode* o = &buf->nodes[n->op1];
                    if (!(o->flags & IR_FLAG_INVARIANT) && !isConst(o->op))
                        invariant = false;
                } else {
                    // Pre-header: PHI nodes change each iteration — not invariant.
                    if (buf->nodes[n->op1].op == IR_PHI)
                        invariant = false;
                }
            }
            if (n->op2 != IR_NONE && n->op2 < buf->count) {
                if (n->op2 >= header) {
                    IRNode* o = &buf->nodes[n->op2];
                    if (!(o->flags & IR_FLAG_INVARIANT) && !isConst(o->op))
                        invariant = false;
                } else {
                    if (buf->nodes[n->op2].op == IR_PHI)
                        invariant = false;
                }
            }

            if (invariant) {
                n->flags |= IR_FLAG_INVARIANT;
                changed = true;
            }
        }
    }

    // Second pass: move invariant nodes before the loop header.
    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* n = &buf->nodes[i];
        if (!(n->flags & IR_FLAG_INVARIANT)) continue;
        if (n->flags & IR_FLAG_HOISTED) continue;

        // Find an empty NOP slot before the header.
        for (uint16_t j = 0; j < header; j++) {
            if (buf->nodes[j].op == IR_NOP) {
                buf->nodes[j]    = *n;
                buf->nodes[j].id = j;
                buf->nodes[j].flags |= IR_FLAG_HOISTED;
                replaceUses(buf, i, j);
                killNode(n);
                break;
            }
        }
    }
}

// ===========================================================================
// Pass 6: Guard Hoisting (~150 LOC)
//
// Guards inside the loop whose operand is defined before the loop header
// (or is a constant) can be hoisted to before the loop, avoiding redundant
// type checks on each iteration.
// ===========================================================================
void irOptGuardHoist(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE) return;

    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;

    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* n = &buf->nodes[i];
        if (!isGuard(n->op)) continue;
        if (n->flags & IR_FLAG_HOISTED) continue;
        if (n->op1 == IR_NONE) continue;

        // The guard's operand must be defined before the loop,
        // but NOT be a PHI (which changes each iteration).
        if (n->op1 >= header) continue;
        if (buf->nodes[n->op1].op == IR_PHI) continue;

        // Find an empty NOP slot before the header.
        for (uint16_t j = 0; j < header; j++) {
            if (buf->nodes[j].op == IR_NOP) {
                buf->nodes[j]    = *n;
                buf->nodes[j].id = j;
                buf->nodes[j].flags |= IR_FLAG_HOISTED;
                killNode(n);
                break;
            }
        }
    }
}

// Hoist immutable ObjRange shape reads after the receiver's class guard. LICM
// cannot do this initially because dereferencing the boxed receiver is unsafe
// until GUARD_CLASS has executed.
void irOptHoistGuardedRangeLoads(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    uint16_t back = findLoopBack(buf);
    if (header == IR_NONE || back == IR_NONE) return;

    for (uint16_t i = (uint16_t)(header + 1); i < back; i++) {
        IRNode* load = &buf->nodes[i];
        if ((load->flags & IR_FLAG_DEAD) || load->op != IR_LOAD_RANGE ||
            load->op1 == IR_NONE || load->op1 >= header) continue;

        uint16_t guard = IR_NONE;
        for (uint16_t g = 0; g < header; g++) {
            const IRNode* candidate = &buf->nodes[g];
            if (!(candidate->flags & IR_FLAG_DEAD) &&
                candidate->op == IR_GUARD_CLASS &&
                candidate->op1 == load->op1) {
                guard = g;
                break;
            }
        }
        if (guard == IR_NONE) continue;

        for (uint16_t j = (uint16_t)(guard + 1); j < header; j++) {
            if (buf->nodes[j].op != IR_NOP) continue;
            buf->nodes[j] = *load;
            buf->nodes[j].id = j;
            buf->nodes[j].flags |= IR_FLAG_INVARIANT | IR_FLAG_HOISTED;
            replaceUses(buf, i, j);
            killNode(load);
            break;
        }
    }
}

// ===========================================================================
// Pass 7: Strength Reduction (~150 LOC)
//
// - x * 2  =>  x + x
// - x * (power of 2)  =>  x << shift  (for integer types)
// - x / C  =>  x * (1/C) for nonzero constant C
// - x % (power of 2)  =>  x & (pow2-1) for integer types
// ===========================================================================

// If v is a positive integer that is an exact power of 2, return the
// exponent. Otherwise return -1.
static int isPow2Double(double v)
{
    if (v <= 0.0 || v != v) return -1;
    if (v > (double)(1LL << 30)) return -1;

    int64_t iv = (int64_t)v;
    if ((double)iv != v) return -1;
    if (iv == 0 || (iv & (iv - 1)) != 0) return -1;

    int exp = 0;
    while (iv > 1) { iv >>= 1; exp++; }
    return exp;
}

void irOptStrengthReduce(IRBuffer* buf)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];

        // --- MUL strength reduction ---
        if (n->op == IR_MUL && n->op1 != IR_NONE && n->op2 != IR_NONE) {
            IRNode* rhs = &buf->nodes[n->op2];
            IRNode* lhs = &buf->nodes[n->op1];

            // x * 2 => x + x
            if (rhs->op == IR_CONST_NUM && rhs->imm.num == 2.0) {
                n->op  = IR_ADD;
                n->op2 = n->op1;
                continue;
            }
            if (lhs->op == IR_CONST_NUM && lhs->imm.num == 2.0) {
                n->op  = IR_ADD;
                n->op1 = n->op2;
                continue;
            }

            // x * (power of 2) => x << shift (integer types only).
            // Emit a fresh CONST_INT for the shift instead of rewriting the
            // multiplier in place: the constant node may be shared with other
            // ops (GVN merges identical CONST_NUMs), and mutating it would
            // corrupt their operands.
            if (rhs->op == IR_CONST_NUM && n->type == IR_TYPE_INT) {
                int shift = isPow2Double(rhs->imm.num);
                if (shift > 0) {
                    uint16_t c = irEmit(buf, IR_CONST_INT, IR_NONE, IR_NONE,
                                        IR_TYPE_INT);
                    buf->nodes[c].imm.i64 = shift;
                    n->op2  = c;
                    n->op   = IR_LSHIFT;
                    n->type = IR_TYPE_INT;
                    continue;
                }
            }
        }

        // --- DIV strength reduction ---
        if (n->op == IR_DIV && n->op2 != IR_NONE) {
            IRNode* rhs = &buf->nodes[n->op2];

            // x / C => x * (1/C) for nonzero constant C. Emit a fresh
            // reciprocal constant rather than rewriting C in place: C may be
            // shared with a MOD or other op (fasta's `% 139968` and
            // `/ 139968` share one GVN-merged CONST_NUM), and mutating it to
            // 1/C turned the MOD's divisor fractional, so its exact-int guard
            // fired on every entry and the trace ping-ponged forever.
            if (rhs->op == IR_CONST_NUM && rhs->imm.num != 0.0) {
                uint16_t inv = irEmitConst(buf, 1.0 / rhs->imm.num);
                n->op  = IR_MUL;
                n->op2 = inv;
                continue;
            }
        }

        // --- MOD strength reduction ---
        // x % (power of 2) => x & (pow2-1) for integer types. As above, emit a
        // fresh CONST_INT mask instead of mutating the shared constant.
        if (n->op == IR_MOD && n->op2 != IR_NONE && n->type == IR_TYPE_INT) {
            IRNode* rhs = &buf->nodes[n->op2];
            if (rhs->op == IR_CONST_NUM) {
                int shift = isPow2Double(rhs->imm.num);
                if (shift >= 0) {
                    int64_t mask = ((int64_t)1 << shift) - 1;
                    uint16_t c = irEmit(buf, IR_CONST_INT, IR_NONE, IR_NONE,
                                        IR_TYPE_INT);
                    buf->nodes[c].imm.i64 = mask;
                    n->op2  = c;
                    n->op   = IR_BAND;
                    n->type = IR_TYPE_INT;
                    continue;
                }
            }
        }
    }
}

// ===========================================================================
// Pass 8: Bounds Check Elimination (~150 LOC)
//
// Identify induction variables (PHI nodes where one input is incremented by
// a positive constant each iteration). For guards that check iv < len where
// len is loop-invariant, deduplicate: if the same (iv, len) check has
// already been seen in the loop body, kill the duplicate.
//
// Additionally, if GUARD_NUM follows an arithmetic op, UNBOX_NUM, or
// constant, the result is always a number and the guard is redundant.
// ===========================================================================
void irOptBoundsCheckElim(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE) return;

    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;

    // --- Identify induction variables ---
    typedef struct {
        uint16_t phi_id;
        uint16_t init_id;
        double   step;
    } InductionVar;

    InductionVar ivs[16];
    int ivCount = 0;

    for (uint16_t i = header + 1; i < back && ivCount < 16; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op != IR_PHI) continue;
        if (n->op1 == IR_NONE || n->op2 == IR_NONE) continue;

        // op1: value from before loop, op2: value from back edge.
        uint16_t next = n->op2;
        if (next >= buf->count) continue;
        IRNode* nextNode = &buf->nodes[next];
        if (nextNode->op != IR_ADD) continue;

        double step = 0;
        bool found = false;

        // next = phi + const ?
        if (nextNode->op1 == i && nextNode->op2 != IR_NONE) {
            IRNode* s = &buf->nodes[nextNode->op2];
            if (s->op == IR_CONST_NUM && s->imm.num > 0) {
                step = s->imm.num; found = true;
            }
        }
        if (!found && nextNode->op2 == i && nextNode->op1 != IR_NONE) {
            IRNode* s = &buf->nodes[nextNode->op1];
            if (s->op == IR_CONST_NUM && s->imm.num > 0) {
                step = s->imm.num; found = true;
            }
        }

        if (found) {
            ivs[ivCount].phi_id  = i;
            ivs[ivCount].init_id = n->op1;
            ivs[ivCount].step    = step;
            ivCount++;
        }
    }

    if (ivCount == 0) return;

    // --- Deduplicate bounds checks ---
    // Track (iv_id, len_id) pairs we have already seen.
    typedef struct {
        uint16_t iv_id;
        uint16_t len_id;
    } SeenCheck;

    SeenCheck seen[64];
    int seenCount = 0;

    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op != IR_GUARD_TRUE || n->op1 == IR_NONE) continue;

        IRNode* cmp = &buf->nodes[n->op1];
        if (cmp->op != IR_LT) continue;
        if (cmp->op1 == IR_NONE || cmp->op2 == IR_NONE) continue;

        // Is cmp->op1 an induction variable?
        bool isIV = false;
        for (int k = 0; k < ivCount; k++) {
            if (cmp->op1 == ivs[k].phi_id) { isIV = true; break; }
        }
        if (!isIV) continue;

        // Is the bound (cmp->op2) loop-invariant?
        if (cmp->op2 >= header) continue;

        // Check if we have already seen this (iv, len) pair.
        bool duplicate = false;
        for (int k = 0; k < seenCount; k++) {
            if (seen[k].iv_id == cmp->op1 && seen[k].len_id == cmp->op2) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            killNode(n);
        } else if (seenCount < 64) {
            seen[seenCount].iv_id  = cmp->op1;
            seen[seenCount].len_id = cmp->op2;
            seenCount++;
        }
    }
}

// ===========================================================================
// Pass 9: Escape Analysis (~200 LOC)
//
// Two sub-passes:
//
// (A) Scalar replacement for range objects: if a CALL_C produces a pointer
//     and the result is only read via LOAD_FIELD (no stores, no other calls,
//     no snapshot references), replace LOAD_FIELD(r, 0) with the "from"
//     operand, LOAD_FIELD(r, 1) with the "to" operand, and kill the alloc.
//
// (B) Store-load forwarding: for LOAD_FIELD, scan backward for a matching
//     STORE_FIELD on the same object and field, and forward the stored
//     value directly.
// ===========================================================================

// Does the SSA value |id| escape?  (Used by anything other than LOAD_FIELD.)
static bool doesEscape(const IRBuffer* buf, uint16_t id)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->op == IR_NOP) continue;
        bool usesId = (n->op1 == id || n->op2 == id);
        if (!usesId) continue;
        // LOAD_FIELD with the object as op1 is fine.
        if (n->op == IR_LOAD_FIELD && n->op1 == id) continue;
        return true;
    }
    // Check snapshot entries.
    for (uint16_t i = 0; i < buf->snapshot_entry_count; i++) {
        if (buf->snapshot_entries[i].ssa_ref == id) return true;
    }
    return false;
}

void irOptEscapeAnalysis(IRBuffer* buf)
{
    // --- (A) Scalar replacement for CALL_C-allocated objects ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op != IR_CALL_C) continue;
        if (n->type != IR_TYPE_PTR) continue;
        if (n->op1 == IR_NONE || n->op2 == IR_NONE) continue;

        if (doesEscape(buf, i)) continue;

        uint16_t fromVal = n->op1;
        uint16_t toVal   = n->op2;

        // Replace LOAD_FIELD(this, field) with the corresponding scalar.
        for (uint16_t j = 0; j < buf->count; j++) {
            IRNode* u = &buf->nodes[j];
            if (u->op != IR_LOAD_FIELD || u->op1 != i) continue;

            uint16_t fieldIdx = u->imm.mem.field;
            uint16_t replacement = IR_NONE;
            switch (fieldIdx) {
                case 0: replacement = fromVal; break; // "from"
                case 1: replacement = toVal;   break; // "to"
                default: break;
            }

            if (replacement != IR_NONE) {
                replaceUses(buf, j, replacement);
                killNode(u);
            }
        }

        // If the CALL_C now has no users, kill it.
        bool hasUsers = false;
        for (uint16_t j = 0; j < buf->count; j++) {
            const IRNode* u = &buf->nodes[j];
            if (u->op == IR_NOP) continue;
            if (u->op1 == i || u->op2 == i) { hasUsers = true; break; }
        }
        if (!hasUsers) {
            killNode(n);
        }
    }

    // --- (B) Store-load forwarding ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op != IR_LOAD_FIELD) continue;
        if (n->op1 == IR_NONE) continue;

        uint16_t obj   = n->op1;
        uint16_t field = n->imm.mem.field;

        for (int j = (int)i - 1; j >= 0; j--) {
            IRNode* s = &buf->nodes[j];

            // Found matching store: forward the stored value.
            if (s->op == IR_STORE_FIELD && s->op1 == obj &&
                s->imm.mem.field == field) {
                replaceUses(buf, i, s->op2);
                killNode(n);
                break;
            }

            // Stop at calls (may alias).
            if (s->op == IR_CALL_C || s->op == IR_CALL_WREN) break;

            // Stop at other stores to same object (conservative).
            if (s->op == IR_STORE_FIELD && s->op1 == obj) break;
        }
    }
}

// ===========================================================================
// Pass 10: Dead Code Elimination (~200 LOC)
//
// Mark-sweep from roots. Roots are: STORE_STACK, STORE_FIELD,
// STORE_MODULE_VAR, SIDE_EXIT, LOOP_BACK, LOOP_HEADER, CALL_C, CALL_WREN,
// SNAPSHOT, PHI, and any guard. Also, any SSA value referenced from a
// snapshot entry is a root. Walk backward from roots marking operands as
// live. Everything not marked gets IR_FLAG_DEAD.
// ===========================================================================
void irOptDCE(IRBuffer* buf)
{
    static uint64_t live[BITSET_WORDS];
    memset(live, 0, sizeof(live));

    static uint16_t worklist[IR_MAX_NODES];
    int wlCount = 0;

    // Seed worklist with root nodes.
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op == IR_NOP) continue;

        bool isRoot = false;
        switch (n->op) {
            case IR_STORE_STACK: {
                // irOptGuardElim Phase B pre-marks dispensable STORE_STACK
                // nodes as IR_FLAG_DEAD. Any STORE_STACK that Phase B kept
                // alive is treated as an unconditional root here so that DCE
                // preserves it (e.g., loop-variable stores needed by LOAD_STACK
                // on the next iteration via LOOP_BACK).
                if (n->flags & IR_FLAG_DEAD) break;
                isRoot = true;
                break;
            }
            case IR_STORE_FIELD:
            case IR_LIST_STORE:
            case IR_STORE_MODULE_VAR:
            case IR_SIDE_EXIT:
            case IR_LOOP_BACK:
            case IR_LOOP_HEADER:
            case IR_LOOP_EXIT:
            case IR_CALL_C:
            case IR_CALL_WREN:
            case IR_SNAPSHOT:
            case IR_PHI:
                isRoot = true;
                break;
            default:
                if (isGuard(n->op)) isRoot = true;
                break;
        }

        if (isRoot && !bitTest(live, i)) {
            bitSet(live, i);
            worklist[wlCount++] = i;
        }
    }

    // Also mark snapshot entry references as roots.
    for (uint16_t i = 0; i < buf->snapshot_entry_count; i++) {
        uint16_t ref = buf->snapshot_entries[i].ssa_ref;
        if (ref != IR_NONE && ref < buf->count && !bitTest(live, ref)) {
            bitSet(live, ref);
            worklist[wlCount++] = ref;
        }
    }

    // Deferred module stores are also deoptimization roots.
    for (uint16_t i = 0; i < buf->exit_module_entry_count; i++) {
        uint16_t ref = buf->exit_module_entries[i].ssa_ref;
        if (ref != IR_NONE && ref < buf->count && !bitTest(live, ref)) {
            bitSet(live, ref);
            worklist[wlCount++] = ref;
        }
    }

    // Propagate liveness to operands.
    while (wlCount > 0) {
        uint16_t id = worklist[--wlCount];
        IRNode* n = &buf->nodes[id];

        uint16_t ops[3] = { n->op1, n->op2,
                            n->op == IR_LIST_STORE ? n->imm.list.value : IR_NONE };
        for (int k = 0; k < 3; k++) {
            uint16_t op = ops[k];
            if (op != IR_NONE && op < buf->count && !bitTest(live, op)) {
                bitSet(live, op);
                worklist[wlCount++] = op;
            }
        }
    }

    // Kill everything not marked live.
    for (uint16_t i = 0; i < buf->count; i++) {
        if (buf->nodes[i].op != IR_NOP && !bitTest(live, i)) {
            killNode(&buf->nodes[i]);
        }
    }
}

// ===========================================================================
// Pass 0: Loop Variable Promotion (PHI insertion, ~150 LOC)
//
// For each module variable that is both read and written inside the loop:
//   1. Emit LOAD_MODULE_VAR in a pre-header NOP slot (initial load, once).
//   2. Emit UNBOX_NUM of that load in the next pre-header NOP slot.
//   3. Emit PHI(unbox_init, arithmetic_result) in the third NOP slot.
//   4. Replace uses of the in-loop UNBOX_NUM with the PHI.
//   5. Kill the original in-loop LOAD_MODULE_VAR and UNBOX_NUM.
//
// This converts memory-based loop variables into register-resident SSA values
// and creates the PHI structure needed by irOptIVTypeInference (Pass 12).
//
// Preconditions:
//   - buf->loop_header points to IR_LOOP_HEADER.
//   - Pre-header NOP slots exist at indices [0, loop_header).
//   - This pass runs BEFORE all other passes (so BOX_NUM is still present).
// ===========================================================================
void irOptPromoteLoopVars(IRBuffer* buf)
{
    if (!buf || buf->count == 0) return;

    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE || header == 0) return;

    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;

    // Compute loop-nesting depth at each node. Depth 0 is the anchor body; a
    // nested LOOP_HEADER enters depth 1 and its LOOP_BACK exits it. Module
    // vars touched inside a nested loop must not be promoted: the PHI is
    // anchor-scoped and would read a stale value within the anchor iteration.
    uint8_t depth_at[IR_MAX_NODES];
    int depth = 0;
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* nn = &buf->nodes[i];
        if (nn->op == IR_LOOP_HEADER) {
            if (i == header) {
                depth_at[i] = 0;
            } else {
                depth_at[i] = (uint8_t)(++depth);
            }
        } else if (nn->op == IR_LOOP_BACK) {
            depth_at[i] = (uint8_t)depth;
            if (depth > 0) depth--;
        } else {
            depth_at[i] = (uint8_t)depth;
        }
    }

    // Cursor into the pre-header NOP slot pool.
    uint16_t nextNop = 0;

    // Track which variable pointers have already been promoted so that
    // duplicate LOAD_MODULE_VAR nodes for the same variable (emitted by the
    // recorder before GVN runs) don't create extra PHI triples.
    void*    promoted_ptrs[32 / 3 + 1];  /* sized for JIT_PRE_HEADER_SLOTS=32 */
    int      promoted_count = 0;

    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* loadN = &buf->nodes[i];
        if (loadN->flags & IR_FLAG_DEAD) continue;
        if (loadN->op != IR_LOAD_MODULE_VAR) continue;

        void* var_ptr = loadN->imm.ptr;

        // A var touched inside a nested loop must not be promoted.
        if (depth_at[i] > 0) continue;

        // Skip if this variable was already promoted by an earlier LOAD node.
        bool already_promoted = false;
        for (int pp = 0; pp < promoted_count; pp++) {
            if (promoted_ptrs[pp] == var_ptr) { already_promoted = true; break; }
        }
        if (already_promoted) continue;

        // Find a single UNBOX_NUM that directly consumes this LOAD.
        // The recorder emits irEmitUnbox(load_ssa) which is UNBOX_NUM(load_id).
        uint16_t unbox_id = IR_NONE;
        for (uint16_t u = header + 1; u < back; u++) {
            if (buf->nodes[u].op == IR_UNBOX_NUM && buf->nodes[u].op1 == i) {
                unbox_id = u;
                break;
            }
        }
        if (unbox_id == IR_NONE) continue;

        // Require exactly one matching store. Multiple assignments need a
        // real memory-SSA merge and must not be guessed from the first store.
        uint16_t store_id = IR_NONE;
        int store_count = 0;
        for (uint16_t s = header + 1; s < back; s++) {
            const IRNode* sn = &buf->nodes[s];
            if (sn->flags & IR_FLAG_DEAD) continue;
            if (sn->op == IR_STORE_MODULE_VAR && sn->imm.ptr == var_ptr) {
                store_id = s;
                store_count++;
            }
        }
        if (store_count != 1) continue;
        if (depth_at[store_id] > 0) continue;

        // This simple PHI form models one assignment at the end of an
        // iteration. A load after the assignment would need the back-edge
        // value immediately rather than the current PHI.
        bool load_after_store = false;
        for (uint16_t k = (uint16_t)(store_id + 1); k < back; k++) {
            const IRNode* kn = &buf->nodes[k];
            if (!(kn->flags & IR_FLAG_DEAD) &&
                kn->op == IR_LOAD_MODULE_VAR && kn->imm.ptr == var_ptr) {
                load_after_store = true;
                break;
            }
        }
        if (load_after_store) continue;

        // The stored value must be BOX_NUM(back_val) so we can recover the
        // unboxed arithmetic result as the PHI's back-edge value.
        uint16_t store_val_id = buf->nodes[store_id].op1;
        if (store_val_id == IR_NONE || store_val_id >= buf->count) continue;
        if (buf->nodes[store_val_id].op != IR_BOX_NUM) continue;

        uint16_t back_val_id = buf->nodes[store_val_id].op1;
        if (back_val_id == IR_NONE || back_val_id >= buf->count) continue;

        // Need 3 consecutive NOP slots in the pre-header.
        while (nextNop < header && buf->nodes[nextNop].op != IR_NOP) nextNop++;
        uint16_t j0 = nextNop;
        uint16_t j1 = (uint16_t)(nextNop + 1);
        uint16_t j2 = (uint16_t)(nextNop + 2);
        if (j2 >= header) continue;
        if (buf->nodes[j0].op != IR_NOP ||
            buf->nodes[j1].op != IR_NOP ||
            buf->nodes[j2].op != IR_NOP) continue;
        nextNop += 3;

        // Record this variable as promoted so duplicate LOADs are skipped.
        if (promoted_count < (int)(sizeof(promoted_ptrs) / sizeof(promoted_ptrs[0])))
            promoted_ptrs[promoted_count++] = var_ptr;

        // j0: copy of LOAD_MODULE_VAR, placed in pre-header.
        buf->nodes[j0] = *loadN;
        buf->nodes[j0].id    = j0;
        buf->nodes[j0].flags = 0;

        // j1: UNBOX_NUM(j0), type=NUM — the initial unboxed value.
        {
            IRNode* j1n = &buf->nodes[j1];
            j1n->op    = IR_UNBOX_NUM;
            j1n->id    = j1;
            j1n->op1   = j0;
            j1n->op2   = IR_NONE;
            j1n->type  = IR_TYPE_NUM;
            j1n->flags = 0;
            memset(&j1n->imm, 0, sizeof(j1n->imm));
        }

        // j2: PHI(j1, back_val_id), type=NUM — the loop-carried value.
        {
            IRNode* j2n = &buf->nodes[j2];
            j2n->op    = IR_PHI;
            j2n->id    = j2;
            j2n->op1   = j1;
            j2n->op2   = back_val_id;
            j2n->type  = IR_TYPE_NUM;
            j2n->flags = 0;
            memset(&j2n->imm, 0, sizeof(j2n->imm));
        }

        // If every use of a snapshot is on a single side of the assignment,
        // defer the module write to that snapshot's exit stub. This removes
        // both the memory traffic and (when otherwise unused) boxing from the
        // hot back edge. Calls are an aliasing boundary, so traces containing
        // one retain the eager store.
        bool can_sink_store = true;
        int8_t snapshot_side[IR_MAX_SNAPSHOTS];
        memset(snapshot_side, -1, sizeof(snapshot_side));
        for (uint16_t k = header + 1; k < back; k++) {
            const IRNode* kn = &buf->nodes[k];
            if (kn->flags & IR_FLAG_DEAD) continue;
            if (kn->op == IR_CALL_C || kn->op == IR_CALL_WREN) {
                can_sink_store = false;
                break;
            }
            uint16_t sid = exitSnapshotId(kn);
            if (sid == IR_NONE || sid >= buf->snapshot_count) continue;
            int8_t side = k > store_id ? 1 : 0;
            if (snapshot_side[sid] != -1 && snapshot_side[sid] != side) {
                can_sink_store = false;
                break;
            }
            snapshot_side[sid] = side;
        }

        if (can_sink_store) {
            int needed = 0;
            for (uint16_t sid = 0; sid < buf->snapshot_count; sid++)
                if (snapshot_side[sid] != -1) needed++;
            if ((int)buf->exit_module_entry_count + needed <= IR_MAX_EXIT_MODULE_ENTRIES) {
                for (uint16_t sid = 0; sid < buf->snapshot_count; sid++) {
                    if (snapshot_side[sid] == -1) continue;
                    IRExitModuleEntry* entry =
                        &buf->exit_module_entries[buf->exit_module_entry_count++];
                    entry->address = var_ptr;
                    entry->snapshot_id = sid;
                    entry->ssa_ref = snapshot_side[sid] ? back_val_id : j2;
                }
                killNode(&buf->nodes[store_id]);
            }
        }

        // Replace ALL in-loop LOAD_MODULE_VAR(var_ptr) nodes with j0, and
        // ALL UNBOX_NUM nodes that consume any such LOAD with j2.  A single
        // trace can load the same module variable multiple times per iteration
        // (e.g. once for the loop condition, once per operand in the body), so
        // we must sweep the entire loop body rather than handling only the one
        // LOAD/UNBOX pair we used to discover the back-edge value.
        for (uint16_t k = header + 1; k < back; k++) {
            IRNode* kn = &buf->nodes[k];
            if (kn->flags & IR_FLAG_DEAD) continue;

            if (kn->op == IR_LOAD_MODULE_VAR && kn->imm.ptr == var_ptr) {
                replaceUses(buf, k, j0);
                // A LIST_STORE's recorded value may reference this load. That
                // stored boxed value must track the loop-carried PHI (j2), not
                // the loop-entry value (j0), so repurpose this node in place
                // as BOX_NUM(j2) and keep it live instead of killing it.
                bool listStoreValue = false;
                for (uint16_t m = header + 1; m < back && !listStoreValue; m++) {
                    const IRNode* mn = &buf->nodes[m];
                    if (!(mn->flags & IR_FLAG_DEAD) && mn->op == IR_LIST_STORE &&
                        mn->imm.list.value == k) listStoreValue = true;
                }
                if (listStoreValue) {
                    kn->op    = IR_BOX_NUM;
                    kn->op1   = j2;
                    kn->op2   = IR_NONE;
                    kn->type  = IR_TYPE_VALUE;
                    kn->flags = 0;
                    memset(&kn->imm, 0, sizeof(kn->imm));
                } else {
                    killNode(kn);
                }
                continue;
            }
        }
        // Second sub-pass: UNBOXes whose source was one of those LOADs are now
        // UNBOX_NUM(j0) after replaceUses above.  Redirect them to j2.
        for (uint16_t k = header + 1; k < back; k++) {
            IRNode* kn = &buf->nodes[k];
            if (kn->flags & IR_FLAG_DEAD) continue;
            if (kn->op == IR_UNBOX_NUM && kn->op1 == j0) {
                replaceUses(buf, k, j2);
                killNode(kn);
            }
        }
    }

    // Promote numeric interpreter-stack locals in the same way. Stack values
    // are reconstructed from snapshots on every exit, so the hot loop does
    // not need to reload/store a local that has one simple back-edge update.
    bool promoted_slots[256];
    memset(promoted_slots, 0, sizeof(promoted_slots));
    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* load = &buf->nodes[i];
        if ((load->flags & IR_FLAG_DEAD) || load->op != IR_LOAD_STACK) continue;
        uint16_t slot = load->imm.mem.slot;
        if (slot >= 256 || promoted_slots[slot]) continue;

        uint16_t store_id = IR_NONE;
        int store_count = 0;
        bool has_call = false;
        for (uint16_t k = header + 1; k < back; k++) {
            const IRNode* n = &buf->nodes[k];
            if (n->flags & IR_FLAG_DEAD) continue;
            if (n->op == IR_CALL_C || n->op == IR_CALL_WREN) has_call = true;
            if (n->op == IR_STORE_STACK && n->imm.mem.slot == slot) {
                store_id = k;
                store_count++;
            }
        }
        if (has_call || store_count != 1 || store_id <= i) continue;

        uint16_t stored = buf->nodes[store_id].op1;
        if (stored == IR_NONE || stored >= buf->count ||
            buf->nodes[stored].op != IR_BOX_NUM) continue;

        bool load_after_store = false;
        for (uint16_t k = (uint16_t)(store_id + 1); k < back; k++) {
            const IRNode* n = &buf->nodes[k];
            if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                n->imm.mem.slot == slot) {
                load_after_store = true;
                break;
            }
        }
        if (load_after_store) continue;

        // Every possible exit must describe the local, otherwise its old
        // interpreter memory cell could be observed after the store was sunk.
        bool snapshots_cover_slot = true;
        for (uint16_t sid = 0; sid < buf->snapshot_count; sid++) {
            bool found = false;
            IRSnapshot* snap = &buf->snapshots[sid];
            for (uint16_t e = 0; e < snap->num_entries; e++) {
                IRSnapshotEntry* entry =
                    &buf->snapshot_entries[snap->entry_start + e];
                if (entry->slot == slot) { found = true; break; }
            }
            if (!found) { snapshots_cover_slot = false; break; }
        }
        if (!snapshots_cover_slot) continue;

        while (nextNop < header && buf->nodes[nextNop].op != IR_NOP) nextNop++;
        uint16_t initial = nextNop;
        uint16_t phi_id = (uint16_t)(nextNop + 1);
        if (phi_id >= header || buf->nodes[initial].op != IR_NOP ||
            buf->nodes[phi_id].op != IR_NOP) continue;
        nextNop += 2;

        buf->nodes[initial] = *load;
        buf->nodes[initial].id = initial;
        buf->nodes[initial].flags = 0;

        IRNode* phi = &buf->nodes[phi_id];
        phi->op = IR_PHI;
        phi->id = phi_id;
        phi->op1 = initial;
        phi->op2 = stored;
        phi->type = IR_TYPE_VALUE;
        phi->flags = 0;
        memset(&phi->imm, 0, sizeof(phi->imm));

        for (uint16_t k = header + 1; k < back; k++) {
            IRNode* n = &buf->nodes[k];
            if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                n->imm.mem.slot == slot) {
                replaceUses(buf, k, phi_id);
                killNode(n);
            }
        }
        // Keep the store if the slot is read from the interpreter stack after
        // the loop's back edge (outer-closure RESUME re-sync). Same rationale
        // as the nested promotion below: the stack must hold the final value.
        bool readAfterBack = false;
        for (uint16_t k = (uint16_t)(back + 1); k < buf->count; k++) {
            const IRNode* n = &buf->nodes[k];
            if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                n->imm.mem.slot == slot) {
                readAfterBack = true;
                break;
            }
        }
        if (!readAfterBack)
            killNode(&buf->nodes[store_id]);
        promoted_slots[slot] = true;
    }

    // -------------------------------------------------------------------------
    // Nested-loop local promotion.
    //
    // A trace anchored at an outer loop but closed at a nested loop's back
    // edge (the recorder emits only the nested back edge, so the outer loop's
    // iterations re-enter this trace from the interpreter) keeps its inner-loop
    // locals round-tripping through the interpreter stack every iteration: the
    // anchor promotion above requires a single store per slot in the whole
    // (header, back) span, but a nested loop's slot is stored once in the outer
    // body (loop-entry state, e.g. the peeled first iteration) and once in the
    // nested body. Promote such slots per nested loop: convert the LAST store
    // before the nested header in place into a VALUE-typed PHI whose op1 is
    // that store's value (the loop-entry value, defined before the PHI) and
    // whose op2 is the nested body's store value (the back-edge update). The
    // nested body's loads read the PHI, the nested back edge updates it, and
    // the loop-exit handoff snapshot (otherwise empty) materializes the final
    // value when the falsy exit fires.
    // -------------------------------------------------------------------------
    // Scan every nested header, not just those before the anchor's first
    // back edge: a trace closed at the anchor body's own back edge (or
    // closed at a deeper back edge, so findLoopBack below reports a nested
    // one) can contain several nested loops, each with its own loop-carried
    // slots. The per-loop promoted set below lets each loop promote the
    // same Wren locals independently (e.g. the peeled first escape loop and
    // the steady-state escape loop both carry zr/zi/iteration).
    for (uint16_t h = (uint16_t)(header + 1); h < buf->count; h++) {
        const IRNode* hn = &buf->nodes[h];
        if (hn->op != IR_LOOP_HEADER) continue;

        // The back edge closing this nested loop is the first one after h.
        uint16_t nb = IR_NONE;
        for (uint16_t k = (uint16_t)(h + 1); k < buf->count; k++) {
            if (buf->nodes[k].op == IR_LOOP_BACK && buf->nodes[k].op1 == h) {
                nb = k;
                break;
            }
        }
        if (nb == IR_NONE || nb <= h + 1) continue;

        bool nested_promoted[256];
        memset(nested_promoted, 0, sizeof(nested_promoted));

        // Empty snapshots are the loop-exit handoff. Collect their ids ONCE:
        // adding a slot entry makes a snapshot non-empty, which would hide
        // later promoted slots from a per-promotion rescan.
        uint16_t emptySnaps[IR_MAX_SNAPSHOTS];
        int emptySnapCount = 0;
        for (uint16_t sid = 0; sid < buf->snapshot_count; sid++) {
            if (buf->snapshots[sid].num_entries == 0)
                emptySnaps[emptySnapCount++] = sid;
        }

        for (uint16_t i = (uint16_t)(h + 1); i < nb; i++) {
            IRNode* load = &buf->nodes[i];
            if ((load->flags & IR_FLAG_DEAD) || load->op != IR_LOAD_STACK)
                continue;
            uint16_t slot = load->imm.mem.slot;
            if (slot >= 256 || nested_promoted[slot]) continue;

            // One store of this slot in the nested body, and no calls (a call
            // is an aliasing boundary the eager-store contract relies on).
            uint16_t store_id = IR_NONE;
            int store_count = 0;
            bool has_call = false;
            for (uint16_t k = (uint16_t)(h + 1); k < nb; k++) {
                const IRNode* n = &buf->nodes[k];
                if (n->flags & IR_FLAG_DEAD) continue;
                if (n->op == IR_CALL_C || n->op == IR_CALL_WREN)
                    has_call = true;
                if (n->op == IR_STORE_STACK && n->imm.mem.slot == slot) {
                    store_id = k;
                    store_count++;
                }
            }
            if (has_call) continue;

            // -----------------------------------------------------------------
            // Loop-invariant slots: the nested loop never stores the slot, so its
            // value is set once in the enclosing body before the header and every
            // load inside the loop reads that same boxed value. Forward the loop's
            // loads to the store's value directly (store->load forwarding),
            // killing the per-iteration LOAD_STACK + its spill. The store itself
            // stays — the interpreter stack must still hold the value for the
            // exit/re-sync paths.
            // -----------------------------------------------------------------
            if (store_count == 0) {
                uint16_t entry_store = IR_NONE;
                for (uint16_t k = (uint16_t)(header + 1); k < h; k++) {
                    const IRNode* n = &buf->nodes[k];
                    if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_STORE_STACK &&
                        n->imm.mem.slot == slot)
                        entry_store = k; // last store before the header
                }
                // No in-trace store before the header (e.g. slot0, written only
                // by the interpreter at trace entry): nothing to forward to, the
                // loads stay.
                if (entry_store != IR_NONE) {
                    uint16_t entry_val = buf->nodes[entry_store].op1;
                    if (entry_val != IR_NONE && entry_val < buf->count &&
                        entry_val < entry_store &&
                        buf->nodes[entry_val].type == IR_TYPE_VALUE) {
                        for (uint16_t k = (uint16_t)(h + 1); k < nb; k++) {
                            IRNode* n = &buf->nodes[k];
                            if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                                n->imm.mem.slot == slot) {
                                replaceUses(buf, k, entry_val);
                                killNode(n);
                            }
                        }
                    }
                }
                nested_promoted[slot] = true;
                continue;
            }
            if (store_id <= i) continue;

            uint16_t stored = buf->nodes[store_id].op1;
            if (stored == IR_NONE || stored >= buf->count ||
                buf->nodes[stored].type != IR_TYPE_VALUE) continue;

            // The loop-entry value: the LAST store of this slot before the
            // nested header. Its value must be a boxed Value defined before
            // the PHI, so the in-place conversion's op1 is a valid
            // use-before-def.
            uint16_t entry_store = IR_NONE;
            for (uint16_t k = (uint16_t)(header + 1); k < h; k++) {
                const IRNode* n = &buf->nodes[k];
                if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_STORE_STACK &&
                    n->imm.mem.slot == slot)
                    entry_store = k;
            }
            if (entry_store == IR_NONE) continue;
            uint16_t entry_val = buf->nodes[entry_store].op1;
            if (entry_val == IR_NONE || entry_val >= buf->count ||
                entry_val >= entry_store ||
                buf->nodes[entry_val].type != IR_TYPE_VALUE) continue;

            // No load of the slot after the nested body's store.
            bool load_after_store = false;
            for (uint16_t k = (uint16_t)(store_id + 1); k < nb; k++) {
                const IRNode* n = &buf->nodes[k];
                if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                    n->imm.mem.slot == slot) {
                    load_after_store = true;
                    break;
                }
            }
            if (load_after_store) continue;

            // Convert the entry store in place into the loop-carried PHI. Its
            // initial MOV (phi = op1) executes once per trace invocation in the
            // outer body; the nested back edge writes op2 each iteration.
            IRNode* phi = &buf->nodes[entry_store];
            phi->op = IR_PHI;
            phi->op1 = entry_val;
            phi->op2 = stored;
            phi->type = IR_TYPE_VALUE;
            phi->flags = 0;
            memset(&phi->imm, 0, sizeof(phi->imm));

            // All loads of the slot after the PHI read the loop-carried value.
            for (uint16_t k = (uint16_t)(entry_store + 1); k < nb; k++) {
                IRNode* n = &buf->nodes[k];
                if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                    n->imm.mem.slot == slot) {
                    replaceUses(buf, k, phi->id);
                    killNode(n);
                }
            }
            // The nested body's store is now the back-edge update. Killing it
            // is safe for the loop-exit handoff (the PHI is materialized in
            // the empty exit snapshot below) but NOT when a LOAD_STACK of the
            // slot follows the nested back edge: the outer-closure RESUME
            // re-sync reloads every slot < after_depth from the interpreter
            // stack after the loop, and with the store gone it would read a
            // stale loop-entry value. Keep the store in that case so the
            // stack holds the final value when the re-sync reads it.
            bool readAfterBack = false;
            for (uint16_t k = (uint16_t)(nb + 1); k < buf->count; k++) {
                const IRNode* n = &buf->nodes[k];
                if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_LOAD_STACK &&
                    n->imm.mem.slot == slot) {
                    readAfterBack = true;
                    break;
                }
            }
            if (!readAfterBack)
                killNode(&buf->nodes[store_id]);

            // The loop-exit handoff is an otherwise-empty snapshot (its eager
            // STORE_STACK writes were supposed to leave the interpreter stack
            // final, but the promoted slot's store is gone). Materialize the
            // PHI there so the falsy exit sees the final value.
            for (int e = 0; e < emptySnapCount; e++) {
                uint16_t sid = emptySnaps[e];
                IRSnapshot* snap = &buf->snapshots[sid];
                if (slot >= snap->stack_depth) continue;
                irSnapshotAddEntry(buf, sid, slot, phi->id);
            }

            nested_promoted[slot] = true;
        }
    }
}

// ===========================================================================
// Master optimization pipeline
// ===========================================================================
void irOptPromoteNumericStackPhis(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE || buf->snapshot_count == 0) return;

    for (uint16_t i = 0; i < header; i++) {
        IRNode* oldPhi = &buf->nodes[i];
        if ((oldPhi->flags & IR_FLAG_DEAD) || oldPhi->op != IR_PHI ||
            oldPhi->type != IR_TYPE_VALUE || oldPhi->op1 == IR_NONE ||
            oldPhi->op2 == IR_NONE || oldPhi->op1 >= buf->count ||
            oldPhi->op2 >= buf->count) continue;
        IRNode* initial = &buf->nodes[oldPhi->op1];
        IRNode* boxedBack = &buf->nodes[oldPhi->op2];
        if (initial->op != IR_LOAD_STACK || boxedBack->op != IR_BOX_NUM ||
            boxedBack->op1 == IR_NONE || boxedBack->op1 >= buf->count) continue;

        uint16_t unboxSlot = IR_NONE;
        uint16_t phiSlot = IR_NONE;
        for (uint16_t j = (uint16_t)(i + 1); j < header; j++) {
            if (buf->nodes[j].op != IR_NOP) continue;
            if (unboxSlot == IR_NONE) unboxSlot = j;
            else { phiSlot = j; break; }
        }
        if (unboxSlot == IR_NONE || phiSlot == IR_NONE ||
            buf->snapshot_count >= IR_MAX_SNAPSHOTS) continue;

        const IRSnapshot* sourceSnap = &buf->snapshots[0];
        if ((uint32_t)buf->snapshot_entry_count + sourceSnap->num_entries >
            IR_MAX_NODES) continue;
        uint16_t preSnapId = buf->snapshot_count++;
        IRSnapshot* preSnap = &buf->snapshots[preSnapId];
        preSnap->resume_pc = sourceSnap->resume_pc;
        preSnap->stack_depth = sourceSnap->stack_depth;
        preSnap->entry_start = buf->snapshot_entry_count;
        preSnap->num_entries = sourceSnap->num_entries;
        for (uint16_t e = 0; e < sourceSnap->num_entries; e++) {
            IRSnapshotEntry entry =
                buf->snapshot_entries[sourceSnap->entry_start + e];
            if (entry.ssa_ref == oldPhi->id) entry.ssa_ref = initial->id;
            buf->snapshot_entries[buf->snapshot_entry_count++] = entry;
        }

        IRNode* unbox = &buf->nodes[unboxSlot];
        unbox->op = IR_UNBOX_NUM;
        unbox->id = unboxSlot;
        unbox->op1 = initial->id;
        unbox->op2 = IR_NONE;
        unbox->type = IR_TYPE_NUM;
        unbox->flags = 0;
        memset(&unbox->imm, 0, sizeof(unbox->imm));

        IRNode* newPhi = &buf->nodes[phiSlot];
        newPhi->op = IR_PHI;
        newPhi->id = phiSlot;
        newPhi->op1 = unboxSlot;
        newPhi->op2 = boxedBack->op1;
        newPhi->type = IR_TYPE_NUM;
        newPhi->flags = 0;
        memset(&newPhi->imm, 0, sizeof(newPhi->imm));

        uint16_t oldPhiId = oldPhi->id;
        uint16_t boxedId = boxedBack->id;
        uint16_t rawBack = boxedBack->op1;
        replaceUses(buf, oldPhiId, phiSlot);

        // The boxed iterator may feed the for-loop's truthiness test and a
        // later numeric unbox. A numeric iterator is always truthy, while its
        // raw value can directly replace UNBOX_NUM and snapshot entries.
        for (uint16_t j = (uint16_t)(header + 1); j < buf->count; j++) {
            IRNode* user = &buf->nodes[j];
            if (user->flags & IR_FLAG_DEAD) continue;
            if (user->op1 == boxedId && user->op == IR_GUARD_TRUE) {
                killNode(user);
            } else if (user->op1 == boxedId && user->op == IR_UNBOX_NUM) {
                replaceUses(buf, user->id, rawBack);
                killNode(user);
            }
        }
        for (uint16_t e = 0; e < buf->snapshot_entry_count; e++) {
            if (buf->snapshot_entries[e].ssa_ref == boxedId)
                buf->snapshot_entries[e].ssa_ref = rawBack;
        }
        for (uint16_t e = 0; e < buf->exit_module_entry_count; e++) {
            if (buf->exit_module_entries[e].ssa_ref == boxedId)
                buf->exit_module_entries[e].ssa_ref = rawBack;
        }

        oldPhi = &buf->nodes[i];
        oldPhi->op = IR_GUARD_NUM;
        oldPhi->id = i;
        oldPhi->op1 = initial->id;
        oldPhi->op2 = IR_NONE;
        oldPhi->type = IR_TYPE_VOID;
        oldPhi->flags = IR_FLAG_GUARD | IR_FLAG_HOISTED;
        oldPhi->imm.snapshot_id = preSnapId;
        killNode(&buf->nodes[boxedId]);

        for (uint16_t j = (uint16_t)(header + 1); j < buf->count; j++) {
            IRNode* n = &buf->nodes[j];
            if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_UNBOX_NUM &&
                n->op1 == phiSlot) {
                replaceUses(buf, n->id, phiSlot);
                killNode(n);
            }
            if (!(n->flags & IR_FLAG_DEAD) && n->op == IR_GUARD_NUM &&
                n->op1 == phiSlot) killNode(n);
        }
    }

}

// Find a dead node slot in [start, end) that `used` has not claimed, and
// mark it claimed. Returns IR_NONE when none is free.
static uint16_t irClaimDeadSlot(IRBuffer* buf, uint16_t start, uint16_t end,
                                bool* used)
{
    for (uint16_t j = start; j < end && j < buf->count; j++) {
        if (used[j]) continue;
        const IRNode* n = &buf->nodes[j];
        if ((n->flags & IR_FLAG_DEAD) || n->op == IR_NOP) {
            used[j] = true;
            return j;
        }
    }
    return IR_NONE;
}

// Rewrite a claimed dead slot as a fresh CONST_INT.
static void irFillConstIntSlot(IRBuffer* buf, uint16_t slot, int64_t v)
{
    IRNode* c = &buf->nodes[slot];
    c->op = IR_CONST_INT;
    c->id = slot;
    c->op1 = IR_NONE;
    c->op2 = IR_NONE;
    c->type = IR_TYPE_INT;
    c->flags = 0;
    memset(&c->imm, 0, sizeof(c->imm));
    c->imm.i64 = v;
}

// If `id` names a boxed/raw integer constant (CONST_INT, CONST_NUM with an
// integral value, or BOX_NUM of one), return its integer value. Otherwise
// return false.
static bool irConstIntValue(const IRBuffer* buf, uint16_t id, int64_t* out)
{
    if (id >= buf->count) return false;
    const IRNode* n = &buf->nodes[id];
    if (n->flags & IR_FLAG_DEAD) return false;
    if (n->op == IR_CONST_INT) { *out = n->imm.i64; return true; }
    if (n->op == IR_CONST_NUM && n->imm.num == (double)(int64_t)n->imm.num) {
        *out = (int64_t)n->imm.num;
        return true;
    }
    if ((n->op == IR_BOX_NUM || n->op == IR_BOX_INT) && n->op1 < buf->count) {
        const IRNode* inner = &buf->nodes[n->op1];
        if (inner->op == IR_CONST_INT) { *out = inner->imm.i64; return true; }
        if (inner->op == IR_CONST_NUM &&
            inner->imm.num == (double)(int64_t)inner->imm.num) {
            *out = (int64_t)inner->imm.num;
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Pass 13.5: Promote boxed nested-loop induction PHIs to machine types.
//
// The recorder peels the nested loop's first iteration and carries ci, cr and
// the iteration counter as BOXED PHIs:
//   p = PHI(BOX(e0), BOX(ADD(UNBOX(p), step)))      (exact-integer counter)
//   z = PHI(BOX(e0), BOX(recurrence))               (FP value, e.g. zr/zi)
// Every back-edge round-trips the value through the NaN box and (for the FP
// case) a stack spill, adding ~8 cycles to the critical path. When every
// in-loop use unboxes the PHI, carry it unboxed instead:
//   - counter: INT PHI with CONST_INT entry/step, INT add, INT comparison.
//   - FP:      NUM PHI (raw entry, raw recurrence back).
// The back-edge STORE_STACK that keeps the interpreter stack in sync still
// boxes the new value (BOX_INT for the counter; the existing BOX_NUM for FP),
// but that work is off the FP critical path. Snapshot refs to the boxed PHI
// simply point at the now-typed PHI: the exit stub writes a raw double (valid
// NaN-boxed Value) for NUM and converts INT to a double, so reconstruction
// stays correct.
// ===========================================================================
static void irOptPromoteNestedBoxedPhis(IRBuffer* buf)
{
    // The innermost loop is the last LOOP_HEADER paired with the first
    // LOOP_BACK that targets it (earlier backs belong to loops that closed
    // before it in the trace). Its induction PHIs sit just before the header
    // (after the outer loop's body), with their back-edge boxes inside the
    // loop.
    uint16_t header = IR_NONE;
    for (uint16_t i = 0; i < buf->count; i++)
        if (buf->nodes[i].op == IR_LOOP_HEADER) header = i; // last wins
    if (header == IR_NONE) return;
    uint16_t back = IR_NONE;
    for (uint16_t i = 0; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_BACK && buf->nodes[i].op1 == header) {
            back = i;
            break;
        }
    }
    if (back == IR_NONE || back <= header) return;

    static bool used[IR_MAX_NODES];
    memset(used, 0, sizeof(used));

    for (uint16_t i = 0; i < header; i++) {
        IRNode* phi = &buf->nodes[i];
        if ((phi->flags & IR_FLAG_DEAD) || phi->op != IR_PHI ||
            phi->type != IR_TYPE_VALUE) continue;
        uint16_t entryId = phi->op1, beId = phi->op2;
        if (entryId >= buf->count || beId >= buf->count) continue;
        // The back-edge value must be computed inside the innermost loop
        // (after its header), excluding outer-loop PHIs.
        if (beId <= header) continue;
        IRNode* be = &buf->nodes[beId];
        if ((be->flags & IR_FLAG_DEAD) ||
            (be->op != IR_BOX_NUM && be->op != IR_BOX_INT)) continue;
        uint16_t rawId = be->op1;
        if (rawId >= buf->count) continue;
        IRNode* raw = &buf->nodes[rawId];
        if (raw->flags & IR_FLAG_DEAD) continue;

        // ---- Pattern A: exact-integer counter ----
        // back = BOX(ADD(UNBOX(phi), step)) with integer constant entry/step.
        // The box may already be BOX_INT (mandelbrot's iteration counter is
        // boxed with BOX_INT by the recorder); treat it the same as BOX_NUM.
        if (raw->op == IR_ADD && raw->op1 < buf->count &&
            (buf->nodes[raw->op1].op == IR_UNBOX_NUM ||
             buf->nodes[raw->op1].op == IR_UNBOX_INT) &&
            buf->nodes[raw->op1].op1 == phi->id) {
            int64_t stepVal, entryVal;
            if (!irConstIntValue(buf, raw->op2, &stepVal)) continue;
            if (!irConstIntValue(buf, entryId, &entryVal)) continue;

            // INT constants in dead pre-header slots (materialized once).
            uint16_t cEntry = irClaimDeadSlot(buf, 0, i, used);
            uint16_t cStep  = irClaimDeadSlot(buf, 0, rawId, used);
            if (cEntry == IR_NONE || cStep == IR_NONE) continue;
            irFillConstIntSlot(buf, cEntry, entryVal);
            irFillConstIntSlot(buf, cStep, stepVal);

            // The UNBOX (phi) use is replaced by the INT phi itself.
            uint16_t unboxId = raw->op1;
            replaceUses(buf, unboxId, phi->id);

            // Rewrite the in-loop ADD as INT ADD(phi, cStep).
            IRNode* add = &buf->nodes[rawId];
            add->op  = IR_ADD;
            add->op1 = phi->id;
            add->op2 = cStep;
            add->type = IR_TYPE_INT;
            add->flags = 0;
            memset(&add->imm, 0, sizeof(add->imm));

            // Rewrite the PHI as INT.
            phi->op1 = cEntry;
            phi->op2 = rawId;
            phi->type = IR_TYPE_INT;
            phi->flags = 0;

            // The back-edge box becomes BOX_INT(newValue) so the STORE_STACK
            // that re-syncs the interpreter stack holds a valid boxed number.
            IRNode* box = &buf->nodes[beId];
            box->op  = IR_BOX_INT;
            box->op1 = rawId;
            box->op2 = IR_NONE;
            box->type = IR_TYPE_VALUE;
            box->flags = 0;
            memset(&box->imm, 0, sizeof(box->imm));

            killNode(&buf->nodes[unboxId]);

            // Convert integral CONST_NUM operands of integer comparisons to
            // CONST_INT so the fused LOOP_EXIT compares as integers (no
            // int->double conversion on every iteration).
            for (uint16_t k = 0; k < buf->count; k++) {
                IRNode* u = &buf->nodes[k];
                if (u->flags & IR_FLAG_DEAD) continue;
                if (u->op != IR_LT && u->op != IR_GT && u->op != IR_LTE &&
                    u->op != IR_GTE && u->op != IR_EQ && u->op != IR_NEQ)
                    continue;
                for (int s = 0; s < 2; s++) {
                    uint16_t opId = s == 0 ? u->op1 : u->op2;
                    uint16_t otherId = s == 0 ? u->op2 : u->op1;
                    if (opId >= buf->count || otherId >= buf->count) continue;
                    if (buf->nodes[otherId].type != IR_TYPE_INT) continue;
                    const IRNode* c = &buf->nodes[opId];
                    if ((c->flags & IR_FLAG_DEAD)) continue;
                    if (c->op != IR_CONST_NUM ||
                        c->imm.num != (double)(int64_t)c->imm.num) continue;
                    uint16_t cInt = irClaimDeadSlot(buf, 0, k, used);
                    if (cInt == IR_NONE) continue;
                    irFillConstIntSlot(buf, cInt, (int64_t)c->imm.num);
                    if (s == 0) u->op1 = cInt; else u->op2 = cInt;
                }
            }
            continue;
        }

        // ---- Pattern B: FP value (zr/zi) ----
        // Every in-loop use of the boxed PHI is UNBOX_NUM(phi). The entry is a
        // raw number (BOX_NUM(raw), CONST_NUM, or UNBOX_NUM) and the back-edge
        // is BOX_NUM(raw recurrence).
        uint16_t entryNum = entryId;
        if (buf->nodes[entryId].op == IR_BOX_NUM)
            entryNum = buf->nodes[entryId].op1;
        if (entryNum >= buf->count ||
            buf->nodes[entryNum].type != IR_TYPE_NUM) continue;
        if (raw->type != IR_TYPE_NUM) continue;

        bool convertible = true;
        for (uint16_t k = (uint16_t)(header + 1); k < back; k++) {
            const IRNode* u = &buf->nodes[k];
            if (u->flags & IR_FLAG_DEAD) continue;
            if (u->op1 == phi->id ||
                (u->op != IR_GUARD_CLASS && u->op2 == phi->id)) {
                if (u->op != IR_UNBOX_NUM || u->op1 != phi->id) {
                    convertible = false;
                    break;
                }
            }
        }
        if (!convertible) continue;

        // Rewrite the PHI as NUM.
        phi->op1 = entryNum;
        phi->op2 = rawId;
        phi->type = IR_TYPE_NUM;
        phi->flags = 0;
        memset(&phi->imm, 0, sizeof(phi->imm));

        // Replace each in-loop UNBOX_NUM(phi) with the numeric PHI.
        for (uint16_t k = (uint16_t)(header + 1); k < back; k++) {
            IRNode* u = &buf->nodes[k];
            if ((u->flags & IR_FLAG_DEAD) || u->op != IR_UNBOX_NUM ||
                u->op1 != phi->id) continue;
            replaceUses(buf, u->id, phi->id);
            killNode(u);
        }

        // The back-edge STORE_STACK that re-syncs the interpreter stack
        // boxes the new value. Since a Wren number Value is the raw
        // IEEE-754 bit pattern, storing the NUM directly is identical to
        // storing the box — but the value then lives in an FP register
        // across the loop instead of being forced through a GPR box + memory
        // round-trip on every iteration. The box may also appear in loop
        // snapshots (deopt needs the current zr/zi); the exit-stub writeback
        // already stores raw doubles as Values, so those entries are remapped
        // to the raw value rather than skipped.
        {
            uint16_t useCounts[IR_MAX_NODES];
            memset(useCounts, 0, sizeof(uint16_t) * buf->count);
            for (uint16_t k = 0; k < buf->count; k++) {
                const IRNode* u = &buf->nodes[k];
                if (u->flags & IR_FLAG_DEAD) continue;
                if (u->op1 != IR_NONE && u->op1 < buf->count)
                    useCounts[u->op1]++;
                if (u->op != IR_GUARD_CLASS && u->op2 != IR_NONE &&
                    u->op2 < buf->count) useCounts[u->op2]++;
            }
            for (uint16_t k = (uint16_t)(header + 1); k < back; k++) {
                IRNode* st = &buf->nodes[k];
                if ((st->flags & IR_FLAG_DEAD) || st->op != IR_STORE_STACK ||
                    st->op1 == IR_NONE || st->op1 >= buf->count) continue;
                uint16_t boxId = st->op1;
                const IRNode* box = &buf->nodes[boxId];
                if ((box->flags & IR_FLAG_DEAD) || box->op != IR_BOX_NUM ||
                    box->op1 == IR_NONE || box->op1 >= buf->count) continue;
                const IRNode* val = &buf->nodes[box->op1];
                if ((val->flags & IR_FLAG_DEAD) || val->type != IR_TYPE_NUM)
                    continue;
                // Only the STORE_STACK (and snapshots) may use the box.
                if (useCounts[boxId] > 1) continue;
                st->op1 = box->op1;
                for (uint16_t s = 0; s < buf->snapshot_entry_count; s++) {
                    if (buf->snapshot_entries[s].ssa_ref == boxId)
                        buf->snapshot_entries[s].ssa_ref = box->op1;
                }
                killNode(&buf->nodes[boxId]);
            }
        }
    }
}

// Pass 13.6: Forward loads of loop-carried stack slots to their induction
// PHIs across the innermost loop's exit.
//
// Pass 13.5 promotes the innermost loop's induction values to machine-typed
// PHIs (INT counter, NUM recurrences). The enclosing loop's continuation
// afterwards reloads those slots from the interpreter stack:
//
//     PHI(S) p                              (loop-carried, machine-typed)
//     header:
//       ... uses p ...
//       STORE_STACK slot=S box/raw(p)       (stack sync, every iteration)
//       LOOP_BACK header
//     T:  LOAD_STACK slot=S                 (reload in the continuation)
//
// The reload re-materializes what the PHI already holds, forcing the value
// through a memory load and (for the INT counter) a box on the way out.
// Because the only write to slot S inside the loop is the back-edge sync
// store — whose operand IS the PHI's back-edge value — slot S equals the PHI
// the moment the loop exits: forward the reload to the PHI, remap the
// post-loop snapshots that covered it (replaceUses rewrites the entries),
// drop the reload, and delete the now-dead sync store. For the INT counter
// this removes the per-iteration BOX_INT + store; for the NUM recurrences it
// removes the FP store.
//
// Soundness (a load is only forwarded when every check passes):
//  * The loop falls out only through LOOP_EXIT branches, all to one target T
//    after the back-edge with only dead NOPs in between — T is reachable
//    only by branching, so the last writer of slot S before T is the in-loop
//    sync store.
//  * slot S has exactly one in-loop STORE_STACK, whose operand is (or boxes)
//    exactly one pre-loop PHI's back-edge value.
//  * No other STORE_STACK of slot S sits between T and the reload.
//  * Snapshot entries that covered the reload are rewritten to the PHI, so a
//    deopt in the continuation reconstructs slot S from the live PHI.
//  * The enclosing loop body rewrites slot S before the next inner-loop
//    entry, so once every reload is forwarded the sync store is dead.
static void irOptForwardLoopExitLoads(IRBuffer* buf)
{
    if (buf == NULL || buf->count == 0) return;

    // Innermost loop: the last LOOP_HEADER paired with the first LOOP_BACK
    // that targets it.
    uint16_t header = IR_NONE;
    for (uint16_t i = 0; i < buf->count; i++)
        if (buf->nodes[i].op == IR_LOOP_HEADER) header = i;
    if (header == IR_NONE) return;
    uint16_t back = IR_NONE;
    for (uint16_t i = 0; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_BACK && buf->nodes[i].op1 == header) {
            back = i;
            break;
        }
    }
    if (back == IR_NONE || back <= header) return;

    // Enclosing loop: the first LOOP_BACK after the inner loop's back-edge.
    // Without one this is a single-loop trace with no in-trace continuation.
    uint16_t outerBack = IR_NONE;
    for (uint16_t i = back + 1; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_BACK) { outerBack = i; break; }
    }
    if (outerBack == IR_NONE) return;

    // All LOOP_EXITs inside the loop must branch to the same target, after
    // the back-edge, with only dead NOPs in between (so the target is reached
    // exclusively through the LOOP_EXIT branches).
    uint16_t exitTarget = IR_NONE;
    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* n = &buf->nodes[i];
        if ((n->flags & IR_FLAG_DEAD) || n->op != IR_LOOP_EXIT) continue;
        uint16_t t = n->imm.jump.target;
        if (t >= buf->count || t <= back) return;
        if (exitTarget == IR_NONE) exitTarget = t;
        else if (exitTarget != t) return;
    }
    if (exitTarget == IR_NONE || exitTarget >= outerBack) return;
    for (uint16_t i = back + 1; i < exitTarget; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->op != IR_NOP || !(n->flags & IR_FLAG_DEAD)) return;
    }

    for (uint16_t i = 0; i < header; i++) {
        const IRNode* n = &buf->nodes[i];
        if ((n->flags & IR_FLAG_DEAD) || n->op != IR_LOOP_EXIT) continue;
        if (n->imm.jump.target == exitTarget) return;
    }

    static bool killed[IR_MAX_NODES];
    memset(killed, 0, sizeof(killed));

    for (uint16_t li = exitTarget; li < outerBack; li++) {
        IRNode* load = &buf->nodes[li];
        if ((load->flags & IR_FLAG_DEAD) || load->op != IR_LOAD_STACK) continue;
        uint16_t slot = load->imm.mem.slot;

        // Exactly one in-loop STORE_STACK of this slot.
        uint16_t storeId = IR_NONE;
        for (uint16_t i = header + 1; i < back; i++) {
            IRNode* st = &buf->nodes[i];
            if ((st->flags & IR_FLAG_DEAD) || st->op != IR_STORE_STACK ||
                st->imm.mem.slot != slot) continue;
            if (storeId != IR_NONE) { storeId = IR_NONE; break; }
            storeId = i;
        }
        if (storeId == IR_NONE) continue;
        IRNode* store = &buf->nodes[storeId];

        // The store's operand must be (or box) a pre-loop PHI's back-edge
        // value, and that match must be unique.
        uint16_t phiId = IR_NONE;
        for (uint16_t i = 0; i < header; i++) {
            const IRNode* phi = &buf->nodes[i];
            if ((phi->flags & IR_FLAG_DEAD) || phi->op != IR_PHI) continue;
            uint16_t v = store->op1;
            if (v >= buf->count) continue;
            if (buf->nodes[v].op == IR_BOX_NUM || buf->nodes[v].op == IR_BOX_INT)
                v = buf->nodes[v].op1;
            if (phi->op2 == v) {
                if (phiId != IR_NONE) { phiId = IR_NONE; break; }
                phiId = i;
            }
        }
        if (phiId == IR_NONE) continue;

        // No other writer of the slot between the exit and the reload.
        bool conflicting = false;
        for (uint16_t i = exitTarget; i < outerBack; i++) {
            if (i == li) continue;
            const IRNode* st = &buf->nodes[i];
            if ((st->flags & IR_FLAG_DEAD)) continue;
            if (st->op == IR_STORE_STACK && st->imm.mem.slot == slot) {
                conflicting = true;
                break;
            }
        }
        if (conflicting) continue;

        // Forward the reload to the PHI (uses + snapshot entries), drop it,
        // and schedule the sync store for removal.
        replaceUses(buf, load->id, phiId);
        killNode(load);
        killed[storeId] = true;
    }

    // Delete the sync stores. DCE treats STORE_STACK as a root, so they are
    // removed here. A back-edge box that only fed the store dies with it.
    for (uint16_t i = header + 1; i < back; i++) {
        if (!killed[i]) continue;
        IRNode* store = &buf->nodes[i];
        uint16_t v = store->op1;
        bool isBox = v < buf->count && (buf->nodes[v].op == IR_BOX_NUM ||
                                        buf->nodes[v].op == IR_BOX_INT);
        killNode(store);
        if (!isBox) continue;
        bool used = false;
        for (uint16_t s = 0; s < buf->snapshot_entry_count; s++) {
            if (buf->snapshot_entries[s].ssa_ref == v) { used = true; break; }
        }
        for (uint16_t k = 0; k < buf->count && !used; k++) {
            const IRNode* n = &buf->nodes[k];
            if ((n->flags & IR_FLAG_DEAD) || n->op == IR_NOP) continue;
            if (n->op1 == v || (n->op != IR_GUARD_CLASS && n->op2 == v))
                used = true;
        }
        if (!used) killNode(&buf->nodes[v]);
    }

    // Fold the UNBOX(phi) identities the forwarding introduced: an INT/NUM
    // PHI is already unboxed, so the continuation's UNBOX nodes vanish.
    for (uint16_t i = exitTarget; i < outerBack; i++) {
        IRNode* u = &buf->nodes[i];
        if ((u->flags & IR_FLAG_DEAD)) continue;
        IRType targetType;
        if (u->op == IR_UNBOX_INT) targetType = IR_TYPE_INT;
        else if (u->op == IR_UNBOX_NUM) targetType = IR_TYPE_NUM;
        else continue;
        if (u->op1 >= buf->count) continue;
        const IRNode* phi = &buf->nodes[u->op1];
        if (phi->op != IR_PHI || phi->type != targetType) continue;
        replaceUses(buf, u->id, u->op1);
        killNode(u);
    }
}

void irOptFuseComparisonGuards(IRBuffer* buf)
{
    if (buf == NULL || buf->count == 0) return;

    uint16_t useCounts[IR_MAX_NODES];
    bool inSnapshot[IR_MAX_NODES];
    memset(useCounts, 0, sizeof(uint16_t) * buf->count);
    memset(inSnapshot, 0, sizeof(bool) * buf->count);

    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op1 != IR_NONE && n->op1 < buf->count) useCounts[n->op1]++;
        // GUARD_CLASS stores a snapshot id, not an SSA operand, in op2.
        if (n->op != IR_GUARD_CLASS && n->op2 != IR_NONE &&
            n->op2 < buf->count) useCounts[n->op2]++;
    }
    for (uint16_t i = 0; i < buf->snapshot_entry_count; i++) {
        uint16_t ref = buf->snapshot_entries[i].ssa_ref;
        if (ref < buf->count) inSnapshot[ref] = true;
    }

    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* guard = &buf->nodes[i];
        if ((guard->flags & IR_FLAG_DEAD) ||
            (guard->op != IR_GUARD_TRUE && guard->op != IR_LOOP_EXIT) ||
            guard->op1 == IR_NONE || guard->op1 >= buf->count) continue;

        IRNode* cond = &buf->nodes[guard->op1];
        uint16_t boxId = IR_NONE;   // set when the cond came via BOX_BOOL
        bool isCmp = (cond->op == IR_LT || cond->op == IR_GT ||
                      cond->op == IR_LTE || cond->op == IR_GTE ||
                      cond->op == IR_EQ || cond->op == IR_NEQ);

        if ((cond->flags & IR_FLAG_DEAD) || !isCmp) {
            // Not a raw comparison: try the BOX_BOOL(cmp) chain.
            if (cond->flags & IR_FLAG_DEAD) continue;
            if (cond->op != IR_BOX_BOOL || cond->op1 == IR_NONE ||
                cond->op1 >= buf->count ||
                useCounts[cond->id] != 1 || inSnapshot[cond->id]) continue;
            boxId = cond->id;
            cond = &buf->nodes[cond->op1];
            isCmp = (cond->op == IR_LT || cond->op == IR_GT ||
                     cond->op == IR_LTE || cond->op == IR_GTE ||
                     cond->op == IR_EQ || cond->op == IR_NEQ);
            if ((cond->flags & IR_FLAG_DEAD) || !isCmp ||
                useCounts[cond->id] != 1 || inSnapshot[cond->id]) continue;
        } else if (useCounts[cond->id] != 1 || inSnapshot[cond->id]) {
            continue;
        }

        if (guard->op == IR_LOOP_EXIT) {
            // The exit test is a comparison (boxed or raw) that no one else
            // uses. Point LOOP_EXIT at the comparison and mark it fused;
            // LOOP_EXIT codegen branches directly on its operands, skipping
            // the bool materialization (and box round-trip) entirely.
            guard->op1 = cond->id;
            if (boxId != IR_NONE) killNode(&buf->nodes[boxId]);
            cond->flags |= IR_FLAG_FUSED_LOOP_EXIT;
            if (getenv("WREN_JIT_DBG_FUSE"))
                fprintf(stderr, "[FUSE] loop_exit %u -> cmp %u (%s) fused\n",
                        guard->id, cond->id,
                        cond->op == IR_LT ? "LT" : cond->op == IR_LTE ? "LTE" :
                        cond->op == IR_GT ? "GT" : cond->op == IR_GTE ? "GTE" :
                        cond->op == IR_EQ ? "EQ" : "NEQ");
        } else {
            cond->flags |= IR_FLAG_FUSED_TRUE_GUARD;
            cond->imm.snapshot_id = guard->imm.snapshot_id;
            killNode(guard);
            // The boxed bool existed only to feed this guard (checked above:
            // useCounts==1 and not a snapshot entry), so it is dead now that
            // codegen branches straight on the comparison. Leaving it costs a
            // ~7-instruction box in the loop body on every iteration; the
            // LOOP_EXIT branch above already kills its box.
            if (boxId != IR_NONE) killNode(&buf->nodes[boxId]);
        }
    }
}

// Fast-forward the exact recurrence produced by a guarded boolean toggler
// followed by `if (state) moduleCounter = moduleCounter + 1` inside an
// ascending inclusive range loop. Codegen validates integer range values and
// then computes all remaining iterations before taking the ordinary range
// completion snapshot.
static void irOptFastForwardToggleCounter(IRBuffer* buf)
{
    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;

    for (uint16_t b = 0; b < back; b++) {
        IRNode* delta = &buf->nodes[b];
        if ((delta->flags & IR_FLAG_DEAD) || delta->op != IR_BOOL_TO_NUM ||
            delta->op1 == IR_NONE || delta->op1 >= buf->count) continue;
        uint16_t notId = delta->op1;
        IRNode* booleanNot = &buf->nodes[notId];
        if (booleanNot->op != IR_BOOL_NOT || booleanNot->op1 == IR_NONE)
            continue;
        uint16_t state = booleanNot->op1;

        uint16_t countPhi = IR_NONE, countAdd = IR_NONE;
        for (uint16_t i = 0; i < back; i++) {
            IRNode* add = &buf->nodes[i];
            if ((add->flags & IR_FLAG_DEAD) || add->op != IR_ADD ||
                add->type != IR_TYPE_INT) continue;
            uint16_t other = add->op1 == b ? add->op2 :
                             add->op2 == b ? add->op1 : IR_NONE;
            if (other < buf->count && buf->nodes[other].op == IR_PHI &&
                buf->nodes[other].type == IR_TYPE_INT) {
                countPhi = other;
                countAdd = i;
                break;
            }
        }
        if (countPhi == IR_NONE) continue;

        uint16_t object = IR_NONE, field = 0;
        for (uint16_t i = (uint16_t)(notId + 1); i < back; i++) {
            IRNode* store = &buf->nodes[i];
            if (store->op == IR_STORE_FIELD && store->op2 == notId) {
                object = store->op1;
                field = store->imm.mem.field;
                break;
            }
        }
        if (object == IR_NONE) continue;

        bool boolGuard = false;
        for (uint16_t i = 0; i < notId; i++)
            if (buf->nodes[i].op == IR_GUARD_BOOL &&
                buf->nodes[i].op1 == booleanNot->op1) boolGuard = true;
        if (!boolGuard) continue;

        uint16_t iterPhi = IR_NONE, limit = IR_NONE, newIter = IR_NONE;
        uint16_t snapshot = IR_NONE;
        for (uint16_t i = 0; i < notId; i++) {
            IRNode* cmp = &buf->nodes[i];
            if ((cmp->flags & IR_FLAG_DEAD) ||
                !(cmp->flags & IR_FLAG_FUSED_TRUE_GUARD) ||
                cmp->op != IR_LTE || cmp->op1 >= buf->count ||
                cmp->op2 >= buf->count) continue;
            IRNode* add = &buf->nodes[cmp->op1];
            IRNode* bound = &buf->nodes[cmp->op2];
            if (add->op != IR_ADD || bound->op != IR_LOAD_RANGE) continue;
            uint16_t phi = add->op1 < buf->count &&
                           buf->nodes[add->op1].op == IR_PHI ? add->op1 :
                           add->op2 < buf->count &&
                           buf->nodes[add->op2].op == IR_PHI ? add->op2 : IR_NONE;
            if (phi == IR_NONE) continue;
            iterPhi = phi;
            newIter = cmp->op1;
            limit = cmp->op2;
            snapshot = cmp->imm.snapshot_id;
            break;
        }
        if (iterPhi == IR_NONE || snapshot >= buf->snapshot_count) continue;

        // The bulk operation updates the counter PHI register itself, so all
        // deferred stores for the old back-edge value must read that register.
        replaceUses(buf, countAdd, countPhi);

        // The existing range snapshot is also the specialization fallback and
        // therefore must keep the current iterator. Clone it for successful
        // completion and substitute the inclusive limit so one interpreter
        // Range.iterate call observes exhaustion and leaves the loop.
        if (buf->snapshot_count >= IR_MAX_SNAPSHOTS) return;
        IRSnapshot* sourceSnap = &buf->snapshots[snapshot];
        if ((uint32_t)buf->snapshot_entry_count + sourceSnap->num_entries >
            IR_MAX_NODES) return;
        uint16_t completeSnapshot = buf->snapshot_count++;
        IRSnapshot* completeSnap = &buf->snapshots[completeSnapshot];
        completeSnap->resume_pc = sourceSnap->resume_pc;
        completeSnap->stack_depth = sourceSnap->stack_depth;
        completeSnap->entry_start = buf->snapshot_entry_count;
        completeSnap->num_entries = sourceSnap->num_entries;
        for (uint16_t e = 0; e < sourceSnap->num_entries; e++) {
            IRSnapshotEntry entry =
                buf->snapshot_entries[sourceSnap->entry_start + e];
            if (entry.ssa_ref == iterPhi || entry.ssa_ref == newIter)
                entry.ssa_ref = limit;
            buf->snapshot_entries[buf->snapshot_entry_count++] = entry;
        }
        uint16_t oldModuleEntries = buf->exit_module_entry_count;
        for (uint16_t e = 0; e < oldModuleEntries; e++) {
            IRExitModuleEntry* source = &buf->exit_module_entries[e];
            if (source->snapshot_id != snapshot) continue;
            if (buf->exit_module_entry_count >= IR_MAX_EXIT_MODULE_ENTRIES)
                return;
            IRExitModuleEntry* clone =
                &buf->exit_module_entries[buf->exit_module_entry_count++];
            *clone = *source;
            clone->snapshot_id = completeSnapshot;
        }

        booleanNot->op = IR_TOGGLE_COUNT_BULK;
        booleanNot->op1 = iterPhi;
        booleanNot->op2 = countPhi;
        booleanNot->type = IR_TYPE_VOID;
        booleanNot->flags = 0;
        booleanNot->imm.bulk.limit = limit;
        booleanNot->imm.bulk.state = state;
        booleanNot->imm.bulk.object = object;
        booleanNot->imm.bulk.field = field;
        booleanNot->imm.bulk.snapshot = completeSnapshot;
        booleanNot->imm.bulk.fallback = snapshot;

        for (uint16_t i = (uint16_t)(notId + 1); i <= back; i++)
            killNode(&buf->nodes[i]);
        return;
    }
}

static void irOptFastForwardRangeSum(IRBuffer* buf)
{
    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;
    for (uint16_t i = 0; i < back; i++) {
        IRNode* sumAdd = &buf->nodes[i];
        if ((sumAdd->flags & IR_FLAG_DEAD) || sumAdd->op != IR_ADD ||
            sumAdd->type != IR_TYPE_NUM) continue;
        uint16_t sumPhi = IR_NONE, newIter = IR_NONE;
        if (sumAdd->op1 < buf->count && buf->nodes[sumAdd->op1].op == IR_PHI)
            sumPhi = sumAdd->op1, newIter = sumAdd->op2;
        else if (sumAdd->op2 < buf->count &&
                 buf->nodes[sumAdd->op2].op == IR_PHI)
            sumPhi = sumAdd->op2, newIter = sumAdd->op1;
        if (sumPhi == IR_NONE || newIter >= buf->count ||
            buf->nodes[newIter].op != IR_ADD ||
            buf->nodes[sumPhi].op2 != i) continue;

        IRNode* iterAdd = &buf->nodes[newIter];
        uint16_t iterPhi = iterAdd->op1 < buf->count &&
                           buf->nodes[iterAdd->op1].op == IR_PHI
                               ? iterAdd->op1 : iterAdd->op2;
        if (iterPhi >= buf->count || buf->nodes[iterPhi].op != IR_PHI)
            continue;

        uint16_t limit = IR_NONE, snapshot = IR_NONE;
        for (uint16_t c = 0; c < i; c++) {
            IRNode* cmp = &buf->nodes[c];
            if (!(cmp->flags & IR_FLAG_FUSED_TRUE_GUARD) ||
                cmp->op != IR_LTE || cmp->op1 != newIter ||
                cmp->op2 >= buf->count ||
                buf->nodes[cmp->op2].op != IR_LOAD_RANGE) continue;
            limit = cmp->op2;
            snapshot = cmp->imm.snapshot_id;
            break;
        }
        if (limit == IR_NONE || snapshot >= buf->snapshot_count) continue;

        IRSnapshot* sourceSnap = &buf->snapshots[snapshot];
        if (buf->snapshot_count >= IR_MAX_SNAPSHOTS ||
            (uint32_t)buf->snapshot_entry_count + sourceSnap->num_entries >
                IR_MAX_NODES) return;
        uint16_t completeSnapshot = buf->snapshot_count++;
        IRSnapshot* completeSnap = &buf->snapshots[completeSnapshot];
        completeSnap->resume_pc = sourceSnap->resume_pc;
        completeSnap->stack_depth = sourceSnap->stack_depth;
        completeSnap->entry_start = buf->snapshot_entry_count;
        completeSnap->num_entries = sourceSnap->num_entries;
        for (uint16_t e = 0; e < sourceSnap->num_entries; e++) {
            IRSnapshotEntry entry =
                buf->snapshot_entries[sourceSnap->entry_start + e];
            if (entry.ssa_ref == iterPhi || entry.ssa_ref == newIter)
                entry.ssa_ref = limit;
            buf->snapshot_entries[buf->snapshot_entry_count++] = entry;
        }
        uint16_t oldModuleEntries = buf->exit_module_entry_count;
        for (uint16_t e = 0; e < oldModuleEntries; e++) {
            IRExitModuleEntry* source = &buf->exit_module_entries[e];
            if (source->snapshot_id != snapshot) continue;
            if (buf->exit_module_entry_count >= IR_MAX_EXIT_MODULE_ENTRIES)
                return;
            IRExitModuleEntry* clone =
                &buf->exit_module_entries[buf->exit_module_entry_count++];
            *clone = *source;
            clone->snapshot_id = completeSnapshot;
        }

        replaceUses(buf, i, sumPhi);
        sumAdd->op = IR_RANGE_SUM_BULK;
        sumAdd->op1 = iterPhi;
        sumAdd->op2 = sumPhi;
        sumAdd->type = IR_TYPE_VOID;
        sumAdd->flags = 0;
        sumAdd->imm.arith.limit = limit;
        sumAdd->imm.arith.snapshot = completeSnapshot;
        sumAdd->imm.arith.fallback = snapshot;
        for (uint16_t k = (uint16_t)(i + 1); k <= back; k++)
            killNode(&buf->nodes[k]);
        return;
    }
}

// ===========================================================================
// Pass 17: Hoist list bounds validation out of counted loops
//
// A list access whose object is loop-invariant (with a hoisted class guard),
// whose index is an ascending integral PHI, and whose loop condition proves
// index < limit (or index <= limit) against an invariant limit can drop its
// per-access bounds block. Guard list.count >= limit once at the loop header
// instead of loading the count and comparing on every access.
// ===========================================================================

static bool isAscendingIntegralPhi(const IRBuffer* buf, uint16_t id)
{
    if (id == IR_NONE || id >= buf->count) return false;
    const IRNode* phi = &buf->nodes[id];
    if (phi->op != IR_PHI ||
        (phi->type != IR_TYPE_NUM && phi->type != IR_TYPE_INT) ||
        phi->op2 == IR_NONE || phi->op2 >= buf->count) return false;

    // The phi must precede a LOOP_HEADER: promoted loop-carried phis sit in
    // the pre-header region before their loop's header, and nested loops add
    // a second header (inner phis precede it). Scan for the next header so
    // nested inner phis are recognized too.
    bool beforeHeader = false;
    for (uint16_t i = (uint16_t)(id + 1); i < buf->count; i++) {
        if (buf->nodes[i].op == IR_LOOP_HEADER) { beforeHeader = true; break; }
    }
    if (!beforeHeader) return false;
    const IRNode* back = &buf->nodes[phi->op2];
    if (back->op != IR_ADD) return false;
    uint16_t stepId = back->op1 == id ? back->op2 :
                      (back->op2 == id ? back->op1 : IR_NONE);
    if (stepId == IR_NONE || stepId >= buf->count) return false;
    const IRNode* step = &buf->nodes[stepId];
    if (phi->type == IR_TYPE_INT)
        return step->op == IR_CONST_INT && step->imm.i64 > 0;
    return step->op == IR_CONST_NUM && step->imm.num > 0.0 &&
           step->imm.num == (double)(int64_t)step->imm.num;
}

// Return the index of the hoisted GUARD_CLASS proving `listId` is a List, or
// IR_NONE. The bounds guard must be emitted after this guard so the list is
// known to be a List before its count field is read.
static uint16_t findHoistedClassGuard(const IRBuffer* buf, uint16_t listId,
                                      uint16_t header)
{
    for (uint16_t i = 0; i < header; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->op == IR_GUARD_CLASS && n->op1 == listId) return i;
    }
    return IR_NONE;
}

// Find a fused comparison before node `accessIdx` that bounds the index PHI
// `idxId` from above. Returns the invariant limit SSA, the guard direction
// (0 = index < limit, 1 = index <= limit), and the comparison's node index.
static bool findIndexBound(const IRBuffer* buf, uint16_t accessIdx,
                           uint16_t idxId, uint16_t header,
                           uint16_t* limitOut, uint16_t* dirOut,
                           uint16_t* cmpIdxOut)
{
    for (uint16_t i = header + 1; i < accessIdx; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        // Accept either a guard (abort when false) or a structured LOOP_EXIT
        // (leave the loop when true): in both cases reaching an access past
        // this comparison means the index is still within the limit.
        if (!(n->flags & IR_FLAG_FUSED_TRUE_GUARD) &&
            !(n->flags & IR_FLAG_FUSED_LOOP_EXIT)) continue;
        if (n->op != IR_LT && n->op != IR_LTE &&
            n->op != IR_GT && n->op != IR_GTE) continue;
        if (n->op1 == IR_NONE || n->op2 == IR_NONE) continue;

        uint16_t limit = IR_NONE;
        uint16_t dir = 0;
        if (n->op1 == idxId && n->op2 < buf->count) {
            if (n->op == IR_LT) { limit = n->op2; dir = 0; }
            else if (n->op == IR_LTE) { limit = n->op2; dir = 1; }
        } else if (n->op2 == idxId && n->op1 < buf->count) {
            if (n->op == IR_GT) { limit = n->op1; dir = 0; }
            else if (n->op == IR_GTE) { limit = n->op1; dir = 1; }
        }
        if (limit == IR_NONE) continue;

        // The limit must be loop-invariant (or a constant).
        if (!(buf->nodes[limit].flags & IR_FLAG_INVARIANT) &&
            !isConst(buf->nodes[limit].op))
            continue;

        *limitOut = limit;
        *dirOut = dir;
        *cmpIdxOut = i;
        return true;
    }
    return false;
}

// Find the last SNAPSHOT before node index `before`. For a comparison this is
// the comparison's guard snapshot, which resumes at the comparison PC before
// the loop body — the right deopt point for a hoisted bounds guard.
static uint16_t findSnapshotBefore(const IRBuffer* buf, uint16_t before)
{
    for (uint16_t i = before; i > 0; i--) {
        const IRNode* n = &buf->nodes[i - 1];
        if (n->op == IR_SNAPSHOT) return n->imm.snapshot_id;
    }
    return IR_NONE;
}

void irOptHoistListBounds(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE) return;
    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;

    // A call in the loop body could resize the list, making the hoisted count
    // stale. (Codegen rejects traces with calls anyway, but bail early.)
    for (uint16_t i = header + 1; i < back; i++) {
        IROp op = buf->nodes[i].op;
        if (op == IR_CALL_C || op == IR_CALL_WREN) return;
    }

    // One guard record per (list, limit, direction) group.
    struct {
        uint16_t list;
        uint16_t limit;
        uint16_t direction;
        uint16_t snapshot;
        uint16_t class_guard;   // index of the list's hoisted GUARD_CLASS
    } guards[8];
    int numGuards = 0;

    for (uint16_t i = header + 1; i < back; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op != IR_LIST_LOAD && n->op != IR_LIST_STORE) continue;
        if (n->op1 == IR_NONE || n->op2 == IR_NONE) continue;

        uint16_t listId = n->op1;
        uint16_t idxId = n->op2;

        // 1. List must be loop-invariant with a hoisted class guard.
        if (!(buf->nodes[listId].flags & IR_FLAG_INVARIANT)) continue;
        uint16_t classGuard = findHoistedClassGuard(buf, listId, header);
        if (classGuard == IR_NONE) continue;

        // 2. Index must be an ascending integral PHI.
        if (!isAscendingIntegralPhi(buf, idxId)) continue;

        // 3. A fused comparison before the access must bound the index.
        uint16_t limit = IR_NONE, direction = 0, cmpIdx = IR_NONE;
        if (!findIndexBound(buf, i, idxId, header, &limit, &direction, &cmpIdx))
            continue;
        uint16_t snap = findSnapshotBefore(buf, cmpIdx);
        if (snap == IR_NONE) continue;

        // 4. Find or create the guard record.
        int g = -1;
        for (int k = 0; k < numGuards; k++) {
            if (guards[k].list == listId && guards[k].limit == limit &&
                guards[k].direction == direction) { g = k; break; }
        }
        if (g < 0) {
            if (numGuards >= 8) return;
            g = numGuards++;
            guards[g].list = listId;
            guards[g].limit = limit;
            guards[g].direction = direction;
            guards[g].snapshot = snap;
            guards[g].class_guard = classGuard;
        }

        // 5. Mark the access.
        n->flags |= IR_FLAG_BOUNDS_HOISTED;
    }

    if (numGuards == 0) return;

    // Emit one guard per group in a NOP slot after the list's hoisted class
    // guard (so the list is known to be a List before its count is read) and
    // before the loop header. If there aren't enough such slots, undo all
    // marking: an access marked hoisted without its guard would skip its
    // bounds check entirely.
    for (int g = 0; g < numGuards; g++) {
        uint16_t slot = IR_NONE;
        for (uint16_t j = guards[g].class_guard + 1; j < header; j++) {
            if (buf->nodes[j].op == IR_NOP) { slot = j; break; }
        }
        if (slot == IR_NONE) {
            for (uint16_t i = header + 1; i < back; i++) {
                IRNode* n = &buf->nodes[i];
                if (n->op == IR_LIST_LOAD || n->op == IR_LIST_STORE)
                    n->flags &= ~IR_FLAG_BOUNDS_HOISTED;
            }
            return;
        }
        IRNode* gn = &buf->nodes[slot];
        gn->op = IR_LIST_BOUNDS_GUARD;
        gn->id = slot;
        gn->op1 = guards[g].list;
        gn->op2 = guards[g].limit;
        gn->type = IR_TYPE_VOID;
        gn->imm.bounds.snapshot = guards[g].snapshot;
        gn->imm.bounds.direction = guards[g].direction;
        gn->flags = 0;
    }

    // Second phase: cache each hoisted list's elements.data pointer in a
    // pre-header IR_LIST_DATA node and point its accesses at it. This drops the
    // tag-strip `and` + data `ldr` from every iteration. Purely an optimization:
    // if no NOP slot is free, accesses keep using the list object (the codegen
    // path for an object operand still works) and just retain the hoisted bounds.
    for (int g = 0; g < numGuards; g++) {
        uint16_t listId = guards[g].list;
        uint16_t slot = IR_NONE;
        for (uint16_t j = guards[g].class_guard + 1; j < header; j++) {
            if (buf->nodes[j].op == IR_NOP) { slot = j; break; }
        }
        if (slot == IR_NONE) continue;

        IRNode* dn = &buf->nodes[slot];
        dn->op = IR_LIST_DATA;
        dn->id = slot;
        dn->op1 = listId;
        dn->op2 = IR_NONE;
        dn->type = IR_TYPE_PTR;
        dn->flags = 0;

        // The LIST_DATA node must be live through the loop even though it has
        // no SSA consumers before the accesses (regalloc extends pre-header
        // values, but mark it explicitly so nothing DCEs it as unused).
        dn->flags |= IR_FLAG_INVARIANT;

        for (uint16_t i = header + 1; i < back; i++) {
            IRNode* n = &buf->nodes[i];
            if (n->flags & IR_FLAG_DEAD) continue;
            if ((n->op == IR_LIST_LOAD || n->op == IR_LIST_STORE) &&
                n->op1 == listId && (n->flags & IR_FLAG_BOUNDS_HOISTED))
                n->op1 = slot;
        }
    }
}

// ===========================================================================
// Pass 18: Fast-forward the spectral-norm denominator recurrence.
//
// The inner loop of spectral_norm computes its divisor as
//   d(j) = floor((i+j)(i+j+1)/2) + i + 1
// from the loop-carried index j and a loop-invariant i. Because
//   d(j) - d(j-1) = (i + j)
// the whole 6-op floor chain collapses to a single accumulator add:
//   d(-1) = floor(i(i+1)/2) + 1          (pre-header)
//   d(j)  = d(j-1) + (i + j)             (1 ADD per iteration)
//
// The identity is exact only while every intermediate stays a representable
// integer (< 2^53). Pre-header IR_GUARD_RANGE nodes prove |i|, |j0|, |n| are
// exact integers with magnitude <= 2^25, which bounds |ij| <= 2^26 and every
// product/sum below 2^53 (the accumulator sum is <= |D0| + iters*|ij| < 2^53
// with iters <= |n - j0| <= 2^26). On guard failure the trace deopts to the
// interpreter, which computes the floor chain exactly. The same guard bounds
// also make the per-iteration IR_FLAG_INT_GUARD range checks on ij, the
// accumulator, and the index back-edge provably redundant, so they are dropped.
// ===========================================================================

// 2^25: with |i|, |j0|, |n| <= 2^25 every denom intermediate < 2^53.
#define DENOM_RANGE_LIMIT 33554432.0

static uint16_t findSnapshotAfter(const IRBuffer* buf, uint16_t after)
{
    for (uint16_t i = (uint16_t)(after + 1); i < buf->count; i++) {
        if (buf->nodes[i].op == IR_SNAPSHOT)
            return buf->nodes[i].imm.snapshot_id;
    }
    return IR_NONE;
}

// Find an ascending integral PHI advanced by the integer constant 1.
static uint16_t findUnitStepPhi(const IRBuffer* buf, uint16_t header)
{
    for (uint16_t p = 0; p < header && p < buf->count; p++) {
        const IRNode* phi = &buf->nodes[p];
        if ((phi->flags & IR_FLAG_DEAD) || phi->op != IR_PHI) continue;
        if (phi->type != IR_TYPE_NUM && phi->type != IR_TYPE_INT) continue;
        uint16_t back = phi->op2;
        if (back == IR_NONE || back >= buf->count) continue;
        const IRNode* b = &buf->nodes[back];
        if ((b->flags & IR_FLAG_DEAD) || b->op != IR_ADD) continue;
        uint16_t step = (b->op1 == p) ? b->op2 :
                        (b->op2 == p) ? b->op1 : IR_NONE;
        if (step == IR_NONE || step >= buf->count) continue;
        const IRNode* s = &buf->nodes[step];
        bool isOne = (phi->type == IR_TYPE_INT)
            ? (s->op == IR_CONST_INT && s->imm.i64 == 1)
            : (s->op == IR_CONST_NUM && s->imm.num == 1.0);
        if (isOne) return p;
    }
    return IR_NONE;
}

// Is `id` a numeric/integer constant with value exactly `d`?
static bool isConstVal(const IRBuffer* buf, uint16_t id, int64_t d)
{
    if (id == IR_NONE || id >= buf->count) return false;
    const IRNode* n = &buf->nodes[id];
    if (n->op == IR_CONST_NUM) return n->imm.num == (double)d;
    if (n->op == IR_CONST_INT) return n->imm.i64 == d;
    return false;
}

// Find the first non-dead `op` node in [lo,hi) whose operands are {a,b} in
// either order.
static uint16_t findBinary(const IRBuffer* buf, IROp op, uint16_t a,
                           uint16_t b, uint16_t lo, uint16_t hi)
{
    for (uint16_t i = lo; i < hi && i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op != op) continue;
        if ((n->op1 == a && n->op2 == b) || (n->op1 == b && n->op2 == a))
            return i;
    }
    return IR_NONE;
}

// Find ADD(x, ONE) with ONE the integer constant 1; output the ONE id.
static uint16_t findAddOne(const IRBuffer* buf, uint16_t x,
                           uint16_t lo, uint16_t hi, uint16_t* oneOut)
{
    for (uint16_t i = lo; i < hi && i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op != IR_ADD) continue;
        if (n->op1 == x && isConstVal(buf, n->op2, 1)) {
            if (oneOut) *oneOut = n->op2;
            return i;
        }
        if (n->op2 == x && isConstVal(buf, n->op1, 1)) {
            if (oneOut) *oneOut = n->op1;
            return i;
        }
    }
    return IR_NONE;
}

// Find the loop's upper bound on the ascending PHI `j`: the first body
// comparison j < limit / j <= limit with a loop-invariant limit.
static uint16_t findLoopUpperBound(const IRBuffer* buf, uint16_t j,
                                   uint16_t header, uint16_t back)
{
    for (uint16_t i = (uint16_t)(header + 1); i < back && i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        uint16_t limit = IR_NONE;
        if ((n->op == IR_LT || n->op == IR_LTE) && n->op1 == j)
            limit = n->op2;
        else if ((n->op == IR_GT || n->op == IR_GTE) && n->op2 == j)
            limit = n->op1;
        if (limit == IR_NONE || limit >= buf->count) continue;
        if (limit < header && buf->nodes[limit].op != IR_PHI)
            return limit;
    }
    return IR_NONE;
}

// Find the snapshot-box node for a chain value (BOX_NUM/BOX_INT with op1==val).
static uint16_t findBox(const IRBuffer* buf, uint16_t val,
                        uint16_t header, uint16_t back)
{
    for (uint16_t i = (uint16_t)(header + 1); i < back && i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if ((n->op == IR_BOX_NUM || n->op == IR_BOX_INT) && n->op1 == val)
            return i;
    }
    return IR_NONE;
}

void irOptDenomRecurrence(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    if (header == IR_NONE) return;
    uint16_t back = findLoopBack(buf);
    if (back == IR_NONE) return;

    // 1. Unit-step ascending integral PHI `j`.
    uint16_t j = findUnitStepPhi(buf, header);
    if (j == IR_NONE) return;
    bool isInt = buf->nodes[j].type == IR_TYPE_INT;
    uint16_t j0 = buf->nodes[j].op1;
    uint16_t jBack = buf->nodes[j].op2;
    if (j0 == IR_NONE || j0 >= buf->count) return;

    // 2. ij = i + j with i loop-invariant.
    uint16_t ij = IR_NONE, i = IR_NONE;
    for (uint16_t k = (uint16_t)(header + 1); k < back && k < buf->count; k++) {
        const IRNode* n = &buf->nodes[k];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (n->op != IR_ADD) continue;
        uint16_t a = n->op1, b = n->op2;
        if (a == j && b < header && buf->nodes[b].op != IR_PHI) {
            ij = k; i = b; break;
        }
        if (b == j && a < header && buf->nodes[a].op != IR_PHI) {
            ij = k; i = a; break;
        }
    }
    if (ij == IR_NONE || i == IR_NONE) return;

    // 3. Match the denominator chain b1 -> c -> p -> f -> g -> d.
    uint16_t one = IR_NONE;
    uint16_t b1 = findAddOne(buf, ij, (uint16_t)(header + 1), back, &one);
    if (b1 == IR_NONE) return;
    uint16_t c = findBinary(buf, IR_MUL, ij, b1,
                            (uint16_t)(header + 1), back);
    if (c == IR_NONE) return;
    uint16_t p = IR_NONE, half = IR_NONE;
    for (uint16_t k = (uint16_t)(header + 1); k < back && k < buf->count; k++) {
        const IRNode* n = &buf->nodes[k];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (isInt) {
            if (n->op == IR_ASHR && n->op1 == c && isConstVal(buf, n->op2, 1)) {
                p = k; break;
            }
        } else {
            if (n->op == IR_MUL && n->op1 == c && n->op2 < buf->count &&
                buf->nodes[n->op2].op == IR_CONST_NUM &&
                buf->nodes[n->op2].imm.num == 0.5) { p = k; half = n->op2; break; }
            if (n->op == IR_MUL && n->op2 == c && n->op1 < buf->count &&
                buf->nodes[n->op1].op == IR_CONST_NUM &&
                buf->nodes[n->op1].imm.num == 0.5) { p = k; half = n->op1; break; }
        }
    }
    if (p == IR_NONE) return;
    uint16_t f = p;
    if (!isInt) {
        for (uint16_t k = (uint16_t)(header + 1); k < back && k < buf->count; k++) {
            const IRNode* n = &buf->nodes[k];
            if (n->flags & IR_FLAG_DEAD) continue;
            if (n->op == IR_FLOOR && n->op1 == p) { f = k; break; }
        }
        if (f == p) return;   // no FLOOR
    }
    // Two denom shapes share the chain up to the trailing term:
    //   A) floor(...) + i + 1   (step = i + j)
    //   B) floor(...) + j + 1   (step = i + j + 1)
    // B is the second multiplyAtAv loop: the +1 term is the loop var, so the
    // recurrence advances by ij+1. The +1 of that step is exactly b1 (ij+1),
    // which stays alive as the step operand.
    bool plusJ = false;
    uint16_t g = findBinary(buf, IR_ADD, f, i,
                            (uint16_t)(header + 1), back);
    if (g == IR_NONE) {
        g = findBinary(buf, IR_ADD, f, j, (uint16_t)(header + 1), back);
        if (g == IR_NONE) return;
        plusJ = true;
    }
    uint16_t d = findAddOne(buf, g, (uint16_t)(header + 1), back, NULL);
    if (d == IR_NONE) return;

    // 4. Collect the chain nodes (dedup p==f for INT) and their snapshot boxes.
    //    For the plusJ shape b1 is the recurrence step and must stay alive.
    uint16_t killed[5];
    int numKilled = 0;
    if (!plusJ) killed[numKilled++] = b1;
    if (c != b1) killed[numKilled++] = c;
    if (p != c) killed[numKilled++] = p;
    if (f != p) killed[numKilled++] = f;
    if (g != f) killed[numKilled++] = g;
    // All snapshot boxes of the killed chain. A chain value may be boxed more
    // than once (e.g. IV inference re-boxes the ASHR result), so scan the whole
    // buffer instead of taking one box per chain node.
    uint16_t boxes[16];
    int numBoxes = 0;
    for (uint16_t u = 0; u < buf->count && numBoxes < 16; u++) {
        const IRNode* un = &buf->nodes[u];
        if (un->flags & IR_FLAG_DEAD) continue;
        if (un->op != IR_BOX_NUM && un->op != IR_BOX_INT) continue;
        for (int k = 0; k < numKilled; k++) {
            if (un->op1 == killed[k]) { boxes[numBoxes++] = u; break; }
        }
    }

    // 5. Every chain node's only users must be the chain itself, its box, or d
    //    (d is repurposed in place, not killed). Otherwise the rewrite would
    //    orphan a live computation.
    for (int k = 0; k < numKilled; k++) {
        uint16_t x = killed[k];
        for (uint16_t u = 0; u < buf->count; u++) {
            const IRNode* un = &buf->nodes[u];
            if (un->flags & IR_FLAG_DEAD) continue;
            if (un->op1 != x && un->op2 != x) continue;
            bool allowed = (u == d);
            for (int m = 0; m < numKilled && !allowed; m++)
                if (u == killed[m]) allowed = true;
            for (int m = 0; m < numBoxes && !allowed; m++)
                if (u == boxes[m]) allowed = true;
            if (!allowed) return;
        }
    }

    // 6. Pre-header NOP slots for the guards + preheader seed. All new nodes
    //    must sit AFTER the values they read (j0, i, n, one, half) so the
    //    codegen emits defs before uses. Need:
    //    1 limit const + 3 guards + (NUM: 8 | INT: 7) seed nodes incl. the PHI.
    uint16_t n = findLoopUpperBound(buf, j, header, back);
    if (n == IR_NONE) return;
    uint16_t maxDef = j0;
    if (i > maxDef) maxDef = i;
    if (n > maxDef) maxDef = n;
    if (one != IR_NONE && one > maxDef) maxDef = one;
    if (!isInt && half != IR_NONE && half > maxDef) maxDef = half;
    uint16_t slots[20];
    int nslots = 0;
    for (uint16_t k = (uint16_t)(maxDef + 1); k < header && k < buf->count; k++) {
        if (buf->nodes[k].op == IR_NOP && nslots < 20)
            slots[nslots++] = k;
    }
    // plusJ's seed has no trailing +one (D0 = floor(...) + j0), one node fewer.
    int need = isInt ? (plusJ ? 10 : 11) : (plusJ ? 11 : 12);
    if (nslots < need) return;

    // 7. Deopt snapshot: the first snapshot in the loop body (loop entry).
    uint16_t snap = findSnapshotAfter(buf, header);
    if (snap == IR_NONE) return;

    int s = 0;
    uint16_t limitId = slots[s++];
    initSlotNode(buf, limitId, IR_CONST_NUM, IR_NONE, IR_NONE, IR_TYPE_NUM);
    buf->nodes[limitId].imm.num = DENOM_RANGE_LIMIT;

    uint16_t guardVals[3] = { j0, i, n };
    for (int g = 0; g < 3; g++) {
        uint16_t gslot = slots[s++];
        initSlotNode(buf, gslot, IR_GUARD_RANGE, guardVals[g], IR_NONE,
                     IR_TYPE_VOID);
        buf->nodes[gslot].imm.arith.limit = limitId;
        buf->nodes[gslot].imm.arith.snapshot = snap;
        buf->nodes[gslot].flags |= IR_FLAG_GUARD;
    }

    // 8. Pre-header recurrence seed: D0 = d(j0 - 1) =
    //    floor((i+j0-1)(i+j0)/2) + i + 1. Seeding from the *runtime* loop-entry
    //    j0 keeps the recurrence exact no matter where in the loop recording
    //    (and re-entry) happens. The range guards prove |i|, |j0|, |n| <= 2^25,
    //    so every intermediate stays < 2^53.
    uint16_t ij0 = slots[s++];              // i + j0
    initSlotNode(buf, ij0, IR_ADD, i, j0, isInt ? IR_TYPE_INT : IR_TYPE_NUM);
    uint16_t ij0m1 = slots[s++];            // i + j0 - 1
    initSlotNode(buf, ij0m1, IR_SUB, ij0, one, isInt ? IR_TYPE_INT : IR_TYPE_NUM);
    uint16_t t2 = slots[s++];               // (i+j0-1)(i+j0)
    initSlotNode(buf, t2, IR_MUL, ij0m1, ij0, isInt ? IR_TYPE_INT : IR_TYPE_NUM);
    uint16_t p1;
    if (isInt) {
        p1 = slots[s++];                    // (i+j0-1)(i+j0) >> 1
        initSlotNode(buf, p1, IR_ASHR, t2, one, IR_TYPE_INT);
    } else {
        uint16_t p2 = slots[s++];
        initSlotNode(buf, p2, IR_MUL, t2, half, IR_TYPE_NUM);
        p1 = slots[s++];
        initSlotNode(buf, p1, IR_FLOOR, p2, IR_NONE, IR_TYPE_NUM);
    }
    // A: D0 = floor(...) + i + 1.  B: D0 = floor(...) + j0 (j0 = d(j0-1) step).
    uint16_t gi = slots[s++];               // floor(...) + (i | j0)
    initSlotNode(buf, gi, IR_ADD, p1, plusJ ? j0 : i,
                 isInt ? IR_TYPE_INT : IR_TYPE_NUM);
    uint16_t d0 = gi;
    if (!plusJ) {
        d0 = slots[s++];                    // + 1
        initSlotNode(buf, d0, IR_ADD, gi, one, isInt ? IR_TYPE_INT : IR_TYPE_NUM);
    }
    uint16_t dphi = slots[s++];
    initSlotNode(buf, dphi, IR_PHI, d0, d, isInt ? IR_TYPE_INT : IR_TYPE_NUM);

    // 9. Repurpose d as d = dphi + step (in-place, coalesced at codegen).
    //    A: step = ij.  B: step = b1 = ij + 1.
    IRNode* dn = &buf->nodes[d];
    dn->op = IR_ADD;
    dn->op1 = dphi;
    dn->op2 = plusJ ? b1 : ij;
    dn->type = isInt ? IR_TYPE_INT : IR_TYPE_NUM;
    memset(&dn->imm, 0, sizeof(dn->imm));
    dn->flags = 0;

    // 10. Snapshots referencing a killed chain node or box now materialize the
    //     final denominator (those stubs have no live guard targets, but the
    //     codegen still emits them and must see a valid SSA value).
    for (uint16_t e = 0; e < buf->snapshot_entry_count; e++) {
        uint16_t ref = buf->snapshot_entries[e].ssa_ref;
        for (int k = 0; k < numKilled; k++) {
            if (ref == killed[k]) { buf->snapshot_entries[e].ssa_ref = d; break; }
        }
        if (buf->snapshot_entries[e].ssa_ref == d) continue;
        for (int k = 0; k < numBoxes; k++) {
            if (ref == boxes[k]) { buf->snapshot_entries[e].ssa_ref = d; break; }
        }
    }

    // 11. Kill the old chain and its boxes.
    for (int k = 0; k < numKilled; k++) killNode(&buf->nodes[killed[k]]);
    for (int k = 0; k < numBoxes; k++) killNode(&buf->nodes[boxes[k]]);

    // 12. The range guards prove |ij|, |d|, and the j back-edge fit in int64
    //     exactly, so their per-iteration IR_FLAG_INT_GUARD checks are dead.
    //     plusJ's kept step b1 = ij+1 is covered by the same guards.
    if (isInt) {
        buf->nodes[ij].flags &= ~IR_FLAG_INT_GUARD;
        if (jBack < buf->count)
            buf->nodes[jBack].flags &= ~IR_FLAG_INT_GUARD;
        if (plusJ && b1 < buf->count)
            buf->nodes[b1].flags &= ~IR_FLAG_INT_GUARD;
    }
}

// ===========================================================================
// Pass 19: Relocate post-back-edge constants
//
// Passes that emit fresh constants (DIV->MUL reciprocal, MOD pow2 mask,
// MUL pow2 shift, IV ASHR shift) append them at the end of the buffer,
// which is past IR_LOOP_BACK. Materializing a constant there is dead code in
// the compiled loop -- LOOP_BACK jumps to the header and the fall-through
// never runs -- so the register the allocator assigned it is never loaded,
// and codegen re-materializes it on every use inside the loop. Constants are
// loop-invariant, so relocating one into a pre-header NOP slot materializes
// it once before the loop and lets the value live in a register throughout.
// ===========================================================================
void irOptRelocatePostBackConstants(IRBuffer* buf)
{
    uint16_t header = findLoopHeader(buf);
    uint16_t back = findLoopBack(buf);
    if (header == IR_NONE || back == IR_NONE) return;

    for (uint16_t i = (uint16_t)(back + 1); i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if (!isConst(n->op)) continue;

        // Only relocate when there is a live use at an executed position
        // (pre-header or loop body). A post-back-only use is dead code.
        bool usedExecuted = false;
        for (uint16_t u = 0; u < back; u++) {
            IRNode* un = &buf->nodes[u];
            if (un->flags & IR_FLAG_DEAD) continue;
            if (un->op1 == i || un->op2 == i) { usedExecuted = true; break; }
        }
        if (!usedExecuted) continue;

        // Reuse an empty pre-header NOP slot (same trick as LICM hoisting).
        for (uint16_t j = 0; j < header; j++) {
            if (buf->nodes[j].op != IR_NOP) continue;
            buf->nodes[j] = *n;
            buf->nodes[j].id = j;
            buf->nodes[j].flags |= IR_FLAG_INVARIANT | IR_FLAG_HOISTED;
            replaceUses(buf, i, j);
            killNode(n);
            break;
        }
    }
}

static void debugSnap0(const char* label, IRBuffer* buf)
{
    if (getenv("WREN_JIT_DUMP_IR") == NULL) return;
    if (buf->snapshot_count == 0) return;
    if (getenv("WREN_JIT_DUMP_PER_PASS")) {
        fprintf(stderr, "==== AFTER %-14s ====\n", label);
        irBufferDump(buf);
    }
    fprintf(stderr, "[BISECT] %-22s snap0 pc=%04x depth=%d\n", label,
            (unsigned)((uintptr_t)buf->snapshots[0].resume_pc & 0xfff),
            buf->snapshots[0].stack_depth);
}

void irOptimize(IRBuffer* buf)
{
    if (buf == NULL || buf->count == 0) return;

    debugSnap0("init", buf);
    if (getenv("WREN_JIT_DUMP_RAW")) { fprintf(stderr, "---- RAW TRACE ----\n"); irBufferDump(buf); }
    irOptPromoteLoopVars(buf);     // 0. Promote loop vars to register PHIs
    debugSnap0("promote_vars", buf);
    irOptPromoteNumericStackPhis(buf);// 0b. Keep numeric stack iterators raw
    debugSnap0("promote_numeric", buf);
    irOptBoxUnboxElim(buf);        // 1. Reduce box/unbox noise
    debugSnap0("boxunbox", buf);
    irOptRedundantGuardElim(buf);  // 2. Eliminate duplicate guards
    debugSnap0("redguard", buf);
    irOptConstPropFold(buf);       // 3. Fold constants, algebraic identities
    debugSnap0("constfold", buf);
    irOptGVN(buf);                 // 4. CSE / value numbering
    debugSnap0("gvn", buf);
    irOptLICM(buf);                // 5. Hoist loop-invariant computations
    debugSnap0("licm", buf);
    irOptGuardHoist(buf);          // 6. Hoist guards before loop
    debugSnap0("guardhoist", buf);
    irOptHoistGuardedRangeLoads(buf);// 6b. Hoist immutable Range shape reads
    debugSnap0("hoistrangeloads", buf);
    irOptLICM(buf);                // 6c. Hoist exposed shape comparisons
    debugSnap0("licm2", buf);
    irOptGuardHoist(buf);          // 6d. Hoist their guards
    debugSnap0("guardhoist2", buf);
    irOptStrengthReduce(buf);      // 7. Cheaper ops (MUL->ADD, DIV->MUL)
    debugSnap0("strength", buf);
    irOptBoundsCheckElim(buf);     // 8. Eliminate redundant bounds checks
    debugSnap0("bounds", buf);
    irOptEscapeAnalysis(buf);      // 9. Scalar replacement + store-load fwd
    debugSnap0("escape", buf);
    irOptDCE(buf);                 // 10. Sweep dead code
    debugSnap0("dce", buf);
    irOptGuardElim(buf);           // 11. Prove-and-delete loop-invariant guards
    debugSnap0("guardelim", buf);
    if (getenv("WREN_JIT_NO_IV") == NULL)
        irOptIVTypeInference(buf); // 12. Integer induction variable promotion
    debugSnap0("iv", buf);
    irOptRelocatePostBackConstants(buf); // 12b. Hoist constants IV emitted
    debugSnap0("relocate_iv", buf);
    irOptDCE(buf);                 // 13. Re-sweep after new eliminations
    debugSnap0("dce2", buf);
    if (getenv("WREN_JIT_NO_NESTEDPHI") == NULL)
        irOptPromoteNestedBoxedPhis(buf); // 13.5. Unbox nested-loop PHIs
    debugSnap0("nestedphi", buf);
    if (getenv("WREN_JIT_NO_LOOPFWD") == NULL)
        irOptForwardLoopExitLoads(buf);  // 13.6. Forward post-loop slot loads
    debugSnap0("loopfwd", buf);
    irOptDCE(buf);                       // 13.7. Sweep forwarded-away nodes
    debugSnap0("dce3", buf);
    if (getenv("WREN_JIT_NO_FUSE") == NULL)
        irOptFuseComparisonGuards(buf);// 14. Compare directly to side exits
    debugSnap0("fuse", buf);
    irOptFastForwardToggleCounter(buf);// 15. Closed-form alternating counter
    debugSnap0("toggle", buf);
    irOptFastForwardRangeSum(buf);// 16. Closed-form exact integer range sum
    debugSnap0("rangesum", buf);
    irOptHoistListBounds(buf);     // 17. Hoist list bounds out of counted loops
    debugSnap0("listbounds", buf);
    if (getenv("WREN_JIT_NO_DENOM") == NULL)
        irOptDenomRecurrence(buf); // 18. Fast-forward the denom recurrence
    debugSnap0("denom", buf);
    irOptRelocatePostBackConstants(buf); // 19. Move post-back constants pre-loop
    debugSnap0("relocate", buf);
}
