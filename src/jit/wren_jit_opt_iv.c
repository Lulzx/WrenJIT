// ===========================================================================
// Pass 12: Induction Variable Type Inference (~350 LOC)
//
// Detects integer induction variables (loop counters that increment by a
// constant integer each iteration) and marks them IR_TYPE_INT so that the
// code generator can emit native integer arithmetic instead of the slower
// FP box/unbox pipeline.
//
// Algorithm:
//   1. Find IR_PHI nodes where:
//        op1 (pre-loop value)   is CONST_NUM with an integer value
//        op2 (back-edge value)  is IR_ADD/IR_SUB with one operand being the
//                               PHI itself and the other an integer CONST_NUM
//   2. Tag those PHIs IR_TYPE_INT.
//   3. Propagate forward: IR_ADD / IR_SUB / IR_MUL with both operands
//      IR_TYPE_INT produce an IR_TYPE_INT result.
//   4. Replace type-conversion ops:
//        IR_UNBOX_NUM whose source is IR_TYPE_INT  ->  IR_UNBOX_INT
//        IR_BOX_NUM   whose source is IR_TYPE_INT  ->  IR_BOX_INT
//   5. Mark comparisons (IR_LT etc.) on two IR_TYPE_INT operands as
//      IR_TYPE_INT so the codegen selects the integer compare path.
// ===========================================================================

#include "wren_jit_ir.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isIntegerConstNum(const IRNode* n)
{
    if (!n) return false;
    if (n->op != IR_CONST_NUM) return false;
    double v = n->imm.num;
    return (v == (double)(int64_t)v) &&
           (v >= -((double)(1LL << 52))) &&
           (v <=  ((double)(1LL << 52)));
}

static bool isIntType(const IRBuffer* buf, uint16_t id)
{
    if (id == IR_NONE || id >= buf->count) return false;
    return buf->nodes[id].type == IR_TYPE_INT;
}

// A null-select deliberately evaluates its numeric arm speculatively. Its
// boxed input may be null, so converting the associated UNBOX_NUM to the
// guarded UNBOX_INT form would deopt on the very value the select handles.
static bool isNullableSelectInput(const IRBuffer* buf, uint16_t boxed)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;
        if ((n->op == IR_SELECT_NULL || n->op == IR_GUARD_NUM_OR_NULL) &&
            n->op1 == boxed)
            return true;
    }
    return false;
}

static bool ivIsComparison(IROp op)
{
    return op == IR_LT || op == IR_GT || op == IR_LTE ||
           op == IR_GTE || op == IR_EQ || op == IR_NEQ;
}

