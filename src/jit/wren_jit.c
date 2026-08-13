// Include Wren VM types before our headers so all types are defined.
#include "wren_vm.h"
#include "wren_value.h"

#include "wren_debug.h"
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

#ifdef __cplusplus
extern "C" {
#endif
const char* const wrenOpNames[] = {
    #define OPCODE(name, _) #name,
    #include "wren_opcodes.h"
    #undef OPCODE
};
#ifdef __cplusplus
}
#endif

// Failing to trace a loop is ordinary operation, not an error: any loop the
// recorder does not support simply keeps running interpreted. Reporting that on
// stderr corrupts the output of any embedder that captures it, so every JIT
// diagnostic is gated behind WREN_JIT_DEBUG. The upstream Wren suite is one
// such embedder, and treats a stray stderr line as a test failure.
bool wrenJitDebugEnabled(void)
{
    // getenv is not cheap enough to call from a recorder abort, which can run
    // once per unsupported loop. -1 means "not yet looked up".
    static int cached = -1;
    if (cached < 0) {
        const char* value = getenv("WREN_JIT_DEBUG");
        cached = (value != NULL && value[0] != '\0' && strcmp(value, "0") != 0);
    }
    return cached != 0;
}

#define JIT_DEBUG_LOG(...)                           \
    do {                                            \
        if (wrenJitDebugEnabled()) {                \
            fprintf(stderr, __VA_ARGS__);           \
        }                                           \
    } while (0)

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

    // Most programs only trace their handful of genuinely hot loops, which is
    // the point, but it makes a general test suite a poor net for codegen bugs:
    // upstream Wren's 900 tests are small enough that only two of them ever
    // reach the default threshold. Lowering it to 1 forces every loop in a
    // suite through the recorder and code generator instead.
    const char* threshold = getenv("WREN_JIT_HOT_THRESHOLD");
    if (threshold != NULL && threshold[0] != '\0') {
        long parsed = strtol(threshold, NULL, 10);
        // The counter is a uint16_t and UINT16_MAX is the blacklist sentinel,
        // so a threshold at or above it could never be reached.
        if (parsed >= 1 && parsed < JIT_HOT_BLACKLISTED) {
            jit->hot_threshold = (int)parsed;
        }
    }

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
            if (getenv("WREN_JIT_DBG_ENTRY")) {
                fprintf(stderr, "[DBG] trace anchor=%p exec=%llu exit=%llu bad=%u "
                        "loopsnap=%d loopsnap2=%d disabled=%d\n",
                        (void*)t->anchor_pc, (unsigned long long)t->exec_count,
                        (unsigned long long)t->exit_count, t->bad_exit_count,
                        t->loop_exit_snapshot, t->loop_exit_snapshot2,
                        t->disabled);
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
    if (jit == NULL || jit->traces == NULL || jit->trace_count == 0) return NULL;

    uint32_t mask = jit->trace_capacity - 1;
    uint32_t idx = hash_pc(pc) & mask;

    for (uint32_t i = 0; i < jit->trace_capacity; i++) {
        JitTrace* t = &jit->traces[idx];
        if (t->anchor_pc == NULL) return NULL;
        if (t->anchor_pc == pc) return t->disabled ? NULL : t;
        idx = (idx + 1) & mask;
    }

    return NULL;
}

