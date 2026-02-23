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

            // Pre-loop value must be an integer constant.
            if (!isIntegerConstNum(preNode) &&
                preNode->type != IR_TYPE_INT) continue;

            // Back-edge must be ADD or SUB of (phi, const) or (const, phi).
            bool backIsIV = false;
            if (backNode->op == IR_ADD || backNode->op == IR_SUB) {
                uint16_t b1 = backNode->op1;
                uint16_t b2 = backNode->op2;
                if (b1 == IR_NONE || b2 == IR_NONE) continue;
                if (b1 >= buf->count || b2 >= buf->count) continue;

                bool phiIsLHS = (b1 == i) ||
                                (buf->nodes[b1].op == IR_PHI && b1 == i);
                bool phiIsRHS = (b2 == i) ||
                                (buf->nodes[b2].op == IR_PHI && b2 == i);
                const IRNode* step = phiIsLHS ? &buf->nodes[b2]
                                              : &buf->nodes[b1];
                if ((phiIsLHS || phiIsRHS) && isIntegerConstNum(step)) {
                    backIsIV = true;
                }
            }

            if (backIsIV) {
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
                    if (n->op1 != IR_NONE && n->op2 != IR_NONE &&
                        isIntType(buf, n->op1) && isIntType(buf, n->op2)) {
                        n->type = IR_TYPE_INT;
                        changed = true;
                    }
                    break;
                // IR_CONST_NUM is intentionally NOT promoted here.
                // CONST_NUM codegen uses getFP (FP register) regardless of
                // type; promoting to IR_TYPE_INT would cause regalloc to
                // assign a GP register, breaking the FP codegen path.
                // When PHI-based IV promotion is fully wired, CONST_NUM will
                // need a separate IR_CONST_INT opcode instead.
                default:
                    break;
            }
        }
    }

    // --- Step 4: replace UNBOX_NUM / BOX_NUM for INT-typed sources ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;

        if (n->op == IR_UNBOX_NUM && n->op1 != IR_NONE &&
            n->op1 < buf->count && isIntType(buf, n->op1)) {
            n->op   = IR_UNBOX_INT;
            n->type = IR_TYPE_INT;
        }

        if (n->op == IR_BOX_NUM && n->op1 != IR_NONE &&
            n->op1 < buf->count && isIntType(buf, n->op1)) {
            n->op = IR_BOX_INT;
        }
    }

    // --- Step 5: mark comparisons on INT operands ---
    for (uint16_t i = 0; i < buf->count; i++) {
        IRNode* n = &buf->nodes[i];
        if (n->flags & IR_FLAG_DEAD) continue;

        switch (n->op) {
            case IR_LT:
            case IR_GT:
            case IR_LTE:
            case IR_GTE:
            case IR_EQ:
            case IR_NEQ:
                if (n->op1 != IR_NONE && n->op2 != IR_NONE &&
                    isIntType(buf, n->op1) && isIntType(buf, n->op2)) {
                    n->type = IR_TYPE_INT; // signal integer comparison to codegen
                }
                break;
            default:
                break;
        }
    }
}
