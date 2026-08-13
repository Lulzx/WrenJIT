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

// Sentinel stored in a loop counter after recording or compilation fails.
#define JIT_HOT_BLACKLISTED UINT16_MAX

// Maximum traces in the cache
#define JIT_MAX_TRACES 1024

// Number of NOP slots pre-allocated before the loop header so that
// irOptPromoteLoopVars can fill them with LOAD + UNBOX + PHI tuples.
// Must be even; irOptPromoteLoopVars uses 3 slots per promoted variable
// (LOAD, UNBOX, PHI). 32 handles up to 10 loop-carried module variables.
#define JIT_PRE_HEADER_SLOTS 32

// A trace recorded on a path that skipped a nested loop has only the loop's
// entry guard in-trace; the common path deopts to the interpreter at the
// nested loop's body on every entry. After this many such handoff exits the
// trace is retired and its loop's hot counter re-armed so the next pass
// re-records -- a re-record through the loop compiles it as a native nested
// loop with in-trace exits (the LOOP_ONLY resume in wren_jit_trace.c), which
// eliminates the storm. Re-records are bounded so a loop whose body always
// skips the nested loop cannot thrash the recorder forever.
#define JIT_RE_RECORD_HANDOFF_EXITS 100
#define JIT_MAX_RE_RECORDS 8

// Recording that starts on an inner loop's terminating back-edge ("recording
// started at loop completion") re-arms the hot counter to retry at a fresh
// entry. A loop that never reaches a clean entry would otherwise re-record
// forever; give up after this many consecutive retries of the same anchor.
#define JIT_MAX_LOOP_RETRIES 16

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

    // Known loop-condition exits and diagnostics for exits through other
    // guards. Exit frequency alone is not a profitability measurement because
    // a native prefix may do substantial work before deoptimizing.
    uint16_t loop_exit_snapshot;
    uint16_t loop_exit_snapshot2;
    uint32_t bad_exit_count;
    bool disabled;

    // Exits whose resume_pc lies inside a nested loop's body (the trace was
    // recorded on a path that skipped that loop, so it deopts on the common
    // path). Drives the degenerate-trace re-record policy.
    uint32_t loop_handoff_exits;

    // Number of IR_LIST_BOUNDS_GUARD nodes in this trace (diagnostic for
    // tests: > 0 means list bounds were hoisted out of a counted loop).
    uint16_t bounds_hoisted_guards;

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
    uint16_t* active_hot_count;       // counter for the trace being recorded

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
    uint32_t re_records_done;   // degenerate traces retired for re-recording

    // Bounded "recording started at loop completion" retries. Recording that
    // begins on an inner loop's terminating back-edge aborts and re-arms the
    // hot counter to try again at a fresh entry; without a bound, a loop that
    // never reaches a clean entry would re-record forever (an exit storm).
    // Track the last retried anchor and give up after a handful of attempts.
    uint8_t* loop_retry_anchor;
    uint8_t loop_retry_count;

    // Debug (temporary): LIST_LOAD instrumentation cells written by generated
    // code and read back on side exit.
    uintptr_t dbg_list[8];
    uint64_t dbg_list_seq;

    // Runtime flag written by generated code. A sunk module store's exit stub
    // writes the loop-carried PHI back to the module variable only when at
    // least one loop back-edge has executed -- i.e. the store actually ran.
    // Before the first back-edge (a loop-header guard firing on entry) the
    // module variable still holds its pre-loop value and must be left alone;
    // writing the PHI there would clobber it with a truncated integer.
    // Reset to 0 by each trace's prologue, set to 1 by its LOOP_BACK.
    // Placed last so generated code accesses it at an 8-aligned offset.
    uintptr_t trace_store_flag;
} WrenJitState;

// ---- Public API ----

// Initialize JIT state for a VM. Returns NULL on failure.
WrenJitState* wrenJitInit(WrenVM* vm);

// Free all JIT resources.
void wrenJitFree(WrenVM* vm, WrenJitState* jit);

// Enable or disable the JIT.
void wrenJitSetEnabled(WrenJitState* jit, bool enabled);

// True when WREN_JIT_DEBUG is set to something other than "0". All JIT
// diagnostics are gated on this: a trace that fails to record or compile is
// ordinary operation, and writing that to stderr would corrupt the output of an
// embedder that captures it. Looked up once and cached.
bool wrenJitDebugEnabled(void);

// Look up a compiled trace by anchor PC. Returns NULL if not found.
JitTrace* wrenJitLookup(WrenJitState* jit, uint8_t* pc);

// Execute a compiled trace. Returns 0 on success, exit index on side exit.
int wrenJitExecute(WrenVM* vm, JitTrace* trace);

// Increment a loop's per-function hot counter. Returns true exactly once when
// it becomes hot. Blacklisted and already-hot counters require no write.
bool wrenJitIncrementHot(WrenJitState* jit, uint16_t* hot_count);

// Permanently suppress recording attempts for the currently active loop.
void wrenJitBlacklistCurrent(WrenJitState* jit);

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
