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

    // Direction and inclusivity select which instructions this trace emits, so
    // they have to be fixed at record time. The range's bounds do not: baking
    // them would be wrong, because one loop PC is reached with many different
    // ranges and the class guard only proves the receiver is *a* Range. So the
    // limit is read from the range at run time, and the two structural facts
    // get guards that side-exit when a differently shaped range shows up.
    bool ascending  = (range->from <= range->to);
    bool inclusive  = range->isInclusive;
    double step     = ascending ? 1.0 : -1.0;

    // Guard: arg is Num (the iterator is always a number in a hot loop).
    irEmitGuardNum(&r->ir, arg_ssa, snap);

    uint16_t from_ssa = irEmitLoadRange(&r->ir, recv_ssa, IR_RANGE_FROM);
    uint16_t to_ssa   = irEmitLoadRange(&r->ir, recv_ssa, IR_RANGE_TO);
    uint16_t incl_ssa = irEmitLoadRange(&r->ir, recv_ssa, IR_RANGE_INCLUSIVE);

    // Guard direction: (from <= to) must still match what was recorded,
    // otherwise the baked step would run away from the limit and never exit.
    uint16_t dir_cmp = irEmit(&r->ir, IR_LTE, from_ssa, to_ssa, IR_TYPE_BOOL);
    uint16_t dir_boxed = irEmit(&r->ir, IR_BOX_BOOL, dir_cmp, IR_NONE,
                                IR_TYPE_VALUE);
    if (ascending) {
        irEmitGuardTrue(&r->ir, dir_boxed, snap);
    } else {
        irEmitGuardFalse(&r->ir, dir_boxed, snap);
    }

    // Guard inclusivity: it picks < versus <=, so an exclusive range running a
    // trace recorded as inclusive would overshoot by one element.
    uint16_t incl_expected = irEmitConst(&r->ir, inclusive ? 1.0 : 0.0);
    uint16_t incl_cmp = irEmit(&r->ir, IR_EQ, incl_ssa, incl_expected,
                               IR_TYPE_BOOL);
    uint16_t incl_boxed = irEmit(&r->ir, IR_BOX_BOOL, incl_cmp, IR_NONE,
                                 IR_TYPE_VALUE);
    irEmitGuardTrue(&r->ir, incl_boxed, snap);

    // Unbox current iterator.
    uint16_t iter_fp = irEmitUnbox(&r->ir, arg_ssa);

    // Advance iterator by step (CONST_NUM is already IR_TYPE_NUM = unboxed FP).
    uint16_t step_ssa    = irEmitConst(&r->ir, step);
    uint16_t new_iter    = irEmit(&r->ir, IR_ADD, iter_fp, step_ssa, IR_TYPE_NUM);

    // Emit bound guard against the range's actual `to`.
    // Ascending  + inclusive:  exit when new_iter >  to → guard new_iter <= to
    // Ascending  + exclusive:  exit when new_iter >= to → guard new_iter <  to
    // Descending + inclusive:  exit when new_iter <  to → guard new_iter >= to
    // Descending + exclusive:  exit when new_iter <= to → guard new_iter >  to
    IROp cmp_op = ascending ? (inclusive ? IR_LTE : IR_LT)
                            : (inclusive ? IR_GTE : IR_GT);
    uint16_t cmp_result  = irEmit(&r->ir, cmp_op, new_iter, to_ssa, IR_TYPE_BOOL);
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

    // ------------------------------------------------------------------
    // List subscript getter
    // ------------------------------------------------------------------
    if (IS_LIST(recv_val) && IS_NUM(arg_val) &&
        widenMethodNameEquals(vm, symbol, "[_]")) {
        uint16_t snap = widenEmitSnapshot(r, ip);
        uint16_t recv_ssa = widenSlotGet(r, recv_slot);
        uint16_t arg_ssa = widenSlotGet(r, arg_slot);
        if (recv_ssa == IR_NONE) {
            recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
            widenSlotSet(r, recv_slot, recv_ssa);
        }
        if (arg_ssa == IR_NONE) {
            arg_ssa = irEmitLoad(&r->ir, (uint16_t)arg_slot);
            widenSlotSet(r, arg_slot, arg_ssa);
        }
        irEmitGuardClass(&r->ir, recv_ssa, vm->listClass, snap);
        irEmitGuardNum(&r->ir, arg_ssa, snap);
        uint16_t index = irEmitUnbox(&r->ir, arg_ssa);
        uint16_t result = irEmit(&r->ir, IR_LIST_LOAD, recv_ssa, index,
                                 IR_TYPE_VALUE);
        r->ir.nodes[result].imm.list.snapshot = snap;
        r->stack_top--;
        r->slot_live[r->stack_top] = false;
        widenSlotSet(r, recv_slot, result);
        return true;
    }

    return false; // unsupported receiver type
}

