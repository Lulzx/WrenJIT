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

// Recorder state
typedef struct {
    IRBuffer ir;

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
    bool aborted;
    const char* abort_reason;
} JitRecorder;

// Start recording a new trace. Called when a loop becomes hot.
void jitRecorderStart(WrenJitState* jit, uint8_t* anchor_pc, int num_slots);

// Record a single bytecode instruction. Returns true if trace completed.
// The recorder needs the VM to inspect current state (stack values, classes, etc.).
bool jitRecorderStep(WrenJitState* jit, WrenVM* vm, uint8_t* ip);

// Abort the current recording.
void jitRecorderAbort(WrenJitState* jit, const char* reason);

// Get the recorder (NULL if not recording).
JitRecorder* jitRecorderGet(WrenJitState* jit);

#endif