// Which SSA values may safely be materialized as a raw int64 (GP register)?
// The pass specializes a NUM value to INT only when every consumer can accept
// an int. Float-domain consumers — a DIV, or an ADD/SUB/MUL/MOD or comparison
// that stays in the FP domain — read their operands as doubles, so an int
// producer there would be re-read as garbage (a GP register index used as an
// FP register). Solved as a backward fixed point:
//
//   - canBeInt[N] starts true.
//   - any op whose result is never a raw int (Value/bool/void, DIV, FLOOR,
//     LOAD_RANGE, LIST_COUNT, ...) -> canBeInt[N] = false.
//   - an ADD/SUB/MUL/MOD/PHI additionally needs every operand canBeInt.
//   - a consumer that stays in the FP domain forces its numeric operands to
//     stay float. FLOOR is the exception: FLOOR(int) is the identity / a
//     shift, so it accepts an int source and never forces.
//   - a comparison forces its operands float only when it cannot be an
//     integer compare (i.e. not both operands can be int).
static void ivComputeCanBeInt(const IRBuffer* buf, bool* canBeInt)
{
    for (uint16_t i = 0; i < buf->count; i++) canBeInt[i] = true;

    bool changed;
    do {
        changed = false;

        // Forward: what each node's own result may be.
        for (uint16_t i = 0; i < buf->count; i++) {
            const IRNode* n = &buf->nodes[i];
            if (n->flags & IR_FLAG_DEAD) continue;
            if (!canBeInt[i]) continue;

            IROp op = n->op;
            bool isIntCandidate =
                (op == IR_CONST_INT) ||
                (op == IR_CONST_NUM && isIntegerConstNum(n)) ||
                (op == IR_ADD || op == IR_SUB || op == IR_MUL ||
                 op == IR_MOD || op == IR_PHI) ||
                (op == IR_UNBOX_NUM);
            if (!isIntCandidate) {
                canBeInt[i] = false; changed = true;
                continue;
            }

            if ((op == IR_ADD || op == IR_SUB || op == IR_MUL ||
                 op == IR_MOD || op == IR_PHI) &&
                ((n->op1 != IR_NONE && n->op1 < buf->count &&
                  !canBeInt[n->op1]) ||
                 (n->op2 != IR_NONE && n->op2 < buf->count &&
                  !canBeInt[n->op2]))) {
                canBeInt[i] = false; changed = true;
            }
        }

        // Backward: FP-domain consumers force their numeric operands float.
        for (uint16_t i = 0; i < buf->count; i++) {
            const IRNode* n = &buf->nodes[i];
            if (n->flags & IR_FLAG_DEAD) continue;
            IROp op = n->op;

            bool forces;
            if (ivIsComparison(op)) {
                // A comparison reads doubles unless both operands can be int.
                // An integer-capable compare lets its operands stay int; only a
                // compare that must stay in the FP domain (a float operand, or
                // a missing operand) forces them back to double.
                if (n->op1 == IR_NONE || n->op2 == IR_NONE ||
                    !canBeInt[n->op1] || !canBeInt[n->op2]) {
                    forces = true;
                } else {
                    continue;    // integer compare: operands may stay int
                }
            } else {
                if (canBeInt[i]) continue;    // int-capable consumer is fine
                if (op == IR_FLOOR) continue; // FLOOR accepts an int source
                forces = (op == IR_DIV || op == IR_ADD || op == IR_SUB ||
                          op == IR_MUL || op == IR_MOD);
                if (!forces) continue;
            }
            if (n->op1 != IR_NONE && n->op1 < buf->count &&
                canBeInt[n->op1]) { canBeInt[n->op1] = false; changed = true; }
            if (n->op2 != IR_NONE && n->op2 < buf->count &&
                canBeInt[n->op2]) { canBeInt[n->op2] = false; changed = true; }
        }
    } while (changed);
}

// Use the bytecode snapshot immediately preceding an integer operation.  For
// pre-header conversions there is no preceding snapshot, so use the first loop
// snapshot.  It contains the original boxed module values and is therefore a
// valid deoptimization state if integer specialization is not applicable.
static uint16_t snapshotForNode(const IRBuffer* buf, uint16_t id)
{
    uint16_t snap = IR_NONE;
    for (uint16_t i = 0; i < id; i++) {
        if (buf->nodes[i].op == IR_SNAPSHOT) snap = buf->nodes[i].imm.snapshot_id;
    }
    if (snap != IR_NONE) return snap;
    for (uint16_t i = id; i < buf->count; i++) {
        if (buf->nodes[i].op == IR_SNAPSHOT) return buf->nodes[i].imm.snapshot_id;
    }
    return IR_NONE;
}

