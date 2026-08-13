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

// A snapshot with no stack entries. Used to hand control back to the
// interpreter without touching the interpreter stack: the machine code's
// STORE_STACK writes have already left the final values in place, so
// re-materializing the recorded (first-iteration) SSA values would corrupt it.
static uint16_t emitEmptySnapshot(JitRecorder* r, uint8_t* resume_pc)
{
    return irEmitSnapshot(&r->ir, resume_pc, r->stack_top);
}

// Flush any slot whose value is held only in a register to the interpreter
// stack. A local var's initializer is recorded as a plain push (Wren emits no
// STORE for a var definition), so its slot may never have been written to the
// stack; code that later re-syncs the slot with a naked LOAD_STACK would read
// a stale value left over from before the trace entered (or from the previous
// loop iteration). Skip slots whose current binding is already a load of that
// same slot, which is already in sync with memory. Raw number/int/bool
// bindings are boxed first: the interpreter stack holds NaN-boxed Values.
static void flushRegisterHeldSlots(JitRecorder* r)
{
    for (int s = 0; s < r->stack_top; s++) {
        uint16_t cur = slotGet(r, s);
        if (cur == IR_NONE) continue;
        IRNode* n = &r->ir.nodes[cur];
        if (n->op == IR_LOAD_STACK && n->imm.mem.slot == s) continue;
        uint16_t toStore = cur;
        if (n->type == IR_TYPE_NUM) {
            toStore = irEmitBox(&r->ir, cur);
        } else if (n->type == IR_TYPE_INT) {
            toStore = irEmit(&r->ir, IR_BOX_INT, cur, IR_NONE,
                             IR_TYPE_VALUE);
        } else if (n->type == IR_TYPE_BOOL) {
            toStore = irEmit(&r->ir, IR_BOX_BOOL, cur, IR_NONE,
                             IR_TYPE_VALUE);
        }
        irEmitStore(&r->ir, (uint16_t)s, toStore);
    }
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
    if (methodNameEquals(vm, symbol, "&(_)"))  return IR_BAND;
    if (methodNameEquals(vm, symbol, "|(_)"))  return IR_BOR;
    if (methodNameEquals(vm, symbol, "^(_)"))  return IR_BXOR;
    if (methodNameEquals(vm, symbol, "<<(_)")) return IR_LSHIFT;
    if (methodNameEquals(vm, symbol, ">>" "(_)")) return IR_RSHIFT;
    return IR_NOP;
}

// Returns true if an IR op is a comparison (result is bool, not num).
static bool isComparisonOp(IROp op)
{
    return op == IR_LT || op == IR_GT || op == IR_LTE || op == IR_GTE ||
           op == IR_EQ || op == IR_NEQ;
}

static bool isBitwiseOp(IROp op)
{
    return op == IR_BAND || op == IR_BOR || op == IR_BXOR ||
           op == IR_LSHIFT || op == IR_RSHIFT || op == IR_ASHR;
}

// Unary Num methods (CALL_0 on Num receiver).
static IROp numUnaryToIROp(WrenVM* vm, int symbol)
{
    if (methodNameEquals(vm, symbol, "-"))  return IR_NEG;
    if (methodNameEquals(vm, symbol, "sqrt")) return IR_SQRT;
    if (methodNameEquals(vm, symbol, "floor")) return IR_FLOOR;
    return IR_NOP;
}