// True if a side exit resuming at resume_pc lands inside the body of a nested
// loop: a loop whose header lies strictly between the trace's own anchor header
// and resume_pc, and whose back-edge is at or after resume_pc. Such an exit
// means the trace was recorded on a path that skipped that loop -- its only
// in-trace test is the loop's entry guard, so the common path deopts to the
// interpreter on every entry. Healthy exits are excluded: after-loop code and
// guards in the trace's own body resume at a pc where the next back-edge
// targets the anchor exactly (the trace's own loop), and an enclosing loop's
// back-edge targets a header before the anchor.
static bool exit_resumes_in_nested_loop(uint8_t* resume_pc, uint8_t* anchor_pc,
                                        uint8_t* code_base, int code_count)
{
    uint8_t* fnEnd = code_base + code_count;
    uint8_t* scanEnd = resume_pc + 1024;   // cap the scan window
    if (scanEnd > fnEnd) scanEnd = fnEnd;
    for (uint8_t* p = resume_pc; p + 2 < scanEnd; p++) {
        if (*p != CODE_LOOP) continue;
        uint16_t offset = (uint16_t)(((uint16_t)p[1] << 8) | p[2]);
        uint8_t* target = p + 3 - offset;
        if (target == anchor_pc) break;    // the trace's own back-edge
        if (target > anchor_pc && target < resume_pc) return true;
    }
    return false;
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
    if (getenv("WREN_JIT_DBG_ENTRY") && trace->exec_count <= 24) {
        Value* ss = frame->stackStart;
        fprintf(stderr, "[ENTRY] exec=%u anchor=%p anchoridx=%ld ip=%p depth=%ld code=%u stack[",
                trace->exec_count, (void*)trace->anchor_pc,
                (long)(trace->anchor_pc - traceFn->code.data),
                (void*)frame->ip,
                (long)(fiber->stackTop - ss), trace->code_size);
        int d = (int)(fiber->stackTop - ss);
        for (int si = 0; si < d && si < 16; si++) {
            double dv = 0;
            if (IS_NUM(ss[si])) dv = AS_NUM(ss[si]);
            fprintf(stderr, "%d=%s%g/%p ", si, IS_NUM(ss[si]) ? "" : "n",
                    dv, (void*)(uintptr_t)ss[si]);
        }
        fprintf(stderr, "]\n");
        if (getenv("WREN_JIT_DBG_BC")) {
            int idx = (int)(trace->anchor_pc - traceFn->code.data);
            fprintf(stderr, "  anchor opcode=%d at index %d bytes:",
                    (int)trace->anchor_pc[0], idx);
            for (int b = -4; b < 8; b++) {
                int at = idx + b;
                if (at < 0) continue;
                fprintf(stderr, " %02x", traceFn->code.data[at]);
            }
            fprintf(stderr, "\n");
        }
    }
    int result = fn(vm, fiber, frame->stackStart, modVarsData);

    if (result != 0) {
        trace->exit_count++;
        if (vm && vm->jit) vm->jit->total_exits++;
        uint16_t sid = (uint16_t)(result - 1);
        if (sid != trace->loop_exit_snapshot &&
            sid != trace->loop_exit_snapshot2) {
            // An exit can still follow a profitable native prefix (regex
            // scanners are a common example), so frequency alone must not
            // retire the trace. Preserve the diagnostic counter for a future
            // progress- or time-based profitability policy.
            if (trace->bad_exit_count != UINT32_MAX)
                trace->bad_exit_count++;
        } else {
            trace->bad_exit_count = 0;
        }
        if (wrenJitDebugEnabled() && trace->exit_count <= 16) {
            int ei = result - 1;
            uint8_t* rpc = (trace->snapshots && ei >= 0 &&
                            ei < (int)trace->num_snapshots)
                ? trace->snapshots[ei].resume_pc : NULL;
            fprintf(stderr, "[JIT] side exit: anchor=%p snapshot=%d rpc=%p "
                    "fn=%s idx=%d rpcidx=%d\n",
                    (void*)trace->anchor_pc, result - 1, (void*)rpc,
                    traceFn->debug->name ? traceFn->debug->name : "?",
                    (int)(trace->anchor_pc - traceFn->code.data),
                    (int)(rpc - traceFn->code.data));
            if (getenv("WREN_JIT_DBG_BC") && rpc != NULL) {
                int idx = (int)(rpc - traceFn->code.data);
                int start = idx - 8; if (start < 0) start = 0;
                int k = start;
                for (;;) {
                    int off = wrenDumpInstruction(vm, traceFn, k);
                    if (off == -1) break;
                    k += off;
                    if (k > idx + 8) break;
                }
                fprintf(stderr, "  ^^^ rpc at index %d\n", idx);
            }
        }

        // The generated exit stub has already materialized live values. Finish
        // the tiny metadata restore here while frame and fiber are hot instead
        // of crossing a second C call boundary from the interpreter.
        int exitIdx = result - 1;
        if (trace->snapshots != NULL && exitIdx >= 0 &&
            exitIdx < (int)trace->num_snapshots) {
            JitSnapshot* snap = &trace->snapshots[exitIdx];
            frame->ip = snap->resume_pc;
            fiber->stackTop = frame->stackStart + snap->stack_depth;

            // Degenerate-trace re-record: an exit that resumes inside a nested
            // loop's body means this trace was recorded on a path that skipped
            // that loop, so it deopts on the common path (fasta's outer loop,
            // whose inner `while` only runs 73% of the time, is the textbook
            // case: exec == exit == 365k). Retire it and re-arm the loop's hot
            // counter so the next pass re-records; a re-record through the
            // loop compiles it as a native nested loop with in-trace exits
            // (the LOOP_ONLY resume in wren_jit_trace.c), eliminating the
            // storm. Bounded so a loop that genuinely always skips a nested
            // loop cannot thrash the recorder.
            if (snap->resume_pc != NULL && trace->anchor_pc != NULL &&
                trace->loop_handoff_exits < JIT_RE_RECORD_HANDOFF_EXITS &&
                vm->jit != NULL &&
                vm->jit->re_records_done < JIT_MAX_RE_RECORDS) {
                if (exit_resumes_in_nested_loop(snap->resume_pc,
                                                trace->anchor_pc,
                                                traceFn->code.data,
                                                traceFn->code.count)) {
                    trace->loop_handoff_exits++;
                    if (trace->loop_handoff_exits ==
                            JIT_RE_RECORD_HANDOFF_EXITS &&
                        !trace->disabled) {
                        trace->disabled = true;
                        vm->jit->re_records_done++;
                        int pcOffset = (int)(trace->anchor_pc -
                                             traceFn->code.data);
                        if (pcOffset >= 0 && pcOffset < traceFn->code.count &&
                            traceFn->jitHotCounts != NULL) {
                            traceFn->jitHotCounts[pcOffset] =
                                (uint16_t)(vm->jit->hot_threshold - 1);
                        }
                        if (wrenJitDebugEnabled()) {
                            fprintf(stderr,
                                    "[JIT] retire degenerate trace anchor=%p "
                                    "re-record #%u (exit snap=%d)\n",
                                    (void*)trace->anchor_pc,
                                    vm->jit->re_records_done, exitIdx);
                        }
                    }
                }
            }
        }
        if (getenv("WREN_JIT_TRACE_EXITS")) {
            extern const char* const wrenOpNames[];
            uint8_t* base = frame->closure->fn->code.data;
            long off = (long)(frame->ip - base);
            fprintf(stderr, "[JIT] exit trace=%p sid=%d ip=0x%lx op=%s | ",
                    (void*)trace->anchor_pc, exitIdx, off,
                    frame->ip[0] < 255 && wrenOpNames[frame->ip[0]]
                        ? wrenOpNames[frame->ip[0]] : "?");
            for (long k = off - 6; k <= off + 14; k++) {
                if (k < 0) { fprintf(stderr, ".. "); continue; }
                uint8_t op = base[k];
                fprintf(stderr, "%s:%ld ", op < 255 ? wrenOpNames[op] : "?", k);
            }
            // Dump module var 26 (seed) for fasta debugging.
            if (frame->closure->fn->module &&
                frame->closure->fn->module->variables.count > 26) {
                Value* mv = frame->closure->fn->module->variables.data;
                fprintf(stderr, " | seed=%g", IS_NUM(mv[26]) ? AS_NUM(mv[26]) : -999.0);
            }
            // Dump materialized stack slots after the restore.
            fprintf(stderr, " | stack[");
            Value* ss = frame->stackStart;
            for (int si = 0; si < trace->snapshots[exitIdx].stack_depth; si++) {
                double dv = 0;
                if (IS_NUM(ss[si])) dv = AS_NUM(ss[si]);
                fprintf(stderr, "%d=%s%g ", si, IS_NUM(ss[si]) ? "" : "n", dv);
            }
            fprintf(stderr, "]");
        }
    }

    return result;
}

