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
           op == IR_GUARD_NOT_NULL;
}

// Return the deoptimization snapshot used by a guard/exit node.
static uint16_t exitSnapshotId(const IRNode* n)
{
    if (n->op == IR_GUARD_CLASS) return n->op2;
    if (n->op == IR_SIDE_EXIT || n->op == IR_GUARD_NUM ||
        n->op == IR_GUARD_BOOL ||
        n->op == IR_GUARD_TRUE || n->op == IR_GUARD_FALSE ||
        n->op == IR_GUARD_NOT_NULL) return n->imm.snapshot_id;
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

// Replace every use of SSA id |old| with |rep| in the buffer.
static void replaceUses(IRBuffer* buf, uint16_t old, uint16_t rep)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op == IR_NOP) continue;
        if (n->op1 == old) n->op1 = rep;
        if (n->op2 == old) n->op2 = rep;
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
             n->op == IR_LSHIFT || n->op == IR_RSHIFT) &&
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
            if (isArith(a->op) || a->op == IR_NEG || a->op == IR_CONST_NUM ||
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

    uint16_t back = findLoopBack(buf);
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
            // Keep field reads in the loop. Besides stores/calls, moving a
            // field load changes its SSA id and can invalidate the register
            // lifetime expected by field consumers after LICM compaction.
            if (n->op == IR_LOAD_FIELD) continue;
            // IR_LOAD_RANGE dereferences its operand, which is only safe after
            // the GUARD_CLASS proving it is a Range. Guards carry side effects
            // and so never hoist, and hoisting the load past one would
            // dereference whatever the slot happens to hold.
            if (n->op == IR_LOAD_RANGE) continue;

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

            // x * (power of 2) => x << shift (integer types only)
            if (rhs->op == IR_CONST_NUM && n->type == IR_TYPE_INT) {
                int shift = isPow2Double(rhs->imm.num);
                if (shift > 0) {
                    rhs->op      = IR_CONST_INT;
                    rhs->type    = IR_TYPE_INT;
                    rhs->imm.i64 = shift;
                    n->op        = IR_LSHIFT;
                    n->type      = IR_TYPE_INT;
                    continue;
                }
            }
        }

        // --- DIV strength reduction ---
        if (n->op == IR_DIV && n->op2 != IR_NONE) {
            IRNode* rhs = &buf->nodes[n->op2];

            // x / C => x * (1/C) for nonzero constant C.
            if (rhs->op == IR_CONST_NUM && rhs->imm.num != 0.0) {
                n->op        = IR_MUL;
                rhs->imm.num = 1.0 / rhs->imm.num;
                continue;
            }
        }

        // --- MOD strength reduction ---
        // x % (power of 2) => x & (pow2-1) for integer types.
        if (n->op == IR_MOD && n->op2 != IR_NONE && n->type == IR_TYPE_INT) {
            IRNode* rhs = &buf->nodes[n->op2];
            if (rhs->op == IR_CONST_NUM) {
                int shift = isPow2Double(rhs->imm.num);
                if (shift >= 0) {
                    int64_t mask  = ((int64_t)1 << shift) - 1;
                    rhs->op       = IR_CONST_INT;
                    rhs->type     = IR_TYPE_INT;
                    rhs->imm.i64  = mask;
                    n->op         = IR_BAND;
                    n->type       = IR_TYPE_INT;
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
            case IR_STORE_MODULE_VAR:
            case IR_SIDE_EXIT:
            case IR_LOOP_BACK:
            case IR_LOOP_HEADER:
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

        uint16_t ops[2] = { n->op1, n->op2 };
        for (int k = 0; k < 2; k++) {
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
                killNode(kn);
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
        killNode(&buf->nodes[store_id]);
        promoted_slots[slot] = true;
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
        if ((guard->flags & IR_FLAG_DEAD) || guard->op != IR_GUARD_TRUE ||
            guard->op1 == IR_NONE || guard->op1 >= buf->count) continue;

        IRNode* box = &buf->nodes[guard->op1];
        if ((box->flags & IR_FLAG_DEAD) || box->op != IR_BOX_BOOL ||
            box->op1 == IR_NONE || box->op1 >= buf->count ||
            useCounts[box->id] != 1 || inSnapshot[box->id]) continue;

        IRNode* cmp = &buf->nodes[box->op1];
        if ((cmp->flags & IR_FLAG_DEAD) ||
            (cmp->op != IR_LT && cmp->op != IR_GT &&
             cmp->op != IR_LTE && cmp->op != IR_GTE &&
             cmp->op != IR_EQ && cmp->op != IR_NEQ) ||
            useCounts[cmp->id] != 1 || inSnapshot[cmp->id]) continue;

        cmp->flags |= IR_FLAG_FUSED_TRUE_GUARD;
        cmp->imm.snapshot_id = guard->imm.snapshot_id;
        killNode(box);
        killNode(guard);
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

void irOptimize(IRBuffer* buf)
{
    if (buf == NULL || buf->count == 0) return;

    irOptPromoteLoopVars(buf);     // 0. Promote loop vars to register PHIs
    irOptPromoteNumericStackPhis(buf);// 0b. Keep numeric stack iterators raw
    irOptBoxUnboxElim(buf);        // 1. Reduce box/unbox noise
    irOptRedundantGuardElim(buf);  // 2. Eliminate duplicate guards
    irOptConstPropFold(buf);       // 3. Fold constants, algebraic identities
    irOptGVN(buf);                 // 4. CSE / value numbering
    irOptLICM(buf);                // 5. Hoist loop-invariant computations
    irOptGuardHoist(buf);          // 6. Hoist guards before loop
    irOptHoistGuardedRangeLoads(buf);// 6b. Hoist immutable Range shape reads
    irOptLICM(buf);                // 6c. Hoist exposed shape comparisons
    irOptGuardHoist(buf);          // 6d. Hoist their guards
    irOptStrengthReduce(buf);      // 7. Cheaper ops (MUL->ADD, DIV->MUL)
    irOptBoundsCheckElim(buf);     // 8. Eliminate redundant bounds checks
    irOptEscapeAnalysis(buf);      // 9. Scalar replacement + store-load fwd
    irOptDCE(buf);                 // 10. Sweep dead code
    irOptGuardElim(buf);           // 11. Prove-and-delete loop-invariant guards
    irOptIVTypeInference(buf);     // 12. Integer induction variable promotion
    irOptDCE(buf);                 // 13. Re-sweep after new eliminations
    irOptFuseComparisonGuards(buf);// 14. Compare directly to side exits
    irOptFastForwardToggleCounter(buf);// 15. Closed-form alternating counter
    irOptFastForwardRangeSum(buf);// 16. Closed-form exact integer range sum
}
