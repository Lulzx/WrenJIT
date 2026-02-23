// Include Wren VM types before our headers so all types are defined.
#include "wren_vm.h"
#include "wren_value.h"

#include "wren_jit.h"
#include "wren_jit_trace.h"
#include "wren_jit_snapshot.h"
#include "wren_jit_ir.h"
#include "wren_jit_opt.h"
#include "wren_jit_regalloc.h"
#include "wren_jit_codegen.h"

#include "sljitLir.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Hash function for PC-keyed open-addressing table.
static uint32_t hash_pc(uint8_t* pc)
{
    return (uint32_t)(((uintptr_t)pc >> 2) * 2654435761u);
}

WrenJitState* wrenJitInit(WrenVM* vm)
{
    (void)vm;

    WrenJitState* jit = (WrenJitState*)calloc(1, sizeof(WrenJitState));
    if (jit == NULL) return NULL;

    jit->trace_capacity = JIT_MAX_TRACES;
    jit->traces = (JitTrace*)calloc(jit->trace_capacity, sizeof(JitTrace));
    if (jit->traces == NULL) {
        free(jit);
        return NULL;
    }

    jit->state = JIT_STATE_IDLE;
    jit->enabled = true;
    jit->hot_threshold = JIT_HOT_THRESHOLD;

    return jit;
}

void wrenJitFree(WrenVM* vm, WrenJitState* jit)
{
    (void)vm;
    if (jit == NULL) return;

    if (jit->traces != NULL) {
        for (uint32_t i = 0; i < jit->trace_capacity; i++) {
            JitTrace* t = &jit->traces[i];
            if (t->anchor_pc == NULL) continue;

            if (t->code != NULL) {
                sljit_free_code(t->code, NULL);
            }
            free(t->snapshots);
            free(t->gc_roots);
        }
        free(jit->traces);
    }

    free(jit->recording_ir);
    free(jit->slot_map);
    free(jit);
}

void wrenJitSetEnabled(WrenJitState* jit, bool enabled)
{
    jit->enabled = enabled;
}

JitTrace* wrenJitLookup(WrenJitState* jit, uint8_t* pc)
{
    if (jit == NULL || jit->traces == NULL) return NULL;

    uint32_t mask = jit->trace_capacity - 1;
    uint32_t idx = hash_pc(pc) & mask;

    for (uint32_t i = 0; i < jit->trace_capacity; i++) {
        JitTrace* t = &jit->traces[idx];
        if (t->anchor_pc == NULL) return NULL;
        if (t->anchor_pc == pc) return t;
        idx = (idx + 1) & mask;
    }

    return NULL;
}

int wrenJitExecute(WrenVM* vm, JitTrace* trace)
{
    if (trace == NULL || trace->code == NULL) return -1;

    trace->exec_count++;

    ObjFiber* fiber = vm->fiber;
    CallFrame* frame = &fiber->frames[fiber->numFrames - 1];

    // Compute module variables data pointer for the current function.
    ObjFn* traceFn = frame->closure->fn;
    Value* modVarsData = traceFn->module->variables.data;

    JitTraceFunc fn = (JitTraceFunc)trace->code;
    int result = fn(vm, fiber, frame->stackStart, modVarsData);

    if (result != 0) {
        trace->exit_count++;
        if (vm && vm->jit) vm->jit->total_exits++;
    }

    return result;
}

bool wrenJitIncrementHot(WrenJitState* jit, uint8_t* bytecode,
                          uint16_t* hot_counts, int pc_offset)
{
    (void)bytecode;

    if (!jit->enabled) return false;

    hot_counts[pc_offset]++;
    return hot_counts[pc_offset] == (uint16_t)jit->hot_threshold;
}

void wrenJitStartRecording(WrenJitState* jit, uint8_t* pc)
{
    if (jit->state != JIT_STATE_IDLE) return;

    // Allocate and initialize a fresh IR buffer for this trace.
    IRBuffer* ir = (IRBuffer*)calloc(1, sizeof(IRBuffer));
    if (!ir) return;
    irBufferInit(ir);

    // Emit the loop header marker as the first node.
    irEmitLoopHeader(ir);

    jit->state = JIT_STATE_RECORDING;
    jit->anchor_pc = pc;
    jit->record_depth = 0;
    jit->record_count = 0;
    jit->recording_ir = ir;
}

void wrenJitAbortRecording(WrenJitState* jit)
{
    if (jit->state != JIT_STATE_RECORDING) return;

    free(jit->recording_ir);
    jit->recording_ir = NULL;
    jit->anchor_pc = NULL;
    jit->state = JIT_STATE_IDLE;
    jit->traces_aborted++;
}

// Legacy API stub (recording is handled by jitRecorderStep in wren_jit_trace.c).
bool wrenJitRecordInstruction(WrenJitState* jit, WrenVM* vm, uint8_t* ip)
{
    (void)jit; (void)vm; (void)ip;
    return false;
}

