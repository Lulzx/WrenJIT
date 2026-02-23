#include "wren_jit_trace.h"
#include "wren_jit_trace_widen.h"
#include "wren_jit.h"

// Include Wren VM headers for access to Code enum, Value manipulation
#include "wren_vm.h"
#include "wren_value.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// -------------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------------

// Set a slot in the recorder's slot map to an SSA value.
static void slotSet(JitRecorder* r, int slot, uint16_t ssa_id)
{
    if (slot < 0 || slot >= JIT_TRACE_MAX_SLOTS) return;
    r->slot_map[slot] = ssa_id;
    r->slot_live[slot] = true;
    if (slot + 1 > r->num_slots) r->num_slots = slot + 1;
}

// Get the SSA value for a slot, or IR_NONE if not live.
static uint16_t slotGet(JitRecorder* r, int slot)
{
    if (slot >= 0 && slot < JIT_TRACE_MAX_SLOTS && r->slot_live[slot])
        return r->slot_map[slot];
    return IR_NONE;
}

// Read a two-byte big-endian operand from bytecode.
static inline uint16_t readShort(uint8_t* ip)
{
    return (uint16_t)((ip[1] << 8) | ip[2]);
}

// Emit a snapshot capturing all live slots. Returns the snapshot SSA id.
static uint16_t emitSnapshot(JitRecorder* r, uint8_t* resume_pc)
{
    uint16_t snap_id = irEmitSnapshot(&r->ir, resume_pc, r->stack_top);
    for (int i = 0; i < r->stack_top; i++) {
        if (r->slot_live[i]) {
            irSnapshotAddEntry(&r->ir, snap_id, (uint16_t)i, r->slot_map[i]);
        }
    }
    return snap_id;
}

// Check if a method symbol name matches a given C string.
// Uses the VM's global methodNames symbol table.
static bool methodNameEquals(WrenVM* vm, int symbol, const char* name)
{
    if (symbol < 0 || symbol >= vm->methodNames.count) return false;
    ObjString* sym = vm->methodNames.data[symbol];
    if (sym == NULL) return false;
    size_t len = strlen(name);
    return wrenStringEqualsCString(sym, name, len);
}

// Map a Num method symbol to an IR arithmetic/comparison opcode.
// Returns IR_NOP if unrecognised.
static IROp numMethodToIROp(WrenVM* vm, int symbol)
{
    if (methodNameEquals(vm, symbol, "+(_)"))  return IR_ADD;
    if (methodNameEquals(vm, symbol, "-(_)"))  return IR_SUB;
    if (methodNameEquals(vm, symbol, "*(_)"))  return IR_MUL;
    if (methodNameEquals(vm, symbol, "/(_)"))  return IR_DIV;
    if (methodNameEquals(vm, symbol, "%(_)"))  return IR_MOD;
    if (methodNameEquals(vm, symbol, "<(_)"))  return IR_LT;
    if (methodNameEquals(vm, symbol, ">(_)"))  return IR_GT;
    if (methodNameEquals(vm, symbol, "<=(_)")) return IR_LTE;
    if (methodNameEquals(vm, symbol, ">=(_)")) return IR_GTE;
    if (methodNameEquals(vm, symbol, "==(_)")) return IR_EQ;
    if (methodNameEquals(vm, symbol, "!=(_)")) return IR_NEQ;
    return IR_NOP;
}

// Returns true if an IR op is a comparison (result is bool, not num).
static bool isComparisonOp(IROp op)
{
    return op == IR_LT || op == IR_GT || op == IR_LTE || op == IR_GTE ||
           op == IR_EQ || op == IR_NEQ;
}

// Unary Num methods (CALL_0 on Num receiver).
static IROp numUnaryToIROp(WrenVM* vm, int symbol)
{
    if (methodNameEquals(vm, symbol, "-"))  return IR_NEG;
    return IR_NOP;
}

// -------------------------------------------------------------------------
// jitRecorderStart
// -------------------------------------------------------------------------