// Promote a NUM value to INT when it feeds integer arithmetic alongside an
// already-INT operand. Integer-valued CONST_NUMs become CONST_INT. A
// loop-invariant UNBOX_NUM (a runtime value loaded before the loop) becomes
// UNBOX_INT carrying an INT_GUARD, so a non-integral value side-exits at the
// loop header rather than being silently truncated. This is what lets a
// second loop counter or a loop limit participate in integer arithmetic.
static bool tryPromoteToInt(IRBuffer* buf, uint16_t id, uint16_t header,
                            const bool* canBeInt)
{
    if (id == IR_NONE || id >= buf->count) return false;
    IRNode* n = &buf->nodes[id];
    if (n->flags & IR_FLAG_DEAD) return false;
    // Never promote a value a float-domain consumer reads as a double.
    if (!canBeInt[id]) return false;
    if (n->type == IR_TYPE_INT) return true;
    if (n->op == IR_CONST_NUM && isIntegerConstNum(n)) {
        n->op      = IR_CONST_INT;
        n->imm.i64 = (int64_t)n->imm.num;
        n->type    = IR_TYPE_INT;
        return true;
    }
    if (n->op == IR_UNBOX_NUM) {
        if (isNullableSelectInput(buf, n->op1)) return false;
        n->op   = IR_UNBOX_INT;
        n->type = IR_TYPE_INT;
        // The INT_GUARD integrality check is emitted at the LOOP_HEADER for a
        // pre-header conversion, and inline at the conversion site for a
        // loop-body one (see codegen IR_UNBOX_INT). A non-integral runtime
        // value side-exits either way, so a loop-carried value like dna[i]
        // that feeds integer arithmetic is safe to promote.
        uint16_t snap = snapshotForNode(buf, id);
        if (snap != IR_NONE) {
            n->flags |= IR_FLAG_INT_GUARD;
            n->imm.snapshot_id = snap;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// FLOOR pattern helpers
// ---------------------------------------------------------------------------

// If v is a reciprocal of a power of two (1/2^k), return k. Else -1.
static int ivPow2Shift(double v)
{
    if (v <= 0.0 || v != v) return -1;
    double r = 1.0 / v;
    if (r > (double)(1LL << 30)) return -1;
    int64_t iv = (int64_t)r;
    if ((double)iv != r) return -1;
    if (iv == 0 || (iv & (iv - 1)) != 0) return -1;
    int exp = 0;
    while (iv > 1) { iv >>= 1; exp++; }
    return exp;
}

// Replace every use of SSA id |old| with |rep| in the buffer.
static void ivReplaceUses(IRBuffer* buf, uint16_t old, uint16_t rep)
{
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->op == IR_NOP) continue;
        if (n->op1 == old) n->op1 = rep;
        if (n->op2 == old) n->op2 = rep;
    }
    for (uint16_t i = 0; i < buf->snapshot_entry_count; i++) {
        if (buf->snapshot_entries[i].ssa_ref == old)
            buf->snapshot_entries[i].ssa_ref = rep;
    }
    for (uint16_t i = 0; i < buf->exit_module_entry_count; i++) {
        if (buf->exit_module_entries[i].ssa_ref == old)
            buf->exit_module_entries[i].ssa_ref = rep;
    }
}

// Kill a node (mark dead, turn to NOP).
static void ivKillNode(IRNode* n)
{
    n->op    = IR_NOP;
    n->op1   = IR_NONE;
    n->op2   = IR_NONE;
    memset(&n->imm, 0, sizeof(n->imm));
    n->flags |= IR_FLAG_DEAD;
}

// True if |id| is referenced by at least one snapshot entry.
static bool ivIsInSnapshot(const IRBuffer* buf, uint16_t id)
{
    for (uint16_t e = 0; e < buf->snapshot_entry_count; e++) {
        if (buf->snapshot_entries[e].ssa_ref == id) return true;
    }
    return false;
}

// True if no live node other than |except| uses |id|.
static bool ivHasNoUsesExcept(const IRBuffer* buf, uint16_t id, uint16_t except)
{
    for (uint16_t j = 0; j < buf->count; j++) {
        const IRNode* u = &buf->nodes[j];
        if (u->flags & IR_FLAG_DEAD) continue;
        if (j == except) continue;
        if (u->op1 == id || u->op2 == id) return false;
    }
    return true;
}

// FLOOR(x) where x is INT is the identity. FLOOR(x * (1/2^k)) where x is INT
// is x >> k: the floor makes the halving exact for any integer x. The MUL is
// repurposed as the shift; its snapshot-boxes then box the shifted value,
// which the interpreter's subsequent floor consumes identically.
static bool ivFloorPattern(IRBuffer* buf, uint16_t i)
{
    IRNode* floor = &buf->nodes[i];
    if (floor->op != IR_FLOOR) return false;
    if (floor->op1 == IR_NONE || floor->op1 >= buf->count) return false;
    IRNode* src = &buf->nodes[floor->op1];
    if (src->flags & IR_FLAG_DEAD) return false;

    // FLOOR(x) where x is INT -> x
    if (src->type == IR_TYPE_INT) {
        ivReplaceUses(buf, i, floor->op1);
        ivKillNode(floor);
        return true;
    }

    // FLOOR(MUL(x, 1/2^k)) where x is INT -> x >> k
    if (src->op != IR_MUL) return false;
    if (src->op1 == IR_NONE || src->op2 == IR_NONE) return false;
    if (src->op1 >= buf->count || src->op2 >= buf->count) return false;
    IRNode* a = &buf->nodes[src->op1];
    IRNode* b = &buf->nodes[src->op2];
    if (a->flags & IR_FLAG_DEAD || b->flags & IR_FLAG_DEAD) return false;

    uint16_t xId = IR_NONE, cId = IR_NONE;
    if (a->type == IR_TYPE_INT && b->op == IR_CONST_NUM) { xId = src->op1; cId = src->op2; }
    else if (b->type == IR_TYPE_INT && a->op == IR_CONST_NUM) { xId = src->op2; cId = src->op1; }
    else return false;

    int shift = ivPow2Shift(buf->nodes[cId].imm.num);
    if (shift < 0) return false;

    // Every other use of the MUL must be a snapshot-box: a BOX_NUM referenced
    // by a snapshot entry with no other uses. The FLOOR is the one real use.
    // (A snapshot-box boxes the shifted value; the interpreter's subsequent
    // floor consumes it identically. A real use would observe x*0.5, which the
    // shift does not produce for odd x.)
    for (uint16_t j = 0; j < buf->count; j++) {
        const IRNode* u = &buf->nodes[j];
        if (u->flags & IR_FLAG_DEAD) continue;
        if (j == i) continue;
        if (u->op1 != src->id && u->op2 != src->id) continue;
        if (u->op != IR_BOX_NUM) return false;
        if (!ivIsInSnapshot(buf, j)) return false;
        if (!ivHasNoUsesExcept(buf, j, j)) return false;
    }

    // Repurpose the MUL as the shift. Emit a fresh CONST_INT for the shift
    // amount rather than repurposing the reciprocal constant, which may be a
    // snapshot operand materialized as a boxed Value on exit.
    uint16_t shiftConst = irEmit(buf, IR_CONST_INT, IR_NONE, IR_NONE,
                                 IR_TYPE_INT);
    buf->nodes[shiftConst].imm.i64 = shift;
    src->op   = IR_ASHR;
    src->type = IR_TYPE_INT;
    src->op1  = xId;
    src->op2  = shiftConst;

    // floor(x * (1/2^k)) == x >> k holds for every integer x only while the
    // FP multiply is exact, i.e. |x| <= 2^53. If the INT operand is not
    // already guarded (its producer carries an INT_GUARD that exits when the
    // value exceeds 2^53), guard the shift so a huge x side-exits to the
    // interpreter rather than diverging.
    if (!(buf->nodes[xId].flags & IR_FLAG_INT_GUARD)) {
        src->flags |= IR_FLAG_INT_GUARD;
        uint16_t snap = snapshotForNode(buf, src->id);
        if (snap != IR_NONE) src->imm.snapshot_id = snap;
    }

    // The FLOOR's source is now INT, so the FLOOR is the identity.
    ivReplaceUses(buf, i, src->id);
    ivKillNode(floor);
    return true;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
void irOptIVTypeInference(IRBuffer* buf)
{
    if (!buf || buf->count == 0) return;

    uint16_t header = buf->loop_header;
    if (header >= buf->count || buf->nodes[header].op != IR_LOOP_HEADER) {
        // Scan for loop header if not recorded.
        for (uint16_t i = 0; i < buf->count; i++) {
            if (buf->nodes[i].op == IR_LOOP_HEADER) { header = i; break; }
        }
        if (header >= buf->count) return;
    }

    // A value is materialized as a raw int only if no consumer reads it as a
    // double. Compute that set once; every conversion below is gated on it.
    bool* canBeInt = (bool*)malloc(buf->count * sizeof(bool));
    if (canBeInt == NULL) return;
    ivComputeCanBeInt(buf, canBeInt);

    bool changed = true;
    int iters = 0;

    while (changed && iters++ < 8) {
        changed = false;

        // --- Step 1 & 2: find and tag PHI induction variables ---
        for (uint16_t i = 0; i < buf->count; i++) {
            IRNode* phi = &buf->nodes[i];
            if (phi->flags & IR_FLAG_DEAD) continue;
            if (phi->op != IR_PHI) continue;
            if (phi->type == IR_TYPE_INT) continue; // already tagged

            uint16_t pre = phi->op1; // pre-loop value
            uint16_t back = phi->op2; // back-edge value

            if (pre == IR_NONE || back == IR_NONE) continue;
            if (pre >= buf->count || back >= buf->count) continue;

            const IRNode* preNode = &buf->nodes[pre];
            const IRNode* backNode = &buf->nodes[back];

            // Pre-loop value must be an integer constant, INT, or NUM type.
            // We accept NUM because irOptPromoteLoopVars places UNBOX_NUM
            // (type NUM) as the initial value in the pre-header PHI.
            // A generic NUM has no proof that its runtime value is integral,
            // but Step 4b converts the UNBOX_NUM pre-value to UNBOX_INT with
            // an INT_GUARD, so a non-integral runtime value side-exits at the
            // loop header instead of silently truncating. This lets loop
            // counters that start from a stack slot (e.g. `var j = 0`) use
            // native integer arithmetic.
            if (!isIntegerConstNum(preNode) && preNode->type != IR_TYPE_INT &&
                !(preNode->op == IR_UNBOX_NUM)) continue;

            // Back-edge must be ADD or SUB of (phi, const/int) or (const/int, phi).
            bool backIsIV = false;
            if (backNode->op == IR_ADD || backNode->op == IR_SUB) {
                uint16_t b1 = backNode->op1;
                uint16_t b2 = backNode->op2;
                if (b1 == IR_NONE || b2 == IR_NONE) continue;
                if (b1 >= buf->count || b2 >= buf->count) continue;

                bool phiIsLHS = (b1 == i);
                bool phiIsRHS = (b2 == i);
                uint16_t step_id = phiIsLHS ? b2 : b1;
                const IRNode* step = &buf->nodes[step_id];

                if (phiIsLHS || phiIsRHS) {
                    // Accept constant-integer step or INT-typed step.
                    if (isIntegerConstNum(step) || isIntType(buf, step_id)) {
                        backIsIV = true;
                    }
                }
            }

            // Modulo recurrences (checksum = (checksum*k + x) mod C) carry in
            // GP registers too. The back-edge MOD is not affine, but its
            // operands are each proven integral and guarded by their producers,
            // so the recurrence stays integral across the back-edge. Tagging
            // the PHI INT lets Step 3 promote the whole chain, collapsing the
            // FP modulo's per-iteration conversion noise.
            if (!backIsIV && backNode->op == IR_MOD &&
                canBeInt[back] && canBeInt[i]) {
                backIsIV = true;
            }

            if (backIsIV && canBeInt[i]) {
                phi->type = IR_TYPE_INT;
                changed = true;
            }
        }

        // --- Step 3: propagate integer type through arithmetic ---
        for (uint16_t i = 0; i < buf->count; i++) {
            IRNode* n = &buf->nodes[i];
            if (n->flags & IR_FLAG_DEAD) continue;
            if (n->type == IR_TYPE_INT) continue;

            switch (n->op) {
                case IR_ADD:
                case IR_SUB:
                case IR_MUL:
                case IR_MOD: {
                    if (n->op1 == IR_NONE || n->op2 == IR_NONE) break;
                    IRNode* a = &buf->nodes[n->op1];
                    IRNode* b = &buf->nodes[n->op2];
                    bool aInt = isIntType(buf, n->op1) ||
                                (a->op == IR_CONST_NUM && isIntegerConstNum(a));
                    bool bInt = isIntType(buf, n->op2) ||
                                (b->op == IR_CONST_NUM && isIntegerConstNum(b));
                    // A single INT operand pulls a loop-invariant NUM operand
                    // into the integer domain (guarded at the loop header).
                    if (aInt && !bInt)
                        bInt = tryPromoteToInt(buf, n->op2, header, canBeInt);
                    if (!aInt && bInt)
                        aInt = tryPromoteToInt(buf, n->op1, header, canBeInt);
                    if (aInt && bInt && canBeInt[n->id]) {
                        // Promote integer-valued CONST_NUM operands to CONST_INT
                        // so the codegen can use the GP (integer) code path.
                        if (a->op == IR_CONST_NUM && isIntegerConstNum(a)) {
                            a->op      = IR_CONST_INT;
                            a->imm.i64 = (int64_t)a->imm.num;
                            a->type    = IR_TYPE_INT;
                        }
                        if (b->op == IR_CONST_NUM && isIntegerConstNum(b)) {
                            b->op      = IR_CONST_INT;
                            b->imm.i64 = (int64_t)b->imm.num;
                            b->type    = IR_TYPE_INT;
                        }
                        n->type = IR_TYPE_INT;
                        uint16_t snap = snapshotForNode(buf, i);
                        if (snap != IR_NONE) {
                            n->flags |= IR_FLAG_INT_GUARD;
                            n->imm.snapshot_id = snap;
                        }
                        changed = true;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // --- Step 3b: FLOOR pattern ---
        // FLOOR(x) where x is INT is the identity; FLOOR(x * (1/2^k)) where x
        // is INT is x >> k. Runs inside the loop so the propagation continues
        // through the ADD/SUB/MUL that consume the FLOOR's result.
        if (getenv("WREN_JIT_NO_FLOOR") == NULL) {
            for (uint16_t i = 0; i < buf->count; i++) {
                if (ivFloorPattern(buf, i)) changed = true;
            }
        }
    }

    // --- Step 4: replace UNBOX_NUM / BOX_NUM for INT-typed sources ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;

        if (n->op == IR_UNBOX_NUM && n->op1 != IR_NONE &&
            n->op1 < buf->count && isIntType(buf, n->op1)) {
            if (isNullableSelectInput(buf, n->op1)) continue;
            n->op   = IR_UNBOX_INT;
            n->type = IR_TYPE_INT;
        }

        if (n->op == IR_BOX_NUM && n->op1 != IR_NONE &&
            n->op1 < buf->count && isIntType(buf, n->op1)) {
            n->op = IR_BOX_INT;
        }
    }

    // --- Step 4b: backward — UNBOX_NUM that feeds op1 of INT PHI → UNBOX_INT ---
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* phi = &buf->nodes[i];
        if (phi->flags & IR_FLAG_DEAD) continue;
        if (phi->op != IR_PHI || phi->type != IR_TYPE_INT) continue;
        if (phi->op1 == IR_NONE || phi->op1 >= buf->count) continue;
        IRNode* preVal = &buf->nodes[phi->op1];
        if (!(preVal->flags & IR_FLAG_DEAD) && preVal->op == IR_UNBOX_NUM) {
            if (isNullableSelectInput(buf, preVal->op1)) continue;
            preVal->op   = IR_UNBOX_INT;
            preVal->type = IR_TYPE_INT;
            uint16_t snap = snapshotForNode(buf, phi->op1);
            if (snap != IR_NONE) {
                preVal->flags |= IR_FLAG_INT_GUARD;
                preVal->imm.snapshot_id = snap;
            }
        }
    }

    // --- Step 5: mark comparisons on INT operands ---
    // Also promote integer-valued CONST_NUM operands to CONST_INT, same as
    // step 3 does for arithmetic, so the codegen uses the integer compare path.
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;

        switch (n->op) {
            case IR_LT:
            case IR_GT:
            case IR_LTE:
            case IR_GTE:
            case IR_EQ:
            case IR_NEQ: {
                if (n->op1 == IR_NONE || n->op2 == IR_NONE) break;
                if (n->op1 >= buf->count || n->op2 >= buf->count) break;
                IRNode* a = &buf->nodes[n->op1];
                IRNode* b = &buf->nodes[n->op2];
                bool aInt = isIntType(buf, n->op1) ||
                            (a->op == IR_CONST_NUM && isIntegerConstNum(a));
                bool bInt = isIntType(buf, n->op2) ||
                            (b->op == IR_CONST_NUM && isIntegerConstNum(b));
                // A single INT operand pulls a loop-invariant NUM operand
                // (e.g. the loop limit) into the integer domain, guarded at
                // the loop header, so the comparison uses the integer path.
                if (aInt && !bInt)
                    bInt = tryPromoteToInt(buf, n->op2, header, canBeInt);
                if (!aInt && bInt)
                    aInt = tryPromoteToInt(buf, n->op1, header, canBeInt);
                // The integer compare path is only usable when neither
                // operand is read as a double anywhere else.
                if (aInt && bInt &&
                    canBeInt[n->op1] && canBeInt[n->op2]) {
                    if (a->op == IR_CONST_NUM && isIntegerConstNum(a)) {
                        a->op      = IR_CONST_INT;
                        a->imm.i64 = (int64_t)a->imm.num;
                        a->type    = IR_TYPE_INT;
                    }
                    if (b->op == IR_CONST_NUM && isIntegerConstNum(b)) {
                        b->op      = IR_CONST_INT;
                        b->imm.i64 = (int64_t)b->imm.num;
                        b->type    = IR_TYPE_INT;
                    }
                    n->type = IR_TYPE_INT; // signal integer comparison to codegen
                }
                break;
            }
            default:
                break;
        }
    }

    free(canBeInt);
}
