#ifndef wren_jit_trace_widen_h
#define wren_jit_trace_widen_h

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
#ifndef wren_h
typedef struct WrenVM WrenVM;
#endif
#ifndef wren_jit_h
typedef struct WrenJitState WrenJitState;
#endif

// Value type (Wren NaN-boxed 64-bit value)
#ifndef WREN_VALUE_TYPE_DEFINED
typedef uint64_t Value;
#endif

// Attempt to inline a CALL_1 method on a non-Num receiver.
// Returns true if the call was successfully inlined (caller should break).
// Returns false if not handled (caller should call jitRecorderAbort).
bool jitTryWidenCall1(WrenJitState* jit, WrenVM* vm, Value* stackStart,
                      uint16_t symbol, uint8_t* ip);

// Attempt to inline a CALL_0 method on a non-Num receiver.
// Returns true if successfully inlined.
// Returns false if not handled.
bool jitTryWidenCall0(WrenJitState* jit, WrenVM* vm, Value* stackStart,
                      uint16_t symbol, uint8_t* ip);

#endif // wren_jit_trace_widen_h