void jitRecorderStart(WrenJitState* jit, uint8_t* anchor_pc, int num_slots)
{
    if (jit == NULL) return;

    // Allocate recorder on first use.
    if (jit->recorder == NULL) {
        jit->recorder = calloc(1, sizeof(JitRecorder));
        if (jit->recorder == NULL) return;
    }

    JitRecorder* r = (JitRecorder*)jit->recorder;
    memset(r, 0, sizeof(JitRecorder));

    r->anchor_pc = anchor_pc;
    r->aborted = false;
    r->abort_reason = NULL;
    r->instr_count = 0;
    r->call_depth = 0;

    // Initialise the IR buffer.
    irBufferInit(&r->ir);

    // Pre-allocate NOP slots before the loop header so that
    // irOptPromoteLoopVars can fill them with LOAD+UNBOX+PHI tuples.
    for (int _k = 0; _k < JIT_PRE_HEADER_SLOTS; _k++) {
        irEmit(&r->ir, IR_NOP, IR_NONE, IR_NONE, IR_TYPE_VOID);
    }

    // Emit the loop header node.
    irEmitLoopHeader(&r->ir);

    // Pre-populate the slot map: emit IR_LOAD_STACK for each interpreter slot
    // so that values flowing into the loop have SSA names.
    if (num_slots > JIT_TRACE_MAX_SLOTS) num_slots = JIT_TRACE_MAX_SLOTS;
    r->num_slots = num_slots;
    r->stack_top = num_slots;

    for (int s = 0; s < num_slots; s++) {
        uint16_t ssa = irEmitLoad(&r->ir, (uint16_t)s);
        slotSet(r, s, ssa);
    }

    // Mark JIT state as recording.
    jit->state = JIT_STATE_RECORDING;
    jit->anchor_pc = anchor_pc;
}

// -------------------------------------------------------------------------
// jitRecorderAbort
// -------------------------------------------------------------------------

void jitRecorderAbort(WrenJitState* jit, const char* reason)
{
    if (jit == NULL || jit->recorder == NULL) return;

    fprintf(stderr, "[JIT] abort: %s\n", reason ? reason : "unknown");

    JitRecorder* r = (JitRecorder*)jit->recorder;
    r->aborted = true;
    r->abort_reason = reason;

    jit->state = JIT_STATE_IDLE;
    jit->traces_aborted++;
}

// -------------------------------------------------------------------------
// jitRecorderGet
// -------------------------------------------------------------------------

JitRecorder* jitRecorderGet(WrenJitState* jit)
{
    if (jit == NULL || jit->recorder == NULL) return NULL;
    JitRecorder* r = (JitRecorder*)jit->recorder;
    if (r->aborted) return NULL;
    return r;
}

// -------------------------------------------------------------------------
// jitRecorderStep  --  the main bytecode dispatch
// -------------------------------------------------------------------------