// Grow the trace hash table to double its current capacity.
static bool grow_trace_table(WrenJitState* jit)
{
    uint32_t new_cap = jit->trace_capacity * 2;
    JitTrace* new_traces = (JitTrace*)calloc(new_cap, sizeof(JitTrace));
    if (new_traces == NULL) return false;

    uint32_t new_mask = new_cap - 1;
    for (uint32_t i = 0; i < jit->trace_capacity; i++) {
        JitTrace* t = &jit->traces[i];
        if (t->anchor_pc == NULL) continue;

        uint32_t idx = hash_pc(t->anchor_pc) & new_mask;
        while (new_traces[idx].anchor_pc != NULL) {
            idx = (idx + 1) & new_mask;
        }
        new_traces[idx] = *t;
    }

    free(jit->traces);
    jit->traces = new_traces;
    jit->trace_capacity = new_cap;
    return true;
}

void wrenJitStoreTrace(WrenJitState* jit, JitTrace* trace)
{
    if (jit == NULL || trace == NULL) return;

    // Grow if load factor exceeds 0.7.
    if (jit->trace_count * 10 >= jit->trace_capacity * 7) {
        if (!grow_trace_table(jit)) return;
    }

    uint32_t mask = jit->trace_capacity - 1;
    uint32_t idx = hash_pc(trace->anchor_pc) & mask;

    while (jit->traces[idx].anchor_pc != NULL) {
        if (jit->traces[idx].anchor_pc == trace->anchor_pc) {
            // Replace existing trace at same PC.
            if (jit->traces[idx].code != NULL) {
                sljit_free_code(jit->traces[idx].code, NULL);
            }
            free(jit->traces[idx].snapshots);
            free(jit->traces[idx].gc_roots);
            jit->traces[idx] = *trace;
            return;
        }
        idx = (idx + 1) & mask;
    }

    jit->traces[idx] = *trace;
    jit->trace_count++;
    jit->traces_compiled++;
}

void wrenJitMarkRoots(WrenVM* vm, WrenJitState* jit)
{
    (void)vm;
    if (jit == NULL || jit->traces == NULL) return;

    for (uint32_t i = 0; i < jit->trace_capacity; i++) {
        JitTrace* t = &jit->traces[i];
        if (t->anchor_pc == NULL) continue;

        // Each gc_root should be marked via wrenGrayObj(vm, root).
        // Since we don't have the Wren GC header here yet, this is a
        // placeholder that iterates the roots. The actual call will be:
        //   for (int j = 0; j < t->num_gc_roots; j++) {
        //       wrenGrayObj(vm, (Obj*)t->gc_roots[j]);
        //   }
        (void)t;
    }
}

// ---------------------------------------------------------------------------
// wrenJitCompileAndStore
// ---------------------------------------------------------------------------

JitTrace* wrenJitCompileAndStore(WrenVM* vm, WrenJitState* jit,
                                   ObjFiber* fiber, void* framePtr)
{
    (void)fiber; (void)framePtr;

    if (!jit || (jit->state != JIT_STATE_RECORDING &&
                 jit->state != JIT_STATE_COMPILING)) return NULL;

    // Transition out of recording state.
    jit->state = JIT_STATE_IDLE;

    // Get the IR from the recorder (jitRecorderStep built it).
    JitRecorder* rec = jitRecorderGet(jit);
    if (!rec) {
        fprintf(stderr, "[JIT] compile: no recorder\n");
        jit->traces_aborted++;
        return NULL;
    }
    IRBuffer* ir = &rec->ir;

    // Require at least one guard/arithmetic node between LOOP_HEADER and
    // LOOP_BACK. A trace without guards would loop forever in native code.
    if (ir->snapshot_count == 0) {
        fprintf(stderr, "[JIT] compile: no snapshots, aborting\n");
        jit->traces_aborted++;
        return NULL;
    }

    // Run optimizer.
    irOptimize(ir);

    // Register allocation.
    RegAllocState ra;
    regAllocInit(&ra, (int)ir->count);
    regAllocComputeRanges(&ra, ir);
    regAllocRun(&ra);

    // Code generation.  (ir is part of the recorder struct, not heap-allocated.)
    JitTrace* trace = wrenJitCodegen(vm, ir, &ra, jit->anchor_pc);
    regAllocFree(&ra);

    if (!trace) {
        fprintf(stderr, "[JIT] compile: codegen failed\n");
        jit->traces_aborted++;
        return NULL;
    }

    trace->anchor_pc = jit->anchor_pc;
    wrenJitStoreTrace(jit, trace);
    return trace;
}

// ---------------------------------------------------------------------------
// wrenJitRestoreExit
// ---------------------------------------------------------------------------

void wrenJitRestoreExit(WrenVM* vm, WrenJitState* jit,
                         ObjFiber* fiber, void* framePtr,
                         JitTrace* trace, int exitIdx)
{
    (void)vm;
    (void)jit;
    (void)framePtr;

    if (!trace || !trace->snapshots) return;
    if (exitIdx < 0 || exitIdx >= (int)trace->num_snapshots) return;

    JitSnapshot* snap = &trace->snapshots[exitIdx];
    CallFrame* frame = &fiber->frames[fiber->numFrames - 1];
    frame->ip = snap->resume_pc;
    // Restore the stack top to the depth captured at the snapshot.
    // The side-exit stub already wrote all live SSA values back to the stack.
    fiber->stackTop = frame->stackStart + snap->stack_depth;
}