// Turn the canonical short body
//
//     if (bool) moduleNumber = moduleNumber + 1
//
// into an unconditional add of 0.0/1.0. This lets a trace cover alternating
// boolean branches without taking a side exit on every true iteration. The
// exact bytecode match preserves the normal exit for every other branch body.
static bool tryEmitConditionalModuleIncrement(JitRecorder* r, WrenVM* vm,
                                               CallFrame* frame, uint8_t* ip,
                                               uint16_t cond_ssa,
                                               uint16_t offset)
{
    IRNode* cond = cond_ssa < r->ir.count ? &r->ir.nodes[cond_ssa] : NULL;
    // Before optimization, a getter after the toggling store is still a
    // LOAD_FIELD. Recover the immediately dominating stored SSA value; the
    // alias-aware forwarding pass will make the same replacement later.
    if (cond != NULL && cond->op == IR_LOAD_FIELD) {
        for (uint16_t i = cond_ssa; i-- > 0;) {
            IRNode* prior = &r->ir.nodes[i];
            if (prior->op == IR_STORE_FIELD && prior->op1 == cond->op1 &&
                prior->imm.mem.field == cond->imm.mem.field) {
                cond_ssa = prior->op2;
                cond = cond_ssa < r->ir.count ? &r->ir.nodes[cond_ssa] : NULL;
                break;
            }
            if ((prior->op == IR_STORE_FIELD && prior->op1 == cond->op1) ||
                prior->op == IR_CALL_C || prior->op == IR_CALL_WREN)
                break;
        }
    }
    if (cond == NULL || cond->op1 == IR_NONE) return false;

    // The condition must be a Wren boolean so IR_BOOL_TO_NUM's low-bit
    // extraction yields the correct 0/1. A comparison or boolean constant
    // (BOX_BOOL / CONST_BOOL) is always a bool; a BOOL_NOT (as in `!field`
    // toggles) is only valid once the recorder's GUARD_BOOL admitted the
    // operand.
    bool condIsBool = false;
    if (cond->op == IR_BOX_BOOL || cond->op == IR_CONST_BOOL) {
        condIsBool = true;
    } else if (cond->op == IR_BOOL_NOT) {
        for (uint16_t i = 0; i < r->ir.count; i++) {
            if (r->ir.nodes[i].op == IR_GUARD_BOOL &&
                r->ir.nodes[i].op1 == cond->op1) {
                condIsBool = true;
                break;
            }
        }
    }
    if (!condIsBool) return false;

    uint8_t* body = ip + 3;
    uint8_t* target = body + offset;
    // LOAD_MODULE_VAR u16; CONSTANT u16; CALL_1 u16;
    // STORE_MODULE_VAR u16; POP
    if (target != body + 13 ||
        body[0] != CODE_LOAD_MODULE_VAR || body[3] != CODE_CONSTANT ||
        body[6] != CODE_CALL_1 || body[9] != CODE_STORE_MODULE_VAR ||
        body[12] != CODE_POP)
        return false;

    uint16_t var_idx = readShort(body);
    uint16_t const_idx = readShort(body + 3);
    uint16_t symbol = readShort(body + 6);
    if (readShort(body + 9) != var_idx ||
        !methodNameEquals(vm, symbol, "+(_)"))
        return false;

    ObjFn* fn = frame->closure->fn;
    if (var_idx >= (uint16_t)fn->module->variables.count ||
        const_idx >= (uint16_t)fn->constants.count)
        return false;
    Value one = fn->constants.data[const_idx];
    if (!IS_NUM(one) || AS_NUM(one) != 1.0) return false;

    // The condition has already been popped. If the counter guard fails,
    // resume at the original body so the interpreter performs the update.
    uint16_t snap = emitSnapshot(r, body);
    Value* varPtr = &fn->module->variables.data[var_idx];
    uint16_t old = irEmit(&r->ir, IR_LOAD_MODULE_VAR, var_idx, IR_NONE,
                          IR_TYPE_VALUE);
    r->ir.nodes[old].imm.ptr = (void*)varPtr;
    irEmitGuardNum(&r->ir, old, snap);
    uint16_t oldNum = irEmitUnbox(&r->ir, old);
    uint16_t delta = irEmit(&r->ir, IR_BOOL_TO_NUM, cond_ssa, IR_NONE,
                            IR_TYPE_INT);
    uint16_t sum = irEmit(&r->ir, IR_ADD, oldNum, delta, IR_TYPE_NUM);
    uint16_t boxed = irEmitBox(&r->ir, sum);
    uint16_t store = irEmit(&r->ir, IR_STORE_MODULE_VAR, boxed, IR_NONE,
                            IR_TYPE_VOID);
    r->ir.nodes[store].imm.ptr = (void*)varPtr;
    return true;
}

// -------------------------------------------------------------------------
// jitRecorderStart
// -------------------------------------------------------------------------

