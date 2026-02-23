// =============================================================================
// wren_jit_trace_widen.c — Monomorphic inlining for non-Num CALL_0 / CALL_1
//
// Currently supported:
//   Range.iterate(_)       — inline the iteration step as integer arithmetic
//   Range.iteratorValue(_) — trivial (return iterator as value)
// =============================================================================

#include "wren_jit_trace_widen.h"
#include "wren_jit_trace.h"
#include "wren_jit.h"
#include "wren_jit_ir.h"

// Wren VM headers
#include "wren_vm.h"
#include "wren_value.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Internal helpers (mirror static helpers from wren_jit_trace.c)
// ---------------------------------------------------------------------------

static bool widenMethodNameEquals(WrenVM* vm, int symbol, const char* name)
{
    if (symbol < 0 || symbol >= vm->methodNames.count) return false;
    ObjString* sym = vm->methodNames.data[symbol];
    if (sym == NULL) return false;
    return wrenStringEqualsCString(sym, name, strlen(name));
}

static uint16_t widenSlotGet(JitRecorder* r, int slot)
{
    if (slot >= 0 && slot < JIT_TRACE_MAX_SLOTS && r->slot_live[slot])
        return r->slot_map[slot];
    return IR_NONE;
}

static void widenSlotSet(JitRecorder* r, int slot, uint16_t ssa)
{
    if (slot < 0 || slot >= JIT_TRACE_MAX_SLOTS) return;
    r->slot_map[slot] = ssa;
    r->slot_live[slot] = true;
    if (slot + 1 > r->num_slots) r->num_slots = slot + 1;
}

static uint16_t widenEmitSnapshot(JitRecorder* r, uint8_t* ip)
{
    uint16_t snap_id = irEmitSnapshot(&r->ir, ip, r->stack_top);
    for (int i = 0; i < r->stack_top; i++) {
        if (r->slot_live[i]) {
            irSnapshotAddEntry(&r->ir, snap_id, (uint16_t)i, r->slot_map[i]);
        }
    }
    return snap_id;
}

// ---------------------------------------------------------------------------
// Range.iterate(_) inlining
//
// Semantics (from wren_core.c range_iterate):
//   if IS_NULL(arg): return from           (first iteration — never traced hot)
//   else:
//     iterator++  or iterator--           (depending on direction)
//     if out_of_range: return false        (loop done — guarded exit)
//     if exclusive and at_to: return false
//     return iterator
//
// Since we only reach here when the loop is hot (arg is always a Num), the
// IS_NULL branch is never seen during tracing.
// ---------------------------------------------------------------------------
static bool inlineRangeIterate(JitRecorder* r, WrenVM* vm,
                                Value recv_val, Value arg_val,
                                int recv_slot, int arg_slot,
                                uint16_t snap,
                                uint16_t recv_ssa, uint16_t arg_ssa)
{
    (void)vm; (void)arg_val;

    ObjRange* range = AS_RANGE(recv_val);

    // Determine iteration direction and step at trace time.
    bool ascending  = (range->from <= range->to);
    bool inclusive  = range->isInclusive;
    double step     = ascending ? 1.0 : -1.0;
    double limit    = range->to;

    // Guard: arg is Num (the iterator is always a number in a hot loop).
    irEmitGuardNum(&r->ir, arg_ssa, snap);

    // Unbox current iterator.
    uint16_t iter_fp = irEmitUnbox(&r->ir, arg_ssa);

    // Advance iterator by step (CONST_NUM is already IR_TYPE_NUM = unboxed FP).
    uint16_t step_ssa    = irEmitConst(&r->ir, step);
    uint16_t new_iter    = irEmit(&r->ir, IR_ADD, iter_fp, step_ssa, IR_TYPE_NUM);

    // Emit bound guard.
    // Ascending  + inclusive:  exit when new_iter >  to → guard new_iter <= to
    // Ascending  + exclusive:  exit when new_iter >= to → guard new_iter <  to
    // Descending + inclusive:  exit when new_iter <  to → guard new_iter >= to
    // Descending + exclusive:  exit when new_iter <= to → guard new_iter >  to
    uint16_t limit_ssa = irEmitConst(&r->ir, limit);
    IROp cmp_op = ascending ? (inclusive ? IR_LTE : IR_LT)
                            : (inclusive ? IR_GTE : IR_GT);
    uint16_t cmp_result  = irEmit(&r->ir, cmp_op, new_iter, limit_ssa, IR_TYPE_BOOL);
    uint16_t boxed_cmp   = irEmit(&r->ir, IR_BOX_BOOL, cmp_result, IR_NONE,
                                  IR_TYPE_VALUE);
    irEmitGuardTrue(&r->ir, boxed_cmp, snap);

    // Box new iterator and store as result.
    uint16_t boxed_iter = irEmitBox(&r->ir, new_iter);

    // CALL_1 stack effect: pop arg, replace receiver with result.
    r->stack_top--;
    r->slot_live[r->stack_top] = false;
    widenSlotSet(r, recv_slot, boxed_iter);

    (void)arg_slot;
    return true;
}