bool wrenJitIncrementHot(WrenJitState* jit, uint16_t* hot_count)
{
    if (jit == NULL || !jit->enabled || hot_count == NULL) return false;

    // Do not keep dirtying memory for compiled/failed loops. Saturating also
    // prevents a failed loop from wrapping around and becoming hot again.
    if (*hot_count >= (uint16_t)jit->hot_threshold) return false;

    (*hot_count)++;
    return *hot_count == (uint16_t)jit->hot_threshold;
}

void wrenJitBlacklistCurrent(WrenJitState* jit)
{
    if (jit != NULL && jit->active_hot_count != NULL) {
        *jit->active_hot_count = JIT_HOT_BLACKLISTED;
        jit->active_hot_count = NULL;
    }
}

void wrenJitStartRecording(WrenJitState* jit, uint8_t* pc)
{
    if (jit->state != JIT_STATE_IDLE) return;

    // Allocate and initialize a fresh IR buffer for this trace.
    IRBuffer* ir = (IRBuffer*)calloc(1, sizeof(IRBuffer));
    if (!ir) return;
    irBufferInit(ir);

    // Pre-allocate NOP slots for the variable-promotion pass.
    // These slots (indices 0..JIT_PRE_HEADER_SLOTS-1) precede the loop
    // header and can be converted to LOAD+PHI pairs by irOptPromoteLoopVars.
    // JIT_PRE_HEADER_SLOTS must be even; irOptPromoteLoopVars uses 2 slots
    // per promoted variable (one for the moved LOAD, one for the PHI).
    for (int _k = 0; _k < JIT_PRE_HEADER_SLOTS; _k++) {
        irEmit(ir, IR_NOP, IR_NONE, IR_NONE, IR_TYPE_VOID);
    }

    // Emit the loop header marker.
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
    wrenJitBlacklistCurrent(jit);
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
    if (vm == NULL || jit == NULL || jit->traces == NULL) return;

    for (uint32_t i = 0; i < jit->trace_capacity; i++) {
        JitTrace* t = &jit->traces[i];
        if (t->anchor_pc == NULL || t->gc_roots == NULL) continue;
        for (uint16_t j = 0; j < t->num_gc_roots; j++) {
            if (t->gc_roots[j] != NULL)
                wrenGrayObj(vm, (Obj*)t->gc_roots[j]);
        }
    }
}

