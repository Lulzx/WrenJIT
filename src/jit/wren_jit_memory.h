#ifndef WREN_JIT_MEMORY_H
#define WREN_JIT_MEMORY_H

#include <stddef.h>

// Allocate executable+writable memory of [size] bytes.
// Returns NULL on failure.
void* jitMemAlloc(size_t size);

// Free executable memory previously allocated with jitMemAlloc.
void jitMemFree(void* ptr, size_t size);

// On Apple Silicon (arm64): switch memory to write mode before writing.
void jitMemBeginWrite(void* ptr, size_t size);

// On Apple Silicon: switch memory back to exec mode after writing.
void jitMemEndWrite(void* ptr, size_t size);

#endif
