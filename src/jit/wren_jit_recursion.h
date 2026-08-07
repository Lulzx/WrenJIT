#ifndef wren_jit_recursion_h
#define wren_jit_recursion_h

#include <stdint.h>

struct WrenVM;
typedef int64_t (*JitRecursiveIntFunc)(int64_t argument);
typedef int64_t (*JitRecursiveIntFunc2)(int64_t, int64_t);
typedef int64_t (*JitRecursiveIntFunc3)(int64_t, int64_t, int64_t);

#define JIT_RECURSIVE_RESULT_NUM  1
#define JIT_RECURSIVE_RESULT_BOOL 2
#define JIT_RECURSIVE_TREE_CHECK  3
#define JIT_RECURSIVE_TREE_MAKE   4

void* wrenJitCompileRecursiveNumeric(struct WrenVM* vm, void* function,
                                     void* receiverClass,
                                     uint8_t* resultKind, uint8_t* arity);
int wrenJitRunTreeCheck(uint64_t receiver, void* expectedClass,
                        int64_t* result);
int wrenJitRunTreeMake(struct WrenVM* vm, void* nodeClass, int64_t depth,
                       uint64_t* resultSlot);
void wrenJitFreeRecursiveCode(void* code, uint8_t kind);

#endif