// ---------------------------------------------------------------------------
// wrenJitCompileAndStore
// ---------------------------------------------------------------------------

JitTrace* wrenJitCompileAndStore(WrenVM* vm, WrenJitState* jit,
                                   ObjFiber* fiber, void* framePtr)
{
    (void)framePtr;

    if (!jit || (jit->state != JIT_STATE_RECORDING &&
                 jit->state != JIT_STATE_COMPILING)) return NULL;

    // Compute module variables base for offset-based codegen.
    void* modVarsBase = NULL;
    if (fiber && fiber->numFrames > 0) {
        CallFrame* frame = &fiber->frames[fiber->numFrames - 1];
        if (frame->closure && frame->closure->fn && frame->closure->fn->module)
            modVarsBase = (void*)frame->closure->fn->module->variables.data;
    }

    // Transition out of recording state.
    jit->state = JIT_STATE_IDLE;

    // Get the IR from the recorder (jitRecorderStep built it).
    JitRecorder* rec = jitRecorderGet(jit);
    if (!rec) {
        JIT_DEBUG_LOG("[JIT] compile: no recorder\n");
        jit->traces_aborted++;
        wrenJitBlacklistCurrent(jit);
        return NULL;
    }
    IRBuffer* ir = &rec->ir;

    // Require at least one guard/arithmetic node between LOOP_HEADER and
    // LOOP_BACK. A trace without guards would loop forever in native code.
    if (ir->snapshot_count == 0) {
        JIT_DEBUG_LOG("[JIT] compile: no snapshots, aborting\n");
        jit->traces_aborted++;
        wrenJitBlacklistCurrent(jit);
        return NULL;
    }

    // Run optimizer.
    JIT_DEBUG_LOG("[JIT] DEBUG: before irOptimize, count=%u anchor=%p\n",
                  ir->count, (void*)jit->anchor_pc);
    if (getenv("WREN_JIT_DBG_BC")) {
        CallFrame* f0 = &fiber->frames[fiber->numFrames - 1];
        fprintf(stderr, "[JIT] compile anchor pc=%p fn=%s idx=%ld\n",
                (void*)jit->anchor_pc,
                f0->closure && f0->closure->fn && f0->closure->fn->debug->name
                    ? f0->closure->fn->debug->name : "?",
                (long)((uint8_t*)jit->anchor_pc - f0->closure->fn->code.data));
    }
    if (getenv("WREN_JIT_DUMP_IR_RAW")) {
        fprintf(stderr, "---- IR RAW (%u nodes, %u snapshots) ----\n",
                ir->count, ir->snapshot_count);
        irBufferDump(ir); fflush(stdout);
    }
    irOptimize(ir);
    irMarkSnapshotOnlyBoxes(ir);
    JIT_DEBUG_LOG("[JIT] DEBUG: after irOptimize, count=%u\n", ir->count);

    // Register allocation.
    RegAllocState ra;
    JIT_DEBUG_LOG("[JIT] DEBUG: regAllocInit\n");
    regAllocInit(&ra, (int)ir->count);
    JIT_DEBUG_LOG("[JIT] DEBUG: regAllocComputeRanges\n");
    regAllocComputeRanges(&ra, ir);
    JIT_DEBUG_LOG("[JIT] DEBUG: regAllocRun\n");
    regAllocRun(&ra);
    JIT_DEBUG_LOG("[JIT] DEBUG: regAlloc done, ranges=%d\n", ra.num_ranges);

    // Dump IR if requested.
    if (getenv("WREN_JIT_DUMP_IR_PRE")) {
        fprintf(stderr, "---- IR PRE-OPT (%u nodes, %u snapshots) ----\n",
                ir->count, ir->snapshot_count);
        irBufferDump(ir); fflush(stdout);
    }
    if (getenv("WREN_JIT_DUMP_IR")) { irBufferDump(ir); fflush(stdout); }

    // TEMP: dump the whole containing fn bytecode for exit-pc mapping.
    if (getenv("WREN_JIT_DUMP_BC") && fiber && fiber->numFrames > 0) {
        CallFrame* f0 = &fiber->frames[fiber->numFrames - 1];
        if (f0->closure && f0->closure->fn) wrenDumpCode(vm, f0->closure->fn);
    }

    // Code generation.  (ir is part of the recorder struct, not heap-allocated.)
    JIT_DEBUG_LOG("[JIT] DEBUG: wrenJitCodegen start\n");
    JitTrace* trace = wrenJitCodegen(vm, ir, &ra, jit->anchor_pc, modVarsBase);
    JIT_DEBUG_LOG("[JIT] DEBUG: wrenJitCodegen done, trace=%p\n", (void*)trace);
    regAllocFree(&ra);

    if (!trace) {
        JIT_DEBUG_LOG("[JIT] compile: codegen failed\n");
        jit->traces_aborted++;
        wrenJitBlacklistCurrent(jit);
        return NULL;
    }

    trace->anchor_pc = jit->anchor_pc;
    jit->active_hot_count = NULL;
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
