#include "wren_jit_memory.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #ifdef __APPLE__
    #include <pthread.h>
    #include <libkern/OSCacheControl.h>
    // MAP_JIT is needed on macOS for JIT code on Apple Silicon.
    #ifndef MAP_JIT
      #define MAP_JIT 0x800
    #endif
  #endif
#endif

void* jitMemAlloc(size_t size)
{
    if (size == 0) return NULL;

#ifdef _WIN32
    void* ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    return ptr;
#else
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

  #ifdef __APPLE__
    #if defined(__aarch64__) || defined(__arm64__)
      // On Apple Silicon, MAP_JIT is required and we start in write mode.
      // Exec permission is toggled via pthread_jit_write_protect_np.
      prot |= PROT_EXEC;
      flags |= MAP_JIT;
    #else
      // macOS x86_64: allocate RW, then add exec after writing.
      prot |= PROT_EXEC;
    #endif
  #endif

    void* ptr = mmap(NULL, size, prot, flags, -1, 0);
    if (ptr == MAP_FAILED) return NULL;

  #if !defined(__APPLE__)
    // Linux: make the mapping executable.
    if (mprotect(ptr, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(ptr, size);
        return NULL;
    }
  #endif

    return ptr;
#endif
}

void jitMemFree(void* ptr, size_t size)
{
    if (ptr == NULL) return;

#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

void jitMemBeginWrite(void* ptr, size_t size)
{
    (void)ptr;
    (void)size;

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    // Disable write protection so we can write to JIT memory.
    pthread_jit_write_protect_np(0);
#endif
}

void jitMemEndWrite(void* ptr, size_t size)
{
    (void)ptr;
    (void)size;

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    // Re-enable write protection (memory becomes executable).
    pthread_jit_write_protect_np(1);
    // Clear instruction cache for the written region.
    sys_icache_invalidate(ptr, size);
#endif
}