// ---------------------------------------------------------------------------
// Range.iteratorValue(_) inlining
//
// range_iteratorValue simply returns args[1] (the iterator IS the value).
// ---------------------------------------------------------------------------
static bool inlineRangeIteratorValue(JitRecorder* r,
                                     int recv_slot, int arg_slot,
                                     uint16_t snap,
                                     uint16_t arg_ssa)
{
    (void)arg_slot;

    // Guard: arg is Num.
    irEmitGuardNum(&r->ir, arg_ssa, snap);

    // CALL_1 stack effect: pop arg, replace receiver with arg (== value).
    r->stack_top--;
    r->slot_live[r->stack_top] = false;
    widenSlotSet(r, recv_slot, arg_ssa);

    return true;
}

// ---------------------------------------------------------------------------
// Public: jitTryWidenCall1
// ---------------------------------------------------------------------------
bool jitTryWidenCall1(WrenJitState* jit, WrenVM* vm, Value* stackStart,
                      uint16_t symbol, uint8_t* ip)
{
    JitRecorder* r = jitRecorderGet(jit);
    if (!r || r->aborted) return false;
    if (r->stack_top < 2) return false;

    int recv_slot = r->stack_top - 2;
    int arg_slot  = r->stack_top - 1;

    Value recv_val = stackStart[recv_slot];
    Value arg_val  = stackStart[arg_slot];

    // ------------------------------------------------------------------
    // Range methods
    // ------------------------------------------------------------------
    if (IS_RANGE(recv_val)) {
        bool is_iterate = widenMethodNameEquals(vm, symbol, "iterate(_)");
        bool is_iterval = widenMethodNameEquals(vm, symbol, "iteratorValue(_)");

        if (!is_iterate && !is_iterval) return false;

        uint16_t snap = widenEmitSnapshot(r, ip);

        // Get or load receiver SSA.
        uint16_t recv_ssa = widenSlotGet(r, recv_slot);
        if (recv_ssa == IR_NONE) {
            recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
            widenSlotSet(r, recv_slot, recv_ssa);
        }

        // Get or load arg SSA.
        uint16_t arg_ssa = widenSlotGet(r, arg_slot);
        if (arg_ssa == IR_NONE) {
            arg_ssa = irEmitLoad(&r->ir, (uint16_t)arg_slot);
            widenSlotSet(r, arg_slot, arg_ssa);
        }

        // Guard: receiver's class == vm->rangeClass.
        irEmitGuardClass(&r->ir, recv_ssa, vm->rangeClass, snap);

        if (is_iterate) {
            return inlineRangeIterate(r, vm, recv_val, arg_val,
                                      recv_slot, arg_slot,
                                      snap, recv_ssa, arg_ssa);
        }
        // is_iterval
        return inlineRangeIteratorValue(r, recv_slot, arg_slot, snap, arg_ssa);
    }

    return false; // unsupported receiver type
}

// ---------------------------------------------------------------------------
// Public: jitTryWidenCall0
// ---------------------------------------------------------------------------
bool jitTryWidenCall0(WrenJitState* jit, WrenVM* vm, Value* stackStart,
                      uint16_t symbol, uint8_t* ip)
{
    (void)jit; (void)vm; (void)stackStart; (void)symbol; (void)ip;
    return false;
}