void jitRecorderStart(WrenJitState* jit, uint8_t* anchor_pc, int num_slots,
                      uint16_t* hot_count)
{
    if (jit == NULL) return;
    jit->active_hot_count = hot_count;

    // Allocate recorder on first use.
    if (jit->recorder == NULL) {
        jit->recorder = calloc(1, sizeof(JitRecorder));
        if (jit->recorder == NULL) {
            wrenJitBlacklistCurrent(jit);
            return;
        }
    }

    JitRecorder* r = (JitRecorder*)jit->recorder;
    memset(r, 0, sizeof(JitRecorder));

    r->anchor_pc = anchor_pc;
    r->aborted = false;
    r->abort_reason = NULL;
    r->instr_count = 0;
    r->call_depth = 0;
    if (getenv("WREN_JIT_DBG_REC")) {
        fprintf(stderr, "[REC] start anchor=%p stack=%d\n",
                (void*)anchor_pc, num_slots);
    }

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

    if (wrenJitDebugEnabled()) {
        fprintf(stderr, "[JIT] abort: %s\n", reason ? reason : "unknown");
    }

    JitRecorder* r = (JitRecorder*)jit->recorder;
    r->aborted = true;
    r->abort_reason = reason;

    jit->state = JIT_STATE_IDLE;
    jit->traces_aborted++;
    wrenJitBlacklistCurrent(jit);
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

    // A recognized user-method call has already been represented in IR. The
    // interpreter must still execute it during recording, but its frame uses a
    // different stackStart. Ignore every hook in that callee and resume only
    // after RETURN restores the caller frame.
    if (r->suppressed_frame_depth != 0) {
        int depth = vm->fiber->numFrames;
        if (depth > r->suppressed_frame_depth) return false;
        r->suppressed_frame_depth = 0;
    }

    // Current fiber and frame for inspecting runtime state.
    ObjFiber* fiber = vm->fiber;
    CallFrame* frame = &fiber->frames[fiber->numFrames - 1];
    Value* stackStart = frame->stackStart;

    // A nested loop marked loop_only (see the CODE_LOOP handler) is running
    // out its remaining iterations in the interpreter while recording is
    // suppressed. Skip its body until the interpreter exits the loop,
    // detected at the loop's exit tests; then resume recording the outer body
    // so the after-loop code stays in-trace (outer closure).
    if (r->nested_depth > 0 && r->nested[r->nested_depth - 1].loop_only) {
        NestedLoop* nl = &r->nested[r->nested_depth - 1];
        r->suppress_count++;

        // A method called from the loop body executes its own bytecode. Its
        // pcs are outside the loop range, so skip every hook inside the callee
        // frame instead of treating it as a loop exit. Recording resumes
        // naturally after RETURN restores the caller frame.
        if (fiber->numFrames > nl->frame_depth) return false;

        bool in_loop = (uintptr_t)ip >= (uintptr_t)nl->loop_pc &&
                       (uintptr_t)ip <= (uintptr_t)nl->backedge_pc;
        if (fiber->numFrames < nl->frame_depth || !in_loop) {
            // The loop was left through a path we do not model (a return, a
            // break, or a jump we did not record as an exit test). The
            // interpreter is no longer inside the loop, so neither resuming
            // nor the close-fallback snapshot is safe: abandon the trace.
            if (getenv("WREN_JIT_DBG_REC")) {
                fprintf(stderr, "[REC] SUPPRESS abort pc=%p frames=%d/%d "
                        "inloop=%d\n", (void*)nl->loop_pc, fiber->numFrames,
                        nl->frame_depth, (int)in_loop);
            }
            jitRecorderAbort(jit, "nested loop exited unexpectedly");
            return false;
        }

        // Budget fallback: the interpreter is still inside this loop after
        // too many suppressed steps (e.g. a 2000-iteration inner loop). Close
        // the trace the way the recorder did before outer-closure -- the loop
        // compiles in place and its exit tests deopt to the interpreter -- so
        // long-running loops never stall or blow the recording budget.
        if (r->suppress_count > JIT_SUPPRESS_MAX) {
            if (getenv("WREN_JIT_DBG_REC")) {
                fprintf(stderr,
                        "[REC] SUPPRESS budget pc=%p n=%d\n",
                        (void*)nl->loop_pc, r->suppress_count);
            }
            irEmitLoopBackTo(&r->ir, nl->ir_header);
            if (nl->exit_count > 0 && nl->pending_exit_pc != NULL) {
                uint16_t snap = emitEmptySnapshot(r, nl->pending_exit_pc);
                for (int k = 0; k < nl->exit_count; k++) {
                    r->ir.nodes[nl->exit_nodes[k]].imm.jump.target = IR_NONE;
                    r->ir.nodes[nl->exit_nodes[k]].imm.jump.snapshot = snap;
                }
            }
            r->nested_depth--;
            r->suppress_count = 0;
            jit->state = JIT_STATE_COMPILING;
            return true;
        }

        // Is the interpreter at one of this loop's exit tests? If so, decide
        // whether it just left the loop and resume recording the outer body.
        //
        // The recorder hook runs BEFORE the current instruction executes (it
        // fires from DISPATCH with ip = the next instruction and the stack in
        // its pre-instruction state). So at an exit test the condition value
        // is still on the top of the stack, at stackStart[fiber_depth - 1].
        int fiber_depth = (int)(fiber->stackTop - frame->stackStart);
        bool exited = false;
        int after_depth = fiber_depth;
        for (int k = 0; k < nl->exit_count; k++) {
            if (ip != nl->exit_pcs[k]) continue;
            if (fiber_depth < 1 || fiber_depth > JIT_TRACE_MAX_SLOTS) break;
            Code op = (Code)nl->exit_pcs[k][0];
            if (op == CODE_JUMP_IF) {
                // JUMP_IF pops the condition: the after-loop runs one slot
                // shallower. The popped value is on the stack top now.
                exited = wrenIsFalsyValue(stackStart[fiber_depth - 1]);
                after_depth = fiber_depth - 1;
            } else {
                // CODE_AND: a falsy left operand short-circuits to the loop
                // exit with the operand left on the stack (no pop); a truthy
                // one pops and stays in the loop.
                exited = wrenIsFalsyValue(stackStart[fiber_depth - 1]);
                after_depth = fiber_depth;
            }
            break;
        }
        if (!exited) return false;   // still inside the loop: skip this step

        // The interpreter just left the loop. Emit its back edge (so the
        // machine loop stays in-trace) and patch every exit test to jump
        // in-trace to the after-loop code: falsy jumps forward past the loop,
        // truthy falls through the body and loops in-trace. Resume recording
        // at the interpreter's after-loop depth, then skip this exit-test
        // instruction -- its effect is already represented by the LOOP_EXITs.
        //
        // The exit test can fire on the first runtime pass, before the body
        // has run (e.g. the last outer iteration of nbody, where the inner
        // condition is false immediately). The after-loop must therefore read
        // the runtime stack, not the second-iteration SSAs: re-sync every live
        // slot with a fresh LOAD_STACK. A slot written inside the loop has its
        // final value on the stack (STORE_STACK roots survive DCE, and the
        // interpreter ran the loop to completion during suppression), so it is
        // left alone. A slot bound to a raw number/int/bool constant was never
        // stored by Wren (var declarations emit no STORE_LOCAL), so flush it
        // to memory first -- the same problem the loop-open prologue solves.
        irEmitLoopBackTo(&r->ir, nl->ir_header);
        uint16_t after = (uint16_t)r->ir.count;   // exit tests jump here
        for (int s = 0; s < after_depth; s++) {
            uint16_t cur = slotGet(r, s);
            if (cur == IR_NONE) continue;
            IRNode* n = &r->ir.nodes[cur];
            if (n->type != IR_TYPE_NUM && n->type != IR_TYPE_INT &&
                n->type != IR_TYPE_BOOL) continue;
            bool written = false;
            for (uint16_t i = nl->ir_header; i < after; i++) {
                IRNode* b = &r->ir.nodes[i];
                if (b->op == IR_STORE_STACK && b->imm.mem.slot == (uint16_t)s) {
                    written = true;
                    break;
                }
            }
            if (written) continue;
            uint16_t toStore = cur;
            if (n->type == IR_TYPE_NUM) {
                toStore = irEmitBox(&r->ir, cur);
            } else if (n->type == IR_TYPE_INT) {
                toStore = irEmit(&r->ir, IR_BOX_INT, cur, IR_NONE,
                                 IR_TYPE_VALUE);
            } else {
                toStore = irEmit(&r->ir, IR_BOX_BOOL, cur, IR_NONE,
                                 IR_TYPE_VALUE);
            }
            irEmitStore(&r->ir, (uint16_t)s, toStore);
        }
        for (int k = 0; k < nl->exit_count; k++) {
            r->ir.nodes[nl->exit_nodes[k]].imm.jump.target = after;
            r->ir.nodes[nl->exit_nodes[k]].imm.jump.snapshot = IR_NONE;
            // A pending entry branch for this same JUMP_IF pc is this loop's
            // entry test recorded before the loop was opened. Patch it the
            // same way so an empty runtime loop skips its body in-trace.
            for (int p = 0; p < r->pending_entry_count; p++) {
                if (r->pending_entry[p].ip == NULL) continue;
                if (r->pending_entry[p].ip != nl->exit_pcs[k]) continue;
                r->ir.nodes[r->pending_entry[p].node].imm.jump.target = after;
                r->ir.nodes[r->pending_entry[p].node].imm.jump.snapshot = IR_NONE;
                r->pending_entry[p].ip = NULL;   // consumed
            }
        }
        if (getenv("WREN_JIT_DBG_REC")) {
            fprintf(stderr,
                    "[REC] RESUME after loop pc=%p ir=%u depth=%d\n",
                    (void*)nl->loop_pc, after, after_depth);
        }
        for (int s = 0; s < after_depth; s++) {
            uint16_t ssa = irEmitLoad(&r->ir, (uint16_t)s);
            slotSet(r, s, ssa);
        }
        r->stack_top = after_depth;
        r->nested_depth--;
        r->suppress_count = 0;
        return false;   // skip the exit test; the next step records the after-loop
    }

    // Abort if we've recorded too many instructions.
    r->instr_count++;
    if (getenv("WREN_JIT_DBG_REC") && r->instr_count <= 45) {
        fprintf(stderr, "[REC] %2d pc=%p op=%d depth=%d top=%d\n",
                r->instr_count, (void*)ip, (int)(*ip), r->nested_depth,
                r->stack_top);
    }
    if (r->instr_count > JIT_TRACE_MAX_INSNS) {
        jitRecorderAbort(jit, "trace too long");
        return false;
    }

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
                if (wrenJitDebugEnabled() && symbol < vm->methodNames.count) {
                    ObjString* name = vm->methodNames.data[symbol];
                    fprintf(stderr, "[JIT] unsupported Num binary symbol: %.*s\n",
                            (int)name->length, name->value);
                }
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

            // Bitwise primitives cast to uint32. Specialize exact integer
            // inputs and side-exit to Wren for fractional/general values.
            uint16_t left;
            uint16_t right;
            if (isBitwiseOp(binop)) {
                left = irEmit(&r->ir, IR_UNBOX_INT, recv_ssa, IR_NONE,
                              IR_TYPE_INT);
                right = irEmit(&r->ir, IR_UNBOX_INT, arg_ssa, IR_NONE,
                               IR_TYPE_INT);
                r->ir.nodes[left].flags |= IR_FLAG_INT_GUARD;
                r->ir.nodes[right].flags |= IR_FLAG_INT_GUARD;
                r->ir.nodes[left].imm.snapshot_id = snap;
                r->ir.nodes[right].imm.snapshot_id = snap;
            } else {
                left = irEmitUnbox(&r->ir, recv_ssa);
                right = irEmitUnbox(&r->ir, arg_ssa);
            }

            // Emit the operation.
            IRType result_type = isComparisonOp(binop) ? IR_TYPE_BOOL :
                                 (isBitwiseOp(binop) ? IR_TYPE_INT : IR_TYPE_NUM);
            uint16_t result = irEmit(&r->ir, binop, left, right, result_type);
            if (binop == IR_MOD) r->ir.nodes[result].imm.snapshot_id = snap;

            // Box the result back into a Wren Value.
            uint16_t boxed;
            if (isComparisonOp(binop)) {
                // Comparison produces a native bool (0/1). Box it to a Wren
                // Value (FALSE_VAL or TRUE_VAL) so that GUARD_TRUE/GUARD_FALSE
                // can check it correctly against the Wren NaN-boxed encoding.
                boxed = irEmit(&r->ir, IR_BOX_BOOL, result, IR_NONE,
                               IR_TYPE_VALUE);
            } else if (isBitwiseOp(binop)) {
                boxed = irEmit(&r->ir, IR_BOX_INT, result, IR_NONE,
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
    case CODE_CALL_2: {
        uint16_t symbol = readShort(ip);
        if (jitTryWidenCall2(jit, vm, stackStart, symbol, ip)) break;
        jitRecorderAbort(jit, "unsupported CALL_2 receiver type");
        return false;
    }
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

        uint16_t offset = readShort(ip);

        // A JUMP_IF reached while an open nested loop is still recording its
        // condition block is that loop's exit test. Emit an in-trace forward
        // branch instead of a deoptimizing guard so the nested loop's
        // termination stays on the trace and the outer body continues.
        if (r->nested_depth > 0) {
            NestedLoop* nl = &r->nested[r->nested_depth - 1];
            uint8_t* jump_target_pc = ip + 3 + offset;
            if (nl->pending_exit_pc == NULL ||
                nl->pending_exit_pc == jump_target_pc) {
                if (nl->pending_exit_pc == NULL)
                    nl->pending_exit_pc = jump_target_pc;
                uint16_t exit_node = irEmitLoopExit(&r->ir, cond_ssa);
                if (nl->exit_count < 4) {
                    nl->exit_nodes[nl->exit_count] = exit_node;
                    nl->exit_pcs[nl->exit_count] = ip;
                    nl->exit_count++;
                }
                if (taken) {
                    // The interpreter left the loop at this condition
                    // evaluation, so only one body iteration was recorded. The
                    // recorded values match the interpreter stack (one
                    // iteration ran), so continue recording the outer body.
                    // A falsy runtime condition jumps forward to it; a truthy
                    // one must hand control back to the interpreter at the
                    // body start (the loop would have continued).
                    uint16_t snap = emitSnapshot(r, ip + 3);
                    for (int k = 0; k < nl->exit_count; k++) {
                        r->ir.nodes[nl->exit_nodes[k]].imm.jump.target =
                            r->ir.count;
                    }
                    // Only the exit test itself deopts on truthy; earlier
                    // AND short-circuit exits fall through to it.
                    r->ir.nodes[exit_node].imm.jump.snapshot = snap;
                    r->nested_depth--;
                }
                break;
            }
            // Otherwise this is a conditional inside the loop body; fall
            // through to the normal guard path below.
        }

        // Compute the not-taken path PC for the side exit snapshot.
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

        // A skipped canonical conditional increment can be represented as a
        // branchless 0/1 add, keeping both boolean outcomes on this trace.
        if (taken && tryEmitConditionalModuleIncrement(r, vm, frame, ip,
                                                       cond_ssa, offset)) {
            break;
        }

        uint16_t snap = emitSnapshot(r, not_taken_pc);

        if (taken) {
            // The value was falsy; guard that it stays falsy.
            irEmitGuardFalse(&r->ir, cond_ssa, snap);
        } else if (r->pending_entry_count < JIT_MAX_PENDING_ENTRY) {
            // A truthy conditional may be the entry test of a loop whose back
            // edge has not been seen yet (its header sits below the current
            // pc). This is true both for the first nested loop opened after
            // the anchor (recorded while nested_depth == 0) and for deeper
            // loops (nested_depth > 0): the entry test of a depth-1 loop is
            // recorded before the loop opens, so it must also be tracked here
            // or its falsy path can never be patched in-trace by the loop's
            // RESUME. Record it as a deopt-capable IR_LOOP_EXIT — target ==
            // NONE + snapshot set means falsy deopts and truthy falls through,
            // i.e. exactly GUARD_TRUE — and remember the branch. When a back
            // edge later proves this JUMP_IF is a loop's exit test, that
            // loop's RESUME patches this branch to jump in-trace past the
            // loop, so an empty inner loop (e.g. nbody's j-loop when the outer
            // i == 4, or fasta's j-loop on the r < cumulative[0] path) skips
            // its body instead of deopting on every outer iteration.
            //
            // Flush register-held slots to memory before the branch. If the
            // branch is later patched to jump in-trace past the loop, the
            // after-loop re-syncs every slot with a LOAD_STACK; a slot whose
            // value the loop would have written (e.g. fasta's `j`, initialized
            // by `var j = 0`, which Wren never stores) must hold its PRE-loop
            // value on that skip path, and the loop body never ran to store it.
            flushRegisterHeldSlots(r);
            uint16_t entry_node = irEmitLoopExit(&r->ir, cond_ssa);
            r->ir.nodes[entry_node].imm.jump.snapshot = snap;
            r->pending_entry[r->pending_entry_count].ip  = ip;
            r->pending_entry[r->pending_entry_count].node = entry_node;
            r->pending_entry_count++;
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

        // A short-circuit && reached while an open nested loop is still
        // recording its condition block (before its exit JUMP_IF) is part of
        // the exit test: a falsy left operand short-circuits to the loop exit.
        if (r->nested_depth > 0) {
            NestedLoop* nl = &r->nested[r->nested_depth - 1];
            if (nl->pending_exit_pc == NULL) {
                uint16_t exit_node = irEmitLoopExit(&r->ir, cond_ssa);
                if (nl->exit_count < 4) {
                    nl->exit_nodes[nl->exit_count] = exit_node;
                    nl->exit_pcs[nl->exit_count] = ip;
                    nl->exit_count++;
                }
                if (!is_falsy) {
                    // Truthy left operand: pop it and evaluate the right side,
                    // matching the interpreter. A falsy left operand leaves
                    // TOS in place and the following JUMP_IF handles it.
                    r->stack_top--;
                    r->slot_live[r->stack_top] = false;
                }
                break;
            }
            // Body-internal && — fall through to the guard path below.
        }

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

        if (getenv("WREN_JIT_DBG_REC")) {
            fprintf(stderr,
                    "[REC] CODE_LOOP ip=%p target=%p anchor=%p depth=%d\n",
                    (void*)ip, (void*)target, (void*)r->anchor_pc,
                    r->nested_depth);
        }

        if (target == r->anchor_pc) {
            // We've looped back to the anchor -- trace is complete. Every
            // nested loop must already be closed (by its own back-edge or a
            // falsy exit test); an open one means the recording is unbalanced.
            if (r->nested_depth != 0) {
                if (getenv("WREN_JIT_DBG_REC")) {
                    fprintf(stderr,
                            "[REC] abort: anchor back-edge, nested_depth=%d "
                            "instr=%d top=%d\n",
                            r->nested_depth, r->instr_count, r->stack_top);
                }
                jitRecorderAbort(jit, "anchor back-edge with open nested loop");
                return false;
            }
            irEmitLoopBack(&r->ir);
            jit->state = JIT_STATE_COMPILING;
            return true;
        }

        // A back-edge to an open nested loop means the interpreter is
        // re-entering that loop. The recording cannot continue into the outer
        // body (the interpreter is not there yet), so the trace ends here with
        // the nested loop compiled in place: emit its back edge, make the exit
        // tests hand control to the interpreter when the loop terminates (the
        // machine code's STORE_STACK writes already left the final values on
        // the interpreter stack), and complete the trace. The outer back-edge
        // will re-enter this trace next outer iteration.
        int found = -1;
        for (int i = r->nested_depth - 1; i >= 0; i--) {
            if (r->nested[i].loop_pc == target) { found = i; break; }
        }
        if (found >= 0) {
            // The interpreter is re-entering the innermost open loop at its
            // back-edge. Instead of closing the trace here -- which would leave
            // the outer body to the interpreter on every outer iteration (an
            // exit storm) -- mark the loop loop_only: its remaining iterations
            // run in the interpreter while recording is suppressed, and when
            // the loop exits the trace resumes with the outer body so the
            // after-loop code stays in-trace (outer closure). The loop's exit
            // tests are patched at resume to jump in-trace past the loop.
            if (found == r->nested_depth - 1) {
                NestedLoop* nl = &r->nested[found];
                nl->loop_only = true;
                nl->frame_depth = fiber->numFrames;
                nl->backedge_pc = ip;
                r->suppress_count = 0;
                if (getenv("WREN_JIT_DBG_REC")) {
                    fprintf(stderr,
                            "[REC] LOOP_ONLY pc=%p exits=%d pending=%p "
                            "frames=%d\n",
                            (void*)nl->loop_pc, nl->exit_count,
                            (void*)nl->pending_exit_pc, nl->frame_depth);
                }
                break;   // skip the back-edge; the next hook suppresses
            }

            // Unbalanced back-edge (a loop still open above the matched one):
            // close this loop and any loops still open above it, innermost
            // first. Each emits its own back edge.
            while (r->nested_depth > found) {
                NestedLoop* nl = &r->nested[r->nested_depth - 1];
                irEmitLoopBackTo(&r->ir, nl->ir_header);
                if (getenv("WREN_JIT_DBG_REC")) {
                    fprintf(stderr,
                            "[REC] CLOSE nested loop pc=%p exits=%d pending=%p\n",
                            (void*)nl->loop_pc, nl->exit_count,
                            (void*)nl->pending_exit_pc);
                }
                if (nl->exit_count > 0 && nl->pending_exit_pc != NULL) {
                    // Falsy runtime condition -> hand control to the
                    // interpreter at the after-loop pc. The stack is already
                    // final, so no entries are captured.
                    uint16_t snap =
                        emitEmptySnapshot(r, nl->pending_exit_pc);
                    for (int k = 0; k < nl->exit_count; k++) {
                        r->ir.nodes[nl->exit_nodes[k]].imm.jump.target = IR_NONE;
                        r->ir.nodes[nl->exit_nodes[k]].imm.jump.snapshot = snap;
                    }
                }
                r->nested_depth--;
            }
            jit->state = JIT_STATE_COMPILING;
            return true;
        }

        // If the target is before our anchor, recording began on the
        // terminating backedge of a short inner loop and execution has
        // now reached an enclosing loop. Do not permanently blacklist the
        // inner loop: retry at its next backedge, which will enter the
        // body and close a trace normally.
        if ((uintptr_t)target < (uintptr_t)r->anchor_pc &&
            jit->active_hot_count != NULL) {
            uint16_t* retry = jit->active_hot_count;
            jit->active_hot_count = NULL;
            jitRecorderAbort(jit, "recording started at loop completion");
            *retry = (uint16_t)(jit->hot_threshold - 1);
            return false;
        }

        // Otherwise this is a genuinely new nested loop after the anchor.
        // Open it: emit a fresh loop header and re-load every live stack slot
        // so loop-carried values are read from the interpreter stack each
        // iteration (mirroring what the anchor header does at trace start).
        //
        // Before re-loading, flush any slot whose value is held only in a
        // register. A local var's initializer is recorded as a plain push
        // (Wren emits no STORE for a var definition), so its slot may never
        // have been written to the interpreter stack; a naked LOAD_STACK here
        // would read a stale value left over from before the trace entered.
        // Skip slots whose current binding is already a load of that same
        // slot, which is already in sync with memory.
        if (r->nested_depth >= JIT_MAX_NESTED_LOOPS) {
            jitRecorderAbort(jit, "too many nested loops");
            return false;
        }
        // Flush any slot whose value is held only in a register BEFORE the
        // loop header (see flushRegisterHeldSlots). Emitting the stores after
        // the header would put them inside the loop, re-executing them every
        // iteration.
        flushRegisterHeldSlots(r);
        uint16_t hdr = irEmitLoopHeaderRaw(&r->ir);
        for (int s = 0; s < r->stack_top; s++) {
            uint16_t ssa = irEmitLoad(&r->ir, (uint16_t)s);
            slotSet(r, s, ssa);
        }
        NestedLoop* nl = &r->nested[r->nested_depth++];
        nl->loop_pc = target;
        nl->ir_header = hdr;
        nl->exit_count = 0;
        nl->pending_exit_pc = NULL;
        nl->loop_only = false;
        nl->open_stack_top = r->stack_top;
        nl->backedge_pc = NULL;
        if (getenv("WREN_JIT_DBG_REC")) {
            int fdep = (int)(vm->fiber->stackTop -
                             vm->fiber->frames[vm->fiber->numFrames - 1].stackStart);
            fprintf(stderr,
                    "[REC] OPEN nested loop pc=%p hdr=%u depth=%d top=%d "
                    "fdep=%d\n",
                    (void*)target, hdr, r->nested_depth, r->stack_top, fdep);
        }
        break;
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
        if (getenv("WREN_JIT_DBG_MODVAR")) {
            SymbolTable* st = &fn2->module->variableNames;
            const char* nm = (var_idx < st->count) ? st->data[var_idx]->value : "?";
            fprintf(stderr, "[REC] MODVAR idx=%u name=%s\n", var_idx, nm);
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