bool jitTryWidenCall2(WrenJitState* jit, WrenVM* vm, Value* stackStart,
                      uint16_t symbol, uint8_t* ip)
{
    JitRecorder* r = jitRecorderGet(jit);
    if (!r || r->aborted || r->stack_top < 3) return false;

    int recv_slot = r->stack_top - 3;
    int index_slot = r->stack_top - 2;
    int value_slot = r->stack_top - 1;
    Value recv_val = stackStart[recv_slot];
    Value index_val = stackStart[index_slot];
    if (!IS_LIST(recv_val) || !IS_NUM(index_val) ||
        !widenMethodNameEquals(vm, symbol, "[_]=(_)")) return false;

    uint16_t snap = widenEmitSnapshot(r, ip);
    uint16_t recv_ssa = widenSlotGet(r, recv_slot);
    uint16_t index_ssa = widenSlotGet(r, index_slot);
    uint16_t value_ssa = widenSlotGet(r, value_slot);
    if (recv_ssa == IR_NONE) recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
    if (index_ssa == IR_NONE) index_ssa = irEmitLoad(&r->ir, (uint16_t)index_slot);
    if (value_ssa == IR_NONE) value_ssa = irEmitLoad(&r->ir, (uint16_t)value_slot);
    irEmitGuardClass(&r->ir, recv_ssa, vm->listClass, snap);
    irEmitGuardNum(&r->ir, index_ssa, snap);
    uint16_t index = irEmitUnbox(&r->ir, index_ssa);
    uint16_t store = irEmit(&r->ir, IR_LIST_STORE, recv_ssa, index,
                            IR_TYPE_VOID);
    r->ir.nodes[store].imm.list.value = value_ssa;
    r->ir.nodes[store].imm.list.snapshot = snap;

    // Setter returns the assigned value and consumes receiver/index.
    r->stack_top -= 2;
    r->slot_live[r->stack_top] = false;
    r->slot_live[r->stack_top + 1] = false;
    widenSlotSet(r, recv_slot, value_ssa);
    return true;
}

