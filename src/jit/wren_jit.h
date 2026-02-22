#ifndef wren_jit_h
#define wren_jit_h

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Forward declarations (guarded to avoid redefinition if wren_value.h is included)
#ifndef wren_h
typedef struct WrenVM WrenVM;
#endif
#ifndef wren_value_h
typedef struct sObjFiber ObjFiber;
typedef struct sObjClass ObjClass;
#endif

// JIT hot count threshold
#define JIT_HOT_THRESHOLD 50

// Maximum traces in the cache
#define JIT_MAX_TRACES 1024

// Trace execution function type
// Returns 0 on success, or exit index (1-based) on side exit
// Args: vm, fiber, stackStart, moduleVarsData (Value* to module variables array)
typedef int (*JitTraceFunc)(WrenVM* vm, ObjFiber* fiber,
                             void* stackStart, void* moduleVarsData);

// A compiled trace
typedef struct {
    uint8_t* anchor_pc;      // bytecode PC where this trace starts (loop header)
    void* code;              // pointer to executable native code
    uint32_t code_size;      // size of native code in bytes

    // Snapshot data for side exits
    struct JitSnapshot* snapshots;
    uint16_t num_snapshots;

    // GC roots: object pointers embedded in the trace
    void** gc_roots;
    uint16_t num_gc_roots;

    // Statistics
    uint64_t exec_count;
    uint64_t exit_count;
} JitTrace;

// Recording state
typedef enum {
    JIT_STATE_IDLE,          // not recording
    JIT_STATE_RECORDING,     // actively recording a trace
    JIT_STATE_COMPILING,     // compiling recorded IR to native code
} JitRecordState;

// The main JIT state, attached to WrenVM
typedef struct WrenJitState {
    // Trace cache: open-addressing hash table keyed by anchor_pc
    JitTrace* traces;
    uint32_t trace_capacity;
    uint32_t trace_count;

    // Recording state
    JitRecordState state;
    void* recording_ir;              // legacy field (unused, kept for ABI compat)
    uint8_t* anchor_pc;              // PC where recording started
    int record_depth;                // call depth during recording
    int record_count;                // instructions recorded so far

    // Slot map: maps interpreter stack slots to IR SSA values during recording
    uint16_t* slot_map;
    int slot_map_size;

    // Hot count array for all bytecode functions
    // (allocated per-function, stored in ObjFn)

    // Configuration
    bool enabled;
    int hot_threshold;

    // Recorder storage (opaque, allocated on first use)
    void* recorder;

    // Memory management
    struct JitMemoryPool* mem_pool;   // executable memory pool

    // Statistics
    uint64_t traces_compiled;
    uint64_t traces_aborted;
    uint64_t total_exits;
} WrenJitState;

// ---- Public API ----

// Initialize JIT state for a VM. Returns NULL on failure.
WrenJitState* wrenJitInit(WrenVM* vm);

// Free all JIT resources.
void wrenJitFree(WrenVM* vm, WrenJitState* jit);

// Enable or disable the JIT.
void wrenJitSetEnabled(WrenJitState* jit, bool enabled);

// Look up a compiled trace by anchor PC. Returns NULL if not found.
JitTrace* wrenJitLookup(WrenJitState* jit, uint8_t* pc);

// Execute a compiled trace. Returns 0 on success, exit index on side exit.
int wrenJitExecute(WrenVM* vm, JitTrace* trace);

// Increment hot count for a loop at the given PC in the given function.
// Returns true if the loop just became hot (should start recording).
bool wrenJitIncrementHot(WrenJitState* jit, uint8_t* bytecode,
                          uint16_t* hot_counts, int pc_offset);

// Start recording a trace at the given PC.
void wrenJitStartRecording(WrenJitState* jit, uint8_t* pc);

// Abort the current recording.
void wrenJitAbortRecording(WrenJitState* jit);

// Check if currently recording.
static inline bool wrenJitIsRecording(const WrenJitState* jit) {
    return jit != NULL && jit->state == JIT_STATE_RECORDING;
}

// Record a single bytecode instruction (called from interpreter dispatch).
// Returns true if recording completed (trace is ready to compile).
bool wrenJitRecordInstruction(WrenJitState* jit, WrenVM* vm, uint8_t* ip);

// Store a compiled trace into the cache.
void wrenJitStoreTrace(WrenJitState* jit, JitTrace* trace);

// Mark JIT roots for GC.
void wrenJitMarkRoots(WrenVM* vm, WrenJitState* jit);

// Compile the current in-progress recording and store it in the trace cache.
// Called when the recorder detects the loop-back edge.
// Returns the compiled trace, or NULL if compilation failed.
JitTrace* wrenJitCompileAndStore(WrenVM* vm, WrenJitState* jit,
                                  ObjFiber* fiber, void* frame);

// Restore interpreter state after a side exit.
// exitIdx is 0-based (JitTraceFunc return value - 1).
void wrenJitRestoreExit(WrenVM* vm, WrenJitState* jit,
                         ObjFiber* fiber, void* frame,
                         JitTrace* trace, int exitIdx);

#endif