bool jitRecorderStep(WrenJitState* jit, WrenVM* vm, uint8_t* ip)
{
    if (jit == NULL || jit->recorder == NULL) return false;

    JitRecorder* r = (JitRecorder*)jit->recorder;
    if (r->aborted) return false;

    // Abort if we've recorded too many instructions.
    r->instr_count++;
    if (r->instr_count > JIT_TRACE_MAX_INSNS) {
        jitRecorderAbort(jit, "trace too long");
        return false;
    }

    // Current fiber and frame for inspecting runtime state.
    ObjFiber* fiber = vm->fiber;
    CallFrame* frame = &fiber->frames[fiber->numFrames - 1];
    Value* stackStart = frame->stackStart;

    Code opcode = (Code)(*ip);

    switch (opcode) {

    // -----------------------------------------------------------------
    // LOAD_LOCAL_0 .. LOAD_LOCAL_8
    // Push the value of a fixed local slot.
    // -----------------------------------------------------------------
    case CODE_LOAD_LOCAL_0:
    case CODE_LOAD_LOCAL_1:
    case CODE_LOAD_LOCAL_2:
    case CODE_LOAD_LOCAL_3:
    case CODE_LOAD_LOCAL_4:
    case CODE_LOAD_LOCAL_5:
    case CODE_LOAD_LOCAL_6:
    case CODE_LOAD_LOCAL_7:
    case CODE_LOAD_LOCAL_8: {
        int src_slot = (int)(opcode - CODE_LOAD_LOCAL_0);
        uint16_t ssa = slotGet(r, src_slot);
        if (ssa == IR_NONE) {
            ssa = irEmitLoad(&r->ir, (uint16_t)src_slot);
            slotSet(r, src_slot, ssa);
        }
        // Push onto logical stack: map stack_top to the same SSA value.
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    // -----------------------------------------------------------------
    // LOAD_LOCAL (with 1-byte arg)
    // -----------------------------------------------------------------
    case CODE_LOAD_LOCAL: {
        int src_slot = ip[1];
        uint16_t ssa = slotGet(r, src_slot);
        if (ssa == IR_NONE) {
            ssa = irEmitLoad(&r->ir, (uint16_t)src_slot);
            slotSet(r, src_slot, ssa);
        }
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    // -----------------------------------------------------------------
    // STORE_LOCAL (with 1-byte arg)
    // Store top-of-stack into a local slot. Does NOT pop.
    // -----------------------------------------------------------------
    case CODE_STORE_LOCAL: {
        int dst_slot = ip[1];
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at STORE_LOCAL");
            return false;
        }
        uint16_t ssa = slotGet(r, r->stack_top - 1);
        if (ssa == IR_NONE) {
            jitRecorderAbort(jit, "untracked value at STORE_LOCAL");
            return false;
        }
        // Write back to the interpreter stack slot so that LOOP_BACK sees
        // the updated value on re-entry at LOOP_HEADER (LOAD_STACK).
        // Phase B in irOptGuardElim will prune stores whose slot is not
        // reloaded in the loop body.
        irEmitStore(&r->ir, (uint16_t)dst_slot, ssa);
        slotSet(r, dst_slot, ssa);
        break;
    }

    // -----------------------------------------------------------------
    // LOAD_FIELD_THIS (with 1-byte field index)
    // Pushes the value of field [arg] of the receiver (slot 0).
    // -----------------------------------------------------------------
    case CODE_LOAD_FIELD_THIS: {
        int field_idx = ip[1];
        uint16_t receiver = slotGet(r, 0);
        if (receiver == IR_NONE) {
            receiver = irEmitLoad(&r->ir, 0);
            slotSet(r, 0, receiver);
        }
        uint16_t ssa = irEmitLoadField(&r->ir, receiver, (uint16_t)field_idx);
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    // -----------------------------------------------------------------
    // STORE_FIELD_THIS (with 1-byte field index)
    // Stores TOS into field [arg] of receiver (slot 0). Does NOT pop.
    // -----------------------------------------------------------------
    case CODE_STORE_FIELD_THIS: {
        int field_idx = ip[1];
        uint16_t receiver = slotGet(r, 0);
        if (receiver == IR_NONE) {
            receiver = irEmitLoad(&r->ir, 0);
            slotSet(r, 0, receiver);
        }
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at STORE_FIELD_THIS");
            return false;
        }
        uint16_t val = slotGet(r, r->stack_top - 1);
        if (val == IR_NONE) {
            jitRecorderAbort(jit, "untracked value at STORE_FIELD_THIS");
            return false;
        }
        irEmitStoreField(&r->ir, receiver, (uint16_t)field_idx, val);
        break;
    }

    // -----------------------------------------------------------------
    // CONSTANT (with 2-byte arg: index into fn constant table)
    // -----------------------------------------------------------------
    case CODE_CONSTANT: {
        uint16_t const_idx = readShort(ip);
        ObjFn* fn = frame->closure->fn;
        if (const_idx >= (uint16_t)fn->constants.count) {
            jitRecorderAbort(jit, "constant index out of range");
            return false;
        }
        Value constant = fn->constants.data[const_idx];
        uint16_t ssa;
        if (IS_NUM(constant)) {
            ssa = irEmitConst(&r->ir, AS_NUM(constant));
        } else if (IS_NULL(constant)) {
            ssa = irEmitConstNull(&r->ir);
        } else if (IS_BOOL(constant)) {
            ssa = irEmitConstBool(&r->ir, AS_BOOL(constant));
        } else {
            // Object constant -- store the pointer.
            ssa = irEmitConstObj(&r->ir, AS_OBJ(constant));
        }
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    // -----------------------------------------------------------------
    // NULL / FALSE / TRUE -- push a constant value
    // -----------------------------------------------------------------
    case CODE_NULL: {
        uint16_t ssa = irEmitConstNull(&r->ir);
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    case CODE_FALSE: {
        uint16_t ssa = irEmitConstBool(&r->ir, false);
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    case CODE_TRUE: {
        uint16_t ssa = irEmitConstBool(&r->ir, true);
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    // -----------------------------------------------------------------
    // POP -- discard top of stack
    // -----------------------------------------------------------------
    case CODE_POP: {
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at POP");
            return false;
        }
        r->stack_top--;
        // Mark the popped slot as dead (optional, for clarity).
        r->slot_live[r->stack_top] = false;
        break;
    }

    // -----------------------------------------------------------------
    // CALL_0 (unary method on receiver, 2-byte symbol)
    // -----------------------------------------------------------------
    case CODE_CALL_0: {
        uint16_t symbol = readShort(ip);
        // The receiver is at stack_top - 1.
        if (r->stack_top < 1) {
            jitRecorderAbort(jit, "stack underflow at CALL_0");
            return false;
        }
        int recv_slot = r->stack_top - 1;
        Value recv_val = stackStart[recv_slot];

        if (IS_NUM(recv_val)) {
            IROp uop = numUnaryToIROp(vm, symbol);
            if (uop == IR_NOP) {
                jitRecorderAbort(jit, "unsupported Num unary method");
                return false;
            }

            uint16_t snap = emitSnapshot(r, ip);
            uint16_t recv_ssa = slotGet(r, recv_slot);
            if (recv_ssa == IR_NONE) {
                recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
                slotSet(r, recv_slot, recv_ssa);
            }

            // Guard that receiver is Num.
            irEmitGuardNum(&r->ir, recv_ssa, snap);

            // Unbox, operate, box.
            uint16_t unboxed = irEmitUnbox(&r->ir, recv_ssa);
            uint16_t result = irEmit(&r->ir, uop, unboxed, IR_NONE, IR_TYPE_NUM);
            uint16_t boxed = irEmitBox(&r->ir, result);

            // CALL_0 has stack effect 0: receiver replaced by result.
            slotSet(r, recv_slot, boxed);
            // stack_top stays the same.
        } else {
            if (jitTryWidenCall0(jit, vm, stackStart, symbol, ip)) break;
            jitRecorderAbort(jit, "unsupported CALL_0 receiver type");
            return false;
        }
        break;
    }

    // -----------------------------------------------------------------
    // CALL_1 (binary method: receiver op arg, 2-byte symbol)
    // -----------------------------------------------------------------
    case CODE_CALL_1: {
        uint16_t symbol = readShort(ip);
        // receiver at stack_top - 2, arg at stack_top - 1.
        if (r->stack_top < 2) {
            jitRecorderAbort(jit, "stack underflow at CALL_1");
            return false;
        }
        int recv_slot = r->stack_top - 2;
        int arg_slot = r->stack_top - 1;
        Value recv_val = stackStart[recv_slot];

        if (IS_NUM(recv_val)) {
            IROp binop = numMethodToIROp(vm, symbol);
            if (binop == IR_NOP) {
                jitRecorderAbort(jit, "unsupported Num binary method");
                return false;
            }

            uint16_t snap = emitSnapshot(r, ip);
            uint16_t recv_ssa = slotGet(r, recv_slot);
            uint16_t arg_ssa = slotGet(r, arg_slot);
            if (recv_ssa == IR_NONE) {
                recv_ssa = irEmitLoad(&r->ir, (uint16_t)recv_slot);
                slotSet(r, recv_slot, recv_ssa);
            }
            if (arg_ssa == IR_NONE) {
                arg_ssa = irEmitLoad(&r->ir, (uint16_t)arg_slot);
                slotSet(r, arg_slot, arg_ssa);
            }

            // Guard both operands are Num.
            irEmitGuardNum(&r->ir, recv_ssa, snap);
            irEmitGuardNum(&r->ir, arg_ssa, snap);

            // Unbox both.
            uint16_t left = irEmitUnbox(&r->ir, recv_ssa);
            uint16_t right = irEmitUnbox(&r->ir, arg_ssa);

            // Emit the operation.
            IRType result_type = isComparisonOp(binop) ? IR_TYPE_BOOL : IR_TYPE_NUM;
            uint16_t result = irEmit(&r->ir, binop, left, right, result_type);

            // Box the result back into a Wren Value.
            uint16_t boxed;
            if (isComparisonOp(binop)) {
                // Comparison produces a native bool (0/1). Box it to a Wren
                // Value (FALSE_VAL or TRUE_VAL) so that GUARD_TRUE/GUARD_FALSE
                // can check it correctly against the Wren NaN-boxed encoding.
                boxed = irEmit(&r->ir, IR_BOX_BOOL, result, IR_NONE,
                               IR_TYPE_VALUE);
            } else {
                boxed = irEmitBox(&r->ir, result);
            }

            // CALL_1 stack effect: -1 (pops arg, replaces receiver with result).
            r->stack_top--;
            r->slot_live[r->stack_top] = false;
            slotSet(r, recv_slot, boxed);
        } else {
            if (jitTryWidenCall1(jit, vm, stackStart, symbol, ip)) break;
            jitRecorderAbort(jit, "unsupported CALL_1 receiver type");
            return false;
        }
        break;
    }

    // -----------------------------------------------------------------
    // CALL_2 .. CALL_16: abort for v0.1
    // -----------------------------------------------------------------
    case CODE_CALL_2:
    case CODE_CALL_3:
    case CODE_CALL_4:
    case CODE_CALL_5:
    case CODE_CALL_6:
    case CODE_CALL_7:
    case CODE_CALL_8:
    case CODE_CALL_9:
    case CODE_CALL_10:
    case CODE_CALL_11:
    case CODE_CALL_12:
    case CODE_CALL_13:
    case CODE_CALL_14:
    case CODE_CALL_15:
    case CODE_CALL_16: {
        jitRecorderAbort(jit, "unsupported CALL_N with N >= 2");
        return false;
    }

    // -----------------------------------------------------------------
    // JUMP (2-byte forward offset)
    // -----------------------------------------------------------------
    case CODE_JUMP: {
        // The interpreter will update ip; we just continue recording.
        // No IR emitted -- the trace follows the taken path.
        break;
    }

    // -----------------------------------------------------------------
    // JUMP_IF (2-byte forward offset)
    // Pops TOS; if falsy, jumps forward. Otherwise falls through.
    // -----------------------------------------------------------------
    case CODE_JUMP_IF: {
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at JUMP_IF");
            return false;
        }
        r->stack_top--;
        uint16_t cond_ssa = slotGet(r, r->stack_top);
        if (cond_ssa == IR_NONE) {
            cond_ssa = irEmitLoad(&r->ir, (uint16_t)r->stack_top);
        }
        r->slot_live[r->stack_top] = false;

        // The interpreter has already decided which branch to take.
        // We inspect the actual value to see which way it went.
        Value cond_val = stackStart[r->stack_top];
        bool taken = wrenIsFalsyValue(cond_val); // jump taken = value is falsy

        // Compute the not-taken path PC for the side exit snapshot.
        uint16_t offset = readShort(ip);
        // ip points at JUMP_IF opcode. After execution:
        //   taken (falsy):     ip + 3 + offset  (jumped forward)
        //   not-taken (truthy): ip + 3           (fall through)
        uint8_t* not_taken_pc;
        if (taken) {
            // Interpreter jumped; the not-taken path is fall-through.
            not_taken_pc = ip + 3;
        } else {
            // Interpreter fell through; the not-taken path is the jump target.
            not_taken_pc = ip + 3 + offset;
        }

        uint16_t snap = emitSnapshot(r, not_taken_pc);

        if (taken) {
            // The value was falsy; guard that it stays falsy.
            irEmitGuardFalse(&r->ir, cond_ssa, snap);
        } else {
            // The value was truthy; guard that it stays truthy.
            irEmitGuardTrue(&r->ir, cond_ssa, snap);
        }
        break;
    }

    // -----------------------------------------------------------------
    // AND (2-byte forward offset)
    // If TOS is false, jump [arg] forward. Otherwise pop and continue.
    // -----------------------------------------------------------------
    case CODE_AND: {
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at AND");
            return false;
        }
        uint16_t cond_ssa = slotGet(r, r->stack_top - 1);
        if (cond_ssa == IR_NONE) {
            cond_ssa = irEmitLoad(&r->ir, (uint16_t)(r->stack_top - 1));
        }

        Value cond_val = stackStart[r->stack_top - 1];
        bool is_falsy = wrenIsFalsyValue(cond_val);

        uint16_t offset = readShort(ip);
        uint8_t* not_taken_pc;
        if (is_falsy) {
            // Interpreter keeps TOS and jumps; not-taken = fall through.
            not_taken_pc = ip + 3;
        } else {
            // Interpreter pops and falls through; not-taken = jump target.
            not_taken_pc = ip + 3 + offset;
        }

        uint16_t snap = emitSnapshot(r, not_taken_pc);

        if (is_falsy) {
            irEmitGuardFalse(&r->ir, cond_ssa, snap);
            // TOS stays (not popped). stack_top unchanged.
        } else {
            irEmitGuardTrue(&r->ir, cond_ssa, snap);
            // Pop TOS (it was truthy, so AND continues with next expr).
            r->stack_top--;
            r->slot_live[r->stack_top] = false;
        }
        break;
    }

    // -----------------------------------------------------------------
    // OR (2-byte forward offset)
    // If TOS is non-false, jump [arg] forward. Otherwise pop and continue.
    // -----------------------------------------------------------------
    case CODE_OR: {
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at OR");
            return false;
        }
        uint16_t cond_ssa = slotGet(r, r->stack_top - 1);
        if (cond_ssa == IR_NONE) {
            cond_ssa = irEmitLoad(&r->ir, (uint16_t)(r->stack_top - 1));
        }

        Value cond_val = stackStart[r->stack_top - 1];
        bool is_truthy = !wrenIsFalsyValue(cond_val);

        uint16_t offset = readShort(ip);
        uint8_t* not_taken_pc;
        if (is_truthy) {
            // Interpreter keeps TOS and jumps; not-taken = fall through.
            not_taken_pc = ip + 3;
        } else {
            // Interpreter pops and falls through; not-taken = jump target.
            not_taken_pc = ip + 3 + offset;
        }

        uint16_t snap = emitSnapshot(r, not_taken_pc);

        if (is_truthy) {
            irEmitGuardTrue(&r->ir, cond_ssa, snap);
            // TOS stays. stack_top unchanged.
        } else {
            irEmitGuardFalse(&r->ir, cond_ssa, snap);
            // Pop TOS.
            r->stack_top--;
            r->slot_live[r->stack_top] = false;
        }
        break;
    }

    // -----------------------------------------------------------------
    // LOOP (2-byte backward offset)
    // -----------------------------------------------------------------
    case CODE_LOOP: {
        uint16_t offset = readShort(ip);
        // The loop target is ip + 3 - offset (3 bytes for opcode + 2-byte arg).
        uint8_t* target = ip + 3 - offset;

        if (target == r->anchor_pc) {
            // We've looped back to the anchor -- trace is complete!
            irEmitLoopBack(&r->ir);
            jit->state = JIT_STATE_COMPILING;
            return true;
        } else {
            // Nested or different loop -- abort.
            jitRecorderAbort(jit, "loop target is not anchor (nested loop)");
            return false;
        }
    }

    // -----------------------------------------------------------------
    // LOAD_MODULE_VAR (2-byte arg)
    // imm.ptr = absolute address of the variable in module->variables.data
    // -----------------------------------------------------------------
    case CODE_LOAD_MODULE_VAR: {
        uint16_t var_idx = readShort(ip);
        ObjFn* fn2 = frame->closure->fn;
        if (var_idx >= (uint16_t)fn2->module->variables.count) {
            jitRecorderAbort(jit, "module var index out of range");
            return false;
        }
        Value* varPtr = &fn2->module->variables.data[var_idx];
        uint16_t ssa = irEmit(&r->ir, IR_LOAD_MODULE_VAR, var_idx, IR_NONE,
                              IR_TYPE_VALUE);
        r->ir.nodes[ssa].imm.ptr = (void*)varPtr;
        slotSet(r, r->stack_top, ssa);
        r->stack_top++;
        break;
    }

    // -----------------------------------------------------------------
    // STORE_MODULE_VAR (2-byte arg)
    // op1 = value SSA, imm.ptr = absolute address of the variable
    // -----------------------------------------------------------------
    case CODE_STORE_MODULE_VAR: {
        uint16_t var_idx = readShort(ip);
        if (r->stack_top <= 0) {
            jitRecorderAbort(jit, "stack underflow at STORE_MODULE_VAR");
            return false;
        }
        uint16_t val_ssa = slotGet(r, r->stack_top - 1);
        if (val_ssa == IR_NONE) {
            val_ssa = irEmitLoad(&r->ir, (uint16_t)(r->stack_top - 1));
        }
        ObjFn* fn2 = frame->closure->fn;
        if (var_idx >= (uint16_t)fn2->module->variables.count) {
            jitRecorderAbort(jit, "module var index out of range");
            return false;
        }
        Value* varPtr = &fn2->module->variables.data[var_idx];
        uint16_t node = irEmit(&r->ir, IR_STORE_MODULE_VAR, val_ssa, IR_NONE,
                               IR_TYPE_VOID);
        r->ir.nodes[node].imm.ptr = (void*)varPtr;
        // Does not pop.
        break;
    }

    // -----------------------------------------------------------------
    // LOAD_UPVALUE (1-byte arg)
    // -----------------------------------------------------------------
    case CODE_LOAD_UPVALUE: {
        // Upvalues are tricky for the JIT. For v0.1, abort.
        jitRecorderAbort(jit, "unsupported opcode: LOAD_UPVALUE");
        return false;
    }

    // -----------------------------------------------------------------
    // STORE_UPVALUE (1-byte arg)
    // -----------------------------------------------------------------
    case CODE_STORE_UPVALUE: {
        jitRecorderAbort(jit, "unsupported opcode: STORE_UPVALUE");
        return false;
    }

    // -----------------------------------------------------------------
    // LOAD_FIELD (pops instance, pushes field value)
    // -----------------------------------------------------------------
    case CODE_LOAD_FIELD: {
        int field_idx = ip[1];
        if (r->stack_top < 1) {
            jitRecorderAbort(jit, "stack underflow at LOAD_FIELD");
            return false;
        }
        int obj_slot = r->stack_top - 1;
        uint16_t obj_ssa = slotGet(r, obj_slot);
        if (obj_ssa == IR_NONE) {
            obj_ssa = irEmitLoad(&r->ir, (uint16_t)obj_slot);
            slotSet(r, obj_slot, obj_ssa);
        }
        uint16_t ssa = irEmitLoadField(&r->ir, obj_ssa, (uint16_t)field_idx);
        // LOAD_FIELD has stack effect 0 (pops instance, pushes value).
        slotSet(r, obj_slot, ssa);
        break;
    }

    // -----------------------------------------------------------------
    // STORE_FIELD (pops instance, stores value; effect -1)
    // -----------------------------------------------------------------
    case CODE_STORE_FIELD: {
        int field_idx = ip[1];
        if (r->stack_top < 2) {
            jitRecorderAbort(jit, "stack underflow at STORE_FIELD");
            return false;
        }
        // TOS = instance, TOS-1 = value to store.
        int inst_slot = r->stack_top - 1;
        int val_slot = r->stack_top - 2;
        uint16_t inst_ssa = slotGet(r, inst_slot);
        uint16_t val_ssa = slotGet(r, val_slot);
        if (inst_ssa == IR_NONE) {
            inst_ssa = irEmitLoad(&r->ir, (uint16_t)inst_slot);
        }
        if (val_ssa == IR_NONE) {
            val_ssa = irEmitLoad(&r->ir, (uint16_t)val_slot);
        }
        irEmitStoreField(&r->ir, inst_ssa, (uint16_t)field_idx, val_ssa);
        // Stack effect -1: pop the instance, value stays.
        r->stack_top--;
        r->slot_live[r->stack_top] = false;
        break;
    }

    // -----------------------------------------------------------------
    // RETURN
    // -----------------------------------------------------------------
    case CODE_RETURN: {
        if (r->call_depth > 0) {
            r->call_depth--;
            // The caller's frame will be restored by the interpreter.
            // We don't emit IR for the return itself; just track call depth.
        } else {
            jitRecorderAbort(jit, "returning out of trace root");
            return false;
        }
        break;
    }

    // -----------------------------------------------------------------
    // Everything else: abort
    // -----------------------------------------------------------------
    default: {
        jitRecorderAbort(jit, "unsupported opcode");
        return false;
    }

    } // end switch

    // Abort if call depth is too deep.
    if (r->call_depth > JIT_TRACE_MAX_CALL_DEPTH) {
        jitRecorderAbort(jit, "call depth too deep");
        return false;
    }

    return false;
}