// ---------------------------------------------------------------------------
// Public: jitTryWidenCall0
// ---------------------------------------------------------------------------
bool jitTryWidenCall0(WrenJitState* jit, WrenVM* vm, Value* stackStart,
                      uint16_t symbol, uint8_t* ip)
{
    JitRecorder* r = jitRecorderGet(jit);
    if (!r || r->aborted || r->stack_top < 1) return false;

    int recv_slot = r->stack_top - 1;
    Value recv_val = stackStart[recv_slot];

    if (IS_LIST(recv_val) && widenMethodNameEquals(vm, symbol, "count")) {
        uint16_t snap = widenEmitSnapshot(r, ip);
        uint16_t recv_ssa = widenSlotGet(r, recv_slot);
        if (recv_ssa == IR_NONE) {
            recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
            widenSlotSet(r, recv_slot, recv_ssa);
        }
        irEmitGuardClass(&r->ir, recv_ssa, vm->listClass, snap);
        uint16_t count = irEmit(&r->ir, IR_LIST_COUNT, recv_ssa, IR_NONE,
                                IR_TYPE_NUM);
        widenSlotSet(r, recv_slot, irEmitBox(&r->ir, count));
        return true;
    }

    if (!IS_INSTANCE(recv_val)) return false;

    ObjInstance* instance = AS_INSTANCE(recv_val);
    ObjClass* classObj = instance->obj.classObj;
    if (symbol >= (uint16_t)classObj->methods.count) return false;
    Method* method = &classObj->methods.data[symbol];
    if (method->type != METHOD_BLOCK || method->as.closure == NULL) return false;

    ObjFn* fn = method->as.closure->fn;
    if (fn == NULL || fn->numUpvalues != 0) return false;
    uint8_t* code = fn->code.data;
    int count = fn->code.count;

    // Admit only compiler-canonical, straight-line methods. Besides making
    // this deliberately small, exact matching ensures no callee side effect
    // is lost when its interpreter bytecodes are suppressed below.
    bool getter = count == 4 &&
                  code[0] == CODE_LOAD_FIELD_THIS &&
                  code[2] == CODE_RETURN && code[3] == CODE_END;
    bool toggler = count == 9 &&
                   code[0] == CODE_LOAD_FIELD_THIS &&
                   code[2] == CODE_CALL_0 &&
                   code[5] == CODE_STORE_FIELD_THIS &&
                   code[7] == CODE_RETURN && code[8] == CODE_END &&
                   code[1] == code[6] &&
                   widenMethodNameEquals(vm,
                       (int)(((uint16_t)code[3] << 8) | code[4]), "!");
    // `_field = !_field; return this` has the same side effect followed by
    // POP, LOAD_LOCAL_0, RETURN and the compiler's NULL/RETURN epilogue.
    bool toggler_returns_this = count == 13 &&
                   code[0] == CODE_LOAD_FIELD_THIS &&
                   code[2] == CODE_CALL_0 &&
                   code[5] == CODE_STORE_FIELD_THIS &&
                   code[7] == CODE_POP && code[8] == CODE_LOAD_LOCAL_0 &&
                   code[9] == CODE_RETURN && code[10] == CODE_NULL &&
                   code[11] == CODE_RETURN && code[12] == CODE_END &&
                   code[1] == code[6] &&
                   widenMethodNameEquals(vm,
                       (int)(((uint16_t)code[3] << 8) | code[4]), "!");
    if (!getter && !toggler && !toggler_returns_this) return false;

    uint16_t field = code[1];
    if (field >= (uint16_t)classObj->numFields) return false;

    uint16_t snap = widenEmitSnapshot(r, ip);
    uint16_t recv_ssa = widenSlotGet(r, recv_slot);
    if (recv_ssa == IR_NONE) {
        recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
        widenSlotSet(r, recv_slot, recv_ssa);
    }
    irEmitGuardClass(&r->ir, recv_ssa, classObj, snap);
    uint16_t obj = recv_ssa;
    uint16_t value = irEmitLoadField(&r->ir, obj, field);

    if (toggler || toggler_returns_this) {
        // `!` accepts every Wren value, but XOR only models its boolean case.
        // A type guard preserves general semantics by side-exiting for a field
        // that has changed to null, a number, or an object since recording.
        irEmitGuardBool(&r->ir, value, snap);
        value = irEmit(&r->ir, IR_BOOL_NOT, value, IR_NONE, IR_TYPE_VALUE);
        irEmitStoreField(&r->ir, obj, field, value);
    }

    widenSlotSet(r, recv_slot, toggler_returns_this ? recv_ssa : value);

    // The real interpreter still enters this METHOD_BLOCK. Its result and
    // side effect agree with the IR above, but recording those callee bytecodes
    // would corrupt the caller-relative slot map. jitRecorderStep skips hooks
    // until this frame depth is restored.
    r->suppressed_frame_depth = vm->fiber->numFrames;
    return true;
}
