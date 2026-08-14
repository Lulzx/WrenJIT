#ifndef wren_jit_trace_h
#define wren_jit_trace_h

#include "wren_jit_ir.h"

// Forward declarations (guarded against redefinition in C99)
#ifndef wren_h
typedef struct WrenVM WrenVM;
#endif
#ifndef wren_value_h
typedef struct sObjFiber ObjFiber;
#endif
#ifndef wren_jit_h
typedef struct WrenJitState WrenJitState;
#endif

#define JIT_TRACE_MAX_INSNS 1000
#define JIT_TRACE_MAX_CALL_DEPTH 8
#define JIT_TRACE_MAX_SLOTS 256

// An open nested loop being recorded inside the anchor trace. When the
// interpreter's CODE_LOOP jumps back to a target that is not the anchor, it is
// either the back-edge of an open nested loop (close it and continue the outer
// body in-trace) or the header of a new nested loop (open it and record its
// body). The loop's condition block emits IR_LOOP_EXIT nodes that, on a falsy
// condition, jump forward past the nested loop to continue the outer body.
typedef struct {
    uint8_t* loop_pc;       // interpreter PC of the loop condition block
    uint16_t ir_header;     // IR node id of the nested IR_LOOP_HEADER
    uint16_t exit_nodes[4]; // IR_LOOP_EXIT ids patched to the after-loop at close
    int exit_count;
    uint8_t* pending_exit_pc; // JUMP_IF target PC once the exit test is seen;
                              // NULL while still recording the condition block

    // Outer closure support: instead of closing the trace at the nested
    // back-edge, the loop can be marked loop_only and its remaining iterations
    // run in the interpreter while recording is suppressed. When the
    // interpreter exits the loop (detected at one of exit_pcs), recording
    // resumes with the outer body so the after-loop code stays in-trace.
    uint8_t* exit_pcs[4];   // PC where each exit node was emitted
    uint8_t* backedge_pc;   // PC of this loop's CODE_LOOP (last body instr)
    bool loop_only;         // running out in the interpreter under suppression
    int frame_depth;        // fiber->numFrames when marked loop_only
    int open_stack_top;     // recorder stack_top when the loop was opened
} NestedLoop;

#define JIT_MAX_NESTED_LOOPS 8

// Maximum suppressed steps before falling back to the old close-at-back-edge
// behavior. Keeps pathological loops (e.g. pidigits' 2000-iteration inner
// loop) from blowing the recording budget; the fallback reproduces the exact
// trace shape the pre-outer-closure recorder produced, so there is no
// regression for those traces.
#define JIT_SUPPRESS_MAX 20000

// Recorder state
typedef struct {
    IRBuffer ir;

    // Inner-loop entry branches. When a JUMP_IF is reached inside an open
    // nested loop but is not that loop's exit test, it may be the entry test
    // of a deeper nested loop. It is recorded as a deopt-capable IR_LOOP_EXIT
    // (falsy -> side exit) until a back-edge proves it is a loop; when the
    // loop's exit test is patched to jump in-trace past the loop, the matching
    // entry branch is patched the same way so an empty inner loop (e.g. the
    // j-loop of nbody when i == 4) skips its body in-trace instead of
    // deopting every outer iteration.
    #define JIT_MAX_PENDING_ENTRY 16
    struct {
        uint8_t* ip;        // JUMP_IF PC of the entry test
        uint16_t node;      // IR_LOOP_EXIT node id
    } pending_entry[JIT_MAX_PENDING_ENTRY];
    int pending_entry_count;

    uint8_t* anchor_pc;    // PC where recording started

    // Slot map: interpreter stack slot -> IR SSA value
    uint16_t slot_map[JIT_TRACE_MAX_SLOTS];
    bool slot_live[JIT_TRACE_MAX_SLOTS];
    int num_slots;

    // Logical stack top (mirrors the interpreter's stack pointer offset
    // relative to stackStart, so slot indices 0..stack_top-1 are in use).
    int stack_top;

    int instr_count;
    int call_depth;

    // A recognized user CALL_0 executes in the interpreter while its effect is
    // already represented in IR. Suppress hooks from that callee frame.
    int suppressed_frame_depth;

    // A recognized straight-line ternary is already represented by a select
    // in IR. Ignore the interpreter's chosen arm until it reaches the merge.
    uint8_t* select_merge_pc;

    // Open nested loops inside the anchor trace (see NestedLoop above).
    int nested_depth;
    NestedLoop nested[JIT_MAX_NESTED_LOOPS];

    // Steps skipped while a loop_only nested loop runs out in the interpreter.
    int suppress_count;

    bool aborted;
    const char* abort_reason;
} JitRecorder;

// Start recording a new trace. Called when a loop becomes hot.
void jitRecorderStart(WrenJitState* jit, uint8_t* anchor_pc, int num_slots,
                      uint16_t* hot_count);

// Record a single bytecode instruction. Returns true if trace completed.
// The recorder needs the VM to inspect current state (stack values, classes, etc.).
bool jitRecorderStep(WrenJitState* jit, WrenVM* vm, uint8_t* ip);

// Abort the current recording.
void jitRecorderAbort(WrenJitState* jit, const char* reason);

// Get the recorder (NULL if not recording).
JitRecorder* jitRecorderGet(WrenJitState* jit);

#endif
