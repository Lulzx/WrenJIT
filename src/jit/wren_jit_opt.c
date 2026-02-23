#include "wren_jit_opt.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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
    return op == IR_GUARD_NUM    || op == IR_GUARD_CLASS ||
           op == IR_GUARD_TRUE   || op == IR_GUARD_FALSE ||
           op == IR_GUARD_NOT_NULL;
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
                if (bitTest(guardedNum, val)) {
                    killNode(n);
                } else {
                    bitSet(guardedNum, val);
                }
                break;

            case IR_GUARD_TRUE:
                if (bitTest(guardedTrue, val)) {
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

            bool invariant = true;

            if (n->op1 != IR_NONE && n->op1 < buf->count) {
                if (n->op1 >= header) {
                    IRNode* o = &buf->nodes[n->op1];
                    if (!(o->flags & IR_FLAG_INVARIANT) && !isConst(o->op))
                        invariant = false;
                }
            }
            if (n->op2 != IR_NONE && n->op2 < buf->count) {
                if (n->op2 >= header) {
                    IRNode* o = &buf->nodes[n->op2];
                    if (!(o->flags & IR_FLAG_INVARIANT) && !isConst(o->op))
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

        // The guard's operand must be defined before the loop.
        if (n->op1 >= header) continue;

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
// Master optimization pipeline
// ===========================================================================
void irOptimize(IRBuffer* buf)
{
    if (buf == NULL || buf->count == 0) return;

    irOptBoxUnboxElim(buf);        // 1. Reduce box/unbox noise
    irOptRedundantGuardElim(buf);  // 2. Eliminate duplicate guards
    irOptConstPropFold(buf);       // 3. Fold constants, algebraic identities
    irOptGVN(buf);                 // 4. CSE / value numbering
    irOptLICM(buf);                // 5. Hoist loop-invariant computations
    irOptGuardHoist(buf);          // 6. Hoist guards before loop
    irOptStrengthReduce(buf);      // 7. Cheaper ops (MUL->ADD, DIV->MUL)
    irOptBoundsCheckElim(buf);     // 8. Eliminate redundant bounds checks
    irOptEscapeAnalysis(buf);      // 9. Scalar replacement + store-load fwd
    irOptDCE(buf);                 // 10. Sweep dead code
    irOptGuardElim(buf);           // 11. Prove-and-delete loop-invariant guards
    irOptIVTypeInference(buf);     // 12. Integer induction variable promotion
    irOptDCE(buf);                 // 13. Re-sweep after new eliminations
}
